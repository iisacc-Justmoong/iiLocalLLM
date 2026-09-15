#include <agent/CommandHooks.h>
#include <agent/Engine.h>
#include <agent/Api.h>
#include <agent/McpServer.h>
#include <agent/Subagents.h>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <future>
#include <mutex>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
using namespace std::chrono_literals;
namespace {
QJsonObject settings(QJsonObject hook,const QString& event="PostToolUse") {
    return {{"hooks",QJsonObject{{event,QJsonArray{QJsonObject{{"hooks",QJsonArray{hook}}}}}}}};
}
a::CommandHookOptions options(const QString& path) {a::CommandHookOptions result;result.workingDirectory=path;result.environment={};return result;}
a::HookInput input() {return {a::HookKind::AfterTool,"session","run",{"call","Read",{{"path","file.txt"}}},{"observed"},{}};}
void write(const QString& path,const QByteArray& value="release") {
    QFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(value)!=value.size())throw std::runtime_error("Cannot write hook fixture");
}
const QString waitForRelease="cat > input.json; touch ready; while [ ! -f release ]; do sleep .01; done; touch completed; printf '%s\\n' '{\"systemMessage\":\"BACKGROUND_CONTEXT\"}'";
class Model final:public a::Model {
public:
    std::mutex mutex;QList<a::ModelRequest> requests;
    std::atomic_bool block=false,waiting=false;
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken& token,const TextCallback&) override {
        {std::lock_guard lock(mutex);requests.append(request);}
        if(block){waiting=true;while(!token.isCancelled())std::this_thread::sleep_for(1ms);token.throwIfCancelled();}
        return {"ANSWER",{}};
    }
    int count(){std::lock_guard lock(mutex);return requests.size();}
    QString lastText(){std::lock_guard lock(mutex);QString result;for(const auto& m:requests.last().messages)result+=m.text+'\n';return result;}
};
struct Host {
    QTemporaryDir root;std::shared_ptr<Model> model=std::make_shared<Model>();a::EngineOptions config;
    Host(){config.sessionsDirectory=root.filePath("sessions");config.compaction.automatic=false;config.projectContext.enabled=false;}
    std::unique_ptr<a::Engine> engine(){return std::make_unique<a::Engine>(model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(),config);}
};
}
class AsyncHooksTests final:public QObject {
    Q_OBJECT
private slots:
    void queuedWakeCancellationDoesNotWaitForAnUnrelatedModelRun() {
        Host host;host.config.maxConcurrentRuns=1;host.config.maxQueuedRuns=1;
        a::CommandHooks hooks(settings({{"type","command"},{"command",waitForRelease+"; exit 2"},{"asyncRewake",true},{"once",true}},"Stop"),options(host.root.path()));
        host.config.hooks={hooks.callback()};auto engine=host.engine();
        const auto first=engine->createSession("test",host.root.path()).id,other=engine->createSession("test",host.root.path()).id;
        QCOMPARE(engine->run({first,"first"}).result.get().status,a::RunStatus::Completed);
        host.model->block=true;auto blocker=engine->run({other,"hold"});QTRY_VERIFY_WITH_TIMEOUT(host.model->waiting.load(),3000);
        write(host.root.filePath("release"));QTRY_COMPARE_WITH_TIMEOUT(engine->hookStatus(first)["wake_run"].toObject()["state"],QJsonValue("queued"),3000);
        QVERIFY(engine->cancelHooks(first)["wake_run_cancel_requested"].toBool());
        QCOMPARE(engine->hookStatus(first)["wake_run"].toObject()["state"],"cancelled");
        QVERIFY(blocker.result.wait_for(0ms)!=std::future_status::ready);blocker.cancel();engine->close();
    }
    void asyncTimeoutMetadataDoesNotReplaceTheCommandDeadline() {
        QTemporaryDir root;
        a::CommandHooks hooks(settings({{"type","command"},{"command","printf '%s\\n' '{\"async\":true,\"asyncTimeout\":1}'; sleep .12; printf '%s\\n' '{\"systemMessage\":\"AFTER_METADATA_TIMEOUT\"}'"},{"timeout",2}}),options(root.path()));
        (void)hooks.callback()(input(),{});
        QTRY_VERIFY_WITH_TIMEOUT(!hooks.asyncResults().isEmpty()&&hooks.asyncResults()[0].toObject()["state"]!="running",3000);
        const auto record=hooks.asyncResults()[0].toObject();QCOMPARE(record["async_timeout_ms"].toInt(),1);
        QCOMPARE(record["state"],"completed");QCOMPARE(record["text"],"AFTER_METADATA_TIMEOUT");
        a::CommandHooks timeout(settings({{"type","command"},{"command","sleep 2"},{"async",true},{"timeout",0.04}}),options(root.path()));
        (void)timeout.callback()(input(),{});
        QTRY_VERIFY_WITH_TIMEOUT(!timeout.asyncResults().isEmpty()&&timeout.asyncResults()[0].toObject()["state"]!="running",3000);
        QCOMPARE(timeout.asyncResults()[0].toObject()["error_code"],"timeout");
    }
    void engineSessionEndIsForcedSynchronousEvenWhenConfiguredAsync() {
        Host host;const auto command="cat > ending.json; sleep .05; touch ended";
        a::CommandHooks hooks(settings({{"type","command"},{"command",command},{"async",true}},"SessionEnd"),options(host.root.path()));
        host.config.hooks={hooks.callback()};auto engine=host.engine();const auto id=engine->createSession("test",host.root.path()).id;
        QCOMPARE(engine->run({id,"first"}).result.get().status,a::RunStatus::Completed);
        const auto ended=engine->endSession(id);QVERIFY(ended["ended"].toBool());QVERIFY(QFileInfo::exists(host.root.filePath("ended")));
        QVERIFY(hooks.asyncResults().isEmpty());QVERIFY(ended["async_hooks"].toArray().isEmpty());
    }
    void subagentStartCompletionIsDeliveredWithinTheChildLifetime() {
        Host host;const auto work=host.root.filePath("work");QDir().mkpath(work);
        a::CommandHooks hooks(settings({{"type","command"},{"command",waitForRelease},{"async",true}},"SubagentStart"),options(work));
        host.config.hooks={hooks.callback(),[&](const a::HookInput& input,const CancellationToken& token) {
            if(input.kind==a::HookKind::BeforeModel) {
                write(work+"/release");const auto deadline=std::chrono::steady_clock::now()+3s;
                for(;;) {
                    token.throwIfCancelled();const auto records=input.modelContext->executionContext.asyncHooks->status();
                    if(!records.isEmpty()&&records[0].toObject()["state"]!="running")break;
                    if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("Child hook did not finish");
                    std::this_thread::sleep_for(1ms);
                }
            }return a::HookResult{};
        }};
        a::SubagentOptions config;config.workingDirectory=work;config.stateDirectory=host.root.filePath("children");
        a::Subagents children(host.model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(),host.config,config);
        const auto parent=a::SessionStore(host.config.sessionsDirectory).create("test","",work);
        a::ToolContext context{parent.id,"run",work};context.sessionSnapshot=std::make_shared<a::Session>(parent);
        const auto result=children.run(context,{{"prompt","first"}});
        QVERIFY2(!result.isError,qPrintable(result.text));QVERIFY(host.model->lastText().contains("BACKGROUND_CONTEXT"));QVERIFY(hooks.asyncResults().isEmpty());
    }
    void wakeDoesNotInheritOneRunToolGrants() {
        class WritesOnWake final:public a::Model {
            int count=0;
            a::ModelReply generate(const a::ModelRequest&,const CancellationToken&,const TextCallback&) override {
                if(++count==2)return {{},{{"write","Write",{{"path","forbidden.txt"},{"content","LEAK"}}}}};
                return {"ANSWER",{}};
            }
        };
        Host host;auto registry=std::make_shared<a::ToolRegistry>();
        a::registerWorkspaceTools(*registry,host.root.path());
        a::CommandHooks hooks(settings({{"type","command"},{"command",waitForRelease+"; exit 2"},{"asyncRewake",true},{"once",true}},"Stop"),options(host.root.path()));
        std::atomic_int asks=0;host.config.hooks={hooks.callback()};host.config.permission=[&](const auto&,const auto&,const auto&){++asks;return false;};
        auto engine=std::make_unique<a::Engine>(std::make_shared<WritesOnWake>(),registry,std::make_shared<a::RulePolicy>(),host.config);
        const auto id=engine->createSession("test",host.root.path()).id;a::RunRequest request{id,"first"};request.allowedTools={"Write"};
        QCOMPARE(engine->run(request).result.get().status,a::RunStatus::Completed);write(host.root.filePath("release"));
        QTRY_COMPARE_WITH_TIMEOUT(engine->hookStatus(id)["wake_run"].toObject()["state"],QJsonValue("completed"),3000);
        QVERIFY(!QFileInfo::exists(host.root.filePath("forbidden.txt")));QCOMPARE(asks.load(),1);
    }
    void apiControlsAreAuthenticatedAndUseReservedCapacity() {
        Host host;const QString first(48,'a'),second(48,'b');a::ApiOptions config;
        config.workingDirectory=host.root.filePath("work");QDir().mkpath(config.workingDirectory);
        config.stateDirectory=host.root.filePath("private");config.clientTokens={{"society",first},{"dreamscapes",second}};
        config.engine=host.config;config.engine.sessionsDirectory.clear();config.maxConcurrentRequests=1;config.maxQueuedRequests=0;
        a::CommandHooks hooks(settings({{"type","command"},{"command",waitForRelease},{"async",true}},"Stop"),options(config.workingDirectory));
        config.engine.hooks={hooks.callback()};a::Api api(host.model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(),config);
        auto call=[&](const QString& method,const QJsonObject& args={},const QString& token=QString()) {
            auto handle=api.dispatch(method,args,token.isEmpty()?first:token);
            if(handle.result.wait_for(3s)!=std::future_status::ready)throw std::runtime_error("API timeout");return handle.result.get().toObject();
        };
        const auto id=call("agent.sessions.create",{{"model","test"}})["session_id"].toString();
        QCOMPARE(call("agent.run",{{"session_id",id},{"prompt","first"}})["status"],"completed");
        const auto record=call("agent.hooks.status",{{"session_id",id}})["hooks"].toArray()[0].toObject();QCOMPARE(record["state"],"running");
        QVERIFY_THROWS_EXCEPTION(Error,call("agent.hooks.status",{{"session_id",id}},second));
        QVERIFY_THROWS_EXCEPTION(Error,call("agent.hooks.cancel",{{"session_id",id}},second));
        host.model->block=true;auto running=api.dispatch("agent.run",{{"session_id",id},{"prompt","hold"}},first);
        QTRY_VERIFY_WITH_TIMEOUT(host.model->waiting.load(),3000);
        QCOMPARE(call("agent.hooks.cancel",{{"session_id",id},{"hook_id",record["hook_id"]}})["cancelled_hooks"].toInt(),1);
        QTRY_COMPARE_WITH_TIMEOUT(call("agent.hooks.status",{{"session_id",id}})["hooks"].toArray()[0].toObject()["state"],QJsonValue("cancelled"),3000);
        QVERIFY(api.isControlMethod("agent.hooks.status"));QVERIFY(api.isControlMethod("agent.hooks.cancel"));
        running.cancellation.cancel();api.close();
    }
    void directMcpHooksBelongToTheConnectionAndCloseCancelsThem() {
        for(const bool engineEnabled:{false,true}) {
        Host host;auto registry=std::make_shared<a::ToolRegistry>();
        a::Tool tool;tool.definition={"Echo","Echo",{{"type","object"}}, {},true,true};
        tool.execute=[](const QJsonObject&,const a::ToolContext&){return a::ToolResult{"observed"};};registry->add(tool);
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);auto engine=std::make_shared<a::Engine>(host.model,registry,policy,host.config);
        a::CommandHooks hooks(settings({{"type","command"},{"command",waitForRelease},{"async",true}}),options(host.root.path()));
        a::McpServerOptions config;if(engineEnabled)config.engine=engine;config.model="test";config.workingDirectory=host.root.path();config.tools.hooks={hooks.callback()};
        auto server=a::mcpServerOptions(registry,policy,config);
        mcp::ServerRequestContext first,second;first.sessionId="first";second.sessionId="second";
        const auto result=server.handlers["tools/call"]({{"name","Echo"},{"arguments",QJsonObject{}}},first);QVERIFY(!result["isError"].toBool());
        auto status=[&]{return server.controlHandlers["iisacc/hooks/status"]({},first);};
        const auto records=status()["hooks"].toArray();QCOMPARE(records.size(),1);QCOMPARE(records[0].toObject()["state"],"running");
        QCOMPARE(server.controlHandlers["iisacc/hooks/status"]({},second)["hooks"].toArray().size(),0);
        QVERIFY_THROWS_EXCEPTION(Error,server.controlHandlers["iisacc/hooks/cancel"]({{"hook_id",records[0].toObject()["hook_id"]}},second));
        QVERIFY_THROWS_EXCEPTION(mcp::RpcError,server.controlHandlers["iisacc/hooks/status"]({{"session_id",status()["session_id"]}},second));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(host.root.filePath("ready")),3000);server.onClosed("first");
        write(host.root.filePath("release"));QTest::qWait(80);QVERIFY(!QFileInfo::exists(host.root.filePath("completed")));
        engine->close();
        }
    }
    void automaticWakeCanBeCancelledAfterTheOriginalHandleCompleted() {
        Host host;a::CommandHooks hooks(settings({{"type","command"},{"command",waitForRelease+"; exit 2"},{"asyncRewake",true},{"once",true}},"Stop"),options(host.root.path()));
        host.config.hooks={hooks.callback()};auto engine=host.engine();const auto id=engine->createSession("test",host.root.path()).id;
        QCOMPARE(engine->run({id,"first"}).result.get().status,a::RunStatus::Completed);
        host.model->block=true;write(host.root.filePath("release"));QTRY_VERIFY_WITH_TIMEOUT(host.model->waiting.load(),3000);
        QVERIFY(engine->cancelHooks(id)["wake_run_cancel_requested"].toBool());
        QTRY_COMPARE_WITH_TIMEOUT(engine->hookStatus(id)["wake_run"].toObject()["state"],QJsonValue("cancelled"),3000);
        QVERIFY(engine->hookStatus(id)["wake_disabled"].toBool());engine->close();
    }
    void automaticWakeChainsAreBoundedAndRetainPendingContext() {
        Host host;host.config.maxAsyncHookWakeRuns=2;
        a::CommandHooks hooks(settings({{"type","command"},{"command",waitForRelease+"; exit 2"},{"asyncRewake",true}},"Stop"),options(host.root.path()));
        host.config.hooks={hooks.callback()};auto engine=host.engine();const auto id=engine->createSession("test",host.root.path()).id;
        a::RunRequest request{id,"first"};request.maxTurns=1;
        QCOMPARE(engine->run(request).result.get().status,a::RunStatus::Completed);write(host.root.filePath("release"));
        QTRY_VERIFY_WITH_TIMEOUT(!engine->hookStatus(id)["wake_error"].toString().isEmpty(),5000);
        QCOMPARE(engine->hookStatus(id)["wake_runs"].toInt(),2);QCOMPARE(host.model->count(),3);
        QCOMPARE(engine->queuedInputs(id)["count"].toInt(),1);engine->close();
    }
    void engineQueuesCompletionForOnlyItsOwningSessionWithoutWaking() {
        Host host;a::CommandHooks hooks(settings({{"type","command"},{"command",waitForRelease},{"async",true},{"once",true}},"Stop"),options(host.root.path()));
        host.config.hooks={hooks.callback()};auto engine=host.engine();
        const auto id=engine->createSession("model://test",host.root.path()).id;
        const auto other=engine->createSession("model://test",host.root.path()).id;
        QCOMPARE(engine->run({id,"first"}).result.get().status,a::RunStatus::Completed);
        write(host.root.filePath("release"));
        QTRY_COMPARE_WITH_TIMEOUT(engine->queuedInputs(id)["count"].toInt(),1,3000);
        QCOMPARE(engine->queuedInputs(other)["count"].toInt(),0);QCOMPARE(host.model->count(),1);
        QCOMPARE(engine->run({id,"second"}).result.get().status,a::RunStatus::Completed);
        QVERIFY(host.model->lastText().contains("BACKGROUND_CONTEXT"));QCOMPARE(engine->queuedInputs(id)["count"].toInt(),0);
    }
    void engineRewakesAnIdleSessionAfterItsOriginalRunFinished() {
        Host host;a::CommandHooks hooks(settings({{"type","command"},{"command",waitForRelease+"; printf REWAKE_CONTEXT >&2; exit 2"},{"asyncRewake",true},{"once",true}},"Stop"),options(host.root.path()));
        host.config.hooks={hooks.callback()};auto engine=host.engine();const auto id=engine->createSession("model://test",host.root.path()).id;
        QCOMPARE(engine->run({id,"first"}).result.get().status,a::RunStatus::Completed);QCOMPARE(host.model->count(),1);
        write(host.root.filePath("release"));QTRY_COMPARE_WITH_TIMEOUT(host.model->count(),2,3000);
        QVERIFY(host.model->lastText().contains("REWAKE_CONTEXT"));engine->close();
        QCOMPARE(engine->session(id).messages.size(),4);
    }
    void endingASessionCancelsItsBackgroundHookBeforeReturning() {
        Host host;a::CommandHooks hooks(settings({{"type","command"},{"command",waitForRelease},{"async",true}},"Stop"),options(host.root.path()));
        host.config.hooks={hooks.callback()};auto engine=host.engine();const auto id=engine->createSession("model://test",host.root.path()).id;
        QCOMPARE(engine->run({id,"first"}).result.get().status,a::RunStatus::Completed);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(host.root.filePath("ready")),3000);engine->endSession(id);
        write(host.root.filePath("release"));QTest::qWait(80);QVERIFY(!QFileInfo::exists(host.root.filePath("completed")));
        QCOMPARE(engine->queuedInputs(id)["count"].toInt(),0);
    }
    void completionsCarryOnlyContextAndCannotApplyLateControl() {
        QTemporaryDir root;
        const QString command="printf '%s\\n' '{\"continue\":false,\"decision\":\"block\",\"systemMessage\":\"SYSTEM_CONTEXT\",\"hookSpecificOutput\":{\"hookEventName\":\"PostToolUse\",\"additionalContext\":\"EXTRA_CONTEXT\",\"updatedMCPToolOutput\":\"TOO_LATE\"}}'";
        a::CommandHooks hooks(settings({{"type","command"},{"command",command},{"async",true}}),options(root.path()));
        const auto result=hooks.callback()(input(),{});QVERIFY(!result.block&&!result.stop&&!result.updatedMCPToolOutput);QVERIFY(result.feedback.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!hooks.asyncResults("session").isEmpty()&&hooks.asyncResults("session")[0].toObject()["state"]!="running",3000);
        const auto records=hooks.asyncResults("session",true);QCOMPARE(records.size(),1);const auto record=records[0].toObject();
        QCOMPARE(record["text"],"SYSTEM_CONTEXT\nEXTRA_CONTEXT");QCOMPARE(record["state"],"completed");QVERIFY(!record["wake"].toBool());
        QVERIFY(hooks.asyncResults("session").isEmpty());QVERIFY(!record.contains("command"));
    }
    void asyncRewakeOnlyRequestsWakeForExitTwo() {
        QTemporaryDir root;
        for(const int code:{0,1,2}) {
            a::CommandHooks hooks(settings({{"type","command"},{"command",QString("printf OUT; printf ERR >&2; exit %1").arg(code)},{"asyncRewake",true}},"Stop"),options(root.path()));
            auto event=input();event.kind=a::HookKind::Stop;
            const auto result=hooks.callback()(event,{});QVERIFY(!result.block&&!result.stop);
            QTRY_VERIFY_WITH_TIMEOUT(!hooks.asyncResults().isEmpty()&&hooks.asyncResults()[0].toObject()["state"]!="running",3000);
            const auto record=hooks.asyncResults()[0].toObject();QCOMPARE(record["exit_code"].toInt(),code);QCOMPARE(record["wake"].toBool(),code==2);
            if(code==2){QVERIFY(record["text"].toString().endsWith("ERR"));QVERIFY(!record["text"].toString().contains("OUT"));}
            else QVERIFY(record["text"].toString().isEmpty());
        }
    }
    void isolatedScopeOwnsDeliveryAndHardCancellation() {
        QTemporaryDir root;QJsonArray delivered;
        auto scope=std::make_shared<a::AsyncHookScope>([&](const QJsonObject& value){delivered.append(value);});
        a::CommandHooks hooks(settings({{"type","command"},{"command",waitForRelease},{"async",true}}),options(root.path()));
        auto context=std::make_shared<a::ModelHookContext>();context->executionContext.asyncHooks=scope;
        CancellationToken hard,interrupted;context->executionContext.hookCancellation=hard;
        auto event=input();event.modelContext=context;
        (void)hooks.callback()(event,interrupted);QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(root.filePath("ready")),3000);
        interrupted.cancel();QTest::qWait(40);QCOMPARE(scope->status()[0].toObject()["state"],"running");
        hard.cancel();QTRY_COMPARE_WITH_TIMEOUT(scope->status()[0].toObject()["state"],QJsonValue("cancelled"),3000);
        scope->close();QVERIFY(delivered.isEmpty());QVERIFY(hooks.asyncResults().isEmpty());QVERIFY(!QFileInfo::exists(root.filePath("completed")));
    }
    void forceSynchronousConsumesAnAsyncHeaderAndFinalDecision() {
        QTemporaryDir root;
        auto context=std::make_shared<a::ModelHookContext>();context->executionContext.forceSynchronousHooks=true;
        auto event=input();event.modelContext=context;
        const QString command="printf '%s\\n' '{\"async\":true}' '{\"continue\":false,\"stopReason\":\"SYNC_STOP\"}'";
        a::CommandHooks hooks(settings({{"type","command"},{"command",command},{"async",true}}),options(root.path()));
        const auto result=hooks.callback()(event,{});QVERIFY(result.stop);QCOMPARE(result.stopReason,"SYNC_STOP");QVERIFY(hooks.asyncResults().isEmpty());
    }
    void capacityAndOnceStayBoundedWhileTheCommandIsRunning() {
        QTemporaryDir root;auto config=options(root.path());config.maxConcurrentProcesses=1;config.maxAsyncRecords=1;
        a::CommandHooks hooks(settings({{"type","command"},{"command",waitForRelease},{"async",true},{"once",true}}),config);
        (void)hooks.callback()(input(),{});QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(root.filePath("ready")),3000);
        QVERIFY(hooks.callback()(input(),{}).diagnostics.isEmpty());
        auto other=input();other.sessionId="other";const auto denied=hooks.callback()(other,{});
        QCOMPARE(denied.diagnostics.last().toObject()["error_code"],"queue_full");QVERIFY(hooks.asyncResults("other").isEmpty());
        hooks.close();QCOMPARE(hooks.asyncResults("session")[0].toObject()["state"],"cancelled");
    }
    void configuredAsyncReturnsBeforeWorkFinishesAndDeliversStdin() {
        QTemporaryDir root;
        try {
            a::CommandHooks hooks(settings({{"type","command"},{"command",waitForRelease},{"async",true}}),options(root.path()));
            CancellationToken token;auto pending=std::async(std::launch::async,[&]{return hooks.callback()(input(),token);});
            struct Cancel {CancellationToken token;~Cancel(){token.cancel();}} cancel{token};
            QVERIFY(pending.wait_for(3s)==std::future_status::ready);const auto result=pending.get();
            QVERIFY(!result.block&&!result.stop);QVERIFY(result.feedback.isEmpty());
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(root.filePath("ready")),3000);
            QVERIFY(!QFileInfo::exists(root.filePath("completed")));
            QFile stdinFile(root.filePath("input.json"));QVERIFY(stdinFile.open(QIODevice::ReadOnly));const auto bytes=stdinFile.readAll();
            QVERIFY(bytes.endsWith('\n'));const auto body=QJsonDocument::fromJson(bytes).object();
            QCOMPARE(body["session_id"],"session");QCOMPARE(body["hook_event_name"],"PostToolUse");QCOMPARE(body["tool_use_id"],"call");
            QCOMPARE(body["tool_response"].toObject()["text"],"observed");
            write(root.filePath("release"));QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(root.filePath("completed")),3000);
        }catch(const Error& error){QFAIL(error.what());}
    }
    void firstLineAsyncHandshakeReturnsWhileProcessRemainsLive() {
        QTemporaryDir root;
        a::CommandHooks hooks(settings({{"type","command"},{"command","printf '%s\\n' '{\"async\":true}'; "+waitForRelease}}),options(root.path()));
        CancellationToken token;auto pending=std::async(std::launch::async,[&]{return hooks.callback()(input(),token);});
        struct Cancel {CancellationToken token;~Cancel(){token.cancel();}} cancel{token};
        QVERIFY(pending.wait_for(3s)==std::future_status::ready);const auto result=pending.get();
        QVERIFY(!result.block&&!result.stop);QVERIFY(result.feedback.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(root.filePath("ready")),3000);QVERIFY(!QFileInfo::exists(root.filePath("completed")));
        write(root.filePath("release"));QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(root.filePath("completed")),3000);
    }
};
QTEST_GUILESS_MAIN(AsyncHooksTests)
#include "async_hooks_tests.moc"
