#include "Engine.h"
#include "../Parameters.h"
#include <QtCore/QThreadPool>
#include <QtCore/QRunnable>
#include <QtCore/QUuid>
#include <QtCore/QJsonDocument>
#include <QtCore/QSet>
#include <mutex>
#include <map>
#include <chrono>
#include <algorithm>

namespace iiLocalLLM::agent {
using namespace std::chrono_literals;
namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
void failure(RunResult& r, const Error& e) {
    r.status = e.code() == ErrorCode::Cancelled ? RunStatus::Cancelled : RunStatus::Failed;
    r.errorCode = e.code(); r.errorMessage = QString::fromUtf8(e.what());
}
}
class Engine::Impl {
public:
    Impl(std::shared_ptr<Model> model, std::shared_ptr<ToolRegistry> registry,
        std::shared_ptr<const PermissionPolicy> policy, EngineOptions options)
        : model(std::move(model)), registry(std::move(registry)), policy(std::move(policy)), options(std::move(options)),
          store(this->options.sessionsDirectory) {
        if (!this->model || !this->registry || !this->policy || this->options.maxConcurrentRuns < 1 || this->options.maxConcurrentRuns > 64
            || this->options.maxQueuedRuns < 0 || this->options.maxConcurrentTools < 1 || this->options.maxConcurrentTools > 64
            || this->options.maxToolCallsPerTurn < 1 || this->options.maxToolCallsPerTurn > 64 || this->options.maxInputCharacters < 1)
            throw Error(ErrorCode::InvalidArgument, "Invalid agent engine configuration");
        pool.setMaxThreadCount(this->options.maxConcurrentRuns);
    }
    std::shared_ptr<Model> model;
    std::shared_ptr<ToolRegistry> registry;
    std::shared_ptr<const PermissionPolicy> policy;
    EngineOptions options;
    SessionStore store;
    QThreadPool pool;
    std::mutex mutex;
    bool stopping = false;
    std::map<QString, CancellationToken> active;
    QSet<QString> busySessions;

    void execute(RunRequest request, QString runId, CancellationToken token,
                 EventCallback callback, std::shared_ptr<std::promise<RunResult>> promise) {
        RunResult result; result.runId = runId; result.sessionId = request.sessionId;
        std::unique_ptr<SessionLease> lease;
        std::mutex eventsMutex;
        EventCallback send = [&](const Event& event) {
            if (!callback) return;
            std::lock_guard lock(eventsMutex);
            try { callback(event); }
            catch (...) { throw Error(ErrorCode::ConsumerFailure, "Agent event consumer threw an exception"); }
        };
        auto append = [&](Message message) {
            lease->append(std::move(message));
            send({EventKind::Message, runId, request.sessionId, {}, {}, toJson(lease->session().messages.back())});
        };
        auto repair = [&] {
            if (!lease) return;
            for (const auto& call : pendingToolCalls(lease->session().messages))
                lease->append({{}, MessageRole::Tool, "Execution was interrupted; the outcome is unknown. Do not automatically repeat this action.",
                    {}, call.id, true, {{"interrupted", true}}});
        };
        auto hooks = [&](HookKind kind, const QString& text) {
            HookResult combined;
            for (const auto& hook : options.hooks) {
                token.throwIfCancelled();
                auto r = hook({kind, request.sessionId, runId, {}, {}, text}, token);
                combined.block |= r.block;
                if (!r.feedback.isEmpty()) {
                    if (!combined.feedback.isEmpty()) combined.feedback += '\n';
                    combined.feedback += r.feedback;
                    send({EventKind::Hook, runId, request.sessionId, {}, r.feedback, {{"blocked", r.block}}});
                }
            }
            return combined;
        };
        try {
            token.throwIfCancelled();
            lease = store.acquire(request.sessionId);
            repair();
            send({EventKind::Started, runId, request.sessionId, {}, {}, {}});
            append({{}, MessageRole::User, request.prompt});
            for (int turn = 1; turn <= request.maxTurns; ++turn) {
                result.turns = turn; token.throwIfCancelled();
                auto before = hooks(HookKind::BeforeModel, request.prompt);
                if (before.block) throw Error(ErrorCode::InvalidArgument, "Before-model hook blocked execution: " + before.feedback);
                if (!before.feedback.isEmpty()) append({{}, MessageRole::User, before.feedback});
                const auto& session = lease->session();
                const auto turnRegistry = registry->snapshot();
                const ToolRunner runner(turnRegistry, policy, {options.hooks, options.permission});
                ModelRequest modelRequest{session.model, session.systemPrompt, session.messages, turnRegistry->definitions(false), request.generation, session.id};
                qsizetype streamed = 0;
                auto reply = model->generate(modelRequest, token, [&](const QString& text) {
                    token.throwIfCancelled(); streamed += text.size();
                    if (streamed > options.maxInputCharacters) throw Error(ErrorCode::ResourceLimit, "Agent model output exceeds limit");
                    send({EventKind::ModelDelta, runId, request.sessionId, {}, text, {}});
                    return true;
                });
                token.throwIfCancelled();
                result.usage.promptTokens += reply.usage.promptTokens;
                result.usage.generatedTokens += reply.usage.generatedTokens;
                result.usage.cachedTokens += reply.usage.cachedTokens;
                result.usage.droppedMessages += reply.usage.droppedMessages;
                if (reply.toolCalls.size() > options.maxToolCallsPerTurn) throw Error(ErrorCode::ResourceLimit, "Too many agent tool calls");
                if (reply.text.size() > options.maxInputCharacters) throw Error(ErrorCode::ResourceLimit, "Agent reply exceeds limit");
                for (auto& call : reply.toolCalls) if (call.id.isEmpty()) call.id = uuid();
                auto after = hooks(HookKind::AfterModel, reply.text);
                if (after.block) {
                    append({{}, MessageRole::User, after.feedback.isEmpty() ? QStringLiteral("The response was blocked by a hook; correct it.") : after.feedback});
                    continue;
                }
                append({{}, MessageRole::Assistant, reply.text, reply.toolCalls});
                if (reply.toolCalls.isEmpty()) {
                    auto stop = hooks(HookKind::Stop, reply.text);
                    if (stop.block) {
                        append({{}, MessageRole::User, stop.feedback.isEmpty() ? QStringLiteral("The stop hook requires more work.") : stop.feedback});
                        continue;
                    }
                    result.text = reply.text; result.status = RunStatus::Completed; break;
                }
                const ToolContext base{session.id, runId, session.workingDirectory, lease->artifactsDirectory(), token, {}};
                auto runTool = [&](const ToolCall& call) {
                    auto context = base;
                    context.progress = [&, id = call.id](const QJsonObject& data) {
                        send({EventKind::ToolProgress, runId, request.sessionId, id, {}, data});
                    };
                    return runner.run(call, context, send);
                };
                for (qsizetype i = 0; i < reply.toolCalls.size();) {
                    token.throwIfCancelled();
                    qsizetype end = i + 1;
                    if (runner.concurrencySafe(reply.toolCalls[i]))
                        while (end < reply.toolCalls.size() && end - i < options.maxConcurrentTools
                               && runner.concurrencySafe(reply.toolCalls[end])) ++end;
                    if (end == i + 1) {
                        auto output = runTool(reply.toolCalls[i]);
                        append({{}, MessageRole::Tool, output.text, {}, reply.toolCalls[i].id, output.isError, output.data, output.content, output.metadata});
                    } else {
                        std::vector<std::future<ToolResult>> futures;
                        for (auto n = i; n < end; ++n)
                            futures.emplace_back(std::async(std::launch::async, [&, call = reply.toolCalls[n]] { return runTool(call); }));
                        try {
                            for (auto n = i; n < end; ++n) {
                                auto output = futures[size_t(n - i)].get();
                                append({{}, MessageRole::Tool, output.text, {}, reply.toolCalls[n].id, output.isError, output.data, output.content, output.metadata});
                            }
                        } catch (...) {
                            token.cancel();
                            for (auto& future : futures) if (future.valid()) future.wait();
                            throw;
                        }
                    }
                    i = end;
                }
            }
            if (result.status != RunStatus::Completed) result.status = RunStatus::TurnLimit;
        } catch (const Error& error) { failure(result, error); }
        catch (const std::exception& error) { failure(result, Error(ErrorCode::RuntimeFailure, QString::fromUtf8(error.what()))); }
        catch (...) { failure(result, Error(ErrorCode::RuntimeFailure, "Unknown agent failure")); }
        try { repair(); }
        catch (const std::exception& error) { failure(result, Error(ErrorCode::StorageFailure, "Transcript recovery failed: " + QString::fromUtf8(error.what()))); }
        lease.reset();
        {
            std::lock_guard lock(mutex); active.erase(runId); busySessions.remove(request.sessionId);
        }
        // Terminal observer failures cannot change the already finalized transcript/result.
        try { send({EventKind::Finished, runId, request.sessionId, {}, result.text, toJson(result)}); } catch (...) {}
        promise->set_value(std::move(result));
    }
};
Engine::Engine(std::shared_ptr<Model> model, std::shared_ptr<ToolRegistry> registry,
    std::shared_ptr<const PermissionPolicy> policy, EngineOptions options)
    : d(std::make_unique<Impl>(std::move(model), std::move(registry), std::move(policy), std::move(options))) {}
Engine::~Engine() {
    { std::lock_guard lock(d->mutex); d->stopping = true; for (const auto& [id, token] : d->active) token.cancel(); }
    d->pool.waitForDone();
}
Session Engine::createSession(QString model, QString workspace, QString systemPrompt) {
    return d->store.create(std::move(model), std::move(systemPrompt), std::move(workspace));
}
Session Engine::session(const QString& id) const { return d->store.load(id); }
RunHandle Engine::run(RunRequest request, EventCallback callback) {
    auto promise = std::make_shared<std::promise<RunResult>>();
    RunHandle handle{uuid(), {}, promise->get_future().share()};
    try {
        if (request.prompt.trimmed().isEmpty() || request.prompt.size() > d->options.maxInputCharacters
            || request.maxTurns < 1 || request.maxTurns > 10000) throw Error(ErrorCode::InvalidArgument, "Invalid agent run request");
        validateGenerationOptions(request.generation);
        std::lock_guard lock(d->mutex);
        if (d->stopping) throw Error(ErrorCode::ShuttingDown, "Agent engine is shutting down");
        if (d->active.size() >= size_t(d->options.maxConcurrentRuns + d->options.maxQueuedRuns))
            throw Error(ErrorCode::QueueFull, "Agent run queue is full");
        if (d->busySessions.contains(request.sessionId)) throw Error(ErrorCode::ModelInUse, "Agent session already has an accepted run");
        d->busySessions.insert(request.sessionId); d->active.emplace(handle.runId, handle.cancellation);
        auto task = QRunnable::create([impl = d.get(), request = std::move(request), callback = std::move(callback),
                id = handle.runId, token = handle.cancellation, promise]() mutable {
            impl->execute(std::move(request), id, token, std::move(callback), promise);
        });
        d->pool.start(task);
    } catch (const Error& error) {
        RunResult result; result.runId = handle.runId; result.sessionId = request.sessionId; failure(result, error);
        try { if (callback) callback({EventKind::Finished, result.runId, result.sessionId, {}, {}, toJson(result)}); } catch (...) {}
        promise->set_value(std::move(result));
    }
    return handle;
}
ServiceModel::ServiceModel(Service& service) : service_(service) {}
ModelReply ServiceModel::generate(const ModelRequest& request, const CancellationToken& token, const TextCallback& onDelta) {
    token.throwIfCancelled();
    ConversationRequest conversation;
    conversation.model = request.model; conversation.contextId = request.contextId; conversation.options = request.generation;
    for (const auto& tool : request.tools)
        conversation.tools.append(QJsonObject{{"type", "function"}, {"function", QJsonObject{
            {"name", tool.name}, {"description", tool.description}, {"parameters", tool.inputSchema}}}});
    QString instruction = QStringLiteral("You are a local agent. Use the available tools to carry out the user's request. "
        "Read files through tools before answering questions about their contents. Tool responses are the actual observations; never invent or replace them. "
        "When the user asks for exact file contents, your final answer must contain only the text observed in the tool response, copied character for character. "
        "Do not add an introduction, explanation, example value, or Markdown code fence. Otherwise, give a concise answer after completing the work.");
    if (!request.systemPrompt.isEmpty()) instruction = request.systemPrompt + "\n\n" + instruction;
    conversation.messages.append(QJsonObject{{"role", "system"}, {"content", instruction}});
    for (const auto& message : request.messages) {
        for (const auto& value : message.content) {
            const auto block = value.toObject(); const auto type = block["type"].toString();
            if (type == "image" || type == "audio" || (type == "resource" && block["resource"].toObject().contains("blob")))
                throw Error(ErrorCode::RuntimeUnavailable, "This native model adapter cannot consume MCP media content; a multimodal Model adapter is required");
        }
        QJsonObject wire{{"role", enumName(message.role)}, {"content", message.text}};
        if (message.role == MessageRole::Tool) {
            wire["tool_call_id"] = message.toolCallId;
            if (message.isError) wire["content"] = "Tool error: " + message.text;
        }
        if (!message.toolCalls.isEmpty()) {
            QJsonArray calls;
            for (const auto& call : message.toolCalls)
                calls.append(QJsonObject{{"id", call.id}, {"type", "function"}, {"function", QJsonObject{
                    {"name", call.name}, {"arguments", QString::fromUtf8(QJsonDocument(call.arguments).toJson(QJsonDocument::Compact))}}}});
            wire["tool_calls"] = calls;
        }
        conversation.messages.append(wire);
    }
    const auto generation = service_.converse(std::move(conversation), [&](const StreamEvent& event) {
        if (event.kind == StreamEventKind::Delta && onDelta && !onDelta(event.text)) throw Error(ErrorCode::Cancelled, "Model stream was cancelled");
    });
    while (generation.result.wait_for(20ms) != std::future_status::ready) if (token.isCancelled()) generation.cancel();
    const auto result = generation.result.get(); token.throwIfCancelled();
    if (result.errorCode != ErrorCode::None) throw Error(result.errorCode, result.errorMessage);
    ModelReply reply; reply.text = result.text; reply.usage = result.usage;
    for (const auto& value : result.toolCalls) {
        const auto call = value.toObject(); const auto f = call["function"].toObject();
        reply.toolCalls.append({call["id"].toString(), f["name"].toString(),
            QJsonDocument::fromJson(f["arguments"].toString().toUtf8()).object()});
    }
    if (reply.text.isEmpty() && reply.toolCalls.isEmpty()) throw Error(ErrorCode::ProtocolError, "Model returned an empty agent turn");
    return reply;
}
}
