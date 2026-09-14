#include "agent/Subagents.h"
#include "agent/ShellTasks.h"
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QDir>
#include <QtTest/QtTest>
#include <atomic>
#include <chrono>
#include <thread>
using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
using namespace std::chrono_literals;
namespace {
class Model final : public a::Model {
public:
    std::function<a::ModelReply(const a::ModelRequest&, const CancellationToken&)> next;
    a::ModelReply generate(const a::ModelRequest& r, const CancellationToken& t, const TextCallback&) override { return next(r,t); }
};
struct Host {
    QTemporaryDir root;
    QString workspace = root.filePath("workspace");
    std::shared_ptr<Model> model = std::make_shared<Model>();
    std::shared_ptr<a::ToolRegistry> registry = std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::PermissionPolicy> policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    a::EngineOptions engineOptions;
    a::SubagentOptions options;
    Host() {
        QDir().mkpath(workspace); engineOptions.sessionsDirectory = root.filePath("parent-sessions");
        engineOptions.compaction.automatic = false; engineOptions.projectContext.enabled = false;
        options.workingDirectory = workspace; options.stateDirectory = root.filePath("children");
    }
    a::Session parent() { return a::SessionStore(engineOptions.sessionsDirectory).create("local", "parent system", workspace); }
    a::ToolContext context(const a::Session& s) {
        a::ToolContext c{s.id,"parent-run",workspace,{}, {}}; c.sessionSnapshot = std::make_shared<a::Session>(s); return c;
    }
    std::shared_ptr<a::Subagents> start() { return std::make_shared<a::Subagents>(model,registry,policy,engineOptions,options); }
};
}
class SubagentTests final : public QObject {
    Q_OBJECT
private slots:
    void childSearchCatalogCannotExposeOrActivateDeniedTools() {
        Host h;h.engineOptions.taskToolsEnabled=true;int invoked=0;
        a::Tool tool;tool.definition={"mcp.fixture.read","Read delegated artifact",{{"type","object"}}, {},true,true,false,true};
        tool.execute=[&](const auto&,const auto&){++invoked;return a::ToolResult{"SCOPED_MCP"};};h.registry->add(tool);
        a::SubagentDefinition profile;profile.name="reader";profile.description="reader";profile.tools={"ToolSearch","mcp.fixture.read"};h.options.definitions={profile};
        h.model->next=[](const auto& r,const auto&){
            for(const auto& t:r.tools) if(t.name.startsWith("Task"))throw std::runtime_error("generated native task escaped scope");
            if(r.messages.last().role!=a::MessageRole::Tool)return a::ModelReply{{},{{"search","ToolSearch",{{"query","select:mcp.fixture.read,TaskCreate"}}}}};
            const auto& observation=r.messages.last();
            if(observation.toolCallId=="search") {
                for(const auto& t:observation.data["tools"].toArray()) if(t.toObject()["name"]!="mcp.fixture.read")throw std::runtime_error("scope leaked through search catalog");
                return a::ModelReply{{},{{"invoke","mcp.fixture.read",{}}}};
            }
            return a::ModelReply{observation.text,{}};
        };
        auto agents=h.start();auto result=agents->run(h.context(h.parent()),{{"prompt","find delegated tool"},{"subagent_type","reader"}});
        QVERIFY(!result.isError);QCOMPARE(invoked,1);QCOMPARE(result.data["result"].toObject()["text"],"SCOPED_MCP");
    }
    void deadlineAndAbruptHostRecordRecovery() {
        Host h;h.options.maxRuntimeMs=40;
        h.model->next=[](const auto&,const auto& token)->a::ModelReply{while(true){token.throwIfCancelled();std::this_thread::sleep_for(2ms);}};
        const auto p=h.parent();QString id;
        {
            auto agents=h.start();const auto result=agents->run(h.context(p),{{"prompt","wait"}});
            QVERIFY(result.isError);QCOMPARE(result.data["status"],"failed");QCOMPARE(result.data["result"].toObject()["error_code"],"timeout");
            id=result.data["agentId"].toString();
        }
        // The ownership lock is released. Simulate the durable record left by
        // a killed host; reopening must mark uncertainty and never rerun it.
        QFile file(QDir(h.options.stateDirectory).filePath(id+".json"));QVERIFY(file.open(QIODevice::ReadOnly));
        auto state=QJsonDocument::fromJson(file.readAll()).object();file.close();state["status"]="running";state.remove("result");
        QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write(QJsonDocument(state).toJson());file.close();
        std::atomic_int calls=0;h.model->next=[&](const auto&,const auto&){++calls;return a::ModelReply{"unexpected",{}};};
        auto recovered=h.start();QCOMPARE(recovered->output(p.id,id)["status"],"interrupted");QCOMPARE(calls.load(),0);
    }
    void resumedProfileCannotWidenItsOriginalTools() {
        Host h;int writes=0;
        h.registry->add({{"Write","write",{{"type","object"}}},[&](const auto&,const auto&){++writes;return a::ToolResult{"bad"};}});
        a::SubagentDefinition profile;profile.name="worker";profile.description="worker";profile.tools={"Read"};h.options.definitions={profile};
        const auto p=h.parent();QString id;h.model->next=[](const auto&,const auto&){return a::ModelReply{"initial",{}};};
        {auto agents=h.start();id=agents->run(h.context(p),{{"prompt","initial"},{"subagent_type","worker"}}).data["agentId"].toString();}
        h.options.definitions[0].tools={"Write"};
        h.model->next=[](const auto& r,const auto&){
            if(!r.tools.isEmpty())throw std::runtime_error("resume widened tools");
            if(r.messages.last().role==a::MessageRole::Tool)return a::ModelReply{"denied",{}};
            return a::ModelReply{{},{{"write","Write",{}}}};
        };
        auto agents=h.start();auto result=agents->run(h.context(p),{{"resume",id},{"prompt","attempt write"}});
        QVERIFY(!result.isError);QCOMPARE(writes,0);
    }
    void childBackgroundShellsReachTerminalState() {
#if !defined(Q_OS_UNIX) || defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
        QSKIP("Background shell execution requires a desktop POSIX host");
#endif
        Host h;auto shells=std::make_shared<a::ShellTasks>(h.workspace,h.root.filePath("shells"));
        a::registerWorkspaceTools(*h.registry,h.workspace,shells);
        h.model->next=[](const auto& r,const auto&){
            if(r.messages.last().role==a::MessageRole::Tool)return a::ModelReply{"done",{}};
            return a::ModelReply{{},{{"shell","Bash",{{"command","sleep 30"},{"run_in_background",true}}}}};
        };
        const auto p=h.parent();auto agents=h.start();auto result=agents->run(h.context(p),{{"prompt","start background work"}});
        QVERIFY(!result.isError);const auto tasks=shells->list(result.data["session_id"].toString());QCOMPARE(tasks.size(),1);
        const auto task=tasks.at(0).toObject();QVERIFY(task["status"]!="pending" && task["status"]!="running");
        QVERIFY(shells->list(p.id).isEmpty());
    }
    void childStorageRejectsSymlinksAndInvalidRequestsLeaveNoRecord() {
        Host h;QDir().mkpath(h.options.stateDirectory);
        QVERIFY(QFile::link(h.workspace,QDir(h.options.stateDirectory).filePath("sessions")));
        QVERIFY_THROWS_EXCEPTION(Error,h.start());
        QVERIFY(QFile::remove(QDir(h.options.stateDirectory).filePath("sessions")));
        auto agents=h.start();const auto p=h.parent();auto c=h.context(p);
        QVERIFY_THROWS_EXCEPTION(Error,agents->run(c,{{"prompt","do work"},{"unknown",true}}));
        QVERIFY_THROWS_EXCEPTION(Error,agents->run(c,{{"prompt","do work"},{"model","forbidden"}}));
        QVERIFY_THROWS_EXCEPTION(Error,agents->run(c,{{"prompt","do work"},{"max_turns",0}}));
        QVERIFY_THROWS_EXCEPTION(Error,agents->run(c,{{"prompt","do work"},{"max_turns",h.options.maxTurns+1}}));
        QVERIFY(agents->list(p.id).isEmpty());
        QVERIFY(a::SessionStore(QDir(h.options.stateDirectory).filePath("sessions")).list().isEmpty());
    }
    void expandedForkRequestIsValidatedBeforeSavingChild() {
        Host h;h.engineOptions.maxInputCharacters=64;
        auto agents=h.start();const auto p=h.parent();
        QVERIFY_THROWS_EXCEPTION(Error,agents->run(h.context(p),{{"prompt","short"},{"fork_context",true}}));
        QVERIFY(agents->list(p.id).isEmpty());
        QVERIFY(a::SessionStore(QDir(h.options.stateDirectory).filePath("sessions")).list().isEmpty());
    }
    void failedResumeCommitPreservesPreviousOutcome() {
        Host h;h.model->next=[](const auto&,const auto&){return a::ModelReply{"first outcome",{}};};
        auto agents=h.start();const auto p=h.parent();const auto c=h.context(p);
        const auto first=agents->run(c,{{"prompt","first"}});QVERIFY(!first.isError);
        const auto id=first.data["agentId"].toString();const auto before=agents->output(p.id,id);
        const auto path=QDir(h.options.stateDirectory).filePath(id+".json"),backup=path+".saved";
        QVERIFY(QFile::rename(path,backup));QVERIFY(QFile::link(backup,path));
        QVERIFY_THROWS_EXCEPTION(Error,agents->run(c,{{"prompt","resume"},{"resume",id}}));
        QCOMPARE(agents->output(p.id,id),before);
        QVERIFY(QFile::remove(path));QVERIFY(QFile::rename(backup,path));
        QCOMPARE(a::SessionStore(QDir(h.options.stateDirectory).filePath("sessions")).list().size(),1);
    }
    void shellCompletingBetweenListAndStopDoesNotFailChild() {
        Host h;int lists=0;bool modelFinished=false;
        a::Tool list;list.definition.name="ShellTaskList";list.definition.readOnly=true;list.definition.metadata={{"source","builtin.shell.control"}};
        list.execute=[&](const auto&,const auto&){
            if(!modelFinished)return a::ToolResult{{},{{"tasks",QJsonArray{}}}};
            return a::ToolResult{{},{{"tasks",QJsonArray{QJsonObject{{"task_id","finished-shell"},{"status",++lists==1?"running":"completed"}}}}}};
        };
        a::Tool stop;stop.definition.name="TaskStop";stop.definition.metadata={{"source","builtin.shell.control"}};
        stop.execute=[](const auto&,const auto&)->a::ToolResult{throw Error(ErrorCode::InvalidArgument,"Shell task is not running");};
        h.registry->add(list);h.registry->add(stop);
        h.model->next=[&](const auto&,const auto&){modelFinished=true;return a::ModelReply{"done",{}};};
        auto agents=h.start();const auto result=agents->run(h.context(h.parent()),{{"prompt","finish"}});
        QVERIFY(!result.isError);QCOMPARE(result.data["status"],"completed");QCOMPARE(lists,2);
    }
    void modelDelegationTraversesParentAndChildEngines() {
        Host h;h.registry->add({{"Read","read",{{"type","object"}}, {}, true},[](const auto&,const auto&){return a::ToolResult{"CHILD_OBSERVATION"};}});
        auto agents=h.start();auto eo=h.engineOptions;eo.additionalTools=a::Subagents::tools(agents);
        a::Engine engine(h.model,h.registry,h.policy,eo);auto parent=engine.createSession("local",h.workspace);
        h.model->next=[parent](const auto& r,const auto&){
            if(r.contextId==parent.id) {
                if(r.messages.last().role==a::MessageRole::Tool) return a::ModelReply{r.messages.last().data["result"].toObject()["text"].toString(),{}};
                return a::ModelReply{{},{{"delegate","Agent",{{"prompt","read file"}}}}};
            }
            for(const auto& t:r.tools) if(t.name=="Agent")throw std::runtime_error("recursive Agent exposed");
            if(r.messages.last().role==a::MessageRole::Tool)return a::ModelReply{r.messages.last().text,{}};
            return a::ModelReply{{},{{"read","Read",{}}}};
        };
        const auto result=engine.run({parent.id,"delegate"}).result.get();
        QCOMPARE(result.status,a::RunStatus::Completed);QCOMPARE(result.text,"CHILD_OBSERVATION");
        const auto saved=engine.session(parent.id);QCOMPARE(saved.messages.size(),4);QVERIFY(a::pendingToolCalls(saved.messages).isEmpty());
        QCOMPARE(agents->list(parent.id).size(),1);
    }
    void foregroundScopeAndResume() {
        Host h; int writes=0;
        h.registry->add({{"Read", "read", {{"type","object"}}, {}, true}, [](const auto&,const auto&){return a::ToolResult{"observed"};}});
        h.registry->add({{"Write", "write", {{"type","object"}}}, [&](const auto&,const auto&){++writes;return a::ToolResult{"wrote"};}});
        a::SubagentDefinition profile; profile.name="reader"; profile.description="Read only"; profile.systemPrompt="specialist"; profile.tools={"Read"};
        h.options.definitions={profile}; auto parent=h.parent(); auto c=h.context(parent);
        h.model->next=[](const auto& r,const auto&){
            for(const auto& tool:r.tools) if(tool.name!="Read") throw std::runtime_error("leaked child tool");
            if(r.systemPrompt!="specialist") throw std::runtime_error("profile prompt missing");
            if(r.messages.last().role==a::MessageRole::Tool) return a::ModelReply{r.messages.last().text,{}};
            return a::ModelReply{{},{{"read-id","Read",{}}}};
        };
        QString id;
        {
            auto agents=h.start(); auto result=agents->run(c,{{"prompt","read"},{"description","inspect"},{"subagent_type","reader"}});
            QVERIFY(!result.isError); QCOMPARE(result.data["status"],"completed"); QCOMPARE(result.data["result"].toObject()["text"],"observed");
            id=result.data["agentId"].toString(); QVERIFY(!id.isEmpty()); QCOMPARE(writes,0);
            QVERIFY(a::SessionStore(h.engineOptions.sessionsDirectory).load(parent.id).messages.isEmpty());
        }
        auto resumed=h.start(); h.model->next=[](const auto& r,const auto&){
            if(r.messages.size()<4) throw std::runtime_error("child history lost");
            return a::ModelReply{"resumed",{}};
        };
        auto result=resumed->run(c,{{"prompt","continue"},{"resume",id}});
        QVERIFY(!result.isError); QCOMPARE(result.data["agentId"],id); QCOMPARE(result.data["result"].toObject()["text"],"resumed");
        QVERIFY_THROWS_EXCEPTION(Error,resumed->output(h.parent().id,id));
    }
    void deniedToolAndCancelledForeground() {
        Host h; std::atomic_int writes=0;
        h.registry->add({{"Write","write",{{"type","object"}}},[&](const auto&,const auto&){++writes;return a::ToolResult{"bad"};}});
        a::SubagentDefinition profile; profile.name="reader"; profile.description="reader"; profile.tools={"Read"}; h.options.definitions={profile};
        h.model->next=[](const auto& r,const auto&){ if(r.messages.last().role==a::MessageRole::Tool) return a::ModelReply{"denial observed",{}};return a::ModelReply{{},{{"write-id","Write",{}}}};};
        auto agents=h.start(); auto c=h.context(h.parent());
        auto result=agents->run(c,{{"prompt","attempt"},{"subagent_type","reader"}}); QVERIFY(!result.isError); QCOMPARE(writes.load(),0);
        h.model->next=[](const auto&,const auto& token)->a::ModelReply{while(true){token.throwIfCancelled();std::this_thread::sleep_for(2ms);}};
        auto pending=std::async(std::launch::async,[&]{return agents->run(c,{{"prompt","wait"},{"subagent_type","reader"}});});
        QTest::qWait(20); c.cancellation.cancel(); QVERIFY(pending.wait_for(2s)==std::future_status::ready);
        QCOMPARE(pending.get().data["status"],"cancelled");
    }
    void backgroundCapacityStopNotificationAndRestart() {
        Host h; h.options.maxConcurrent=1;
        h.model->next=[](const auto&,const auto& token)->a::ModelReply{while(true){token.throwIfCancelled();std::this_thread::sleep_for(2ms);}};
        auto p=h.parent(); auto c=h.context(p); QString id;
        {
            auto agents=h.start(); auto result=agents->run(c,{{"prompt","wait"},{"run_in_background",true}});
            QVERIFY(!result.isError); QCOMPARE(result.data["status"],"async_launched"); id=result.data["agentId"].toString();
            c.cancellation.cancel(); auto state=agents->output(p.id,id,false); QVERIFY(state["status"]=="running" || state["status"]=="queued");
            auto fresh=h.context(p); QVERIFY_THROWS_EXCEPTION(Error,agents->run(fresh,{{"prompt","overflow"},{"run_in_background",true}}));
            agents->stop(p.id,id); QCOMPARE(agents->output(p.id,id,true,2000)["status"],"cancelled");
            const auto queued=a::InputQueue(QDir(h.engineOptions.sessionsDirectory).filePath("inputs")).snapshot(p.id);
            QCOMPARE(queued["inputs"].toArray().size(),1); QVERIFY(queued["inputs"].toArray()[0].toObject()["text"].toString().contains(id));
        }
        auto again=h.start(); QCOMPARE(again->output(p.id,id)["status"],"cancelled");
    }
    void forkContextUsesIndependentPairedSnapshot() {
        Host h; auto p=h.parent();
        p.messages={ {"user",a::MessageRole::User,"parent evidence"}, {"assistant",a::MessageRole::Assistant,{},{{"pending","Agent",{}}}} };
        h.model->next=[](const auto& r,const auto&){
            if(!a::pendingToolCalls(r.messages).isEmpty()) throw std::runtime_error("unpaired parent calls");
            if(r.messages.first().text!="parent evidence" || r.systemPrompt!="parent system") throw std::runtime_error("fork missing context");
            return a::ModelReply{"forked",{}};
        };
        auto agents=h.start(); auto result=agents->run(h.context(p),{{"prompt","use parent"},{"fork_context",true}});
        QVERIFY(!result.isError); QCOMPARE(p.messages.size(),2); QCOMPARE(a::pendingToolCalls(p.messages).size(),1);
    }
};
QTEST_GUILESS_MAIN(SubagentTests)
#include "subagent_tests.moc"
