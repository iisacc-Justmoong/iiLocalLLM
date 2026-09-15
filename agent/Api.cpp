#include "Api.h"
#include "McpConnections.h"
#include "../Parameters.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QThreadPool>
#include <QtCore/QUuid>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <condition_variable>

namespace iiLocalLLM::agent {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
void require(bool ok, QString message, ErrorCode code = ErrorCode::InvalidArgument) { if (!ok) throw Error(code, message); }
void fields(const QJsonObject& p, const QSet<QString>& allowed) {
    for (auto it = p.begin(); it != p.end(); ++it) require(allowed.contains(it.key()), "Unknown agent API parameter: " + it.key());
}
QString text(const QJsonObject& p, const QString& key, bool required = true) {
    const auto value = p.value(key);
    require((!required && value.isUndefined()) || (value.isString() && (!required || !value.toString().trimmed().isEmpty())), "Invalid agent API " + key);
    return value.toString();
}
int integer(const QJsonObject& p, const QString& key, int fallback, int minimum, int maximum) {
    const auto value = p.value(key); if (value.isUndefined()) return fallback;
    require(value.isDouble() && value.toDouble() >= minimum && value.toDouble() <= maximum
        && value.toDouble() == value.toInt(), "Invalid agent API " + key); return value.toInt();
}
QJsonObject object(const QJsonObject& p, const QString& key) {
    const auto value = p.value(key); require(value.isUndefined() || value.isObject(), "Invalid agent API " + key); return value.toObject();
}
QStringList contextPaths(const QJsonObject& parameters, int limit) {
    const auto value = parameters.value("context_paths");
    require(value.isUndefined() || (value.isArray() && value.toArray().size() <= limit), "Invalid agent API context_paths");
    QStringList paths;
    for (const auto& item : value.toArray()) {
        require(item.isString() && !item.toString().isEmpty() && item.toString().size() <= 4096, "Invalid agent API context path");
        paths.append(item.toString());
    }
    return paths;
}
QStringList methods() { return {"agent.info", "agent.sessions.create", "agent.sessions.list", "agent.sessions.get",
    "agent.sessions.fork", "agent.sessions.compact", "agent.context.get", "agent.skills.list", "agent.permissions.get", "agent.mcp.status", "agent.run", "agent.cancel", "agent.status",
    "agent.tasks.create", "agent.tasks.get", "agent.tasks.list", "agent.tasks.update", "agent.tasks.claim", "agent.todos.write", "agent.todos.get",
    "agent.shell.start", "agent.shell.output", "agent.shell.stop", "agent.shell.list",
    "agent.agents.run", "agent.agents.output", "agent.agents.stop", "agent.agents.list", "agent.agents.profiles",
    "agent.inputs.enqueue", "agent.inputs.list", "agent.inputs.remove", "agent.inputs.run", "agent.sessions.end", "agent.sessions.clear",
    "agent.permissions.pending", "agent.permissions.respond", "agent.hooks.status", "agent.hooks.cancel",
    "agent.plan.get", "agent.plan.enter", "agent.plan.exit", "agent.questions.ask",
    "agent.memory.get", "agent.memory.read", "agent.memory.write", "agent.memory.edit", "agent.memory.glob", "agent.memory.grep", "agent.memory.forget", "agent.memory.recall",
    "agent.memory.extract", "agent.memory.extraction.status", "agent.memory.extraction.cancel", "agent.sessions.search",
    "agent.memory.dream", "agent.memory.dream.status", "agent.memory.dream.cancel"}; }
bool dreamControl(const QString& method){return method=="agent.memory.dream.status"||method=="agent.memory.dream.cancel";}
bool extractionControl(const QString& method){return method=="agent.memory.extraction.status"||method=="agent.memory.extraction.cancel";}
bool hookControl(const QString& method) {return method=="agent.hooks.status"||method=="agent.hooks.cancel";}
bool inputControl(const QString& method) {
    return method == "agent.inputs.enqueue" || method == "agent.inputs.list" || method == "agent.inputs.remove" || method=="agent.plan.get";
}
QJsonObject sessionObject(const Session& s, int offset = 0, int limit = 0) {
    require(offset <= s.messages.size(), "Message offset exceeds the session length");
    QJsonArray messages;
    const auto end = std::min<qsizetype>(s.messages.size(), qsizetype(offset) + limit);
    for (auto n = offset; n < end; ++n) messages.append(toJson(s.messages[n]));
    QJsonObject value{{"session_id", s.id}, {"model", s.model}, {"system", s.systemPrompt},
        {"working_directory", s.workingDirectory}, {"message_count", s.messages.size()}, {"messages", messages}};
    value["compaction_count"] = s.compactions.size();
    if(!s.parentSessionId.isEmpty())value["parent_session_id"]=s.parentSessionId;
    if (!s.compactions.isEmpty()) value["compaction"] = toJson(s.compactions.last());
    if (end < s.messages.size()) value["next_offset"] = end;
    return value;
}
bool nested(const QString& path, const QString& root) { return path == root || path.startsWith(root.endsWith('/') ? root : root + '/'); }
}
class Api::Impl : public std::enable_shared_from_this<Impl> {
public:
    struct Client { QString id; QByteArray digest; std::shared_ptr<Engine> engine; std::shared_ptr<Subagents> subagents; std::mutex creation; QSet<QString> ending; int reservedSessions=0; std::shared_ptr<PermissionRequests> permissionRequests; };
    struct Job {
        QString id, method, clientId; QJsonObject params; CancellationToken token;
        Clock::time_point deadline; std::atomic_bool running = false;
        bool inputControl = false;
        std::shared_ptr<std::promise<QJsonValue>> promise;
    };
    ApiOptions options;
    std::mutex mutex, joining;
    std::condition_variable changed;
    bool stopping = false;
    std::map<QString, std::shared_ptr<Client>> clients;
    std::map<QString, std::shared_ptr<Job>> active;
    QThreadPool workers, inputWorkers;
    std::unique_ptr<QLockFile> stateLock;

    Impl(std::shared_ptr<Model> model, std::shared_ptr<ToolRegistry> registry,
        std::shared_ptr<const PermissionPolicy> policy, ApiOptions o) : options(std::move(o)) {
        require(model && registry && policy && !options.clientTokens.isEmpty() && options.clientTokens.size() <= 64
            && options.engine.sessionsDirectory.isEmpty() && options.maxConcurrentRequests >= 1 && options.maxConcurrentRequests <= 64
            && options.maxQueuedRequests >= 0 && options.maxQueuedRequests <= 10000
            && options.maxSessionsPerClient >= 1 && options.maxTurns >= 1 && options.maxTurns <= 10000
            && options.maxResultBytes >= 1024 && options.requestTimeoutMs > 0
            && options.maxConcurrentInputControls >= 1 && options.maxConcurrentInputControls <= 16
            && options.maxQueuedInputControls >= 0 && options.maxQueuedInputControls <= 10000, "Invalid agent API configuration");
        require(!options.engine.permissionRequests,"Agent API assigns private permission channels; configure ApiOptions.permissionRequests");
        require(options.engine.projectMemory.directory.isEmpty(),"Agent API assigns private project memory per client");
        require(!options.engine.memoryDream.automatic||(options.engine.projectMemoryEnabled&&options.engine.sessionHistoryEnabled),
            "Automatic memory consolidation requires project memory and session history");
        if(options.permissionRequests) {
            PermissionRequests validate(*options.permissionRequests);
            require(options.permissionRequests->maxRequestBytes<=options.maxResultBytes-256,"Permission requests must fit the API result limit");
        }
        options.workingDirectory = QFileInfo(options.workingDirectory).canonicalFilePath();
        require(!options.workingDirectory.isEmpty() && QFileInfo(options.workingDirectory).isDir(), "Agent API workspace must exist");
        require(options.engine.projectContext.rootDirectory.isEmpty()
            || QFileInfo(options.engine.projectContext.rootDirectory).canonicalFilePath() == options.workingDirectory,
            "Agent API instruction root must equal its workspace");
        options.engine.projectContext.rootDirectory = options.workingDirectory;
        require(!options.stateDirectory.trimmed().isEmpty() && QDir().mkpath(options.stateDirectory), "Cannot create agent API state directory");
        options.stateDirectory = QFileInfo(options.stateDirectory).canonicalFilePath();
        require(!nested(options.stateDirectory, options.workingDirectory) && !nested(options.workingDirectory, options.stateDirectory),
            "Agent API state and tool workspace must be disjoint");
        require(QFile::setPermissions(options.stateDirectory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner),
            "Cannot make agent API state private", ErrorCode::StorageFailure);
        stateLock = std::make_unique<QLockFile>(QDir(options.stateDirectory).filePath("api.lock")); stateLock->setStaleLockTime(0);
        require(stateLock->tryLock(0), "Agent API state is already owned or inaccessible", ErrorCode::AlreadyExists);
        const QRegularExpression idPattern("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"), tokenPattern("^[A-Za-z0-9_-]{32,256}$");
        QSet<QByteArray> digests;
        for (auto it = options.clientTokens.begin(); it != options.clientTokens.end(); ++it) {
            require(idPattern.match(it.key()).hasMatch() && tokenPattern.match(it.value()).hasMatch(), "Invalid agent API client ID or token format");
            auto client = std::make_shared<Client>(); client->id = it.key();
            client->digest = QCryptographicHash::hash(it.value().toUtf8(), QCryptographicHash::Sha256);
            require(!digests.contains(client->digest), "Agent API tokens must be distinct"); digests.insert(client->digest);
            auto engineOptions = options.engine;
            if(options.permissionRequests) {
                client->permissionRequests=std::make_shared<PermissionRequests>(*options.permissionRequests);
                engineOptions.permissionRequests=client->permissionRequests;
            }
            const auto directory = QString::fromLatin1(QCryptographicHash::hash(client->id.toUtf8(), QCryptographicHash::Sha256).toHex());
            engineOptions.sessionsDirectory = QDir(options.stateDirectory).filePath(directory + "/sessions");
            if (options.subagentsEnabled) {
                require(options.subagents.workingDirectory.isEmpty() && options.subagents.stateDirectory.isEmpty(), "Agent API assigns subagent workspace and state");
                auto so = options.subagents; so.workingDirectory = options.workingDirectory;
                so.stateDirectory = QDir(options.stateDirectory).filePath(directory + "/subagents");
                client->subagents = std::make_shared<Subagents>(model, registry, policy, engineOptions, so);
                Subagents::attach(engineOptions,client->subagents);
            }
            client->engine = std::make_shared<Engine>(model, registry, policy, engineOptions);
            clients.emplace(client->id, std::move(client));
        }
        options.clientTokens.clear(); workers.setMaxThreadCount(options.maxConcurrentRequests);
        inputWorkers.setMaxThreadCount(options.maxConcurrentInputControls);
    }
    std::shared_ptr<Client> authenticateLocked(const QString& credential) {
        require(!stopping, "Agent API is shutting down", ErrorCode::ShuttingDown);
        const auto digest = QCryptographicHash::hash(credential.toUtf8(), QCryptographicHash::Sha256);
        std::shared_ptr<Client> found;
        for (const auto& [id, client] : clients) {
            unsigned difference = 0;
            for (qsizetype n = 0; n < digest.size(); ++n) difference |= unsigned(uchar(digest[n]) ^ uchar(client->digest[n]));
            if (!difference) found = client;
        }
        require(bool(found), "Agent API credential is missing or invalid", ErrorCode::Unauthorized); return found;
    }
    Session session(const std::shared_ptr<Client>& client, const QJsonObject& p) {
        auto result = client->engine->session(text(p, "session_id"));
        require(result.workingDirectory == options.workingDirectory, "Session belongs to a different workspace", ErrorCode::NotFound); return result;
    }
    QJsonValue perform(const std::shared_ptr<Client>& client, const std::shared_ptr<Job>& job, RpcEventCallback callback) {
        const auto& p = job->params; const auto& method = job->method;
        if (method == "agent.info") {
            fields(p, {}); QJsonArray names; for (const auto& name : methods()) names.append(name);
            return QJsonObject{{"protocol", "iisacc.agent/1"}, {"client_id", client->id}, {"methods", names}, {"max_turns", options.maxTurns},
                {"working_directory", options.workingDirectory}, {"project_context_enabled", options.engine.projectContext.enabled},
                {"auto_compact_enabled", options.engine.compaction.automatic}, {"tool_search_enabled", options.engine.toolSearch.enabled},
                {"task_tools_enabled", client->engine->taskToolsEnabled()}, {"plan_tools_enabled",bool(client->engine->planning())}, {"background_tasks_enabled", client->engine->backgroundTasksEnabled()},
                {"input_queue_enabled", true}, {"skills_enabled", options.engine.skills.enabled}, {"subagents_enabled", options.subagentsEnabled},
                {"hooks_enabled",!options.engine.hooks.isEmpty()},{"async_hook_controls_enabled",true},
                {"max_async_hook_wake_runs",options.engine.maxAsyncHookWakeRuns},{"permission_requests_enabled",bool(client->permissionRequests)},
                {"user_questions_enabled",bool(client->engine->userQuestionTool())},{"project_memory_enabled",client->engine->projectMemoryEnabled()},
                {"memory_recall_enabled",client->engine->memoryRecallEnabled()},{"memory_extraction_enabled",client->engine->memoryExtractionEnabled()},
                {"session_history_enabled",bool(client->engine->sessionSearchTool())},{"memory_dream_available",client->engine->memoryDreamAvailable()},
                {"auto_dream_enabled",client->engine->automaticMemoryDream()}};
        }
        if(method=="agent.sessions.search") {
            const auto id=text(p,"session_id");auto arguments=p;arguments.remove("session_id");
            require(client->engine->sessionMetadata(id).workingDirectory==options.workingDirectory,"Session belongs to a different workspace",ErrorCode::NotFound);
            auto future=std::async(std::launch::async,[client,id,arguments,job,callback] {
                return client->engine->runSessionSearch(id,arguments,job->token,[callback](const Event& event){if(callback)callback(toJson(event));});
            });
            bool timedOut=false;while(future.wait_for(10ms)!=std::future_status::ready){timedOut|=Clock::now()>=job->deadline;if(timedOut)job->token.cancel();}
            const auto value=future.get();require(!timedOut&&Clock::now()<job->deadline,"Agent API request deadline exceeded",ErrorCode::Timeout);
            return QJsonObject{{"text",value.text},{"result",value.data},{"is_error",value.isError}};
        }
        if(method=="agent.memory.dream"||dreamControl(method)) {
            if(method=="agent.memory.dream.status")fields(p,{"session_id","offset","limit"});else fields(p,{"session_id"});const auto id=text(p,"session_id");
            require(client->engine->sessionMetadata(id).workingDirectory==options.workingDirectory,"Session belongs to a different workspace",ErrorCode::NotFound);job->token.throwIfCancelled();
            if(method=="agent.memory.dream")return client->engine->consolidateMemory(id,job->token);
            if(method=="agent.memory.dream.cancel")return client->engine->cancelMemoryDream(id);
            return client->engine->memoryDreamStatus(id,integer(p,"offset",0,0,4096),integer(p,"limit",8,1,8));
        }
        if(method=="agent.memory.extract"||extractionControl(method)) {
            if(method=="agent.memory.extraction.status")fields(p,{"session_id","offset","limit"});else fields(p,{"session_id"});const auto id=text(p,"session_id");
            require(client->engine->sessionMetadata(id).workingDirectory==options.workingDirectory,"Session belongs to a different workspace",ErrorCode::NotFound);
            job->token.throwIfCancelled();
            if(method=="agent.memory.extract")return client->engine->extractMemory(id,job->token);
            if(method=="agent.memory.extraction.cancel")return client->engine->cancelMemoryExtraction(id);
            return client->engine->memoryExtractionStatus(id,integer(p,"offset",0,0,4096),integer(p,"limit",32,1,32));
        }
        if(method=="agent.memory.recall") {
            fields(p,{"session_id","query"});const auto id=text(p,"session_id"),query=text(p,"query");
            require(client->engine->sessionMetadata(id).workingDirectory==options.workingDirectory,"Session belongs to a different workspace",ErrorCode::NotFound);
            auto future=std::async(std::launch::async,[client,id,query,job]{return client->engine->recallMemory(id,query,job->token);});
            bool timedOut=false;while(future.wait_for(10ms)!=std::future_status::ready) {
                timedOut|=Clock::now()>=job->deadline;if(timedOut)job->token.cancel();
            }
            auto value=future.get();require(!timedOut&&Clock::now()<job->deadline,"Agent API request deadline exceeded",ErrorCode::Timeout);return value;
        }
        if(method=="agent.memory.get") {
            fields(p,{"session_id","query"});const auto id=text(p,"session_id");
            require(!p.contains("query")||p["query"].isString(),"Invalid memory query");
            require(client->engine->sessionMetadata(id).workingDirectory==options.workingDirectory,"Session belongs to a different workspace",ErrorCode::NotFound);
            return client->engine->memory(id,p["query"].toString(),job->token);
        }
        static const QMap<QString,QString> memoryMethods{{"agent.memory.read","Read"},{"agent.memory.write","Write"},
            {"agent.memory.edit","Edit"},{"agent.memory.glob","Glob"},{"agent.memory.grep","Grep"},{"agent.memory.forget","MemoryForget"}};
        if(memoryMethods.contains(method)) {
            const auto id=text(p,"session_id");auto arguments=p;arguments.remove("session_id");
            require(client->engine->sessionMetadata(id).workingDirectory==options.workingDirectory,"Session belongs to a different workspace",ErrorCode::NotFound);
            auto future=std::async(std::launch::async,[client,id,name=memoryMethods[method],arguments,job,callback] {
                return client->engine->runMemoryTool(id,name,arguments,job->token,[callback](const Event& event){if(callback)callback(toJson(event));});
            });
            bool timedOut=false;while(future.wait_for(10ms)!=std::future_status::ready) {
                timedOut|=Clock::now()>=job->deadline;if(timedOut)job->token.cancel();
            }
            const auto value=future.get();require(!timedOut&&Clock::now()<job->deadline,"Agent API request deadline exceeded",ErrorCode::Timeout);
            return QJsonObject{{"text",value.text},{"result",value.data},{"is_error",value.isError}};
        }
        static const QMap<QString, QString> agentMethods{{"agent.agents.run", "Agent"}, {"agent.agents.output", "AgentOutput"},
            {"agent.agents.stop", "AgentStop"}, {"agent.agents.list", "AgentList"}, {"agent.agents.profiles", "AgentProfiles"}};
        if (agentMethods.contains(method)) {
            const auto id = text(p, "session_id"); auto arguments = p; arguments.remove("session_id");
            require(client->engine->sessionMetadata(id).workingDirectory == options.workingDirectory,
                "Session belongs to a different workspace", ErrorCode::NotFound);
            auto future = std::async(std::launch::async, [client, id, name = agentMethods[method], arguments, job, callback] {
                return client->engine->runSubagentTool(id, name, arguments, job->token,
                    [callback](const Event& event) { if (callback) callback(toJson(event)); });
            });
            bool timedOut = false;
            while (future.wait_for(10ms) != std::future_status::ready) {
                timedOut |= Clock::now() >= job->deadline; if (timedOut) job->token.cancel();
            }
            const auto value = future.get();
            require(!timedOut && Clock::now() < job->deadline, "Agent API request deadline exceeded", ErrorCode::Timeout);
            return QJsonObject{{"text", value.text}, {"result", value.data}, {"is_error", value.isError}};
        }
        if(method=="agent.plan.enter"||method=="agent.plan.exit"||method=="agent.questions.ask") {
            const auto id=text(p,"session_id");auto arguments=p;arguments.remove("session_id");
            require(client->engine->sessionMetadata(id).workingDirectory==options.workingDirectory,"Session belongs to a different workspace",ErrorCode::NotFound);
            auto future=std::async(std::launch::async,[client,id,method,arguments,job,callback] {
                if(method=="agent.questions.ask")return client->engine->runQuestionTool(id,arguments,job->token,
                    [callback](const Event& event){if(callback)callback(toJson(event));});
                return client->engine->runPlanTool(id,method=="agent.plan.enter"?"EnterPlanMode":"ExitPlanMode",arguments,job->token,
                    [callback](const Event& event){if(callback)callback(toJson(event));});
            });
            bool timedOut=false;while(future.wait_for(10ms)!=std::future_status::ready) {
                timedOut|=Clock::now()>=job->deadline;if(timedOut)job->token.cancel();
            }
            const auto value=future.get();require(!timedOut&&Clock::now()<job->deadline,"Agent API request deadline exceeded",ErrorCode::Timeout);
            return QJsonObject{{"text",value.text},{"result",value.data},{"is_error",value.isError}};
        }
        if(hookControl(method)) {
            const auto id=text(p,"session_id");
            require(client->engine->sessionMetadata(id).workingDirectory==options.workingDirectory,"Session belongs to a different workspace",ErrorCode::NotFound);
            job->token.throwIfCancelled();
            if(method=="agent.hooks.cancel") {
                fields(p,{"session_id","hook_id"});const auto hookId=text(p,"hook_id",false);
                require(!p.contains("hook_id")||(!hookId.isEmpty()&&hookId.size()<=128),"Invalid hook_id");
                return client->engine->cancelHooks(id,hookId);
            }
            fields(p,{"session_id","offset","limit"});return client->engine->hookStatus(id,integer(p,"offset",0,0,1000000),integer(p,"limit",32,1,128));
        }
        if (inputControl(method)) {
            const auto id = text(p, "session_id");
            require(client->engine->sessionMetadata(id).workingDirectory == options.workingDirectory,
                "Session belongs to a different workspace", ErrorCode::NotFound);
            if(method=="agent.plan.get") {fields(p,{"session_id"});return client->engine->planStatus(id,job->token);}
            if (method == "agent.inputs.list") {
                fields(p, {"session_id", "offset", "limit"});
                return client->engine->queuedInputs(id, integer(p, "offset", 0, 0, 1000000), integer(p, "limit", 32, 1, 100), job->token);
            }
            if (method == "agent.inputs.remove") {
                fields(p, {"session_id", "input_id"}); return client->engine->removeInput(id, text(p, "input_id"), job->token);
            }
            fields(p, {"session_id", "text", "kind", "priority", "context_paths"});
            auto input = p; input.remove("session_id"); return client->engine->enqueueInput(id, input, job->token);
        }
        static const QMap<QString, QString> shellMethods{{"agent.shell.start", "Bash"}, {"agent.shell.output", "TaskOutput"},
            {"agent.shell.stop", "TaskStop"}, {"agent.shell.list", "ShellTaskList"}};
        if (shellMethods.contains(method)) {
            const auto id = text(p, "session_id"); auto arguments = p; arguments.remove("session_id");
            require(client->engine->sessionMetadata(id).workingDirectory == options.workingDirectory,
                "Session belongs to a different workspace", ErrorCode::NotFound);
            if (method == "agent.shell.start") {
                require(!arguments.contains("run_in_background"), "agent.shell.start always starts in the background");
                arguments["run_in_background"] = true;
            }
            bool deadlineLimited = false;
            if (method == "agent.shell.output") {
                require(!arguments.contains("block") || arguments.value("block").isBool(), "Invalid agent API block");
                const auto wait = integer(arguments, "timeout", 30000, 0, 600000);
                if (arguments.value("block").toBool(true)) {
                    const auto remaining = std::max<qint64>(0, std::chrono::duration_cast<std::chrono::milliseconds>(job->deadline - Clock::now()).count());
                    deadlineLimited = wait > remaining;
                    if (deadlineLimited) arguments["timeout"] = int(remaining);
                }
            }
            const auto value = client->engine->runShellTool(id, shellMethods[method], arguments, job->token,
                [callback](const Event& event) { if (callback) callback(toJson(event)); });
            require(!(deadlineLimited && value.data["retrieval_status"] == "timeout"), "Agent API request deadline exceeded", ErrorCode::Timeout);
            return QJsonObject{{"text", value.text}, {"result", value.data}, {"is_error", value.isError}};
        }
        static const QMap<QString, QString> taskMethods{{"agent.tasks.create", "TaskCreate"}, {"agent.tasks.get", "TaskGet"},
            {"agent.tasks.list", "TaskList"}, {"agent.tasks.update", "TaskUpdate"}, {"agent.tasks.claim", "TaskClaim"},
            {"agent.todos.write", "TodoWrite"}, {"agent.todos.get", "TodoRead"}};
        if (taskMethods.contains(method)) {
            const auto id = text(p, "session_id"); auto arguments = p; arguments.remove("session_id");
            require(client->engine->sessionMetadata(id).workingDirectory == options.workingDirectory,
                "Session belongs to a different workspace", ErrorCode::NotFound);
            const auto value = client->engine->runTaskTool(id, taskMethods[method], arguments, job->token,
                [callback](const Event& event) { if (callback) callback(toJson(event)); });
            return QJsonObject{{"text", value.text}, {"result", value.data}, {"is_error", value.isError}};
        }
        if (method == "agent.mcp.status") {
            fields(p, {});
            return QJsonObject{{"servers", options.mcp ? options.mcp->status() : QJsonArray{}}};
        }
        if (method == "agent.sessions.create") {
            fields(p, {"model", "system"}); const auto model = text(p, "model"), prompt = text(p, "system", false);
            require(model.size() <= 512 && prompt.size() <= options.engine.maxInputCharacters, "Agent session input exceeds limit");
            std::lock_guard lock(client->creation);
            require(client->engine->sessions().size()+client->reservedSessions < options.maxSessionsPerClient, "Agent session limit reached", ErrorCode::ResourceLimit);
            return sessionObject(client->engine->createSession(model, options.workingDirectory, prompt));
        }
        if (method == "agent.sessions.list") {
            fields(p, {"cursor", "limit"}); const auto cursor = text(p, "cursor", false); require(cursor.size() <= 128, "Invalid session cursor");
            const int limit = integer(p, "limit", 32, 1, 100); const auto ids = client->engine->sessions();
            QJsonArray items; QString last; bool more = false;
            for (const auto& id : ids) if (id > cursor) {
                if (items.size() == limit) { more = true; break; }
                items.append(QJsonObject{{"session_id", id}}); last = id;
            }
            QJsonObject result{{"sessions", items}}; if (more) result["next_cursor"] = last; return result;
        }
        if (method == "agent.sessions.get") {
            fields(p, {"session_id", "offset", "limit"});
            return sessionObject(session(client, p), integer(p, "offset", 0, 0, 1000000), integer(p, "limit", 32, 0, 100));
        }
        if(method=="agent.sessions.end"||method=="agent.sessions.clear") {
            const bool clear=method=="agent.sessions.clear";
            fields(p,clear?QSet<QString>{"session_id"}:QSet<QString>{"session_id","reason"});const auto original=client->engine->sessionMetadata(text(p,"session_id"));
            require(original.workingDirectory==options.workingDirectory,"Session belongs to a different workspace",ErrorCode::NotFound);
            auto reason=text(p,"reason",false);if(!p.contains("reason"))reason="other";
            require(QStringList{"clear","resume","logout","prompt_input_exit","other","bypass_permissions_disabled"}.contains(reason),"Invalid session exit reason");
            struct Reservation {std::shared_ptr<Client> client;bool held=false;
                ~Reservation(){if(held){std::lock_guard lock(client->creation);--client->reservedSessions;}}} reservation{client};
            if(clear) {
                std::lock_guard lock(client->creation);
                require(client->engine->sessions().size()+client->reservedSessions<options.maxSessionsPerClient,"Agent session limit reached",ErrorCode::ResourceLimit);
                ++client->reservedSessions;reservation.held=true;
            }
            {
                std::unique_lock lock(mutex);job->token.throwIfCancelled();
                require(!client->ending.contains(original.id),"Agent session is ending",ErrorCode::ModelInUse);client->ending.insert(original.id);
                auto belongs=[&](const auto& candidate){return candidate->clientId==client->id&&candidate->params["session_id"]==original.id
                    &&candidate->method!="agent.sessions.end"&&candidate->method!="agent.sessions.clear";};
                for(const auto& [_,candidate]:active)if(belongs(candidate))candidate->token.cancel();
                // Queued jobs see cancellation before entering perform(). Running
                // jobs finish their Engine admission/cleanup before SessionEnd.
                changed.wait(lock,[&]{return std::none_of(active.begin(),active.end(),[&](const auto& item){return belongs(item.second)&&item.second->running;});});
            }
            struct Finish {Impl& state;std::shared_ptr<Client> client;QString id;
                ~Finish(){std::lock_guard lock(state.mutex);client->ending.remove(id);state.changed.notify_all();}} finish{*this,client,original.id};
            if(clear) {
                auto result=client->engine->clearSession(original.id);
                // Report the committed replacement even if cooperative cleanup
                // exceeded the request budget; do not hide its new identity.
                result["deadline_exceeded"]=Clock::now()>=job->deadline;return result;
            }
            const auto result=client->engine->endSession(original.id,reason);
            require(Clock::now()<job->deadline,"Agent API request deadline exceeded after session cleanup",ErrorCode::Timeout);
            return result;
        }
        if (method == "agent.context.get") {
            fields(p, {"session_id", "context_paths"}); const auto original = session(client, p);
            return client->engine->context(original.id, contextPaths(p, options.engine.projectContext.maxTargetPaths), job->token).toJson();
        }
        if (method == "agent.skills.list") {
            fields(p, {"session_id"}); const auto original = session(client, p);
            return client->engine->skills(original.id, job->token).toJson();
        }
        if(method=="agent.permissions.get") {
            fields(p,{"session_id"});const auto original=session(client,p);
            return client->engine->permissions(original.id,job->token);
        }
        if (method == "agent.sessions.fork") {
            fields(p, {"session_id", "through_message_id"}); const auto original = session(client, p);
            std::lock_guard lock(client->creation);
            require(client->engine->sessions().size()+client->reservedSessions < options.maxSessionsPerClient, "Agent session limit reached", ErrorCode::ResourceLimit);
            auto result = sessionObject(client->engine->forkSession(original.id, text(p, "through_message_id", false)));
            result["parent_session_id"] = original.id; return result;
        }
        const bool compactOnly = method == "agent.sessions.compact";
        const bool queuedOnly = method == "agent.inputs.run";
        if (compactOnly) fields(p, {"session_id", "instructions", "options"});
        else if (queuedOnly) fields(p, {"session_id", "options", "max_turns", "context_paths"});
        else fields(p, {"session_id", "prompt", "options", "max_turns", "context_paths", "skill", "skill_arguments"});
        const auto original = session(client, p);
        RunRequest request{original.id, compactOnly || queuedOnly ? QString() : text(p, "prompt", !p.contains("skill")), generationOptionsFromJson(object(p, "options")), integer(p, "max_turns", options.maxTurns, 1, options.maxTurns)};
        request.contextPaths = contextPaths(p, options.engine.projectContext.maxTargetPaths);
        request.skill = text(p, "skill", false); request.skillArguments = text(p, "skill_arguments", false);
        job->token.throwIfCancelled();
        require(Clock::now() < job->deadline, "Agent API request deadline exceeded", ErrorCode::Timeout);
        quint64 sequence = 0;
        auto observe = [&](const Event& event) {
            auto value = toJson(event); value["sequence"] = double(++sequence);
            require(sequence <= 200000 && QJsonDocument(value).toJson(QJsonDocument::Compact).size() <= options.maxResultBytes,
                "Agent API event exceeds limit", ErrorCode::ResourceLimit);
            if (callback) callback(value);
        };
        auto handle = compactOnly ? client->engine->compact({original.id, request.generation, text(p, "instructions", false)}, observe)
            : queuedOnly ? client->engine->runQueued(std::move(request), observe) : client->engine->run(std::move(request), observe);
        bool timedOut = false;
        while (handle.result.wait_for(10ms) != std::future_status::ready) {
            timedOut |= Clock::now() >= job->deadline;
            if (job->token.isCancelled() || timedOut) handle.cancel();
        }
        auto result = handle.result.get();
        require(!timedOut && Clock::now() < job->deadline, "Agent API request deadline exceeded", ErrorCode::Timeout);
        return toJson(result);
    }
    void execute(std::shared_ptr<Client> client, std::shared_ptr<Job> job, RpcEventCallback callback) {
        QJsonValue result; std::exception_ptr failure;
        try {
            job->token.throwIfCancelled(); require(Clock::now() < job->deadline, "Agent API request expired in queue", ErrorCode::Timeout);
            {std::lock_guard lock(mutex);job->token.throwIfCancelled();job->running=true;}
            result = perform(client, job, std::move(callback));
            require(QJsonDocument(result.toObject()).toJson(QJsonDocument::Compact).size() <= options.maxResultBytes,
                "Agent API result exceeds limit", ErrorCode::ResourceLimit);
        } catch (...) { failure = std::current_exception(); }
        { std::lock_guard lock(mutex); active.erase(job->id);changed.notify_all(); }
        if (failure) job->promise->set_exception(failure); else job->promise->set_value(std::move(result));
    }
    RpcHandle dispatch(QString method, QJsonObject params, QString credential, RpcEventCallback callback) {
        std::lock_guard lock(mutex); auto client = authenticateLocked(credential);
        require(methods().contains(method), "Unknown agent API method", ErrorCode::NotFound);
        require(!client->ending.contains(params.value("session_id").toString()),"Agent session is ending",ErrorCode::ModelInUse);
        require(QJsonDocument(params).toJson(QJsonDocument::Compact).size() <= options.maxResultBytes, "Agent API parameters exceed limit", ErrorCode::ResourceLimit);
        auto promise = std::make_shared<std::promise<QJsonValue>>(); RpcHandle handle{uuid(), {}, promise->get_future().share()};
        // These bounded broker operations never acquire an Engine/session lease or
        // enter the worker pools that may already be waiting for this answer.
        if(method=="agent.permissions.pending"||method=="agent.permissions.respond") {
            require(bool(client->permissionRequests),"Remote permission requests are disabled",ErrorCode::RuntimeUnavailable);
            if(method.endsWith(".pending")) {
                fields(params,{"after","limit"});const auto after=params.value("after");const auto cursor=after.toDouble();
                require(after.isUndefined()||(after.isDouble()&&cursor>=0&&cursor<=9007199254740991.0&&std::floor(cursor)==cursor),"Invalid permission cursor");
                promise->set_value(client->permissionRequests->pending(qint64(cursor),integer(params,"limit",32,1,128),std::min(options.maxResultBytes,64*1024*1024)));
            } else {
                fields(params,{"request_id","decision"});require(params["decision"].isObject(),"Permission decision must be an object");
                promise->set_value(client->permissionRequests->respond(text(params,"request_id"),params["decision"].toObject()));
            }
            return handle;
        }
        if (method == "agent.cancel" || method == "agent.status") {
            fields(params, {"request_id"}); const auto found = active.find(text(params, "request_id"));
            require(found != active.end() && found->second->clientId == client->id, "Agent request was not found", ErrorCode::NotFound);
            const auto job = found->second;
            if (method == "agent.cancel") { job->token.cancel(); promise->set_value(QJsonObject{{"cancel_requested", true}}); }
            else promise->set_value(QJsonObject{{"request_id", job->id}, {"method", job->method}, {"session_id", job->params.value("session_id")},
                {"state", job->running ? "running" : "queued"}, {"cancel_requested", job->token.isCancelled()}});
            return handle;
        }
        const bool control = dreamControl(method) || extractionControl(method) || hookControl(method) || inputControl(method) || method == "agent.sessions.end" || method == "agent.sessions.clear" || method == "agent.agents.output" || method == "agent.agents.stop" || method == "agent.agents.list";
        const auto used = std::count_if(active.begin(), active.end(), [control](const auto& item) { return item.second->inputControl == control; });
        const auto capacity = control ? options.maxConcurrentInputControls + options.maxQueuedInputControls
            : options.maxConcurrentRequests + options.maxQueuedRequests;
        require(used < capacity, "Agent API queue is full", ErrorCode::QueueFull);
        auto job = std::make_shared<Job>(); job->id = handle.requestId; job->method = std::move(method); job->params = std::move(params);
        job->inputControl = control;
        job->clientId = client->id; job->token = handle.cancellation; job->promise = std::move(promise);
        job->deadline = Clock::now() + std::chrono::milliseconds(options.requestTimeoutMs);
        active.emplace(job->id, job); auto self = shared_from_this();
        auto& pool = control ? inputWorkers : workers;
        pool.start([self, client, job, callback = std::move(callback)] { self->execute(client, job, callback); });
        return handle;
    }
    void stop() {
        std::lock_guard join(joining);
        { std::lock_guard lock(mutex); stopping = true; for (const auto& [id, job] : active) job->token.cancel();
            for(const auto& [id,client]:clients)if(client->permissionRequests)client->permissionRequests->close(); }
        workers.waitForDone();
        inputWorkers.waitForDone();
        for (const auto& [id, client] : clients) client->engine->close();
        for (const auto& [id, client] : clients) if (client->subagents) client->subagents->close();
        { std::lock_guard lock(mutex); clients.clear(); stateLock.reset(); }
    }
};
Api::Api(std::shared_ptr<Model> model, std::shared_ptr<ToolRegistry> registry,
    std::shared_ptr<const PermissionPolicy> policy, ApiOptions options)
    : d(std::make_shared<Impl>(std::move(model), std::move(registry), std::move(policy), std::move(options))) {}
Api::~Api() { d->stop(); }
bool Api::isControlMethod(const QString& method) const {
    return dreamControl(method)||extractionControl(method)||hookControl(method)||method=="agent.plan.get"||method=="agent.permissions.pending"||method=="agent.permissions.respond"||method=="agent.cancel"||method=="agent.status";
}
RpcHandle Api::dispatch(QString method, QJsonObject params, QString credential, RpcEventCallback callback) {
    return d->dispatch(std::move(method), std::move(params), std::move(credential), std::move(callback));
}
void Api::close() { d->stop(); }
}
