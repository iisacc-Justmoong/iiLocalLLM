#include <mcp/Server.h>
#include <agent/McpServer.h>
#include <agent/ShellTasks.h>
#include <agent/Subagents.h>
#include <agent/PermissionSettings.h>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonDocument>
#include <QtTest/QTest>
#include <chrono>
#include <thread>
using namespace iiLocalLLM;
using namespace std::chrono_literals;
namespace m = iiLocalLLM::mcp;
namespace a = iiLocalLLM::agent;
namespace {
QJsonObject request(int id, QString method, QJsonObject params = {}) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}
QJsonObject tool(QString name) { return {{"name", name}, {"inputSchema", QJsonObject{{"type", "object"}}}}; }
m::ServerOptions options() {
    m::ServerOptions o;
    o.lists["tools/list"] = [](const auto&) { return QJsonArray{tool("echo")}; };
    o.handlers["tools/call"] = [](const QJsonObject& p, const m::ServerRequestContext& c) {
        c.cancellation.throwIfCancelled();
        c.progress({{"progress", 1}, {"total", 2}});
        c.progress({{"progress", 2}, {"total", 2}});
        return QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", p["arguments"].toObject()["text"].toString()}}}},
            {"structuredContent", p["arguments"].toObject()}, {"_meta", QJsonObject{{"app", "fixture"}}}};
    };
    o.requestTimeoutMs = 2000;
    return o;
}
QJsonObject next(m::ServerSession& s) {
    auto messages = s.takeMessages(2000);
    if (messages.size() != 1) throw std::runtime_error("Expected exactly one response");
    return messages[0].toObject();
}
void initialize(m::ServerSession& s, QJsonObject caps = {}, QString version = "2025-11-25") {
    s.receive(request(1, "initialize", {{"protocolVersion", version}, {"capabilities", caps},
        {"clientInfo", QJsonObject{{"name", "test-client"}, {"version", "1"}}}}));
    const auto result = next(s)["result"].toObject();
    if (result["protocolVersion"] != version) throw std::runtime_error("Handshake failed");
    s.receive({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
}
QJsonObject call(m::ServerSession& s, int id, QString name, QJsonObject args = {}) {
    s.receive(request(id, "tools/call", {{"name", name}, {"arguments", args}}));
    return next(s)["result"].toObject();
}
class HistoryModel final : public a::Model {
public:
    std::atomic_bool waiting = false, released = false;
    std::optional<ContextBudget> measure(const a::ModelRequest& r, const CancellationToken&) override {
        qint64 count = 100 + r.systemPrompt.size() + r.tools.size() * 40;
        for (const auto& message : r.messages) count += message.text.size() + 10;
        return ContextBudget{count, 16384};
    }
    a::ModelReply generate(const a::ModelRequest& r, const CancellationToken& token, const TextCallback&) override {
        token.throwIfCancelled();
        if (!r.messages.isEmpty() && r.messages.last().text == "hold") {
            waiting = true;
            while (!released && !token.isCancelled()) std::this_thread::sleep_for(1ms);
            token.throwIfCancelled();
        }
        if (r.summarizing) return {"MCP conversation summary with preserved source observations", {}, {100, 10, 0, 0}};
        QStringList history;
        for (const auto& m : r.messages) if (m.role == a::MessageRole::User) history.append(m.text);
        return {history.join('|'), {}, {}};
    }
};
}
class McpServerTests : public QObject {
    Q_OBJECT
private slots:
    void clearControlInterruptsActiveRunAndDoesNotAcceptForeignSessionIds() {
        QTemporaryDir root;auto registry=std::make_shared<a::ToolRegistry>();auto model=std::make_shared<HistoryModel>();
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);a::EngineOptions engineConfig;engineConfig.sessionsDirectory=root.filePath("sessions");
        QStringList starts;engineConfig.hooks.append([&](const a::HookInput& input,const CancellationToken&){
            if(input.kind==a::HookKind::SessionStart)starts.append(input.context["source"].toString());return a::HookResult{};});
        auto engine=std::make_shared<a::Engine>(model,registry,policy,engineConfig);
        a::McpServerOptions config;config.workingDirectory=root.path();config.engine=engine;config.model="fixture";
        auto options=a::mcpServerOptions(registry,policy,config);m::ServerSession first(options),second(options);initialize(first);initialize(second);
        QVERIFY(call(second,2,"iiLocalLLM.agent.clear")["isError"].toBool());
        first.receive(request(2,"tools/call",{{"name","iiLocalLLM.agent.run"},{"arguments",QJsonObject{{"prompt","hold"}}}}));
        QTRY_VERIFY(model->waiting.load());
        first.receive(request(3,"tools/call",{{"name","iiLocalLLM.agent.clear"},{"arguments",QJsonObject{}}}));
        QJsonObject run,cleared;QElapsedTimer timer;timer.start();
        while((run.isEmpty()||cleared.isEmpty())&&timer.elapsed()<3000)for(const auto& value:first.takeMessages(20)) {
            const auto message=value.toObject();if(message["id"]==2)run=message;if(message["id"]==3)cleared=message["result"].toObject();
        }
        model->released=true;
        QVERIFY2(run.contains("error"),qPrintable(QString::fromUtf8(QJsonDocument(run).toJson())));QVERIFY(cleared["structuredContent"].toObject()["complete"].toBool());
        QCOMPARE(run["error"].toObject()["data"].toObject()["error_code"],"cancelled");
        const auto id=cleared["structuredContent"].toObject()["session_id"].toString();QVERIFY(engine->session(id).messages.isEmpty());
        QCOMPARE(starts,(QStringList{"startup","clear"}));
        QVERIFY(call(second,3,"iiLocalLLM.agent.clear",{{"session_id",id}})["isError"].toBool());
        QCOMPARE(call(first,4,"iiLocalLLM.agent.run",{{"prompt","fresh"}})["structuredContent"].toObject()["text"],"fresh");
        first.close();second.close();
    }
    void sessionEndFollowsConversationReplacementAndConnectionClose() {
        QTemporaryDir root;const auto workspace=root.filePath("workspace");QDir().mkpath(workspace);
        auto model=std::make_shared<HistoryModel>();auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions eo;eo.sessionsDirectory=root.filePath("sessions");eo.compaction.automatic=false;
        QJsonArray ended;eo.hooks.append([&](const a::HookInput& input,const CancellationToken&){
            if(input.kind==a::HookKind::SessionEnd)ended.append(QJsonObject{{"id",input.sessionId},{"reason",input.context["reason"]}});
            return a::HookResult{};});
        auto engine=std::make_shared<a::Engine>(model,registry,policy,eo);a::McpServerOptions options;options.workingDirectory=workspace;options.engine=engine;options.model="local";
        auto bridge=a::mcpServerOptions(registry,policy,options);m::ServerSession first(bridge),second(bridge);initialize(first);initialize(second);
        const auto old=call(first,2,"iiLocalLLM.agent.run",{{"prompt","one"}})["structuredContent"].toObject().value("session_id");
        const auto other=call(second,2,"iiLocalLLM.agent.run",{{"prompt","two"}})["structuredContent"].toObject().value("session_id");
        QVERIFY(ended.isEmpty());const auto fresh=call(first,3,"iiLocalLLM.agent.run",{{"prompt","fresh"},{"new_session",true}})["structuredContent"].toObject().value("session_id");
        QVERIFY(old!=fresh);QCOMPARE(ended.size(),1);QCOMPARE(ended.first().toObject()["id"],old);QCOMPARE(ended.first().toObject()["reason"],"clear");
        first.close();QCOMPARE(ended.size(),2);QCOMPARE(ended.last().toObject()["id"],fresh);QCOMPARE(ended.last().toObject()["reason"],"other");
        first.close();QCOMPARE(ended.size(),2);second.close();QCOMPARE(ended.size(),3);QCOMPARE(ended.last().toObject()["id"],other);
        engine->close();QCOMPARE(ended.size(),3);QVERIFY(!engine->session(old.toString()).messages.isEmpty());
    }
    void permissionInspectionAndFileReloadUseHostWorkspace() {
        QTemporaryDir root;const auto workspace=root.filePath("workspace");QVERIFY(QDir().mkpath(workspace+"/.claude"));
        const auto path=workspace+"/.claude/settings.json";
        auto save=[&](const char* behavior) { QFile file(path);if(!file.open(QIODevice::WriteOnly))throw std::runtime_error("fixture");file.write(QJsonDocument(QJsonObject{{"permissions",QJsonObject{{behavior,QJsonArray{"Write(/output/**)"}}}}}).toJson()); };
        save("allow");a::PermissionSettingsOptions settings;settings.workingDirectory=workspace;settings.fallbackMode=a::PermissionMode::DontAsk;
        auto policy=std::make_shared<a::SettingsPermissionPolicy>(settings);auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,workspace);
        a::McpServerOptions options;options.workingDirectory=workspace;
        m::ServerSession session(a::mcpServerOptions(registry,policy,options));initialize(session);
        auto result=call(session,2,"iiLocalLLM.agent.permissions.get");QCOMPARE(result["structuredContent"].toObject()["provider"],"settings");
        QVERIFY(!call(session,3,"Write",{{"path","output/first"},{"content","first"}})["isError"].toBool());
        QVERIFY(QFileInfo::exists(workspace+"/output/first"));save("deny");
        QVERIFY(call(session,4,"Write",{{"path","output/second"},{"content","second"}})["isError"].toBool());QVERIFY(!QFileInfo::exists(workspace+"/output/second"));
        QVERIFY(call(session,5,"iiLocalLLM.agent.permissions.get",{{"working_directory",root.path()}})["isError"].toBool());
    }
    void subagentConnectionsAreIsolatedAndClosedChildrenStop() {
        QTemporaryDir root; const auto workspace=root.filePath("workspace");QDir().mkpath(workspace);
        auto model=std::make_shared<HistoryModel>();auto registry=std::make_shared<a::ToolRegistry>();
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions eo;eo.sessionsDirectory=root.filePath("sessions");eo.compaction.automatic=false;
        a::SubagentOptions so;so.workingDirectory=workspace;so.stateDirectory=root.filePath("subagents");
        so.profiles.enabled=true;so.profiles.projectBoundary=workspace;
        auto agents=std::make_shared<a::Subagents>(model,registry,policy,eo,so);a::Subagents::attach(eo,agents);
        auto engine=std::make_shared<a::Engine>(model,registry,policy,eo);
        a::McpServerOptions options;options.workingDirectory=workspace;options.engine=engine;options.model="local";
        auto bridge=a::mcpServerOptions(registry,policy,options);
        m::ServerSession first(bridge),second(bridge);initialize(first);initialize(second);
        auto profiles=call(first,20,"iiLocalLLM.agent.agents.profiles");
        QCOMPARE(profiles["structuredContent"].toObject()["profiles"].toArray().size(),3);
        QVERIFY(QDir().mkpath(workspace+"/.claude/agents"));QFile profile(workspace+"/.claude/agents/check.md");QVERIFY(profile.open(QIODevice::WriteOnly));profile.write("---\nname: custom\ndescription: MCP profile\n---\nInspect data.\n");profile.close();
        profiles=call(first,21,"iiLocalLLM.agent.agents.profiles");QCOMPARE(profiles["structuredContent"].toObject()["profiles"].toArray().size(),4);
        const auto custom=call(first,22,"iiLocalLLM.agent.agents.run",{{"prompt","MCP_PROFILE"},{"subagent_type","custom"}});QVERIFY(!custom["isError"].toBool());
        auto launched=call(first,2,"iiLocalLLM.agent.agents.run",{{"prompt","hold"},{"run_in_background",true}});
        QVERIFY(!launched["isError"].toBool());const auto id=launched["structuredContent"].toObject().value("agentId");
        QTRY_VERIFY(model->waiting.load());
        auto foreign=call(second,2,"iiLocalLLM.agent.agents.output",{{"agent_id",id}});QVERIFY(foreign["isError"].toBool());
        auto listed=call(first,3,"iiLocalLLM.agent.agents.list");
        QJsonObject state;for(const auto& value:listed["structuredContent"].toObject()["agents"].toArray())if(value.toObject()["agentId"]==id)state=value.toObject();
        QVERIFY(!state.isEmpty());
        QVERIFY(QDir().rename(workspace+"/.claude/agents",workspace+"/.claude/saved-agents"));
        QVERIFY(QFile::link(root.path(),workspace+"/.claude/agents"));
        QVERIFY(call(first,23,"iiLocalLLM.agent.agents.profiles")["isError"].toBool());
        QVERIFY(!call(first,24,"iiLocalLLM.agent.agents.output",{{"agent_id",id}})["isError"].toBool());
        // Use the connection's actual parent identity instead of depending on UUID sort order.
        const auto metadata=call(first,4,"iiLocalLLM.agent.session")["structuredContent"].toObject();
        const auto owner=metadata["session_id"].toString();
        first.close();QCOMPARE(agents->output(owner,id.toString(),true,2000)["status"],"cancelled");
        auto other=call(second,3,"iiLocalLLM.agent.agents.list");QVERIFY(other["structuredContent"].toObject()["agents"].toArray().isEmpty());
        QCOMPARE(state["agentId"],id);
    }
    void skillCatalogAndInvocationUseTheConnectionConversation() {
        for(bool fork:{false,true}) {
        QTemporaryDir root; const auto path = root.filePath(".claude/skills/inspect"); QVERIFY(QDir().mkpath(path));
        QFile file(path + "/SKILL.md"); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray("---\ndescription: Inspect\ndisable-model-invocation: true\n")+(fork?"context: fork\n":"")+"---\nMCP_SKILL $ARGUMENTS"); file.close();
        auto registry = std::make_shared<a::ToolRegistry>(); auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions eo; eo.sessionsDirectory = root.filePath("sessions");
        auto model=std::make_shared<HistoryModel>();std::shared_ptr<a::Subagents> agents;QTemporaryDir children;
        if(fork) {
            a::SubagentOptions so;so.workingDirectory=root.path();so.stateDirectory=children.filePath("state");
            agents=std::make_shared<a::Subagents>(model,registry,policy,eo,so);a::Subagents::attach(eo,agents);
        }
        auto engine = std::make_shared<a::Engine>(model, registry, policy, eo);
        a::McpServerOptions config; config.workingDirectory = root.path(); config.engine = engine; config.model = "fixture";
        const auto options = a::mcpServerOptions(registry, policy, config);
        m::ServerSession first(options), second(options); initialize(first); initialize(second);
        QCOMPARE(call(first, 2, "iiLocalLLM.agent.skills.list")["structuredContent"].toObject()["skills"].toArray().size(), 1);
        const auto value = call(first, 3, "iiLocalLLM.agent.run", {{"skill", "inspect"}, {"skill_arguments", "private args"}});
        QVERIFY2(!value["isError"].toBool(), qPrintable(QJsonDocument(value).toJson()));
        QVERIFY(value["structuredContent"].toObject()["text"].toString().contains("MCP_SKILL private args"));
        const auto other = call(second, 2, "iiLocalLLM.agent.run", {{"prompt", "other"}});
        QVERIFY(!other["structuredContent"].toObject()["text"].toString().contains("private args"));
        QVERIFY(call(first, 4, "iiLocalLLM.agent.run", {{"skill", "missing"}})["isError"].toBool());
        QVERIFY(call(first, 5, "iiLocalLLM.agent.inputs.run", {{"skill", "inspect"}})["isError"].toBool());
        if(fork) {
            const auto state=call(first,6,"iiLocalLLM.agent.session",{{"include_messages",true}})["structuredContent"].toObject();
            QCOMPARE(state["messages"].toArray().size(),2);
            const auto jobs=call(first,7,"iiLocalLLM.agent.agents.list")["structuredContent"].toObject()["agents"].toArray();QCOMPARE(jobs.size(),1);
        }
        }
    }
    void urgentInputBypassesTheRunningConversationLock() {
        QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>();
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass); auto model = std::make_shared<HistoryModel>();
        a::EngineOptions eo; eo.sessionsDirectory = root.filePath("sessions");
        auto engine = std::make_shared<a::Engine>(model, registry, policy, eo);
        a::McpServerOptions config; config.workingDirectory = root.path(); config.engine = engine; config.model = "fixture";
        const auto options = a::mcpServerOptions(registry, policy, config);
        m::ServerSession first(options), second(options); initialize(first); initialize(second);
        first.receive(request(10, "tools/call", {{"name", "iiLocalLLM.agent.run"}, {"arguments", QJsonObject{{"prompt", "hold"}}}}));
        QTRY_VERIFY_WITH_TIMEOUT(model->waiting.load(), 2000);
        first.receive(request(11, "tools/call", {{"name", "iiLocalLLM.agent.inputs.enqueue"},
            {"arguments", QJsonObject{{"text", "follow-up"}, {"priority", "now"}}}}));
        QJsonObject queued, done; QElapsedTimer clock; clock.start();
        while ((queued.isEmpty() || done.isEmpty()) && clock.elapsed() < 3000) for (const auto& value : first.takeMessages(20)) {
            const auto message = value.toObject();
            if (message["id"].toInt() == 11) queued = message;
            if (message["id"].toInt() == 10) done = message;
        }
        QVERIFY2(!queued.isEmpty() && !queued.contains("error") && !queued["result"].toObject()["isError"].toBool(), "Input must reach a running MCP agent");
        QVERIFY2(!done.isEmpty(), "Urgent input must interrupt the current model call");
        QVERIFY(done["result"].toObject()["structuredContent"].toObject()["text"].toString().endsWith("follow-up"));
        const auto saved = call(first, 12, "iiLocalLLM.agent.inputs.enqueue", {{"text", "first connection only"}});
        QVERIFY(!saved["isError"].toBool());
        QCOMPARE(call(second, 2, "iiLocalLLM.agent.inputs.list")["structuredContent"].toObject()["count"].toInt(), 0);
        QVERIFY(!call(first, 13, "iiLocalLLM.agent.inputs.run")["isError"].toBool());
        QCOMPARE(call(first, 14, "iiLocalLLM.agent.inputs.list")["structuredContent"].toObject()["count"].toInt(), 0);
    }
    void newConversationPreservesPreviousBackgroundShells() {
#if !defined(Q_OS_UNIX) || defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
        QSKIP("Background shell execution requires a desktop POSIX host");
#endif
        QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>();
        auto shells = std::make_shared<a::ShellTasks>(root.path(), root.filePath("shells"));
        a::registerWorkspaceTools(*registry, root.path(), shells);
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions eo; eo.sessionsDirectory = root.filePath("sessions");
        auto engine = std::make_shared<a::Engine>(std::make_shared<HistoryModel>(), registry, policy, eo);
        a::McpServerOptions config; config.workingDirectory = root.path(); config.engine = engine; config.model = "fixture";
        m::ServerSession session(a::mcpServerOptions(registry, policy, config)); initialize(session);
        const auto started = call(session, 2, "Bash", {{"command", "sleep 30"}, {"run_in_background", true}});
        const auto taskId = started["structuredContent"].toObject()["backgroundTaskId"].toString(); QVERIFY(!taskId.isEmpty());
        const auto original = call(session, 3, "iiLocalLLM.agent.session")["structuredContent"].toObject()["session_id"].toString();
        const auto reset = call(session, 4, "iiLocalLLM.agent.run", {{"prompt", "New session"}, {"new_session", true}});
        QVERIFY(!reset["isError"].toBool());
        const auto fresh=reset["structuredContent"].toObject()["session_id"].toString();
        QVERIFY(reset["structuredContent"].toObject()["clear"].toObject()["complete"].toBool());
        QVERIFY_THROWS_EXCEPTION(Error,shells->output(original,taskId,false));
        QCOMPARE(call(session, 5, "ShellTaskList")["structuredContent"].toObject()["tasks"].toArray().size(),1);
        QVERIFY(!call(session, 6, "TaskOutput", {{"task_id", taskId}, {"block", false}})["isError"].toBool());
        session.close();QCOMPARE(shells->output(fresh,taskId,false)["task"].toObject()["status"],"killed");
    }
    void shellControlsShareAgentIdentityAndInterruptDuringRun() {
#if !defined(Q_OS_UNIX) || defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
        QSKIP("Background shell execution requires a desktop POSIX host");
#endif
        QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>();
        auto shells = std::make_shared<a::ShellTasks>(root.path(), root.filePath("shells"));
        a::registerWorkspaceTools(*registry, root.path(), shells);
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass); auto model = std::make_shared<HistoryModel>();
        a::EngineOptions eo; eo.sessionsDirectory = root.filePath("sessions");
        auto engine = std::make_shared<a::Engine>(model, registry, policy, eo);
        a::McpServerOptions config; config.workingDirectory = root.path(); config.engine = engine; config.model = "fixture";
        const auto options = a::mcpServerOptions(registry, policy, config);
        m::ServerSession first(options), second(options); initialize(first); initialize(second);
        const auto started = call(first, 2, "Bash", {{"command", "printf mcp-job; sleep 30"}, {"run_in_background", true}});
        QVERIFY(!started["isError"].toBool()); const auto taskId = started["structuredContent"].toObject().value("backgroundTaskId");
        QVERIFY(!taskId.toString().isEmpty());
        const auto sessionId = call(first, 3, "iiLocalLLM.agent.session")["structuredContent"].toObject()["session_id"].toString();
        QVERIFY(!sessionId.isEmpty());
        QCOMPARE(engine->runShellTool(sessionId, "ShellTaskList").data["tasks"].toArray().size(), 1);
        QVERIFY(call(second, 2, "TaskOutput", {{"task_id", taskId}, {"block", false}})["isError"].toBool());
        first.receive(request(10, "tools/call", {{"name", "iiLocalLLM.agent.run"}, {"arguments", QJsonObject{{"prompt", "hold"}}}}));
        QTRY_VERIFY_WITH_TIMEOUT(model->waiting.load(), 2000);
        first.receive(request(11, "tools/call", {{"name", "TaskOutput"}, {"arguments", QJsonObject{{"task_id", taskId}, {"timeout", 30000}}}}));
        first.receive(request(12, "tools/call", {{"name", "TaskStop"}, {"arguments", QJsonObject{{"task_id", taskId}}}}));
        QJsonObject stop, output; QElapsedTimer clock; clock.start();
        while ((stop.isEmpty() || output.isEmpty()) && clock.elapsed() < 2000) for (const auto& value : first.takeMessages(20)) {
            const auto message = value.toObject();
            if (message["id"].toInt() == 12) stop = message["result"].toObject();
            if (message["id"].toInt() == 11) output = message["result"].toObject();
        }
        model->released = true;
        QVERIFY2(!stop.isEmpty() && !stop["isError"].toBool(), "TaskStop must run while the model and TaskOutput are waiting");
        QCOMPARE(output["structuredContent"].toObject()["task"].toObject()["status"], "killed");
        const auto remaining = engine->runShellTool(sessionId, "Bash", {{"command", "sleep 30"}, {"run_in_background", true}}).data["backgroundTaskId"].toString();
        first.close(); second.close();
        QCOMPARE(shells->output(sessionId, remaining, false, 0, 0, 1024)["task"].toObject()["status"], "killed");
    }
    void taskToolsAreConnectionBoundAndShareAgentState() {
        QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>();
        auto policy = std::make_shared<a::RulePolicy>();
        a::McpServerOptions config; config.workingDirectory = root.path();
        config.taskStore = std::make_shared<a::TaskStore>(root.filePath("standalone"));
        auto serverOptions = a::mcpServerOptions(registry, policy, config);
        m::ServerSession first(serverOptions), second(serverOptions); initialize(first); initialize(second);
        auto created = call(first, 2, "TaskCreate", {{"subject", "Verify"}, {"description", "Run checks"}});
        QVERIFY(!created["isError"].toBool()); QCOMPARE(created["structuredContent"].toObject()["task"].toObject()["id"], "1");
        QCOMPARE(call(first, 3, "TaskList")["structuredContent"].toObject()["tasks"].toArray().size(), 1);
        QVERIFY(call(second, 2, "TaskList")["structuredContent"].toObject()["tasks"].toArray().isEmpty());
        QVERIFY(call(second, 3, "TaskList", {{"listId", "first"}})["isError"].toBool());
        first.close();
        m::ServerSession replacement(serverOptions); initialize(replacement);
        QVERIFY(call(replacement, 2, "TaskList")["structuredContent"].toObject()["tasks"].toArray().isEmpty());

        a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions"); options.taskToolsEnabled = true;
        auto enginePolicy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        config.taskStore.reset(); config.engine = std::make_shared<a::Engine>(std::make_shared<HistoryModel>(), registry, enginePolicy, options);
        config.model = "fixture";
        m::ServerSession agent(a::mcpServerOptions(registry, enginePolicy, config)); initialize(agent);
        QVERIFY(!call(agent, 2, "TaskCreate", {{"subject", "Persisted"}, {"description", "Inspect actual state"}})["isError"].toBool());
        const auto id = call(agent, 3, "iiLocalLLM.agent.session")["structuredContent"].toObject()["session_id"].toString();
        QCOMPARE(config.engine->runTaskTool(id, "TaskList").data["total"].toInt(), 1);
        const auto reply = call(agent, 4, "iiLocalLLM.agent.run", {{"prompt", "Inspect tasks"}});
        QVERIFY(reply["structuredContent"].toObject()["text"].toString().contains("Persisted"));
        call(agent, 5, "iiLocalLLM.agent.run", {{"prompt", "New work"}, {"new_session", true}});
        QVERIFY(call(agent, 6, "TaskList")["structuredContent"].toObject()["tasks"].toArray().isEmpty());
        QCOMPARE(config.engine->runTaskTool(id, "TaskList").data["total"].toInt(), 1);
    }
    void lifecycleAndValidation() {
        m::ServerSession s(options());
        s.receive(request(9, "tools/list")); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32002);
        initialize(s); QVERIFY(s.isInitialized()); QCOMPARE(s.clientInfo()["name"].toString(), "test-client");
        s.receive(request(2, "tools/list")); QCOMPARE(next(s)["result"].toObject()["tools"].toArray().size(), 1);
        s.receive(request(3, "unknown")); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32601);
        auto invalid = request(4, "tools/call"); invalid["params"] = QJsonArray{};
        s.receive(invalid); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32602);
        s.receive(request(5, "initialize")); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32600);
    }
    void progressAndStructuredResult() {
        m::ServerSession s(options()); initialize(s);
        s.receive(request(2, "tools/call", {{"name", "echo"}, {"arguments", QJsonObject{{"text", "한글 값"}}},
            {"_meta", QJsonObject{{"progressToken", "caller-token"}}}}));
        QList<QJsonValue> messages;
        QElapsedTimer timer; timer.start();
        while (messages.size() < 3 && timer.elapsed() < 5000) messages += s.takeMessages(200);
        QCOMPARE(messages.size(), 3);
        QCOMPARE(messages[0].toObject()["params"].toObject()["progressToken"].toString(), "caller-token");
        QCOMPARE(messages[1].toObject()["params"].toObject()["progress"].toInt(), 2);
        QCOMPARE(messages[2].toObject()["result"].toObject()["structuredContent"].toObject()["text"].toString(), "한글 값");
        QCOMPARE(messages[2].toObject()["result"].toObject()["_meta"].toObject()["app"].toString(), "fixture");
    }
    void boundedRequestsAndCancellation() {
        auto o = options(); o.maxConcurrentRequests = 1; o.maxQueuedRequests = 0;
        std::atomic_bool started = false, stopped = false;
        o.handlers["test/wait"] = [&](const auto&, const m::ServerRequestContext& c) {
            started = true;
            while (!c.cancellation.isCancelled()) std::this_thread::sleep_for(1ms);
            stopped = true; c.cancellation.throwIfCancelled(); return QJsonObject{};
        };
        m::ServerSession s(o); initialize(s);
        s.receive(request(2, "test/wait")); QTRY_VERIFY(started.load());
        s.receive(request(3, "test/wait")); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32000);
        s.receive({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", QJsonObject{{"requestId", 2}}}});
        QTRY_VERIFY(stopped.load());
        s.receive(request(4, "ping")); QCOMPARE(next(s)["id"].toInt(), 4);
        QVERIFY(s.takeMessages().isEmpty());
    }
    void snapshotsAndSessionIsolation() {
        auto o = options(); o.listPageSize = 1;
        QJsonArray definitions{tool("one"), tool("two"), tool("three")};
        o.lists["tools/list"] = [&](const auto&) { return definitions; };
        m::ServerSession a(o), b(o); initialize(a); initialize(b);
        QVERIFY(a.id() != b.id());
        a.receive(request(2, "tools/list")); auto first = next(a)["result"].toObject();
        const auto cursor = first["nextCursor"].toString(); QVERIFY(!cursor.isEmpty());
        definitions = {tool("replacement")};
        b.receive(request(2, "tools/list", {{"cursor", cursor}})); QCOMPARE(next(b)["error"].toObject()["code"].toInt(), -32602);
        a.receive(request(3, "tools/list", {{"cursor", cursor}})); auto second = next(a)["result"].toObject();
        QCOMPARE(second["tools"].toArray()[0].toObject()["name"].toString(), "two");
        a.receive(request(4, "tools/list", {{"cursor", second["nextCursor"]}}));
        const auto last = next(a)["result"].toObject(); QVERIFY(!last.contains("nextCursor"));
        QCOMPARE(last["tools"].toArray()[0].toObject()["name"].toString(), "three");
    }
    void reverseRequestAndTimeout() {
        auto o = options();
        o.handlers["test/roots"] = [](const auto&, const m::ServerRequestContext& c) { return c.requestClient("roots/list", {}, 1000); };
        o.handlers["test/timeout"] = [](const auto&, const m::ServerRequestContext& c) { return c.requestClient("roots/list", {}, 30); };
        m::ServerSession s(o); initialize(s, {{"roots", QJsonObject{}}});
        s.receive(request(2, "test/roots")); const auto reverse = next(s);
        QCOMPARE(reverse["method"].toString(), "roots/list");
        s.receive({{"jsonrpc", "2.0"}, {"id", reverse["id"]}, {"result", QJsonObject{{"roots", QJsonArray{}}}}});
        QCOMPARE(next(s)["id"].toInt(), 2);
        s.receive(request(3, "test/timeout")); const auto pending = next(s); QVERIFY(pending.contains("method"));
        QList<QJsonValue> messages;
        QElapsedTimer timer; timer.start();
        while (messages.size() < 2 && timer.elapsed() < 5000) messages += s.takeMessages(200);
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages[0].toObject()["method"].toString(), "notifications/cancelled");
        QCOMPARE(messages[1].toObject()["id"].toInt(), 3); QVERIFY(messages[1].toObject().contains("error"));
    }
    void legacyBatchAndModernRejection() {
        m::ServerSession legacy(options()); initialize(legacy, {}, "2025-03-26");
        legacy.receiveBatch({request(2, "tools/list"), request(3, "ping"), 42,
            QJsonObject{{"jsonrpc", "2.0"}, {"method", "notifications/example"}}});
        const auto messages = legacy.takeMessages(2000); QCOMPARE(messages.size(), 1); QVERIFY(messages[0].isArray());
        const auto replies = messages[0].toArray(); QCOMPARE(replies.size(), 3);
        QSet<int> ids; for (const auto& value : replies) if (!value.toObject()["id"].isNull()) ids.insert(value.toObject()["id"].toInt());
        QCOMPARE(ids, (QSet<int>{2, 3})); QCOMPARE(legacy.takeNotifications().size(), 1);
        m::ServerSession modern(options()); initialize(modern);
        modern.receiveBatch({request(2, "ping")}); QCOMPARE(next(modern)["error"].toObject()["code"].toInt(), -32600);
        modern.receive(request(3, "ping")); QCOMPARE(next(modern)["id"].toInt(), 3);
    }
    void oversizedLegacyResponseKeepsBatchMembership() {
        auto o = options(); o.maxMessageBytes = 1024; o.maxQueuedBytes = 4096;
        o.handlers["test/large"] = [](const auto&, const auto&) {
            return QJsonObject{{"text", QString(2048, 'x')}};
        };
        m::ServerSession s(o); initialize(s, {}, "2025-03-26");
        s.receiveBatch({request(2, "test/large"), request(3, "ping")});
        const auto frames = s.takeMessages(2000);
        QCOMPARE(frames.size(), 1); QVERIFY(frames[0].isArray());
        const auto replies = frames[0].toArray(); QCOMPARE(replies.size(), 2);
        QSet<int> ids;
        for (const auto& value : replies) {
            const auto response = value.toObject(); ids.insert(response["id"].toInt());
            if (response["id"] == 2) QCOMPARE(response["error"].toObject()["code"].toInt(), -32000);
            else QVERIFY(response.contains("result"));
        }
        QCOMPARE(ids, (QSet<int>{2, 3}));
        s.receive(request(4, "ping")); QCOMPARE(next(s)["id"].toInt(), 4);
    }
    void exportedToolsKeepPolicyAndContent() {
        QTemporaryDir root;
        auto registry = std::make_shared<a::ToolRegistry>();
        a::Tool value; value.definition = {"value", "Read a value", {{"type", "object"}, {"additionalProperties", false}}, {}, true, true};
        value.execute = [](const auto&, const auto&) { return a::ToolResult{"hello", {{"number", 7}}, false,
            {QJsonObject{{"type", "image"}, {"data", "eA=="}, {"mimeType", "image/png"}}}, {{"private", "payload"}}}; };
        registry->add(value);
        a::Tool denied = value; denied.definition.name = "write"; denied.definition.readOnly = false; registry->add(denied);
        a::McpServerOptions config; config.workingDirectory = root.path(); config.appId = "com.iisacc.fixture";
        m::ServerSession s(a::mcpServerOptions(registry, std::make_shared<a::RulePolicy>(), config)); initialize(s);
        const auto result = call(s, 2, "value");
        QCOMPARE(result["structuredContent"].toObject()["number"].toInt(), 7);
        QCOMPARE(result["_meta"].toObject()["private"].toString(), "payload");
        const auto content = result["content"].toArray();
        QCOMPARE(content[0].toObject()["type"].toString(), "text");
        QCOMPARE(content[1].toObject()["type"].toString(), "image");
        QCOMPARE(content.size(), 3);
        QCOMPARE(QJsonDocument::fromJson(content.last().toObject()["text"].toString().toUtf8()).object(),
            result["structuredContent"].toObject());
        QVERIFY(call(s, 3, "write")["isError"].toBool());
        QVERIFY(call(s, 4, "value", {{"extra", true}})["isError"].toBool());
        s.receive(request(5, "tools/call", {{"name", "missing"}})); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32602);
    }
    void structuredOutputTextIsNotDuplicated() {
        QTemporaryDir root;
        for (const auto& json : QStringList{"{\"value\":7}", "{\n  \"value\": 7\n}"}) {
            for (const auto& version : QStringList{"2025-11-25", "2025-03-26"}) {
                auto registry = std::make_shared<a::ToolRegistry>();
                a::Tool value; value.definition = {"value", "Read a value", {{"type", "object"}}, {}, true, true};
                value.execute = [json](const auto&, const auto&) { return a::ToolResult{json, {{"value", 7}}}; };
                registry->add(value);
                a::McpServerOptions config; config.workingDirectory = root.path();
                m::ServerSession session(a::mcpServerOptions(registry, std::make_shared<a::RulePolicy>(), config)); initialize(session, {}, version);
                QCOMPARE(call(session, 2, "value")["content"].toArray().size(), 1);
            }
        }
    }
    void manualCompactionIsConnectionBound() {
        QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>();
        a::EngineOptions engineConfig; engineConfig.sessionsDirectory = root.filePath("sessions");
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        auto engine = std::make_shared<a::Engine>(std::make_shared<HistoryModel>(), registry, policy, engineConfig);
        a::McpServerOptions config; config.workingDirectory = root.path(); config.engine = engine; config.model = "fixture";
        const auto serverOptions = a::mcpServerOptions(registry, policy, config);
        m::ServerSession first(serverOptions), second(serverOptions); initialize(first); initialize(second);
        call(first, 2, "iiLocalLLM.agent.run", {{"prompt", QString(1000, 'a')}});
        call(first, 3, "iiLocalLLM.agent.run", {{"prompt", QString(1000, 'b')}});
        call(first, 4, "iiLocalLLM.agent.run", {{"prompt", "Continue the task"}});
        const auto compacted = call(first, 5, "iiLocalLLM.agent.compact", {{"instructions", "Preserve unfinished work"}});
        QVERIFY(!compacted.value("isError").toBool());
        QCOMPARE(compacted.value("structuredContent").toObject().value("usage").toObject().value("compactions").toInt(), 1);
        const auto state = call(first, 6, "iiLocalLLM.agent.session").value("structuredContent").toObject();
        QCOMPARE(state.value("message_count").toInt(), 6); QCOMPARE(state.value("compaction_count").toInt(), 1);
        QVERIFY(call(second, 2, "iiLocalLLM.agent.compact").value("isError").toBool());
        QVERIFY(call(second, 3, "iiLocalLLM.agent.compact", {{"session_id", state.value("session_id")}}).value("isError").toBool());
    }
    void localAgentConversationsAreIsolated() {
        QTemporaryDir root;
        auto registry = std::make_shared<a::ToolRegistry>();
        a::EngineOptions engineConfig; engineConfig.sessionsDirectory = root.filePath("sessions");
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        auto engine = std::make_shared<a::Engine>(std::make_shared<HistoryModel>(), registry, policy, engineConfig);
        a::McpServerOptions config; config.workingDirectory = root.path(); config.engine = engine; config.model = "fixture";
        const auto serverOptions = a::mcpServerOptions(registry, policy, config);
        m::ServerSession first(serverOptions), second(serverOptions); initialize(first); initialize(second);
        const auto one = call(first, 2, "iiLocalLLM.agent.run", {{"prompt", "private-first"}});
        const auto two = call(second, 2, "iiLocalLLM.agent.run", {{"prompt", "private-second"}});
        const auto resumed = call(first, 3, "iiLocalLLM.agent.run", {{"prompt", "follow-up"}});
        QVERIFY(one["structuredContent"].toObject()["session_id"] != two["structuredContent"].toObject()["session_id"]);
        QCOMPARE(two["structuredContent"].toObject()["text"].toString(), "private-second");
        QCOMPARE(resumed["structuredContent"].toObject()["text"].toString(), "private-first|follow-up");
        QVERIFY(call(second, 3, "iiLocalLLM.agent.session", {{"session_id", one["structuredContent"].toObject()["session_id"]}})["isError"].toBool());
        const auto reset = call(first, 4, "iiLocalLLM.agent.run", {{"prompt", "new"}, {"new_session", true}});
        QCOMPARE(reset["structuredContent"].toObject()["text"].toString(), "new");
        QVERIFY(QDir().mkpath(root.filePath("src")));
        QFile rules(root.filePath("src/AGENTS.md")); QVERIFY(rules.open(QIODevice::WriteOnly));
        rules.write("Scoped MCP instructions"); rules.close();
        const auto scoped = call(first, 5, "iiLocalLLM.agent.run", {{"prompt", "scoped"}, {"context_paths", QJsonArray{"src/new.cpp"}}});
        QVERIFY(scoped["structuredContent"].toObject()["text"].toString().contains("Scoped MCP instructions"));
        const auto unrelated = call(second, 4, "iiLocalLLM.agent.run", {{"prompt", "unchanged"}});
        QVERIFY(!unrelated["structuredContent"].toObject()["text"].toString().contains("Scoped MCP instructions"));
        QVERIFY(call(first, 6, "iiLocalLLM.agent.run", {{"prompt", "invalid"}, {"context_paths", QJsonArray{"../outside"}}})["isError"].toBool());
    }
    void resourcesPromptsSubscriptionsAndLegacyContent() {
        auto o = options();
        o.lists["resources/list"] = [](const auto&) { return QJsonArray{QJsonObject{{"uri", "fixture://value"}, {"name", "value"}}}; };
        o.lists["resources/templates/list"] = [](const auto&) { return QJsonArray{QJsonObject{{"uriTemplate", "fixture://{key}"}, {"name", "template"}}}; };
        o.handlers["resources/read"] = [](const auto& p, const auto&) { return QJsonObject{{"contents", QJsonArray{QJsonObject{{"uri", p["uri"]}, {"text", "resource text"}}}}}; };
        o.handlers["resources/subscribe"] = [](const auto& p, const auto&) { if (p["uri"] != "fixture://value") throw m::RpcError(-32602, "Unknown resource"); return QJsonObject{}; };
        o.lists["prompts/list"] = [](const auto&) { return QJsonArray{QJsonObject{{"name", "summary"}}}; };
        o.handlers["prompts/get"] = [](const auto&, const auto&) { return QJsonObject{{"messages", QJsonArray{QJsonObject{{"role", "user"},
            {"content", QJsonObject{{"type", "text"}, {"text", "Summarize this"}}}}}}}; };
        m::ServerSession first(o), second(o); initialize(first); initialize(second);
        first.receive(request(2, "resources/list")); QCOMPARE(next(first)["result"].toObject()["resources"].toArray().size(), 1);
        first.receive(request(3, "resources/templates/list")); QCOMPARE(next(first)["result"].toObject()["resourceTemplates"].toArray().size(), 1);
        first.receive(request(4, "resources/read", {{"uri", "fixture://value"}})); QCOMPARE(next(first)["result"].toObject()["contents"].toArray().size(), 1);
        first.receive(request(5, "prompts/get", {{"name", "summary"}})); QCOMPARE(next(first)["result"].toObject()["messages"].toArray().size(), 1);
        first.receive(request(6, "resources/subscribe", {{"uri", "fixture://value"}})); QVERIFY(next(first).contains("result"));
        first.notify("notifications/resources/updated", {{"uri", "fixture://value"}});
        second.notify("notifications/resources/updated", {{"uri", "fixture://value"}});
        QCOMPARE(next(first)["params"].toObject()["uri"].toString(), "fixture://value"); QVERIFY(second.takeMessages().isEmpty());
        first.receive(request(7, "resources/unsubscribe", {{"uri", "fixture://value"}})); QVERIFY(next(first).contains("result"));
        first.notify("notifications/resources/updated", {{"uri", "fixture://value"}}); QVERIFY(first.takeMessages().isEmpty());
        o.handlers["tools/call"] = [](const auto&, const auto&) { return QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "resource_link"}, {"uri", "fixture://value"}, {"name", "value"}}}}, {"structuredContent", QJsonObject{{"n", 7}}}}; };
        m::ServerSession legacy(o); initialize(legacy, {}, "2025-03-26");
        const auto value = call(legacy, 2, "echo"); QVERIFY(!value.contains("structuredContent"));
        QCOMPARE(value["content"].toArray().size(), 2);
        for (const auto& block : value["content"].toArray()) QCOMPARE(block.toObject()["type"].toString(), "text");
    }
    void requestDeadlinesLimitsAndClose() {
        auto o = options(); o.requestTimeoutMs = 30; o.maxMessageBytes = 1024; o.maxQueuedBytes = 1024;
        std::atomic_int closed = 0;
        o.onClosed = [&](const QString&) { ++closed; };
        o.handlers["test/wait"] = [](const auto&, const m::ServerRequestContext& c) {
            while (!c.cancellation.isCancelled()) std::this_thread::sleep_for(1ms);
            c.cancellation.throwIfCancelled(); return QJsonObject{};
        };
        o.handlers["test/large"] = [](const auto&, const auto&) { return QJsonObject{{"text", QString(2048, 'x')}}; };
        m::ServerSession s(o); initialize(s);
        s.receive(request(2, "test/wait")); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32000);
        s.receive(request(3, "test/large")); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32000);
        s.receive(request(4, "ping")); QVERIFY(next(s).contains("result"));
        auto invalid = request(5, "ping"); invalid["id"] = QJsonValue::Null;
        s.receive(invalid); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32600);
        try { s.receive(request(4, "ping")); QFAIL("Reused request ID accepted"); }
        catch (const Error& e) { QCOMPARE(int(e.code()), int(ErrorCode::ProtocolError)); }
        QVERIFY(s.isClosed()); s.close(); s.close(); QCOMPARE(closed.load(), 1);
    }
    void exportedReadHistoryDoesNotCrossConnections() {
        QTemporaryDir root; QFile file(root.filePath("existing.txt")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("original"); file.close();
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, root.path());
        a::McpServerOptions config; config.workingDirectory = root.path();
        const auto o = a::mcpServerOptions(registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass), config);
        m::ServerSession first(o), second(o); initialize(first); initialize(second);
        QVERIFY(!call(first, 2, "Read", {{"path", "existing.txt"}})["isError"].toBool());
        QVERIFY(call(second, 2, "Write", {{"path", "existing.txt"}, {"content", "other client"}})["isError"].toBool());
        QVERIFY(!call(first, 3, "Write", {{"path", "existing.txt"}, {"content", "owner"}})["isError"].toBool());
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("owner"));
    }
    void parallelReadersAndExclusiveWriter() {
        QTemporaryDir root; std::atomic_int active = 0; std::atomic_bool release = false, writer = false;
        auto registry = std::make_shared<a::ToolRegistry>();
        a::Tool read; read.definition = {"reader", "read", {{"type", "object"}}, {}, true, true};
        read.execute = [&](const auto&, const a::ToolContext& c) {
            ++active; while (!release && !c.cancellation.isCancelled()) std::this_thread::sleep_for(1ms); --active;
            c.cancellation.throwIfCancelled(); return a::ToolResult{"read"};
        }; registry->add(read);
        a::Tool write; write.definition = {"writer", "write", {{"type", "object"}}};
        write.execute = [&](const auto&, const auto&) { writer = true; return a::ToolResult{"write", {{"readers", active.load()}}}; }; registry->add(write);
        a::McpServerOptions config; config.workingDirectory = root.path();
        const auto o = a::mcpServerOptions(registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass), config);
        m::ServerSession first(o), second(o); initialize(first); initialize(second);
        first.receive(request(2, "tools/call", {{"name", "reader"}})); second.receive(request(2, "tools/call", {{"name", "reader"}}));
        QTRY_COMPARE(active.load(), 2);
        first.receive(request(3, "tools/call", {{"name", "writer"}}));
        QVERIFY(first.takeMessages(30).isEmpty()); QVERIFY(!writer.load()); release = true;
        QList<QJsonValue> messages; QElapsedTimer timer; timer.start();
        while (messages.size() < 2 && timer.elapsed() < 3000) messages += first.takeMessages(100);
        QCOMPARE(messages.size(), 2); QVERIFY(writer.load());
        for (const auto& message : messages) if (message.toObject()["id"] == 3)
            QCOMPARE(message.toObject()["result"].toObject()["structuredContent"].toObject()["readers"].toInt(), 0);
        QCOMPARE(next(second)["id"].toInt(), 2);
    }
};
QTEST_GUILESS_MAIN(McpServerTests)
#include "mcp_server_tests.moc"
