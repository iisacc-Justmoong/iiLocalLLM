#include "ModelManager.h"
#include <algorithm>

namespace iiLocalLLM::detail {
ModelManager::ModelManager(const ServiceOptions& options, HardwareInfo hardware)
    : options_(options), catalog_(options.modelsDirectory), registry_(options.registryFile),
      runtimes_(options.maxModels, hardware), residency_(options, hardware) {}
void ModelManager::addRuntime(std::shared_ptr<Runtime> runtime) { runtimes_.addRuntime(std::move(runtime)); }
ModelRecord ModelManager::install(const QString& directory, const CancellationToken& token) { return catalog_.install(directory, token); }
ModelRecord ModelManager::pull(const QString& reference, const CancellationToken& token, const PullCallback& progress)
{
    auto result = registry_.pull(reference, catalog_, options_.modelsDirectory, token, progress);
    result.loaded = loaded_.contains(result.manifest.id);
    return result;
}
void ModelManager::remove(const QString& reference)
{
    const auto uri = canonical(reference);
    if (loaded_.contains(modelId(uri))) throw Error(ErrorCode::ModelInUse, QStringLiteral("Unload the model before removing its installed files"));
    catalog_.remove(uri);
    remembered_.erase(modelId(uri));
}
ModelListing ModelManager::list()
{
    auto result = catalog_.list();
    for (auto& model : result.models) model.loaded = loaded_.contains(model.manifest.id);
    return result;
}
ModelRecord ModelManager::resolve(const QString& uri)
{
    auto record = catalog_.resolve(canonical(uri)).record;
    record.loaded = loaded_.contains(record.manifest.id);
    return record;
}
ModelVerification ModelManager::verify(const QString& uri, const CancellationToken& token) { return catalog_.verify(canonical(uri), token); }
ModelInfo ModelManager::load(const ModelLoadRequest& input, const CancellationToken& token)
{
    auto request = input;
    request.model = canonical(request.model);
    for (const auto* key : {"runtime", "backend", "device", "device_id", "gpu_layers", "n_gpu_layers"})
        if (request.options.contains(QString::fromLatin1(key)))
            throw Error(ErrorCode::InvalidArgument, QStringLiteral("Execution device is selected by the service"));
    const auto id = modelId(request.model);
    if (request.contextTokens < 0) throw Error(ErrorCode::InvalidArgument, QStringLiteral("contextTokens must be zero or positive"));
    if (request.keepAliveMs < -1 || request.keepAliveMs > 7LL * 86400000)
        throw Error(ErrorCode::InvalidArgument, QStringLiteral("keepAliveMs must be -1 or 0..7 days"));
    if (const auto existing = loaded_.find(id); existing != loaded_.end()) {
        if ((request.contextTokens && request.contextTokens != existing->second.contextTokens)
            || (!request.options.isEmpty() && request.options != existing->second.options))
            throw Error(ErrorCode::ModelInUse, QStringLiteral("Unload the model before changing its load configuration"));
        residency_.touch(id, request.keepAliveMs);
        remembered_.at(id).keepAliveMs = residency_.get(id).keepAliveMs;
        publish();
        return loadedInfo(id);
    }
    auto config = request;
    if (const auto previous = remembered_.find(id); previous != remembered_.end()) {
        if (!config.contextTokens) config.contextTokens = previous->second.contextTokens;
        if (config.options.isEmpty()) config.options = previous->second.options;
        if (config.keepAliveMs < 0) config.keepAliveMs = previous->second.keepAliveMs;
    }
    const auto resolved = catalog_.resolve(request.model);
    const int context = config.contextTokens ? config.contextTokens
        : std::min({resolved.record.manifest.contextLength, options_.defaultContextTokens, options_.maxCachedContextTokens});
    if (context < 2) throw Error(ErrorCode::InvalidArgument, QStringLiteral("Model context must exceed one token"));
    if (context > resolved.record.manifest.contextLength)
        throw Error(ErrorCode::ContextOverflow, QStringLiteral("Requested context exceeds manifest context_length"));
    if (context > options_.maxCachedContextTokens)
        throw Error(ErrorCode::ResourceLimit, QStringLiteral("Model context exceeds cache token budget"));
    const auto verification = catalog_.verify(request.model, token);
    if (!verification.valid) throw Error(ErrorCode::IntegrityFailure, verification.issues.join(QStringLiteral("; ")));
    const ModelSpec spec{id, resolved.entryPath, context, config.options, resolved.record.manifest.format};
    const auto memory = runtimes_.estimateMemory(spec, std::min(options_.maxCachedContexts, options_.maxCachedContextTokens / context));
    if (memory.totalBytes() > residency_.budget())
        throw Error(ErrorCode::ResourceLimit, QStringLiteral("Model weights, KV and runtime reservation exceed the service memory budget"));
    while (!residency_.fits(memory.totalBytes(), availableRamBytes())) {
        token.throwIfCancelled();
        const auto victim = residency_.oldestIdle();
        if (victim.isEmpty()) throw Error(ErrorCode::ResourceLimit, QStringLiteral("Insufficient available RAM or no idle model can be evicted"));
        evict(victim, true);
    }
    const auto runtime = runtimes_.load(spec, token);
    ModelInfo info{resolved.record, context, config.options, runtime.execution};
    info.model.loaded = true;
    try {
        residency_.loaded(id, memory, config.keepAliveMs);
        loaded_.emplace(id, info);
        remembered_[id] = {request.model, context, config.options, residency_.get(id).keepAliveMs};
    } catch (...) {
        runtimes_.unload(id); residency_.erased(id, false); loaded_.erase(id); throw;
    }
    publish();
    return loadedInfo(id);
}
void ModelManager::unload(const QString& uri)
{
    const auto id = modelId(canonical(uri));
    (void)loadedInfo(id);
    evict(id, false);
}
QList<ModelInfo> ModelManager::loaded() const
{
    QList<ModelInfo> result;
    for (const auto& [id, info] : loaded_) result.append(loadedInfo(id));
    return result;
}
LoadedModel& ModelManager::get(const QString& id) { return runtimes_.get(id); }
ModelInfo ModelManager::loadedInfo(const QString& id) const
{
    const auto found = loaded_.find(id);
    if (found == loaded_.end()) throw Error(ErrorCode::NotFound, QStringLiteral("Model is not loaded"));
    auto info = found->second;
    const auto& entry = residency_.get(id);
    info.memory = entry.memory;
    info.keepAliveMs = entry.keepAliveMs;
    info.activeRequests = entry.active;
    info.expiresInMs = entry.active ? -1 : std::max(qint64(0), qint64(std::chrono::duration_cast<std::chrono::milliseconds>(
        entry.expires - ModelResidencyManager::Clock::now()).count()));
    return info;
}
LoadedModel& ModelManager::acquire(const QString& id, qint64 keepAlive, const CancellationToken& token)
{
    (void)load({modelUri(id), 0, {}, keepAlive}, token);
    residency_.acquire(id, keepAlive);
    publish();
    return runtimes_.get(id);
}
void ModelManager::release(const QString& id)
{
    residency_.release(id);
    remembered_.at(id).keepAliveMs = residency_.get(id).keepAliveMs;
    maintain();
    publish();
}
void ModelManager::evict(const QString& id, bool automatic)
{
    if (residency_.get(id).active) throw Error(ErrorCode::ModelInUse, QStringLiteral("Active model cannot be unloaded"));
    if (beforeUnload_) beforeUnload_(id);
    runtimes_.unload(id);
    residency_.erased(id, automatic);
    loaded_.erase(id);
    publish();
}
void ModelManager::maintain()
{
    for (const auto& id : residency_.expired()) evict(id, true);
    // Reclaim idle models when another application creates system memory pressure.
    auto available = availableRamBytes();
    while (available && *available < residency_.reserve()) {
        const auto victim = residency_.oldestIdle();
        if (victim.isEmpty()) break;
        evict(victim, true);
        available = availableRamBytes();
    }
}
void ModelManager::describe(ServiceStats& stats) const
{
    const auto available = availableRamBytes();
    stats.residentBytes = residency_.bytes(); stats.memoryBudgetBytes = residency_.budget();
    stats.availableRamBytes = available.value_or(0); stats.availableRamKnown = available.has_value();
    stats.defaultKeepAliveMs = residency_.defaultKeepAlive();
    stats.modelLoads = residency_.loads(); stats.modelEvictions = residency_.evictions();
}
} // namespace iiLocalLLM::detail
