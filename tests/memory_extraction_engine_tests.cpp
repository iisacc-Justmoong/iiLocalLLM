#include <agent/Engine.h>
#include <agent/Api.h>
#include <agent/McpServer.h>
#include <mcp/Server.h>
#include <QtTest/QTest>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QJsonDocument>
#include <atomic>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
class Model final:public a::Model {
public:
    std::atomic<bool> slow=false,started=false,cancelled=false;std::atomic<int> extractions=0;
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken& token,const TextCallback&)override {
        for(const auto& message:request.messages)if(message.metadata.contains("iilocal.memory_extraction")) {
            started=true;++extractions;while(slow&&!token.isCancelled())QThread::msleep(1);
            if(token.isCancelled()){cancelled=true;token.throwIfCancelled();}
            if(request.messages.last().role==a::MessageRole::Tool)return {"Saved.",{},Usage{20,6}};
            const auto path=QDir(message.metadata["iilocal.memory_extraction"].toObject()["directory"].toString()).filePath("preferences.md");
            return {{},{{"save","Write",{{"path",path},{"content","---\nname: Language\ndescription: Korean reports\ntype: feedback\n---\nUse Korean for future reports."}}}},Usage{18,9}};
        }
        return {"I will use Korean for future reports.",{},Usage{10,7}};
    }
};
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("extraction-engine-XXXXXX")};QString work=root.filePath("work");
    std::shared_ptr<Model> model=std::make_shared<Model>();std::shared_ptr<a::ToolRegistry> tools=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::RulePolicy> policy=std::make_shared<a::RulePolicy>();a::EngineOptions options;
    Fixture(){QDir().mkpath(work);a::registerWorkspaceTools(*tools,work);options.sessionsDirectory=root.filePath("sessions");
        options.projectMemoryEnabled=true;options.memoryRecall.enabled=false;options.memoryExtraction.enabled=true;
        options.skills.enabled=false;options.projectContext.enabled=false;options.toolSearch.enabled=false;options.compaction.automatic=false;}
};
QJsonObject api(a::Api& host,const QString& method,const QJsonObject& params={},const QString& credential=QString(48,'a')) {
    return host.dispatch(method,params,credential).result.get().toObject();
}
QJsonObject exchange(iiLocalLLM::mcp::ServerSession& server,int id,const QString& method,QJsonObject params={}) {
    server.receive(QJsonObject{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}});
    for(int i=0;i<300;++i)for(const auto& item:server.takeMessages(10))if(item.toObject()["id"]==id)return item.toObject();
    throw std::runtime_error("MCP response timed out");
}
}
class MemoryExtractionEngineTests:public QObject {
    Q_OBJECT
private slots:
    void backgroundExtractionDoesNotDelayAnswerOrAppendItsConversation() {
        Fixture f;f.model->slow=true;a::Engine engine(f.model,f.tools,f.policy,f.options);const auto id=engine.createSession("fixture",f.work).id;
        QJsonArray receipts;const auto result=engine.run({id,"Use Korean for future reports"},[&](const a::Event& event){if(event.kind==a::EventKind::MemoryExtraction)receipts.append(event.data);}).result.get();
        QCOMPARE(result.status,a::RunStatus::Completed);QVERIFY(!receipts.isEmpty());QTRY_VERIFY_WITH_TIMEOUT(f.model->started,3000);
        QCOMPARE(engine.session(id).messages.size(),2);QVERIFY(!engine.drainMemoryExtractions(5));f.model->slow=false;
        QVERIFY(engine.drainMemoryExtractions(5000));const auto status=engine.memoryExtractionStatus(id);
        const auto record=status["records"].toArray().last().toObject();QVERIFY2(record["status"]=="completed",QJsonDocument(record).toJson().constData());
        QCOMPARE(record["saved_topics"].toArray().size(),1);QCOMPARE(engine.session(id).messages.size(),2);
        QCOMPARE(result.usage.generatedTokens,7);QCOMPARE(record["usage"].toObject()["generated_tokens"].toInt(),15);
        QVERIFY(engine.queuedInputs(id)["count"].toInt()==0);QCOMPARE(engine.extractMemory(id)["status"].toString(),"up_to_date");
    }
    void offeredBeforeStopHooksAndClearCancelsWithoutReusingOldContext() {
        Fixture f;f.model->slow=true;bool offered=false,stopSawOffer=false;
        f.options.hooks.append([&](const a::HookInput& input,const auto&){if(input.kind==a::HookKind::Stop)stopSawOffer=offered;return a::HookResult{};});
        a::Engine engine(f.model,f.tools,f.policy,f.options);const auto id=engine.createSession("fixture",f.work).id;
        auto result=engine.run({id,"Use Korean"},[&](const a::Event& event){if(event.kind==a::EventKind::MemoryExtraction)offered=true;}).result.get();
        QCOMPARE(result.status,a::RunStatus::Completed);QVERIFY(stopSawOffer);QTRY_VERIFY_WITH_TIMEOUT(f.model->started,3000);
        const auto cleared=engine.clearSession(id);QVERIFY(cleared["complete"].toBool());QVERIFY(f.model->cancelled);
        QVERIFY(!engine.memoryExtractionStatus(id)["has_context"].toBool());QCOMPARE(engine.extractMemory(cleared["session_id"].toString())["status"].toString(),"no_context");
    }
    void closeDrainsSuccessfulWorkAndDisabledHostNeverStartsExtraction() {
        Fixture f;std::atomic<int> hookContexts=0;
        f.options.hooks.append([&](const a::HookInput& input,const auto&) {
            if(input.kind==a::HookKind::BeforeTool) {
                if(!input.modelContext||!input.modelContext->agentExecutor||!input.modelContext->session||input.modelContext->session->parentSessionId.isEmpty())
                    throw std::runtime_error("Extraction omitted the host verifier or fork context");++hookContexts;
            }return a::HookResult{};
        });
        a::Engine engine(f.model,f.tools,f.policy,f.options);auto id=engine.createSession("fixture",f.work).id;
        QCOMPARE(engine.run({id,"Use Korean"}).result.get().status,a::RunStatus::Completed);engine.close();
        const auto record=engine.memoryExtractionStatus(id)["records"].toArray().last().toObject();QCOMPARE(record["status"].toString(),"completed");
        QCOMPARE(record["tool_errors"].toInt(),0);QCOMPARE(hookContexts.load(),1);
        f.options.memoryExtraction.enabled=false;f.options.sessionsDirectory=f.root.filePath("disabled");a::Engine disabled(f.model,f.tools,f.policy,f.options);
        const auto before=f.model->extractions.load();id=disabled.createSession("fixture",f.work).id;QCOMPARE(disabled.run({id,"Use Korean"}).result.get().status,a::RunStatus::Completed);
        QCOMPARE(f.model->extractions.load(),before);QCOMPARE(disabled.extractMemory(id)["status"].toString(),"disabled");
    }
    void apiOwnersAndReservedControls() {
        Fixture f;f.model->slow=true;a::ApiOptions options;options.stateDirectory=f.root.filePath("api");options.workingDirectory=f.work;
        options.engine=f.options;options.engine.sessionsDirectory.clear();options.clientTokens={{"society",QString(48,'a')},{"dreamscapes",QString(48,'b')}};
        a::Api host(f.model,f.tools,f.policy,options);QVERIFY(host.isControlMethod("agent.memory.extraction.status"));QVERIFY(host.isControlMethod("agent.memory.extraction.cancel"));
        const auto id=api(host,"agent.sessions.create",{{"model","fixture"}})["session_id"].toString();
        QCOMPARE(api(host,"agent.memory.extract",{{"session_id",id}})["status"].toString(),"no_context");
        api(host,"agent.run",{{"session_id",id},{"prompt","Use Korean for reports"}});QTRY_VERIFY_WITH_TIMEOUT(f.model->started,3000);
        QVERIFY(api(host,"agent.memory.extraction.status",{{"session_id",id}})["active"].toBool());
        QVERIFY_THROWS_EXCEPTION(Error,api(host,"agent.memory.extraction.status",{{"session_id",id}},QString(48,'b')));
        QVERIFY_THROWS_EXCEPTION(Error,api(host,"agent.memory.extraction.cancel",{{"session_id",id}},QString(48,'b')));
        QVERIFY_THROWS_EXCEPTION(Error,api(host,"agent.memory.extract",{{"session_id",id},{"messages",QJsonArray{}}}));
        api(host,"agent.memory.extraction.cancel",{{"session_id",id}});QTRY_VERIFY_WITH_TIMEOUT(f.model->cancelled,3000);
        QVERIFY(api(host,"agent.info")["memory_extraction_enabled"].toBool());
    }
    void mcpBindsControlsToConnectionAndAdvertisesCapability() {
        Fixture f;auto engine=std::make_shared<a::Engine>(f.model,f.tools,f.policy,f.options);
        a::McpServerOptions options;options.engine=engine;options.model="fixture";options.workingDirectory=f.work;
        iiLocalLLM::mcp::ServerSession server(a::mcpServerOptions(f.tools,f.policy,options));
        const auto initialized=exchange(server,1,"initialize",{{"protocolVersion","2025-03-26"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","test"},{"version","1"}}}});
        QVERIFY(QJsonDocument(initialized).toJson().contains("extractionTools"));server.receive(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/initialized"}});
        const auto receipt=exchange(server,2,"tools/call",{{"name","iiLocalLLM.agent.memory.extract"},{"arguments",QJsonObject{}}});
        QVERIFY2(QJsonDocument(receipt).toJson().contains("no_context"),QJsonDocument(receipt).toJson().constData());
        const auto forged=exchange(server,3,"tools/call",{{"name","iiLocalLLM.agent.memory.extraction.status"},{"arguments",QJsonObject{{"session_id","foreign"}}}});
        QVERIFY(forged.contains("error")||forged["result"].toObject()["isError"].toBool());
        const auto status=exchange(server,4,"iisacc/memory/extraction/status",{{"offset",0},{"limit",1}});
        QVERIFY(status["result"].toObject()["enabled"].toBool());
        QVERIFY(exchange(server,5,"iisacc/memory/extraction/cancel")["result"].toObject()["enabled"].toBool());
        QVERIFY(exchange(server,6,"iisacc/memory/extraction/status",{{"session_id","foreign"}}).contains("error"));
    }
};
QTEST_GUILESS_MAIN(MemoryExtractionEngineTests)
#include "memory_extraction_engine_tests.moc"
