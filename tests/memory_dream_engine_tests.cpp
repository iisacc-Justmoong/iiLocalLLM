#include <agent/Engine.h>
#include <agent/Api.h>
#include <agent/McpServer.h>
#include <mcp/Server.h>
#include <QtTest/QTest>
#include <QtCore/QTemporaryDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QThread>
#include <atomic>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
class Model final:public a::Model {
public:
    std::atomic<bool> slow=false,started=false,cancelled=false;
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken& token,const TextCallback&)override {
        for(const auto& message:request.messages)if(message.metadata.contains("iilocal.memory_dream")) {
            started=true;while(slow&&!token.isCancelled())QThread::msleep(1);if(token.isCancelled()){cancelled=true;token.throwIfCancelled();}
            if(request.messages.last().role==a::MessageRole::Tool)return {"Consolidated.",{},Usage{20,6}};
            const auto path=QDir(message.metadata["iilocal.memory_dream"].toObject()["directory"].toString()).filePath("preferences.md");
            return {{},{{"save","Write",{{"path",path},{"content","---\nname: Language\ndescription: Reporting language\ntype: feedback\n---\nUse Korean.\n"}}}},Usage{15,10}};
        }return {"Acknowledged.",{},Usage{10,7}};
    }
};
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("dream-engine-XXXXXX")};QString work=root.filePath("work");
    std::shared_ptr<Model> model=std::make_shared<Model>();std::shared_ptr<a::ToolRegistry> tools=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::RulePolicy> policy=std::make_shared<a::RulePolicy>();a::EngineOptions options;
    Fixture(){QDir().mkpath(work);a::registerWorkspaceTools(*tools,work);options.sessionsDirectory=root.filePath("sessions");
        options.projectMemoryEnabled=true;options.sessionHistoryEnabled=true;options.memoryRecall.enabled=false;
        options.memoryDream.automatic=true;options.memoryDream.minSessions=1;
        options.skills.enabled=false;options.projectContext.enabled=false;options.toolSearch.enabled=false;options.compaction.automatic=false;
        a::SessionStore store(options.sessionsDirectory);auto previous=store.create("fixture","system",work);store.acquire(previous.id)->append({{},a::MessageRole::User,"Use Korean for reports."});}
};
QJsonObject api(a::Api& host,const QString& method,QJsonObject params={},const QString& credential=QString(48,'a')){return host.dispatch(method,params,credential).result.get().toObject();}
QJsonObject rpc(iiLocalLLM::mcp::ServerSession& server,int id,const QString& method,QJsonObject params={}) {
    server.receive(QJsonObject{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}});
    for(int i=0;i<300;++i)for(const auto& item:server.takeMessages(10))if(item.toObject()["id"]==id)return item.toObject();throw std::runtime_error("MCP response timed out");
}
}
class MemoryDreamEngineTests:public QObject {
    Q_OBJECT
private slots:
    void automaticDreamStartsBeforeStopWithoutBlockingOrAppending() {
        Fixture f;f.model->slow=true;bool offered=false,stopSawOffer=false;
        f.options.hooks.append([&](const a::HookInput& input,const auto&){if(input.kind==a::HookKind::Stop)stopSawOffer=offered;return a::HookResult{};});
        a::Engine engine(f.model,f.tools,f.policy,f.options);const auto id=engine.createSession("fixture",f.work).id;
        auto result=engine.run({id,"Continue"},[&](const a::Event& event){if(event.kind==a::EventKind::MemoryDream)offered=true;}).result.get();
        QCOMPARE(result.status,a::RunStatus::Completed);QVERIFY(stopSawOffer);QTRY_VERIFY_WITH_TIMEOUT(f.model->started,3000);QVERIFY(!engine.drainMemoryDreams(5));
        f.model->slow=false;QVERIFY(engine.drainMemoryDreams(5000));const auto record=engine.memoryDreamStatus(id)["records"].toArray().last().toObject();
        QCOMPARE(record["status"].toString(),"completed");QCOMPARE(record["saved_topics"].toArray().size(),1);QCOMPARE(engine.session(id).messages.size(),2);
        QCOMPARE(result.usage.generatedTokens,7);QCOMPARE(record["usage"].toObject()["generated_tokens"].toInt(),16);QCOMPARE(engine.queuedInputs(id)["count"].toInt(),0);
    }
    void clearCancelsAndCloseDrainsIndependentMaintenance() {
        Fixture f;f.model->slow=true;a::Engine engine(f.model,f.tools,f.policy,f.options);const auto id=engine.createSession("fixture",f.work).id;
        QCOMPARE(engine.run({id,"Continue"}).result.get().status,a::RunStatus::Completed);QTRY_VERIFY_WITH_TIMEOUT(f.model->started,3000);
        const auto cleared=engine.clearSession(id);QVERIFY(cleared["complete"].toBool());QVERIFY(f.model->cancelled);
        QVERIFY(!engine.memoryDreamStatus(id)["has_context"].toBool());QCOMPARE(engine.consolidateMemory(cleared["session_id"].toString())["status"].toString(),"no_context");
        f.model->slow=false;const auto next=cleared["session_id"].toString();QCOMPARE(engine.run({next,"Continue"}).result.get().status,a::RunStatus::Completed);
        engine.consolidateMemory(next);engine.close();QCOMPARE(engine.memoryDreamStatus(next)["records"].toArray().last().toObject()["status"].toString(),"completed");
    }
    void availabilityAutomaticFlagAndDependencies() {
        Fixture f;f.options.memoryDream.automatic=false;a::Engine manual(f.model,f.tools,f.policy,f.options);const auto id=manual.createSession("fixture",f.work).id;
        QVERIFY(manual.memoryDreamAvailable());QVERIFY(!manual.automaticMemoryDream());QCOMPARE(manual.consolidateMemory(id)["status"].toString(),"no_context");
        f.options.sessionHistoryEnabled=false;a::Engine disabled(f.model,f.tools,f.policy,f.options);QVERIFY(!disabled.memoryDreamAvailable());
        QCOMPARE(disabled.consolidateMemory(id)["status"].toString(),"unavailable");f.options.memoryDream.automatic=true;
        QVERIFY_THROWS_EXCEPTION(Error,a::Engine(f.model,f.tools,f.policy,f.options));
    }
    void apiOwnerIsolationAndReservedControls() {
        Fixture f;f.options.memoryDream.automatic=false;f.model->slow=true;a::ApiOptions options;options.stateDirectory=f.root.filePath("api");options.workingDirectory=f.work;
        options.engine=f.options;options.engine.sessionsDirectory.clear();options.clientTokens={{"society",QString(48,'a')},{"dreamscapes",QString(48,'b')}};
        a::Api host(f.model,f.tools,f.policy,options);QVERIFY(host.isControlMethod("agent.memory.dream.status"));QVERIFY(host.isControlMethod("agent.memory.dream.cancel"));
        const auto info=api(host,"agent.info");QVERIFY(info["memory_dream_available"].toBool());QVERIFY(!info["auto_dream_enabled"].toBool());
        const auto id=api(host,"agent.sessions.create",{{"model","fixture"}})["session_id"].toString();
        QCOMPARE(api(host,"agent.run",{{"session_id",id},{"prompt","Continue"}})["status"].toString(),"completed");
        QCOMPARE(api(host,"agent.memory.dream",{{"session_id",id}})["status"].toString(),"queued");QTRY_VERIFY_WITH_TIMEOUT(f.model->started,3000);
        QVERIFY(api(host,"agent.memory.dream.status",{{"session_id",id}})["active"].toBool());
        for(const auto& method:QStringList{"agent.memory.dream","agent.memory.dream.status","agent.memory.dream.cancel"})
            QVERIFY_THROWS_EXCEPTION(Error,api(host,method,{{"session_id",id}},QString(48,'b')));
        QVERIFY_THROWS_EXCEPTION(Error,api(host,"agent.memory.dream",{{"session_id",id},{"workspace",f.work}}));
        api(host,"agent.memory.dream.cancel",{{"session_id",id}});QTRY_VERIFY_WITH_TIMEOUT(f.model->cancelled,3000);
    }
    void mcpConnectionControlsAndCapabilities() {
        Fixture f;f.options.memoryDream.automatic=false;f.model->slow=true;
        f.policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{{"iiLocalLLM.agent.run",a::PermissionBehavior::Allow}});
        auto engine=std::make_shared<a::Engine>(f.model,f.tools,f.policy,f.options);
        a::McpServerOptions options;options.engine=engine;options.model="fixture";options.workingDirectory=f.work;
        iiLocalLLM::mcp::ServerSession server(a::mcpServerOptions(f.tools,f.policy,options));
        const auto init=rpc(server,1,"initialize",{{"protocolVersion","2025-11-25"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","test"},{"version","1"}}}});
        QVERIFY(QJsonDocument(init).toJson().contains("iisacc/memoryDream"));server.receive(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/initialized"}});
        auto run=rpc(server,2,"tools/call",{{"name","iiLocalLLM.agent.run"},{"arguments",QJsonObject{{"prompt","Continue"}}}});QVERIFY2(!run["result"].toObject()["isError"].toBool(),QJsonDocument(run).toJson().constData());
        auto receipt=rpc(server,3,"tools/call",{{"name","iiLocalLLM.agent.memory.dream"},{"arguments",QJsonObject{}}});QCOMPARE(receipt["result"].toObject()["structuredContent"].toObject()["status"].toString(),"queued");
        QTRY_VERIFY_WITH_TIMEOUT(f.model->started,3000);QVERIFY(rpc(server,4,"iisacc/memory/dream/status",{{"limit",1}})["result"].toObject()["active"].toBool());
        QVERIFY(rpc(server,5,"iisacc/memory/dream/status",{{"session_id","foreign"}}).contains("error"));
        QVERIFY(rpc(server,6,"iisacc/memory/dream/cancel")["result"].toObject()["available"].toBool());QTRY_VERIFY_WITH_TIMEOUT(f.model->cancelled,3000);
    }
};
QTEST_GUILESS_MAIN(MemoryDreamEngineTests)
#include "memory_dream_engine_tests.moc"
