#include "Service.h"
#include "service/Managers.h"
#include "service/ModelManager.h"
#include "service/Scheduler.h"
#include "service/Conversation.h"
#include <QtCore/QUuid>
#include <QtCore/QCryptographicHash>
#include <optional>
#include <mutex>

namespace iiLocalLLM {
using namespace detail;
namespace {
ServiceOptions checked(ServiceOptions o)
{
    if (o.maxQueuedRequests < 1 || o.maxModels < 1 || o.maxSessions < 1 || o.maxCachedContexts < 1
        || o.maxCachedContextTokens < 2 || o.maxInputCharacters < 1 || o.defaultContextTokens < 2 || o.modelsDirectory.trimmed().isEmpty()
        || o.keepAliveMs < -1 || o.keepAliveMs > 7LL * 86400000)
        throw Error(ErrorCode::InvalidArgument, QStringLiteral("Service limits must be positive"));
    return o;
}
void failure(GenerationResult& result, const Error& error)
{
    result.errorCode = error.code();
    result.errorMessage = QString::fromUtf8(error.what());
    result.finishReason = error.code() == ErrorCode::Cancelled ? FinishReason::Cancelled : FinishReason::Error;
    result.toolCalls = {}; // A failed/cancelled request never yields executable calls.
    result.reasoning.clear();
}
void finish(const StreamCallback& callback, const GenerationResult& result)
{
    if (callback) {
        try { callback({StreamEventKind::Finished, result.requestId, result.sessionId, {}, result}); }
        catch (...) {} // Terminal notification cannot prevent future settlement or undo a committed turn.
    }
}
}
class Service::Impl {
public:
    GenerationHandle generate(ChatRequest request, std::optional<CompletionRequest> completion, StreamCallback callback);
    struct State {
        State(const ServiceOptions& o, const HardwareInfo& hardware)
            : models(o, hardware), sessions(o.maxSessions), cache(o.maxCachedContexts, o.maxCachedContextTokens)
        { models.setBeforeUnload([this](const QString& id) { cache.eraseModel(id); }); }
        ModelManager models;
        SessionManager sessions;
        ContextCacheManager cache; // Destroy contexts before model weights.
    };
    Impl(ServiceOptions o, MlxRuntimeOptions mlx) : options(checked(o)), hardware(detectHardware()),
        state(std::make_unique<State>(options, hardware)), scheduler(options.maxQueuedRequests, [this] { state.reset(); },
            [this] { state->models.maintain(); })
    {
        scheduler.submit([this, mlx = std::move(mlx)](const auto&) {
            state->models.setChanged([this](QList<ModelInfo> models) {
                std::lock_guard lock(observationMutex);
                observedModels = std::move(models);
                observedAt = std::chrono::steady_clock::now();
            });
            state->models.addRuntime(createLlamaRuntime());
            state->models.addRuntime(createMlxRuntime(mlx));
        }).get();
    }
    ~Impl()
    {
        // Backend objects are destroyed by the worker exit hook after cancellation/drain.
        scheduler.shutdown();
    }
    ServiceOptions options;
    const HardwareInfo hardware;
    std::unique_ptr<State> state;
    std::mutex observationMutex;
    QList<ModelInfo> observedModels;
    std::chrono::steady_clock::time_point observedAt = std::chrono::steady_clock::now();
    Scheduler scheduler;
};
Service::Service(ServiceOptions options, MlxRuntimeOptions mlx) : d(std::make_unique<Impl>(options, std::move(mlx))) {}
Service::~Service() = default;
HardwareInfo Service::hardware() const { return d->hardware; }
std::future<void> Service::registerRuntime(std::shared_ptr<Runtime> runtime)
{
    return d->scheduler.submit([impl = d.get(), runtime = std::move(runtime)](const auto&) { impl->state->models.addRuntime(runtime); });
}
std::future<ModelRecord> Service::installModel(QString directory)
{ return d->scheduler.submit([impl = d.get(), directory = std::move(directory)](const auto& token) { return impl->state->models.install(directory, token); }); }
ModelPullHandle Service::pullModel(QString reference, PullCallback progress)
{
    ModelPullHandle handle;
    handle.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto promise = std::make_shared<std::promise<ModelRecord>>();
    handle.result = promise->get_future().share();
    const auto token = handle.cancellation;
    d->scheduler.enqueue(token, [impl = d.get(), reference = std::move(reference), progress = std::move(progress), promise, token] {
        try { promise->set_value(impl->state->models.pull(reference, token, progress)); }
        catch (...) { promise->set_exception(std::current_exception()); }
    }, [promise](Error error) { promise->set_exception(std::make_exception_ptr(error)); });
    return handle;
}
std::future<void> Service::removeModel(QString model)
{ return d->scheduler.submit([impl = d.get(), model = std::move(model)](const auto&) {
    if (impl->state->sessions.usesModel(modelId(impl->state->models.canonical(model))))
        throw Error(ErrorCode::ModelInUse, QStringLiteral("Close model sessions before removing"));
    impl->state->models.remove(model);
}); }
std::future<ModelListing> Service::installedModels()
{ return d->scheduler.submit([impl = d.get()](const auto&) { return impl->state->models.list(); }); }
std::future<ModelRecord> Service::resolveModel(QString model)
{ return d->scheduler.submit([impl = d.get(), model = std::move(model)](const auto&) { return impl->state->models.resolve(model); }); }
std::future<ModelVerification> Service::verifyModel(QString model)
{ return d->scheduler.submit([impl = d.get(), model = std::move(model)](const auto& token) { return impl->state->models.verify(model, token); }); }
std::future<ModelInfo> Service::loadModel(ModelLoadRequest model)
{
    return d->scheduler.submit([impl = d.get(), model = std::move(model)](const auto& token) {
        return impl->state->models.load(model, token);
    });
}
std::future<void> Service::unloadModel(QString id)
{
    return d->scheduler.submit([impl = d.get(), id = std::move(id)](const auto&) {
        if (impl->state->sessions.usesModel(modelId(impl->state->models.canonical(id)))) throw Error(ErrorCode::ModelInUse, QStringLiteral("Close model sessions before unloading"));
        impl->state->models.unload(id);
    });
}
std::future<QList<ModelInfo>> Service::models()
{
    // ps/HTTP discovery remains responsive while the inference worker is occupied.
    std::promise<QList<ModelInfo>> promise;
    auto future = promise.get_future();
    {
        std::lock_guard lock(d->observationMutex);
        auto models = d->observedModels;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - d->observedAt).count();
        for (auto& model : models) if (model.expiresInMs >= 0) model.expiresInMs = std::max(qint64(0), model.expiresInMs - elapsed);
        promise.set_value(std::move(models));
    }
    return future;
}
std::future<QString> Service::createSession(QString model, QString system)
{
    return d->scheduler.submit([impl = d.get(), model = std::move(model), system = std::move(system)](const auto&) {
        const auto info = impl->state->models.resolve(model);
        const auto id = info.manifest.id;
        if (!info.manifest.capabilities.contains(QStringLiteral("chat")))
            throw Error(ErrorCode::InvalidArgument, QStringLiteral("Model manifest does not declare the chat capability"));
        if (system.size() > impl->options.maxInputCharacters)
            throw Error(ErrorCode::InvalidArgument, QStringLiteral("System prompt exceeds input limit"));
        return impl->state->sessions.create(id, system);
    });
}
std::future<SessionSnapshot> Service::session(QString id)
{ return d->scheduler.submit([impl = d.get(), id = std::move(id)](const auto&) { return impl->state->sessions.get(id); }); }
std::future<void> Service::resetSession(QString id)
{
    return d->scheduler.submit([impl = d.get(), id = std::move(id)](const auto&) {
        impl->state->sessions.reset(id);
        impl->state->cache.erase(id);
    });
}
std::future<void> Service::closeSession(QString id)
{
    return d->scheduler.submit([impl = d.get(), id = std::move(id)](const auto&) {
        impl->state->sessions.close(id);
        impl->state->cache.erase(id);
    });
}
std::future<ServiceStats> Service::stats()
{
    return d->scheduler.submit([impl = d.get()](const auto&) {
        ServiceStats stats;
        stats.loadedModels = impl->state->models.loaded().size();
        stats.sessions = impl->state->sessions.size();
        impl->state->cache.describe(stats);
        impl->state->models.describe(stats);
        return stats;
    });
}
GenerationHandle Service::chat(ChatRequest request, StreamCallback callback)
{ return d->generate(std::move(request), std::nullopt, std::move(callback)); }
GenerationHandle Service::complete(CompletionRequest request, StreamCallback callback)
{ return d->generate({}, std::move(request), std::move(callback)); }
std::future<ContextBudget> Service::measureConversation(ConversationRequest request, CancellationToken token)
{
    auto promise = std::make_shared<std::promise<ContextBudget>>();
    auto future = promise->get_future();
    d->scheduler.enqueue(token, [impl = d.get(), request = std::move(request), token, promise] {
        QString acquired;
        try {
            token.throwIfCancelled();
            validateConversationRequest(request, impl->options.maxInputCharacters);
            auto& state = *impl->state;
            const auto record = state.models.resolve(request.model);
            if (!record.manifest.capabilities.contains("chat"))
                throw Error(ErrorCode::RuntimeUnavailable, "Model does not support structured chat");
            auto& model = state.models.acquire(record.manifest.id, request.keepAliveMs, token);
            acquired = model.spec.id;
            ContextBudget budget;
            {
                const auto prepared = model.runtime->prepareConversation(request, token);
                token.throwIfCancelled();
                budget = {prepared.tokens.size(), model.spec.contextTokens};
            } // Prompt state may borrow model resources; destroy it before keep_alive=0 can unload the model.
            state.models.release(acquired); acquired.clear();
            promise->set_value(budget);
        } catch (...) {
            if (!acquired.isEmpty()) impl->state->models.release(acquired);
            promise->set_exception(std::current_exception());
        }
    }, [promise](Error error) { promise->set_exception(std::make_exception_ptr(error)); });
    return future;
}
GenerationHandle Service::converse(ConversationRequest request, StreamCallback callback)
{
    GenerationHandle handle;
    handle.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto promise = std::make_shared<std::promise<GenerationResult>>();
    handle.result = promise->get_future().share();
    const auto token = handle.cancellation;
    GenerationResult initial; initial.requestId = handle.requestId; initial.sessionId = request.contextId;
    auto reject = [promise, initial, callback](Error error) mutable {
        failure(initial, error); finish(callback, initial); promise->set_value(initial);
    };
    d->scheduler.enqueue(token, [impl = d.get(), request = std::move(request), callback, promise, token, initial]() mutable {
        auto result = initial;
        QString leasedModel, cacheId;
        auto notify = [&](StreamEvent event) {
            if (!callback) return;
            try { callback(event); }
            catch (...) { throw Error(ErrorCode::ConsumerFailure, "Stream consumer threw an exception"); }
        };
        try {
            token.throwIfCancelled();
            validateConversationRequest(request, impl->options.maxInputCharacters);
            const auto record = impl->state->models.resolve(request.model);
            if (!record.manifest.capabilities.contains("chat")) throw Error(ErrorCode::InvalidArgument, "Model does not declare chat capability");
            auto& model = impl->state->models.acquire(record.manifest.id, request.keepAliveMs, token);
            leasedModel = model.spec.id;
            const auto key = request.contextId.isEmpty() ? result.requestId : request.contextId;
            cacheId = "conversation:" + leasedModel + ':' + QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex());
            auto prepared = model.runtime->prepareConversation(request, token);
            if (prepared.tokens.isEmpty() || prepared.tokens.size() > model.spec.contextTokens - request.options.maxTokens)
                throw Error(ErrorCode::ContextOverflow, "Structured conversation exceeds context; compact its complete tool turns before retrying");
            result.usage.promptTokens = prepared.tokens.size();
            auto& context = impl->state->cache.acquire(cacheId, model, token);
            notify({StreamEventKind::Started, result.requestId, result.sessionId, {}, {}});
            StopFilter stop(request.options.stop + prepared.stop);
            QString raw;
            const auto generated = context.generateConversation(prepared, request.options, token, [&](const QString& text) {
                token.throwIfCancelled(); raw += stop.push(text);
                if (raw.size() > impl->options.maxInputCharacters) throw Error(ErrorCode::ResourceLimit, "Structured response exceeds output limit");
                return !stop.stopped();
            });
            token.throwIfCancelled(); raw += stop.finish();
            if (generated.finishReason != FinishReason::Stop && generated.finishReason != FinishReason::Length)
                throw Error(ErrorCode::RuntimeFailure, "Runtime returned an invalid terminal status");
            result.usage.generatedTokens = generated.generatedTokens; result.usage.cachedTokens = generated.cachedTokens;
            result.finishReason = stop.stopped() ? FinishReason::Stop : generated.finishReason;
            // A partial tool call must never reach an executor, even if its JSON prefix parses.
            if (result.finishReason == FinishReason::Length)
                throw Error(ErrorCode::ProtocolError, "Structured response reached the output limit before a complete turn");
            auto reply = model.runtime->parseConversation(prepared, raw);
            validateConversationReply(reply, request);
            result.text = std::move(reply.text); result.reasoning = std::move(reply.reasoning); result.toolCalls = std::move(reply.toolCalls);
            if (!result.text.isEmpty()) notify({StreamEventKind::Delta, result.requestId, result.sessionId, result.text, {}});
            if (stop.stopped()) impl->state->cache.erase(cacheId);
        } catch (const Error& error) {
            impl->state->cache.erase(cacheId); failure(result, error);
        } catch (const std::exception& error) {
            impl->state->cache.erase(cacheId); failure(result, Error(ErrorCode::RuntimeFailure, QString::fromUtf8(error.what())));
        } catch (...) {
            impl->state->cache.erase(cacheId); failure(result, Error(ErrorCode::RuntimeFailure, "Unknown structured runtime failure"));
        }
        if (request.contextId.isEmpty()) impl->state->cache.erase(cacheId);
        if (!leasedModel.isEmpty()) impl->state->models.release(leasedModel);
        finish(callback, result); promise->set_value(std::move(result));
    }, std::move(reject));
    return handle;
}
GenerationHandle Service::Impl::generate(ChatRequest request, std::optional<CompletionRequest> completion, StreamCallback callback)
{
    GenerationHandle handle;
    handle.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto promise = std::make_shared<std::promise<GenerationResult>>();
    handle.result = promise->get_future().share();
    const auto token = handle.cancellation;
    GenerationResult initial;
    initial.requestId = handle.requestId;
    initial.sessionId = request.sessionId;
    auto reject = [promise, initial, callback](Error error) mutable {
        failure(initial, error);
        finish(callback, initial);
        promise->set_value(initial);
    };
    scheduler.enqueue(token, [impl = this, request = std::move(request), completion = std::move(completion), callback, promise, token, initial]() mutable {
        auto result = initial;
        QString temporarySession;
        QString leasedModel;
        auto notify = [&](StreamEvent event) {
            if (!callback) return;
            try { callback(event); }
            catch (...) { throw Error(ErrorCode::ConsumerFailure, QStringLiteral("Stream consumer threw an exception")); }
        };
        try {
            token.throwIfCancelled();
            if (completion) {
                const auto info = impl->state->models.resolve(completion->model);
                const auto id = info.manifest.id;
                if (!info.manifest.capabilities.contains(QStringLiteral("chat")))
                    throw Error(ErrorCode::InvalidArgument, QStringLiteral("Model manifest does not declare the chat capability"));
                const auto& messages = completion->messages;
                if (messages.isEmpty() || messages.back().role != Role::User)
                    throw Error(ErrorCode::InvalidArgument, QStringLiteral("messages must end with a user message"));
                qsizetype characters = 0;
                Role expected = Role::User;
                for (qsizetype i = 0; i < messages.size(); ++i) {
                    const auto& message = messages[i];
                    characters += message.content.size();
                    if (characters > impl->options.maxInputCharacters || message.content.trimmed().isEmpty())
                        throw Error(ErrorCode::InvalidArgument, QStringLiteral("messages are empty or exceed the service input limit"));
                    if (i == 0 && message.role == Role::System) continue;
                    if (message.role != expected)
                        throw Error(ErrorCode::InvalidArgument, QStringLiteral("messages must alternate user/assistant after an optional initial system message"));
                    expected = expected == Role::User ? Role::Assistant : Role::User;
                }
                temporarySession = impl->state->sessions.create(id, {});
                auto& temporary = impl->state->sessions.get(temporarySession);
                temporary.messages = messages;
                temporary.messages.removeLast();
                request = {temporarySession, messages.back().content, completion->options, completion->keepAliveMs};
            }
            auto& session = impl->state->sessions.get(request.sessionId);
            auto& model = impl->state->models.acquire(session.modelId, request.keepAliveMs, token);
            leasedModel = session.modelId;
            auto prepared = PromptEngine::prepare(session, request, model, token, impl->options.maxInputCharacters);
            result.usage.promptTokens = prepared.tokens.size();
            result.usage.droppedMessages = prepared.droppedMessages;
            auto& context = impl->state->cache.acquire(session.id, model, token);
            notify({StreamEventKind::Started, result.requestId, result.sessionId, {}, {}});
            StopFilter stop(request.options.stop);
            auto emitText = [&](const QString& text) {
                if (!text.isEmpty()) {
                    result.text += text;
                    notify({StreamEventKind::Delta, result.requestId, result.sessionId, text, {}});
                }
            };
            const auto generated = context.generate(prepared.tokens, request.options, token, [&](const QString& text) {
                token.throwIfCancelled();
                emitText(stop.push(text));
                return !stop.stopped();
            });
            token.throwIfCancelled();
            emitText(stop.finish());
            token.throwIfCancelled();
            if (generated.finishReason != FinishReason::Stop && generated.finishReason != FinishReason::Length)
                throw Error(ErrorCode::RuntimeFailure, QStringLiteral("Runtime returned an invalid terminal status"));
            result.usage.generatedTokens = generated.generatedTokens;
            result.usage.cachedTokens = generated.cachedTokens;
            result.finishReason = stop.stopped() ? FinishReason::Stop : generated.finishReason;
            prepared.messages.push_back({Role::Assistant, result.text});
            session.messages = std::move(prepared.messages); // Atomic turn commit only after successful generation.
            if (stop.stopped()) impl->state->cache.erase(session.id);
        } catch (const Error& error) {
            impl->state->cache.erase(request.sessionId);
            failure(result, error);
        } catch (const std::exception& error) {
            impl->state->cache.erase(request.sessionId);
            failure(result, Error(ErrorCode::RuntimeFailure, QString::fromUtf8(error.what())));
        } catch (...) {
            impl->state->cache.erase(request.sessionId);
            failure(result, Error(ErrorCode::RuntimeFailure, QStringLiteral("Unknown runtime failure")));
        }
        if (!temporarySession.isEmpty()) {
            impl->state->cache.erase(temporarySession);
            impl->state->sessions.close(temporarySession);
        }
        if (!leasedModel.isEmpty()) impl->state->models.release(leasedModel);
        finish(callback, result);
        promise->set_value(std::move(result));
    }, std::move(reject));
    return handle;
}
} // namespace iiLocalLLM
