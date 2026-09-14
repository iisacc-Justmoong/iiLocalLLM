#include "agent/Engine.h"
#include "agent/CommandHooks.h"
#include "agent/Api.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <atomic>
#include <thread>
using namespace iiLocalLLM;
using namespace std::chrono_literals;
namespace a=iiLocalLLM::agent;
namespace {
class Model final:public a::Model {
public:
    std::atomic_bool waiting=false;
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken& token,const TextCallback&) override {
        if(request.messages.last().text=="wait") {
            waiting=true;while(!token.isCancelled())std::this_thread::sleep_for(1ms);token.throwIfCancelled();
        }
        return {"ANSWER",{}};
    }
};
QJsonObject config(const QString& command,const QString& matcher="*") {
    return {{"hooks",QJsonObject{{"SessionEnd",QJsonArray{QJsonObject{{"matcher",matcher},
        {"hooks",QJsonArray{QJsonObject{{"type","command"},{"command",command}}}}}}}}}};
}
struct Host {
    QTemporaryDir root;
    QString workspace=root.filePath("work");
    std::shared_ptr<Model> model=std::make_shared<Model>();
    a::EngineOptions options;
    Host(){QDir().mkpath(workspace);options.sessionsDirectory=root.filePath("sessions");options.compaction.automatic=false;}
    std::unique_ptr<a::Engine> engine(){return std::make_unique<a::Engine>(model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(),options);}
};
}
class SessionEndTests final:public QObject {
    Q_OBJECT
private slots:
    void endIsOncePerActivationAndAllowsReadOnlyReentryAndResume() {
        Host host;a::Engine* current=nullptr;QStringList sources;int ends=0;bool couldRead=false,admissionBlocked=false;
        host.options.hooks.append([&](const a::HookInput& input,const CancellationToken&) {
            a::HookResult result;
            if(input.kind==a::HookKind::SessionStart)sources.append(input.context["source"].toString());
            if(input.kind==a::HookKind::SessionEnd) {
                ++ends;couldRead=current->session(input.sessionId).messages.last().text=="ANSWER";
                admissionBlocked=current->run({input.sessionId,"cannot race end"}).result.get().errorCode==ErrorCode::ModelInUse;
                result.block=true;result.stop=true;result.feedback="END_MUST_NOT_ENTER_HISTORY";result.initialUserMessage="NO_NEW_INPUT";
            }
            return result;
        });
        auto engine=host.engine();current=engine.get();const auto id=engine->createSession("local",host.workspace).id;
        QVERIFY(!engine->endSession(id)["ended"].toBool());QCOMPARE(ends,0);
        QCOMPARE(engine->run({id,"hello"}).result.get().status,a::RunStatus::Completed);
        engine->enqueueInput(id,{{"text","PRESERVED"},{"priority","later"}});
        const auto report=engine->endSession(id,"logout");QCOMPARE(report["ended"],true);QVERIFY(couldRead&&admissionBlocked);
        QCOMPARE(engine->session(id).messages.size(),2);QCOMPARE(engine->queuedInputs(id)["count"],1);
        QCOMPARE(ends,1);QCOMPARE(report["diagnostics"].toArray().first().toObject()["outcome"],"ignored_control");
        QVERIFY(!engine->endSession(id)["ended"].toBool());QCOMPARE(ends,1);
        QCOMPARE(engine->run({id,"again"}).result.get().status,a::RunStatus::Completed);
        QCOMPARE(sources,(QStringList{"startup","resume"}));
        engine->close();QCOMPARE(ends,2);QVERIFY(engine->close().isEmpty());
        QCOMPARE(engine->run({id,"closed"}).result.get().errorCode,ErrorCode::ShuttingDown);
        QVERIFY_THROWS_EXCEPTION(Error,engine->createSession("local",host.workspace));
        QVERIFY_THROWS_EXCEPTION(Error,engine->forkSession(id));
    }
    void runningAndQueuedCancellationIsScopedAndConcurrentEndDoesNotDuplicate() {
        Host host;host.options.maxConcurrentRuns=1;std::atomic_int ends=0;
        host.options.hooks.append([&](const a::HookInput& input,const CancellationToken&) {
            if(input.kind==a::HookKind::SessionEnd){++ends;std::this_thread::sleep_for(50ms);}return a::HookResult{};
        });
        auto engine=host.engine();const auto first=engine->createSession("local",host.workspace).id;
        const auto second=engine->createSession("local",host.workspace).id;
        auto running=engine->run({first,"wait"});QTRY_VERIFY(host.model->waiting.load());
        auto queued=engine->run({second,"queued"});
        const auto unstarted=engine->endSession(second);QVERIFY(!unstarted["ended"].toBool());
        QCOMPARE(running.result.wait_for(0ms),std::future_status::timeout);
        running.cancel();QCOMPARE(running.result.get().status,a::RunStatus::Cancelled);
        QCOMPARE(queued.result.get().status,a::RunStatus::Cancelled);
        auto one=std::async(std::launch::async,[&]{return engine->endSession(first,"clear");});
        auto two=std::async(std::launch::async,[&]{return engine->endSession(first,"other");});
        const auto a=one.get(),b=two.get();QCOMPARE(int(a["ended"].toBool())+int(b["ended"].toBool()),1);QCOMPARE(ends.load(),1);
        engine->close();QCOMPARE(ends.load(),1);
    }
    void activeRunEndsBeforeTheHookAndOtherSessionsRemainUsable() {
        Host host;int ends=0;host.options.hooks.append([&](const a::HookInput& input,const CancellationToken& token){
            if(input.kind==a::HookKind::SessionEnd){token.throwIfCancelled();++ends;}return a::HookResult{};});
        auto engine=host.engine();const auto id=engine->createSession("local",host.workspace).id;
        auto run=engine->run({id,"wait"});QTRY_VERIFY(host.model->waiting.load());
        const auto result=engine->endSession(id,"resume");QCOMPARE(result["ended"],true);
        QCOMPARE(run.result.get().status,a::RunStatus::Cancelled);QCOMPARE(ends,1);
        const auto other=engine->createSession("local",host.workspace).id;
        QCOMPARE(engine->run({other,"unaffected"}).result.get().status,a::RunStatus::Completed);engine->close();
    }
    void exceptionsAndTimeoutCannotVetoEndAndChildEnginesDoNotEmitMainEnd() {
        Host host;host.options.sessionEndTimeoutMs=40;int afterThrow=0;
        host.options.hooks.append([](const a::HookInput& input,const CancellationToken&){
            if(input.kind==a::HookKind::SessionEnd)throw std::runtime_error("EXPECTED_END_FAILURE");return a::HookResult{};});
        host.options.hooks.append([&](const a::HookInput& input,const CancellationToken& token){
            if(input.kind==a::HookKind::SessionEnd){++afterThrow;while(!token.isCancelled())std::this_thread::sleep_for(1ms);token.throwIfCancelled();}
            return a::HookResult{};});
        auto engine=host.engine();const auto id=engine->createSession("local",host.workspace).id;
        engine->run({id,"hello"}).result.get();const auto begin=std::chrono::steady_clock::now();
        auto result=engine->endSession(id);QVERIFY(std::chrono::steady_clock::now()-begin<1s);QCOMPARE(result["timed_out"],true);
        QVERIFY(QJsonDocument(result).toJson().contains("EXPECTED_END_FAILURE"));QCOMPARE(afterThrow,1);
        engine->close();host.options.sessionStartHooks=false;auto child=host.engine();
        child->run({id,"child"}).result.get();child->close();QCOMPARE(afterThrow,1);
    }
    void commandReasonMatchersAndCommonBudgetStopQueuedProcesses() {
        Host host;host.options.sessionEndTimeoutMs=180;a::CommandHookOptions options;options.workingDirectory=host.workspace;options.maxConcurrentProcesses=1;
        auto settings=config("cat > matched.json; sleep 5; touch TOO_LATE","logout|clear");
        auto groups=settings["hooks"].toObject()["SessionEnd"].toArray();
        groups.append(QJsonObject{{"hooks",QJsonArray{QJsonObject{{"type","command"},{"command","sleep 5; touch ALSO_TOO_LATE"}}}}});
        settings={{"hooks",QJsonObject{{"SessionEnd",groups}}}};a::CommandHooks hooks(settings,options);host.options.hooks={hooks.callback()};
        auto engine=host.engine();const auto id=engine->createSession("local",host.workspace).id;engine->run({id,"hello"}).result.get();
        const auto begin=std::chrono::steady_clock::now();const auto report=engine->endSession(id,"logout");
        QVERIFY(std::chrono::steady_clock::now()-begin<2s);QVERIFY(report["timed_out"].toBool());
        QVERIFY(!QFileInfo::exists(host.workspace+"/TOO_LATE"));QVERIFY(!QFileInfo::exists(host.workspace+"/ALSO_TOO_LATE"));
        // Matcher-only invocation is deterministic, independent of pool order.
        a::CommandHooks matcher(config("cat > reason.json","logout|clear"),options);
        a::HookInput input{a::HookKind::SessionEnd,id,{}, {},{},{},{{"reason","other"}}};
        QVERIFY(matcher.callback()(input,{}).diagnostics.isEmpty());input.context["reason"]="clear";matcher.callback()(input,{});
        QFile f(host.workspace+"/reason.json");QVERIFY(f.open(QIODevice::ReadOnly));QCOMPARE(QJsonDocument::fromJson(f.readAll()).object()["reason"],"clear");
    }
    void invalidOrPreCancelledRequestsLeaveTheSessionOpen() {
        Host host;auto engine=host.engine();const auto id=engine->createSession("local",host.workspace).id;
        engine->run({id,"hello"}).result.get();QVERIFY_THROWS_EXCEPTION(Error,engine->endSession(id,"invented"));
        CancellationToken cancelled;cancelled.cancel();QVERIFY_THROWS_EXCEPTION(Error,engine->endSession(id,"other",cancelled));
        QCOMPARE(engine->endSession(id)["ended"],true);
        host.options.sessionEndTimeoutMs=0;QVERIFY_THROWS_EXCEPTION(Error,host.engine());
    }
    void destructorRunsARealCommandAfterTheTranscriptIsStored() {
        Host host;a::CommandHookOptions options;options.workingDirectory=host.workspace;
        std::unique_ptr<a::CommandHooks> hooks;
        try {hooks=std::make_unique<a::CommandHooks>(config("cat >> end.jsonl; printf 'END_ERROR' >&2; exit 2"),options);}
        catch(const Error& e){QFAIL(e.what());}
        host.options.hooks.append(hooks->callback());QString id;
        {auto engine=host.engine();id=engine->createSession("model://test",host.workspace).id;
            QCOMPARE(engine->run({id,"hello"}).result.get().status,a::RunStatus::Completed);
            QVERIFY(!QFileInfo::exists(host.workspace+"/end.jsonl"));}
        QFile file(host.workspace+"/end.jsonl");QVERIFY(file.open(QIODevice::ReadOnly));
        const auto body=QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(body["hook_event_name"],"SessionEnd");QCOMPARE(body["reason"],"other");QCOMPARE(body["session_id"],id);
        QFile transcript(body["transcript_path"].toString());QVERIFY(transcript.open(QIODevice::ReadOnly));
        QVERIFY(transcript.readAll().contains("ANSWER"));
    }
    void apiExposesAnAuthenticatedSessionEnd() {
        Host host;a::ApiOptions options;options.workingDirectory=host.workspace;options.stateDirectory=host.root.filePath("private");
        const QString token(48,'a');options.clientTokens={{"client",token}};
        a::Api api(host.model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(),options);
        auto call=[&](const QString& method,const QJsonObject& params){return api.dispatch(method,params,token).result.get().toObject();};
        const auto id=call("agent.sessions.create",{{"model","model://test"}}).value("session_id");
        QCOMPARE(call("agent.run",{{"session_id",id},{"prompt","hello"}})["status"],"completed");
        QJsonObject ended;
        try {ended=call("agent.sessions.end",{{"session_id",id},{"reason","logout"}});}
        catch(const Error& e){QFAIL(e.what());}
        QCOMPARE(ended["ended"],true);QCOMPARE(ended["reason"],"logout");
    }
    void apiEndCancelsRunningAndQueuedRequestsBeforeResumption() {
        Host host;a::ApiOptions options;options.workingDirectory=host.workspace;options.stateDirectory=host.root.filePath("private");
        options.maxConcurrentRequests=1;const QString token(48,'a'),other(48,'b');options.clientTokens={{"client",token},{"other",other}};
        QStringList sources;int ends=0;options.engine.hooks.append([&](const a::HookInput& input,const CancellationToken&){
            if(input.kind==a::HookKind::SessionStart)sources.append(input.context["source"].toString());
            if(input.kind==a::HookKind::SessionEnd)++ends;return a::HookResult{};});
        a::Api api(host.model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(),options);
        auto call=[&](const QString& method,const QJsonObject& params){return api.dispatch(method,params,token).result.get().toObject();};
        const auto id=call("agent.sessions.create",{{"model","local"}}).value("session_id");
        QVERIFY_THROWS_EXCEPTION(Error,api.dispatch("agent.sessions.end",{{"session_id",id}},other).result.get());
        auto run=api.dispatch("agent.run",{{"session_id",id},{"prompt","wait"}},token);QTRY_VERIFY(host.model->waiting.load());
        auto queued=api.dispatch("agent.run",{{"session_id",id},{"prompt","must not resume"}},token);
        QCOMPARE(call("agent.sessions.end",{{"session_id",id},{"reason","logout"}})["ended"],true);
        QCOMPARE(run.result.get().toObject()["status"],"cancelled");
        QVERIFY_THROWS_EXCEPTION(Error,queued.result.get());QCOMPARE(ends,1);QCOMPARE(sources,(QStringList{"startup"}));
        QCOMPARE(call("agent.run",{{"session_id",id},{"prompt","explicit resume"}})["status"],"completed");
        QCOMPARE(sources,(QStringList{"startup","resume"}));api.close();QCOMPARE(ends,2);api.close();QCOMPARE(ends,2);
    }
    void nativeOperationsAreCancelledWithoutCancellingTheirCaller() {
        Host host;host.options.taskToolsEnabled=true;std::atomic_bool waiting=false;
        host.options.hooks.append([&](const a::HookInput& input,const CancellationToken& token){
            if(input.kind==a::HookKind::TaskCreated){waiting=true;while(!token.isCancelled())std::this_thread::sleep_for(1ms);token.throwIfCancelled();}
            return a::HookResult{};});
        auto engine=host.engine();const auto id=engine->createSession("local",host.workspace).id;CancellationToken caller;
        auto native=std::async(std::launch::async,[&]{return engine->runTaskTool(id,"TaskCreate",{{"subject","must roll back"},{"description","cancel in hook"}},caller);});
        QTRY_VERIFY(waiting.load());const auto result=engine->endSession(id);QCOMPARE(result["ended"],false);
        QVERIFY_THROWS_EXCEPTION(Error,native.get());QVERIFY(!caller.isCancelled());
        QVERIFY(engine->runTaskTool(id,"TaskList").data["tasks"].toArray().isEmpty());engine->close();
    }
};
QTEST_GUILESS_MAIN(SessionEndTests)
#include "session_end_tests.moc"
