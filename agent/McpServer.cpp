#include "McpServer.h"
#include "McpResult.h"
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>

namespace iiLocalLLM::agent {
namespace {
using namespace std::chrono_literals;
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QJsonObject wireResult(const ToolResult& result) {
    auto content = result.content; QStringList texts;
    for (const auto& v : content) if (v.toObject()["type"] == "text") texts.append(v.toObject()["text"].toString());
    if (!result.text.isEmpty() && texts.join('\n') != result.text)
        content.prepend(QJsonObject{{"type", "text"}, {"text", result.text}});
    content = detail::withStructuredText(std::move(content), result.data);
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
EventCallback permissionEvents(const ToolContext& context) {
    return [progress=context.progress](const Event& event) {
        if(progress&&(event.kind==EventKind::Hook||event.kind==EventKind::PermissionRequested||event.kind==EventKind::PermissionResolved))
            progress({{"progress",1},{"message",enumName(event.kind)},{"_meta",QJsonObject{{"iisacc/agentEvent",toJson(event)}}}});
    };
}
class Bridge : public std::enable_shared_from_this<Bridge> {
public:
    struct Conversation {
        std::timed_mutex mutex,identity;QString id;bool resetting=false;
        std::map<QString,CancellationToken> active;std::condition_variable_any changed;
        std::shared_ptr<PermissionRequests> permissionRequests;
    };
    struct Invocation {
        std::shared_ptr<Conversation> conversation;QString id;CancellationToken token;
        Invocation(std::shared_ptr<Conversation> owner,QString id,const CancellationToken& parent)
            :conversation(std::move(owner)),id(std::move(id)),token(CancellationToken::linkedTo(parent)) {
            std::unique_lock lock(conversation->identity,std::defer_lock);acquire(lock,token);
            if(conversation->resetting)throw Error(ErrorCode::ModelInUse,"MCP conversation is being replaced");
            conversation->active.emplace(this->id,token);
        }
        ~Invocation(){std::lock_guard lock(conversation->identity);conversation->active.erase(id);conversation->changed.notify_all();}
    };
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
        if (options.engine && options.taskStore) throw Error(ErrorCode::InvalidArgument, "Use the Engine task store or an independent MCP task store, not both");
        if(options.tools.permissionRequests||(options.engine&&options.engine->permissionRequestsEnabled()))
            throw Error(ErrorCode::InvalidArgument,"MCP bridge assigns private permission channels; configure McpServerOptions.permissionRequests");
        if(options.permissionRequests){PermissionRequests validate(*options.permissionRequests);}
    }
    std::shared_ptr<Conversation> conversation(const QString& session) {
        std::lock_guard lock(mutex);
        const auto found = conversations.find(session);
        if (found != conversations.end()) return found->second;
        if (conversations.size() >= size_t(options.maxAgentSessions)) throw Error(ErrorCode::QueueFull, "MCP agent session limit reached");
        auto value = std::make_shared<Conversation>();
        if(options.permissionRequests)value->permissionRequests=std::make_shared<PermissionRequests>(*options.permissionRequests);
        conversations.emplace(session, value); return value;
    }
    QString sessionId(const std::shared_ptr<Conversation>& conversation, const CancellationToken& token, bool create = true, bool reset = false,QJsonObject* cleared=nullptr,const QString& current={}) {
        std::unique_lock guard(conversation->identity, std::defer_lock); acquire(guard, token);
        if(conversation->resetting)throw Error(ErrorCode::ModelInUse,"MCP conversation is being replaced");
        if (reset && !conversation->id.isEmpty()) {
            const auto previous=conversation->id;conversation->resetting=true;
            for(const auto& [id,active]:conversation->active)if(id!=current)active.cancel();
            conversation->changed.wait(guard,[&]{return conversation->active.empty()||(conversation->active.size()==1&&conversation->active.contains(current));});
            guard.unlock();
            QJsonObject report;
            try {report=options.engine->clearSession(previous,token);}
            catch(...){guard.lock();conversation->resetting=false;throw;}
            guard.lock();conversation->resetting=false;
            if(!report["session_id"].toString().isEmpty())conversation->id=report["session_id"].toString();
            if(cleared)*cleared=report;
            return conversation->id;
        }
        if (create && (conversation->id.isEmpty() || reset))
            conversation->id = options.engine->createSession(options.model, options.workingDirectory, options.systemPrompt).id;
        return conversation->id;
    }
    void closeConnection(const QString& connection) {
        QString owner = connection; std::shared_ptr<Conversation> current;
        { std::lock_guard guard(mutex); const auto found = conversations.find(connection);
            if (found != conversations.end()) { current = found->second; conversations.erase(found); } }
        if(current&&current->permissionRequests)current->permissionRequests->close();
        if (options.engine) {
            if (!current) return;
            owner = sessionId(current, {}, false); if (owner.isEmpty()) return;
            options.engine->endSession(owner,"other");return;
        }
        stopShells(owner);
    }
    void stopShells(const QString& owner) {
        if (options.engine) { try { options.engine->stopSubagents(owner); } catch (const Error&) {} }
        // This is host lifecycle cleanup, independent of the model's stop rule.
        // Native lists put every active job first (at most 64).
        try {
            const auto list = registry->get("ShellTaskList"), stop = registry->get("TaskStop");
            if (list.definition.metadata["source"] != "builtin.shell.control" || stop.definition.metadata["source"] != "builtin.shell.control") return;
            const ToolContext context{owner, {}, options.workingDirectory};
            for (const auto& value : list.execute({{"limit", 100}}, context).data["tasks"].toArray()) {
                const auto task = value.toObject();
                if (task["status"] == "pending" || task["status"] == "running") {
                    try { stop.execute({{"task_id", task["task_id"]}}, context); } catch (const Error&) {}
                }
            }
        } catch (const Error&) {}
    }
    std::shared_ptr<ToolRegistry> snapshot() {
        auto frozen = registry->snapshot();
        auto self = shared_from_this();
        if (options.taskStore)
            for (auto tool : taskTools(options.taskStore, {}, false)) frozen->add(std::move(tool));
        Tool permissions;permissions.definition.name="iiLocalLLM.agent.permissions.get";
        permissions.definition.description="Inspect current host permission settings and source metadata. Does not change policy or expose unrelated settings values.";
        permissions.definition.readOnly=true;permissions.definition.concurrencySafe=true;
        permissions.definition.inputSchema={{"type","object"},{"additionalProperties",false},{"properties",QJsonObject{}}};
        permissions.execute=[self](const QJsonObject&,const ToolContext& context) {
            return ToolResult{"Host permission settings",self->policy->describe(context)};
        };
        frozen->add(std::move(permissions));
        if (!options.engine) return frozen;
        if (options.engine->subagentsEnabled()) for (auto definition : options.engine->subagentToolDefinitions()) {
            const auto nativeName = definition.name;
            const QMap<QString, QString> names{{"Agent", "run"}, {"AgentOutput", "output"}, {"AgentStop", "stop"}, {"AgentList", "list"}, {"AgentProfiles", "profiles"}};
            definition.name = "iiLocalLLM.agent.agents." + names[nativeName];
            definition.metadata = {{"source", nativeName == "Agent" ? "builtin.subagent.run" : "builtin.subagent.control"}};
            Tool tool; tool.definition = definition;
            tool.execute = [self, nativeName](const QJsonObject& args, const ToolContext& context) {
                const auto id = self->sessionId(self->conversation(context.sessionId), context.cancellation);
                return self->options.engine->runSubagentTool(id, nativeName, args, context.cancellation,permissionEvents(context),context.permissionRequests);
            };
            frozen->add(std::move(tool));
        }
        if (options.engine->taskToolsEnabled()) for (auto definition : taskToolDefinitions(false)) {
            Tool tool; tool.definition = definition;
            tool.execute = [self, name = definition.name](const QJsonObject& args, const ToolContext& context) {
                auto conversation = self->conversation(context.sessionId);
                const auto id = self->sessionId(conversation, context.cancellation);
                return self->options.engine->runTaskTool(id, name, args, context.cancellation,permissionEvents(context),context.permissionRequests);
            };
            frozen->add(std::move(tool));
        }
        Tool run;
        run.definition.name = "iiLocalLLM.agent.run";
        run.definition.description = "Run the configured local agent in this connection's private conversation and workspace. Use new_session to start a new conversation.";
        run.definition.inputSchema = {{"type", "object"}, {"additionalProperties", false},
            {"anyOf", QJsonArray{QJsonObject{{"required", QJsonArray{"prompt"}}}, QJsonObject{{"required", QJsonArray{"skill"}}}}},
            {"properties", QJsonObject{{"prompt", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 1048576}}},
                {"skill", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 129}}},
                {"skill_arguments", QJsonObject{{"type", "string"}, {"maxLength", 65536}}},
                {"new_session", QJsonObject{{"type", "boolean"}}},
                {"context_paths", QJsonObject{{"type", "array"}, {"maxItems", 128}, {"items", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 4096}}}}},
                {"max_turns", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", options.maxAgentTurns}}}}}};
        run.definition.outputSchema = {{"type", "object"}, {"required", QJsonArray{"run_id", "session_id", "text", "status", "turns", "usage"}},
            {"properties", QJsonObject{{"run_id", QJsonObject{{"type", "string"}}}, {"session_id", QJsonObject{{"type", "string"}}},
                {"text", QJsonObject{{"type", "string"}}}, {"status", QJsonObject{{"type", "string"}}},
                {"turns", QJsonObject{{"type", "integer"}}}, {"usage", QJsonObject{{"type", "object"}}}}}};
        auto executeAgent = [self](const QJsonObject& args, const ToolContext& context, bool compactOnly, bool queuedOnly = false) {
            auto conversation = self->conversation(context.sessionId);
            QJsonObject cleared;
            // Clearing owns cancellation, so it must precede the per-run lock
            // held by an active foreground model call on this connection.
            if(args["new_session"].toBool())self->sessionId(conversation,context.cancellation,true,true,&cleared,context.runId);
            std::unique_lock lock(conversation->mutex, std::defer_lock); acquire(lock, context.cancellation);
            const auto id = self->sessionId(conversation, context.cancellation, !compactOnly);
            if(!cleared.isEmpty()&&!cleared["complete"].toBool()) {
                RunResult failed;failed.sessionId=id;failed.status=RunStatus::Failed;failed.errorCode=ErrorCode::StorageFailure;
                failed.errorMessage="Conversation replacement requires attention; inspect clear.diagnostics before continuing.";
                auto data=toJson(failed);data["clear"]=cleared;return ToolResult{failed.errorMessage,data,true};
            }
            if (compactOnly && id.isEmpty()) throw Error(ErrorCode::NotFound, "This MCP connection has no conversation to compact");
            RunRequest request{id, args["prompt"].toString(), self->options.generation, args["max_turns"].toInt(self->options.maxAgentTurns)};
            request.permissionRequests=context.permissionRequests;
            request.skill = args["skill"].toString(); request.skillArguments = args["skill_arguments"].toString();
            for (const auto& path : args["context_paths"].toArray()) request.contextPaths.append(path.toString());
            int progress = 0;
            auto observe = [&](const Event& event) {
                if (context.progress) context.progress({{"progress", ++progress}, {"message", enumName(event.kind)},
                    {"_meta", QJsonObject{{"iisacc/agentEvent", toJson(event)}}}});
            };
            auto handle = compactOnly ? self->options.engine->compact({id, self->options.generation, args["instructions"].toString()}, observe)
                : queuedOnly ? self->options.engine->runQueued(request, observe) : self->options.engine->run(request, observe);
            while (handle.result.wait_for(10ms) != std::future_status::ready)
                if (context.cancellation.isCancelled()) handle.cancel();
            const auto result = handle.result.get(); context.cancellation.throwIfCancelled();
            auto data=toJson(result);if(!cleared.isEmpty())data["clear"]=cleared;
            return ToolResult{result.text.isEmpty() ? result.errorMessage : result.text, data, result.status != RunStatus::Completed};
        };
        run.execute = [executeAgent](const auto& args, const auto& context) { return executeAgent(args, context, false); };
        Tool compact;
        compact.definition.name = "iiLocalLLM.agent.compact";
        compact.definition.description = "Summarize this connection's existing conversation, preserving original records and recent turns.";
        compact.definition.inputSchema = {{"type", "object"}, {"additionalProperties", false}, {"properties", QJsonObject{
            {"instructions", QJsonObject{{"type", "string"}, {"maxLength", 1048576}}}}}};
        compact.definition.outputSchema = run.definition.outputSchema;
        compact.execute = [executeAgent](const auto& args, const auto& context) { return executeAgent(args, context, true); };
        Tool queuedRun;
        queuedRun.definition = run.definition; queuedRun.definition.name = "iiLocalLLM.agent.inputs.run";
        queuedRun.definition.description = "Start this connection's idle agent from pending input. Does not create a placeholder user prompt.";
        auto queuedProperties = run.definition.inputSchema["properties"].toObject();
        queuedProperties.remove("prompt"); queuedProperties.remove("new_session");
        queuedProperties.remove("skill"); queuedProperties.remove("skill_arguments");
        queuedRun.definition.inputSchema = {{"type", "object"}, {"additionalProperties", false}, {"properties", queuedProperties}};
        queuedRun.execute = [executeAgent](const auto& args, const auto& context) { return executeAgent(args, context, false, true); };
        frozen->add(std::move(queuedRun));
        Tool clear;clear.definition.name="iiLocalLLM.agent.clear";
        clear.definition.description="Cancel this connection's foreground work, preserve background jobs, and start an empty conversation with SessionStart(clear). Does not invoke the model.";
        clear.definition.concurrencySafe=true;clear.definition.metadata={{"source","builtin.session.control"}};
        clear.definition.inputSchema={{"type","object"},{"additionalProperties",false},{"properties",QJsonObject{}}};
        clear.execute=[self](const QJsonObject&,const ToolContext& context) {
            QJsonObject report;self->sessionId(self->conversation(context.sessionId),context.cancellation,false,true,&report);
            if(report.isEmpty())throw Error(ErrorCode::NotFound,"This MCP connection has no conversation to clear");
            return ToolResult{report["complete"].toBool()?"Conversation cleared.":"Conversation replacement requires attention; inspect diagnostics.",report,!report["complete"].toBool()};
        };
        frozen->add(std::move(clear));
        Tool skills; skills.definition.name = "iiLocalLLM.agent.skills.list";
        skills.definition.description = "List local skill metadata and unsupported features for this connection's agent. Does not load a model or execute a skill.";
        skills.definition.readOnly = true; skills.definition.concurrencySafe = true;
        skills.definition.inputSchema = {{"type", "object"}, {"additionalProperties", false}, {"properties", QJsonObject{}}};
        skills.execute = [self](const QJsonObject&, const ToolContext& context) {
            const auto id = self->sessionId(self->conversation(context.sessionId), context.cancellation);
            return ToolResult{"Local skill catalog", self->options.engine->skills(id, context.cancellation).toJson()};
        };
        frozen->add(std::move(skills));
        const auto text = QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 65536}};
        const auto inputId = QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
        for (const QString action : {QStringLiteral("enqueue"), QStringLiteral("list"), QStringLiteral("remove")}) {
            Tool control; control.definition.name = "iiLocalLLM.agent.inputs." + action;
            control.definition.description = action == "enqueue" ? "Queue input in this connection's agent conversation, including during execution. now cooperatively interrupts; next waits for tools; later waits for the answer."
                : action == "list" ? "List this connection's pending input in priority order." : "Remove one input which has not yet been delivered.";
            control.definition.readOnly = action == "list"; control.definition.concurrencySafe = true;
            control.definition.metadata = {{"source", "builtin.input.control"}};
            QJsonObject properties; QJsonArray required;
            if (action == "enqueue") {
                properties = {{"text", text}, {"priority", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"now", "next", "later"}}}},
                    {"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"prompt", "notification"}}}},
                    {"context_paths", run.definition.inputSchema["properties"].toObject()["context_paths"]}};
                required.append("text");
            } else if (action == "remove") { properties = {{"input_id", inputId}}; required.append("input_id"); }
            else properties = {{"offset", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 1000000}}},
                {"limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}};
            control.definition.inputSchema = {{"type", "object"}, {"additionalProperties", false}, {"properties", properties}, {"required", required}};
            control.execute = [self, action](const QJsonObject& args, const ToolContext& context) {
                const auto id = self->sessionId(self->conversation(context.sessionId), context.cancellation);
                QJsonObject result;
                if (action == "enqueue") result = self->options.engine->enqueueInput(id, args, context.cancellation);
                else if (action == "remove") result = self->options.engine->removeInput(id, args["input_id"].toString(), context.cancellation);
                else result = self->options.engine->queuedInputs(id, args["offset"].toInt(), args["limit"].toInt(32), context.cancellation);
                return ToolResult{QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)), result};
            };
            frozen->add(std::move(control));
        }
        frozen->add(std::move(run)); frozen->add(std::move(compact));
        Tool session;
        session.definition = {"iiLocalLLM.agent.session", "Inspect only this MCP connection's local agent conversation.",
            {{"type", "object"}, {"additionalProperties", false}, {"properties", QJsonObject{{"include_messages", QJsonObject{{"type", "boolean"}}}}}}, {}, true, true};
        session.execute = [self](const QJsonObject& args, const ToolContext& context) {
            auto conversation = self->conversation(context.sessionId);
            std::unique_lock lock(conversation->mutex, std::defer_lock); acquire(lock, context.cancellation);
            const auto id = self->sessionId(conversation, context.cancellation, false);
            QJsonObject value{{"session_id", id}, {"model", self->options.model}, {"message_count", 0}};
            if (!id.isEmpty()) {
                const auto session = self->options.engine->session(id);
                value["message_count"] = session.messages.size(); value["compaction_count"] = session.compactions.size();
                if (!session.compactions.isEmpty()) value["compaction"] = toJson(session.compactions.last());
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
        auto runnerOptions=options.tools;
        if(options.engine){runnerOptions.hookModel=options.engine->hookModel();runnerOptions.hookModelName=options.model;runnerOptions.hookAgent=options.engine->hookAgent();}
        if(options.permissionRequests)runnerOptions.permissionRequests=conversation(request.sessionId)->permissionRequests;
        ToolRunner runner(frozen, policy, runnerOptions);
        ToolCall call{uuid(), name, params["arguments"].toObject()};
        ToolContext context{request.sessionId, uuid(), options.workingDirectory, {}, request.cancellation, request.progress};
        context.permissionRequests=runnerOptions.permissionRequests;
        // Hooks, nested runs and tool progress all share one MCP request token.
        // Serialize their notifications with a monotonic bridge step counter.
        if(request.progress) {
            struct Progress {std::mutex mutex;quint64 step=0;};auto state=std::make_shared<Progress>();
            context.progress=[state,send=request.progress](QJsonObject data) {
                std::lock_guard lock(state->mutex);auto metadata=data["_meta"].toObject();
                metadata["iisacc/sourceProgress"]=QJsonObject{{"progress",data.value("progress")},{"total",data.value("total")}};
                data["_meta"]=metadata;data["progress"]=double(++state->step);data.remove("total");send(data);
            };
        }
        std::unique_ptr<Invocation> invocation;
        if(options.engine&&name!="iiLocalLLM.agent.clear") {
            invocation=std::make_unique<Invocation>(conversation(request.sessionId),context.runId,request.cancellation);
            context.cancellation=invocation->token;
        }
        if (!options.artifactsDirectory.isEmpty()) context.artifactsDirectory = QDir(options.artifactsDirectory).filePath(context.sessionId + '/' + context.runId);
        const auto source = frozen->get(name).definition.metadata["source"].toString();
        auto bindContext = [&](bool history=false) {
            const bool native=source=="builtin.workspace"||source=="builtin.shell"||source=="builtin.shell.control";
            if(options.engine&&(native||!options.tools.hooks.isEmpty())) {
                const auto owner=sessionId(conversation(request.sessionId),context.cancellation);
                // MCP task/control wrappers still need the connection ID to
                // resolve their owner. Hooks get that actual owner's snapshot.
                if(native)context.sessionId=owner;
                context.transcriptPath=options.engine->transcriptPath(owner);
                if(!options.tools.hooks.isEmpty()) {
                    if(history)try {context.sessionSnapshot=std::make_shared<Session>(options.engine->session(owner));}
                        catch(const Error& error){if(error.code()!=ErrorCode::ModelInUse)throw;}
                    if(!context.sessionSnapshot)context.sessionSnapshot=std::make_shared<Session>(options.engine->sessionMetadata(owner));
                }
            }
        };
        int hookProgress=0;
        auto observe=[&](const Event& event) {
            if((event.kind==EventKind::Hook||event.kind==EventKind::PermissionRequested||event.kind==EventKind::PermissionResolved)&&context.progress)context.progress({{"progress",++hookProgress},{"message",enumName(event.kind)},
                {"_meta",QJsonObject{{"iisacc/agentEvent",toJson(event)}}}});
        };
        const bool shellControl = source == "builtin.shell.control" && QStringList{"TaskOutput", "TaskStop", "ShellTaskList"}.contains(name);
        const bool inputControl = options.engine && source == "builtin.input.control"
            && QStringList{"iiLocalLLM.agent.inputs.enqueue", "iiLocalLLM.agent.inputs.list", "iiLocalLLM.agent.inputs.remove"}.contains(name);
        const bool subagentControl = options.engine && source == "builtin.subagent.control";
        const bool sessionControl=options.engine&&source=="builtin.session.control"&&name=="iiLocalLLM.agent.clear";
        if (shellControl || inputControl || subagentControl || sessionControl) { bindContext(); return wireResult(runner.run(call, context,observe)); }
        std::shared_lock shared(execution, std::defer_lock); std::unique_lock exclusive(execution, std::defer_lock);
        if (runner.concurrencySafe(call)) acquire(shared, context.cancellation); else acquire(exclusive, context.cancellation);
        bindContext(true);
        return wireResult(runner.run(call, context,observe));
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
        for (const auto& tool : state->snapshot()->definitions()) {
            auto definition=wireDefinition(tool,state->options.appId);auto metadata=definition["_meta"].toObject();
            metadata["iisacc/hooksEnabled"]=!state->options.tools.hooks.isEmpty();definition["_meta"]=metadata;result.append(definition);
        }
        return result;
    };
    server.handlers["tools/call"] = [state](const auto& params, const auto& request) { return state->call(params, request); };
    if(state->options.permissionRequests) {
        server.experimentalCapabilities["iisacc/permissionRequests"]=QJsonObject{{"schema","iisacc.permission-request/1"},
            {"pendingMethod","iisacc/permissions/pending"},{"respondMethod","iisacc/permissions/respond"}};
        for(const auto& method:QStringList{"iisacc/permissions/pending","iisacc/permissions/respond"})
            server.controlHandlers[method]=[state,method](const QJsonObject& params,const mcp::ServerRequestContext& request) {
                request.cancellation.throwIfCancelled();const bool pending=method.endsWith("/pending");
                const QStringList keys=pending?QStringList{"after","limit","_meta"}:QStringList{"request_id","decision","_meta"};
                for(auto i=params.begin();i!=params.end();++i)if(!keys.contains(i.key()))throw mcp::RpcError(-32602,"Unknown permission parameter: "+i.key());
                auto broker=state->conversation(request.sessionId)->permissionRequests;
                if(!pending) {
                    if(!params["request_id"].isString()||!params["decision"].isObject())throw mcp::RpcError(-32602,"Permission request_id and decision are required");
                    return broker->respond(params["request_id"].toString(),params["decision"].toObject());
                }
                const auto after=params.value("after");const auto cursor=after.toDouble();const auto limit=params.value("limit");const auto count=limit.toDouble(32);
                if((!after.isUndefined()&&(!after.isDouble()||cursor<0||cursor>9007199254740991.0||std::floor(cursor)!=cursor))
                    ||(!limit.isUndefined()&&(!limit.isDouble()||count<1||count>128||std::floor(count)!=count)))throw mcp::RpcError(-32602,"Invalid permission page");
                // Leave room for the JSON-RPC envelope in the default 8 MiB frame.
                return broker->pending(qint64(cursor),int(count),5*1024*1024);
            };
    }
    server.onClosed = [state](const QString& session) { state->closeConnection(session); };
    return server;
}
}
