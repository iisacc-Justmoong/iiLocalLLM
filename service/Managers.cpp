#include "Managers.h"
#include <QtCore/QFileInfo>
#include <QtCore/QUuid>
#include <algorithm>
#include <cmath>

namespace iiLocalLLM::detail {
namespace {
void require(bool condition, ErrorCode code, const char* message)
{ if (!condition) throw Error(code, QString::fromUtf8(message)); }
}
void RuntimeManager::addRuntime(std::shared_ptr<Runtime> runtime)
{
    require(runtime && !runtime->id().trimmed().isEmpty(), ErrorCode::InvalidArgument, "Runtime id is required");
    const auto id = runtime->id();
    require(!runtimes_.contains(id), ErrorCode::AlreadyExists, "Runtime already registered");
    runtimes_.emplace(id, std::move(runtime));
}
MemoryEstimate RuntimeManager::estimateMemory(const ModelSpec& model, int contextSlots) const
{
    MemoryEstimate result;
    bool found = false;
    for (const auto& [id, runtime] : runtimes_) {
        if (!runtime->supportsModel(model)) continue;
        const auto estimate = runtime->estimateMemory(model, contextSlots);
        if (!found || estimate.totalBytes() > result.totalBytes()) result = estimate;
        found = true;
    }
    require(found, ErrorCode::RuntimeUnavailable, "No installed runtime supports this model format");
    require(result.totalBytes() > 0, ErrorCode::RuntimeFailure, "Runtime returned an empty memory estimate");
    return result;
}
RuntimeModelInfo RuntimeManager::load(ModelSpec model, const CancellationToken& token)
{
    require(!model.id.trimmed().isEmpty() && !model.path.trimmed().isEmpty() && model.contextTokens > 1,
            ErrorCode::InvalidArgument, "Model id, path and contextTokens > 1 are required");
    require(!models_.contains(model.id), ErrorCode::AlreadyExists, "Model already loaded");
    require(models_.size() < static_cast<size_t>(limit_), ErrorCode::ResourceLimit, "Loaded model limit reached");
    for (const auto& key : {"runtime", "backend", "device", "device_id", "gpu_layers", "n_gpu_layers"})
        require(!model.options.contains(QString::fromLatin1(key)), ErrorCode::InvalidArgument,
                "Execution backend and device are selected by the service; remove model options overrides");
    std::map<QString, QList<RuntimeDevice>> candidates;
    QList<RuntimeDevice> allDevices;
    for (const auto& [id, runtime] : runtimes_) {
        token.throwIfCancelled();
        if (!runtime->supportsModel(model)) continue;
        auto devices = runtime->devices(hardware_);
        allDevices.append(devices);
        candidates.emplace(id, std::move(devices));
    }
    if (candidates.empty() && !QFileInfo::exists(model.path))
        throw Error(ErrorCode::NotFound, QStringLiteral("Local model path not found"));
    require(!candidates.empty(), ErrorCode::RuntimeUnavailable, "No installed runtime supports this local model format");
    const auto ordered = orderExecutionDevices(hardware_, allDevices);
    require(!ordered.isEmpty(), ErrorCode::RuntimeUnavailable, "No policy-compatible execution device is available for this model");
    QStringList failures;
    for (const auto& device : ordered) {
        for (const auto& [id, devices] : candidates) {
            if (!devices.contains(device)) continue;
            token.throwIfCancelled();
            try {
                auto loaded = runtimes_.at(id)->load(model, device, token);
                token.throwIfCancelled();
                require(bool(loaded), ErrorCode::RuntimeFailure, "Runtime returned a null model");
                QString reason;
                switch (device.backend) {
                case ComputeBackend::Metal: reason = QStringLiteral("Apple Silicon with usable Metal"); break;
                case ComputeBackend::Cuda: reason = QStringLiteral("NVIDIA GPU with usable CUDA"); break;
                case ComputeBackend::Vulkan: reason = QStringLiteral("AMD/Intel GPU with usable Vulkan"); break;
                case ComputeBackend::Cpu: reason = failures.isEmpty()
                    ? QStringLiteral("No policy-compatible GPU acceleration supported by this model runtime")
                    : QStringLiteral("GPU model initialization failed; using CPU"); break;
                }
                ExecutionSelection selection{id, device, reason, failures};
                models_.emplace(model.id, LoadedModel{model, selection, std::move(loaded)});
                return {model, selection};
            } catch (const Error& error) {
                token.throwIfCancelled();
                if (device.backend == ComputeBackend::Cpu
                    || (error.code() != ErrorCode::RuntimeUnavailable && error.code() != ErrorCode::RuntimeFailure
                        && error.code() != ErrorCode::ResourceLimit)) throw;
                failures.append(id + QLatin1Char('/') + enumName(device.backend) + QStringLiteral(": ") + QString::fromUtf8(error.what()));
            }
        }
    }
    throw Error(ErrorCode::RuntimeUnavailable, QStringLiteral("No execution device could load the model: ") + failures.join(QStringLiteral("; ")));
}
LoadedModel& RuntimeManager::get(const QString& id)
{
    const auto found = models_.find(id);
    require(found != models_.end(), ErrorCode::NotFound, "Model not found");
    return found->second;
}
void RuntimeManager::unload(const QString& id) { get(id); models_.erase(id); }
QList<RuntimeModelInfo> RuntimeManager::list() const
{
    QList<RuntimeModelInfo> result;
    for (const auto& [id, model] : models_) result.push_back({model.spec, model.execution});
    return result;
}
QString SessionManager::create(QString modelId, QString systemPrompt)
{
    require(sessions_.size() < static_cast<size_t>(limit_), ErrorCode::ResourceLimit, "Session limit reached");
    SessionSnapshot s{QUuid::createUuid().toString(QUuid::WithoutBraces), std::move(modelId), {}};
    if (!systemPrompt.isEmpty()) s.messages.push_back({Role::System, std::move(systemPrompt)});
    sessions_.emplace(s.id, s);
    return s.id;
}
SessionSnapshot& SessionManager::get(const QString& id)
{
    const auto found = sessions_.find(id);
    require(found != sessions_.end(), ErrorCode::NotFound, "Session not found");
    return found->second;
}
void SessionManager::reset(const QString& id)
{
    auto& messages = get(id).messages;
    if (!messages.isEmpty() && messages.front().role == Role::System) messages = {messages.front()};
    else messages.clear();
}
void SessionManager::close(const QString& id) { get(id); sessions_.erase(id); }
bool SessionManager::usesModel(const QString& id) const
{
    return std::any_of(sessions_.begin(), sessions_.end(), [&](const auto& pair) { return pair.second.modelId == id; });
}
RuntimeContext& ContextCacheManager::acquire(const QString& id, LoadedModel& model, const CancellationToken& token)
{
    if (auto found = entries_.find(id); found != entries_.end()) {
        found->second.access = ++clock_;
        return *found->second.context;
    }
    const int needed = model.spec.contextTokens;
    require(needed <= maxTokens_, ErrorCode::ResourceLimit, "Model context exceeds cache token budget");
    while (!entries_.empty() && (entries_.size() >= static_cast<size_t>(maxCount_) || reserved_ > maxTokens_ - needed)) {
        const auto oldest = std::min_element(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
            return a.second.access < b.second.access;
        });
        erase(oldest->first);
        ++evictions_;
    }
    auto context = model.runtime->createContext(token);
    require(bool(context), ErrorCode::RuntimeFailure, "Runtime returned a null context");
    auto [inserted, ok] = entries_.emplace(id, Entry{model.spec.id, needed, ++clock_, std::move(context)});
    reserved_ += needed;
    return *inserted->second.context;
}
void ContextCacheManager::erase(const QString& id)
{
    if (auto found = entries_.find(id); found != entries_.end()) {
        reserved_ -= found->second.reservedTokens;
        entries_.erase(found);
    }
}
void ContextCacheManager::eraseModel(const QString& modelId)
{
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.modelId == modelId) { reserved_ -= it->second.reservedTokens; it = entries_.erase(it); ++evictions_; }
        else ++it;
    }
}
void ContextCacheManager::describe(ServiceStats& stats) const
{
    stats.cachedContexts = static_cast<int>(entries_.size());
    stats.reservedContextTokens = reserved_;
    stats.cacheEvictions = evictions_;
}
PreparedChat PromptEngine::prepare(const SessionSnapshot& session, const ChatRequest& request,
    LoadedModel& model, const CancellationToken& token, int inputLimit)
{
    const auto& o = request.options;
    require(!request.prompt.trimmed().isEmpty() && request.prompt.size() <= inputLimit,
            ErrorCode::InvalidArgument, "Prompt is empty or exceeds input limit");
    require(o.maxTokens > 0 && o.maxTokens < model.spec.contextTokens && std::isfinite(o.temperature)
            && o.temperature >= 0 && o.temperature <= 10 && std::isfinite(o.topP) && o.topP > 0
            && o.topP <= 1 && o.topK >= 0 && o.stop.size() <= 16,
            ErrorCode::InvalidArgument, "Invalid generation options");
    for (const auto& stop : o.stop)
        require(!stop.isEmpty() && stop.size() <= 1024, ErrorCode::InvalidArgument, "Invalid stop string");
    PreparedChat prepared{session.messages, {}, 0};
    prepared.messages.push_back({Role::User, request.prompt});
    const int firstTurn = !prepared.messages.isEmpty() && prepared.messages.front().role == Role::System ? 1 : 0;
    for (;;) {
        token.throwIfCancelled();
        prepared.tokens = model.runtime->tokenize(prepared.messages, token);
        require(!prepared.tokens.isEmpty(), ErrorCode::RuntimeFailure, "Tokenizer returned an empty prompt");
        if (prepared.tokens.size() <= model.spec.contextTokens - o.maxTokens) return prepared;
        require(prepared.messages.size() > firstTurn + 2, ErrorCode::ContextOverflow,
                "System prompt and newest turn do not fit the context with reserved output tokens");
        prepared.messages.remove(firstTurn, 2);
        prepared.droppedMessages += 2;
    }
}
QString StopFilter::push(const QString& text)
{
    if (stopped_) return {};
    pending_ += text;
    qsizetype match = -1;
    for (const auto& stop : stops_) {
        const auto pos = pending_.indexOf(stop);
        if (pos >= 0 && (match < 0 || pos < match)) match = pos;
    }
    if (match >= 0) {
        stopped_ = true;
        const auto output = pending_.left(match);
        pending_.clear();
        return output;
    }
    qsizetype held = 0;
    for (const auto& stop : stops_)
        for (qsizetype n = 1; n < stop.size() && n <= pending_.size(); ++n)
            if (pending_.endsWith(stop.first(n))) held = std::max(held, n);
    // Do not split a UTF-16 surrogate pair across stream events.
    if (!pending_.isEmpty() && pending_.back().isHighSurrogate()) held = std::max(held, qsizetype(1));
    const auto output = pending_.first(pending_.size() - held);
    pending_.remove(0, output.size());
    return output;
}
QString StopFilter::finish() { return std::exchange(pending_, {}); }
} // namespace iiLocalLLM::detail
