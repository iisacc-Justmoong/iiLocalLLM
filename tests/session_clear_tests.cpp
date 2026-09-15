#include "agent/Engine.h"
#include "agent/Api.h"
#include "agent/ShellTasks.h"
#include "agent/Subagents.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <atomic>
#include <thread>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
using namespace std::chrono_literals;
namespace {
class Model final:public a::Model {
public:
    std::atomic_bool foreground=false,child=false,releaseChild=false;
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken& token,const TextCallback&) override {
        const auto prompt=request.messages.last().text;
        if(prompt=="foreground") {foreground=true;while(!token.isCancelled())std::this_thread::sleep_for(1ms);token.throwIfCancelled();}
        if(prompt=="child") {child=true;while(!releaseChild&&!token.isCancelled())std::this_thread::sleep_for(1ms);token.throwIfCancelled();}
        if(prompt=="fresh")for(const auto& message:request.messages)if(message.text.contains("OLD_SECRET"))throw std::runtime_error("Old context leaked after clear");
        return {"ANSWER"};
    }
};
struct Host {
    QTemporaryDir root;QString workspace=root.filePath("work");
    std::shared_ptr<Model> model=std::make_shared<Model>();
    std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::RulePolicy> policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    a::EngineOptions options;
    Host(){QDir().mkpath(workspace);options.sessionsDirectory=root.filePath("sessions");options.compaction.automatic=false;options.taskToolsEnabled=true;}
};
}
class SessionClearTests final:public QObject {
    Q_OBJECT
private slots:
    void failuresReturnTheNewIdentityAndCloseWaitsForAnAdmittedClear() {
        Host h;std::atomic_bool entering=false,release=false;bool failStart=true;
        h.options.hooks.append([&](const a::HookInput& input,const CancellationToken&) {
            if(input.kind==a::HookKind::SessionStart&&input.context["source"]=="clear"&&failStart)throw std::runtime_error("EXPECTED_START_FAILURE");
            return a::HookResult{};
        });
        a::Engine engine(h.model,h.registry,h.policy,h.options);const auto old=engine.createSession("fixture",h.workspace).id;
        CancellationToken cancelled;cancelled.cancel();QVERIFY_THROWS_EXCEPTION(Error,engine.clearSession(old,cancelled));QCOMPARE(engine.sessions().size(),1);
        const auto failed=engine.clearSession(old);QVERIFY(!failed["complete"].toBool());const auto next=failed["session_id"].toString();QVERIFY(!next.isEmpty());
        QVERIFY(QJsonDocument(failed).toJson().contains("EXPECTED_START_FAILURE"));failStart=false;
        QCOMPARE(engine.run({next,"recover"}).result.get().status,a::RunStatus::Completed);
        a::Tool barrier;barrier.definition={"owner","Trusted transfer",{{"type","object"}}};barrier.execute=[](const auto&,const auto&){return a::ToolResult{};};
        barrier.transferSession=[&](const auto&,const auto&,const auto&){entering=true;const auto until=std::chrono::steady_clock::now()+2s;
            while(!release&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(1ms);return QJsonArray{};};h.registry->add(barrier);
        auto clear=std::async(std::launch::async,[&]{return engine.clearSession(next);});QTRY_VERIFY(entering.load());
        auto close=std::async(std::launch::async,[&]{return engine.close();});QCOMPARE(close.wait_for(30ms),std::future_status::timeout);
        release=true;const auto completed=clear.get();QVERIFY(completed["complete"].toBool());QVERIFY(!close.get().isEmpty());
        QCOMPARE(engine.run({completed["session_id"].toString(),"closed"}).result.get().errorCode,ErrorCode::ShuttingDown);
    }
    void clearStartsANewConversationImmediatelyAndRetainsOldHistory() {
        Host h;QStringList lifecycle;h.options.hooks.append([&](const a::HookInput& input,const CancellationToken&) {
            a::HookResult result;
            if(input.kind==a::HookKind::SessionStart) {
                const auto source=input.context["source"].toString();lifecycle.append("start:"+source);
                if(source=="clear"){result.feedback="CLEAR_CONTEXT";result.initialUserMessage="INITIAL_AFTER_CLEAR";}
            }
            if(input.kind==a::HookKind::SessionEnd)lifecycle.append("end:"+input.context["reason"].toString());
            return result;
        });
        a::Engine engine(h.model,h.registry,h.policy,h.options);
        const auto old=engine.createSession("fixture",h.workspace,"HOST_SYSTEM").id;
        QCOMPARE(engine.run({old,"OLD_SECRET"}).result.get().status,a::RunStatus::Completed);
        engine.enqueueInput(old,{{"text","OLD_PENDING"}});
        engine.runTaskTool(old,"TaskCreate",{{"subject","old task"},{"description","retained only in old session"}});
        const auto report=engine.clearSession(old);const auto next=report["session_id"].toString();
        QVERIFY2(report["complete"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(report).toJson())));QVERIFY(next!=old&&!next.isEmpty());
        QCOMPARE(report["previous_session_id"],old);QCOMPARE(lifecycle,(QStringList{"start:startup","end:clear","start:clear"}));
        const auto session=engine.session(next);QCOMPARE(session.parentSessionId,old);QCOMPARE(session.systemPrompt,"HOST_SYSTEM");
        QCOMPARE(session.messages.size(),1);QCOMPARE(session.messages.first().text,"CLEAR_CONTEXT");QVERIFY(session.compactions.isEmpty());
        QCOMPARE(engine.session(old).messages.size(),2);QCOMPARE(engine.queuedInputs(old)["count"],1);
        QCOMPARE(engine.queuedInputs(next)["inputs"].toArray().first().toObject()["text"],"INITIAL_AFTER_CLEAR");
        QVERIFY(engine.runTaskTool(next,"TaskList").data["tasks"].toArray().isEmpty());
        QCOMPARE(engine.run({next,"fresh"}).result.get().status,a::RunStatus::Completed);QCOMPARE(lifecycle.size(),3);
        QCOMPARE(a::SessionStore(h.options.sessionsDirectory).metadata(next).parentSessionId,old);
        engine.close();QCOMPARE(lifecycle.last(),"end:other");
    }
    void clearCancelsForegroundButKeepsBackgroundChildrenAndShells() {
#if !defined(Q_OS_MACOS) && !defined(Q_OS_LINUX)
        QSKIP("Requires a desktop POSIX shell");
#endif
        Host h;auto shells=std::make_shared<a::ShellTasks>(h.workspace,h.root.filePath("shells"));a::registerShellTaskControls(*h.registry,shells);
        a::registerWorkspaceTools(*h.registry,h.workspace);
        a::SubagentOptions children;children.workingDirectory=h.workspace;children.stateDirectory=h.root.filePath("children");
        auto agents=std::make_shared<a::Subagents>(h.model,h.registry,h.policy,h.options,children);a::Subagents::attach(h.options,agents);
        a::Engine engine(h.model,h.registry,h.policy,h.options);const auto old=engine.createSession("fixture",h.workspace).id;
        const auto child=engine.runSubagentTool(old,"Agent",{{"prompt","child"},{"run_in_background",true}}).data["agentId"].toString();
        QTRY_VERIFY(h.model->child.load());
        const auto shellId=shells->start({old,{},h.workspace},"while [ ! -f release ]; do sleep .02; done; printf SHELL_FINISHED")["task_id"].toString();
        auto run=engine.run({old,"foreground"});QTRY_VERIFY(h.model->foreground.load());
        const auto report=engine.clearSession(old);QVERIFY(report["complete"].toBool());const auto next=report["session_id"].toString();
        QCOMPARE(run.result.get().status,a::RunStatus::Cancelled);QCOMPARE(agents->list(next).size(),1);QVERIFY(agents->list(old).isEmpty());
        QVERIFY_THROWS_EXCEPTION(Error,agents->output(old,child));QVERIFY_THROWS_EXCEPTION(Error,shells->output(old,shellId));
        h.model->releaseChild=true;QCOMPARE(agents->output(next,child,true)["status"],"completed");
        QFile release(h.workspace+"/release");QVERIFY(release.open(QIODevice::WriteOnly));release.close();
        QCOMPARE(shells->output(next,shellId,true,3000)["task"].toObject()["output"],"SHELL_FINISHED");
        QCOMPARE(engine.queuedInputs(next)["count"],1);QCOMPARE(engine.queuedInputs(old)["count"],0);engine.close();agents->close();
    }
    void apiClearIsScopedAndChecksSessionCapacityBeforeEnding() {
        Host h;a::ApiOptions options;options.workingDirectory=h.workspace;options.stateDirectory=h.root.filePath("private");options.maxSessionsPerClient=2;
        const QString token(48,'a'),other(48,'b');options.clientTokens={{"client",token},{"other",other}};
        a::Api api(h.model,h.registry,h.policy,options);
        auto call=[&](const QString& method,const QJsonObject& params){return api.dispatch(method,params,token).result.get().toObject();};
        const auto id=call("agent.sessions.create",{{"model","fixture"}}).value("session_id");
        QVERIFY_THROWS_EXCEPTION(Error,api.dispatch("agent.sessions.clear",{{"session_id",id}},other).result.get());
        const auto clear=call("agent.sessions.clear",{{"session_id",id}});QVERIFY(clear["complete"].toBool());QVERIFY(clear["session_id"]!=id);
        QCOMPARE(call("agent.sessions.get",{{"session_id",clear["session_id"]}})["parent_session_id"],id);
        QVERIFY_THROWS_EXCEPTION(Error,call("agent.sessions.clear",{{"session_id",clear["session_id"]}}));
        QCOMPARE(call("agent.run",{{"session_id",clear["session_id"]},{"prompt","still active"}})["status"],"completed");
    }
};
QTEST_GUILESS_MAIN(SessionClearTests)
#include "session_clear_tests.moc"
