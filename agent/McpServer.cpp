#include "McpServer.h"
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <chrono>
#include <map>
#include <mutex>
#include <shared_mutex>

namespace iiLocalLLM::agent {
namespace {
using namespace std::chrono_literals;
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QJsonObject wireResult(const ToolResult& result) {
    auto content = result.content; QStringList texts;
    for (const auto& v : content) if (v.toObject()["type"] == "text") texts.append(v.toObject()["text"].toString());
    if (!result.text.isEmpty() && texts.join('\n') != result.text)
        content.prepend(QJsonObject{{"type", "text"}, {"text", result.text}});
    QJsonObject value{{"content", content}, {"structuredContent", result.data}, {"isError", result.isError}};
    if (!result.metadata.isEmpty()) value["_meta"] = result.metadata;
    return value;
}
QJsonObject wireDefinition(const ToolDefinition& d, const QString& appId) {
    QJsonObject value{{"name", d.name}, {"description", d.description}, {"inputSchema", d.inputSchema},
        {"annotations", QJsonObject{{"readOnlyHint", d.readOnly}}}};
    if (!d.outputSchema.isEmpty()) value["outputSchema"] = d.outputSchema;
    if (!appId.isEmpty()) value["_meta"] = QJsonObject{{"iisacc/appId", appId}};
    return value;
}
template<class Lock> void acquire(Lock& lock, const CancellationToken& token) {
    while (!lock.try_lock_for(10ms)) token.throwIfCancelled();
    token.throwIfCancelled();
}
class Bridge : public std::enable_shared_from_this<Bridge> {
public:
    struct Conversation { std::timed_mutex mutex; QString id; };
    std::shared_ptr<ToolRegistry> registry;
    std::shared_ptr<const PermissionPolicy> policy;
    McpServerOptions options;
    std::shared_timed_mutex execution;
    std::mutex mutex;
    std::map<QString, std::shared_ptr<Conversation>> conversations;
    Bridge(std::shared_ptr<ToolRegistry> r, std::shared_ptr<const PermissionPolicy> p, McpServerOptions o)
        : registry(std::move(r)), policy(std::move(p)), options(std::move(o)) {
        if (!registry || !policy || options.maxAgentTurns < 1 || options.maxAgentTurns > 1000 || options.maxAgentSessions < 1)
            throw Error(ErrorCode::InvalidArgument, "Invalid MCP tool bridge configuration");
        const auto workspace = QFileInfo(options.workingDirectory).canonicalFilePath();
        if (workspace.isEmpty() || !QFileInfo(workspace).isDir()) throw Error(ErrorCode::InvalidArgument, "MCP bridge workspace must exist");
        options.workingDirectory = workspace;
        if (!options.artifactsDirectory.isEmpty()) options.artifactsDirectory = QFileInfo(options.artifactsDirectory).absoluteFilePath();
        if (options.engine && options.model.isEmpty()) throw Error(ErrorCode::InvalidArgument, "MCP agent model is required");
    }
    std::shared_ptr<Conversation> conversation(const QString& session) {
        std::lock_guard lock(mutex);
        const auto found = conversations.find(session);
        if (found != conversations.end()) return found->second;
        if (conversations.size() >= size_t(options.maxAgentSessions)) throw Error(ErrorCode::QueueFull, "MCP agent session limit reached");
        auto value = std::make_shared<Conversation>(); conversations.emplace(session, value); return value;
    }
    std::shared_ptr<ToolRegistry> snapshot() {
        auto frozen = registry->snapshot();
        if (!options.engine) return frozen;
        auto self = shared_from_this();
        Tool run;
        run.definition.name = "iiLocalLLM.agent.run";
        run.definition.description = "Run the configured local agent in this connection's private conversation and workspace. Use new_session to start a new conversation.";
        run.definition.inputSchema = {{"type", "object"}, {"additionalProperties", false}, {"required", QJsonArray{"prompt"}},
            {"properties", QJsonObject{{"prompt", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 1048576}}},
                {"new_session", QJsonObject{{"type", "boolean"}}},
                {"context_paths", QJsonObject{{"type", "array"}, {"maxItems", 128}, {"items", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 4096}}}}},
                {"max_turns", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", options.maxAgentTurns}}}}}};
        run.definition.outputSchema = {{"type", "object"}, {"required", QJsonArray{"run_id", "session_id", "text", "status", "turns", "usage"}},
            {"properties", QJsonObject{{"run_id", QJsonObject{{"type", "string"}}}, {"session_id", QJsonObject{{"type", "string"}}},
                {"text", QJsonObject{{"type", "string"}}}, {"status", QJsonObject{{"type", "string"}}},
                {"turns", QJsonObject{{"type", "integer"}}}, {"usage", QJsonObject{{"type", "object"}}}}}};
        run.execute = [self](const QJsonObject& args, const ToolContext& context) {
            auto conversation = self->conversation(context.sessionId);
            std::unique_lock lock(conversation->mutex, std::defer_lock); acquire(lock, context.cancellation);
            if (conversation->id.isEmpty() || args["new_session"].toBool())
                conversation->id = self->options.engine->createSession(self->options.model, self->options.workingDirectory, self->options.systemPrompt).id;
            RunRequest request{conversation->id, args["prompt"].toString(), self->options.generation, args["max_turns"].toInt(self->options.maxAgentTurns)};
            for (const auto& path : args["context_paths"].toArray()) request.contextPaths.append(path.toString());
            int progress = 0;
            auto handle = self->options.engine->run(request, [&](const Event& event) {
                if (context.progress) context.progress({{"progress", ++progress}, {"message", enumName(event.kind)},
                    {"_meta", QJsonObject{{"iisacc/agentEvent", toJson(event)}}}});
            });
            while (handle.result.wait_for(10ms) != std::future_status::ready)
                if (context.cancellation.isCancelled()) handle.cancel();
            const auto result = handle.result.get(); context.cancellation.throwIfCancelled();
            return ToolResult{result.text.isEmpty() ? result.errorMessage : result.text, toJson(result), result.status != RunStatus::Completed};
        };
        frozen->add(std::move(run));
        Tool session;
        session.definition = {"iiLocalLLM.agent.session", "Inspect only this MCP connection's local agent conversation.",
            {{"type", "object"}, {"additionalProperties", false}, {"properties", QJsonObject{{"include_messages", QJsonObject{{"type", "boolean"}}}}}}, {}, true, true};
        session.execute = [self](const QJsonObject& args, const ToolContext& context) {
            auto conversation = self->conversation(context.sessionId);
            std::unique_lock lock(conversation->mutex, std::defer_lock); acquire(lock, context.cancellation);
            QJsonObject value{{"session_id", conversation->id}, {"model", self->options.model}, {"message_count", 0}};
            if (!conversation->id.isEmpty()) {
                const auto session = self->options.engine->session(conversation->id);
                value["message_count"] = session.messages.size();
                if (args["include_messages"].toBool()) { QJsonArray messages; for (const auto& m : session.messages) messages.append(toJson(m)); value["messages"] = messages; }
            }
            return ToolResult{QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)), value};
        };
        frozen->add(std::move(session)); return frozen;
    }
    QJsonObject call(const QJsonObject& params, const mcp::ServerRequestContext& request) {
        auto frozen = snapshot(); const auto name = params["name"].toString();
        try { frozen->get(name); } catch (const Error& e) {
            if (e.code() == ErrorCode::NotFound) throw mcp::RpcError(-32602, "Unknown MCP tool: " + name);
            throw;
        }
        ToolRunner runner(frozen, policy, options.tools);
        ToolCall call{uuid(), name, params["arguments"].toObject()};
        ToolContext context{request.sessionId, uuid(), options.workingDirectory, {}, request.cancellation, request.progress};
        if (!options.artifactsDirectory.isEmpty()) context.artifactsDirectory = QDir(options.artifactsDirectory).filePath(context.sessionId + '/' + context.runId);
        std::shared_lock shared(execution, std::defer_lock); std::unique_lock exclusive(execution, std::defer_lock);
        if (runner.concurrencySafe(call)) acquire(shared, request.cancellation); else acquire(exclusive, request.cancellation);
        return wireResult(runner.run(call, context));
    }
};
}
mcp::ServerOptions mcpServerOptions(std::shared_ptr<ToolRegistry> registry,
    std::shared_ptr<const PermissionPolicy> policy, McpServerOptions options) {
    auto state = std::make_shared<Bridge>(std::move(registry), std::move(policy), std::move(options));
    state->snapshot(); // Validate reserved names and agent schemas before publishing.
    mcp::ServerOptions server;
    server.lists["tools/list"] = [state](const auto&) {
        QJsonArray result;
        for (const auto& tool : state->snapshot()->definitions()) result.append(wireDefinition(tool, state->options.appId));
        return result;
    };
    server.handlers["tools/call"] = [state](const auto& params, const auto& request) { return state->call(params, request); };
    server.onClosed = [state](const QString& session) { std::lock_guard lock(state->mutex); state->conversations.erase(session); };
    return server;
}
}
