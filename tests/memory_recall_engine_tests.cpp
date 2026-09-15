#include <agent/Engine.h>
#include <agent/Api.h>
#include <agent/McpServer.h>
#include <mcp/Server.h>
#include <QtTest/QTest>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QThread>
#include <atomic>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
class Model final:public a::Model {
public:
    std::function<a::ModelReply(const a::ModelRequest&,const CancellationToken&)> action;
    std::atomic<int> selectors=0,mains=0;std::atomic<bool> slow=false,started=false,cancelled=false;
    a::ModelReply generate(const a::ModelRequest& input,const CancellationToken& token,const TextCallback&)override {
        if(action)return action(input,token);
        if(input.responseSchema["properties"].toObject().contains("selected_memories")) {
            ++selectors;started=true;
            while(slow&&!token.isCancelled())QThread::msleep(1);
            if(token.isCancelled()){cancelled=true;token.throwIfCancelled();}
            return {"{\"selected_memories\":[\"topic.md\"]}",{},Usage{11,7}};
        }
        ++mains;
        if(slow) {for(int n=0;n<2000&&!started;++n)QThread::msleep(1);require(started,"Selector never started");return {"DONE",{}};}
        if(input.summarizing)return {"Earlier tasks were completed. Project notes remain available.",{}};
        for(const auto& message:input.messages)if(message.metadata.contains("iilocal.memory_recall"))return {message.text,{}};
        return {{},{{"wait"+QString::number(mains),"Yield",{}}}};
    }
    std::optional<ContextBudget> measure(const a::ModelRequest& request,const CancellationToken&)override {
        ContextBudget b;b.inputTokens=64;for(const auto& message:request.messages)b.inputTokens+=message.text.size()/3+8;b.contextTokens=32768;return b;
    }
};
struct Fixture {
    QTemporaryDir root;QString work=root.filePath("work");std::shared_ptr<Model> model=std::make_shared<Model>();
    std::shared_ptr<a::ToolRegistry> tools=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::RulePolicy> policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);a::EngineOptions options;
    Fixture() {
        QDir().mkpath(work);a::registerWorkspaceTools(*tools,work);a::Tool wait;
        wait.definition={"Yield","Wait briefly for external work",{{"type","object"},{"properties",QJsonObject{}}},{},true,true};
        wait.execute=[](const auto&,const auto& token){QThread::msleep(10);token.cancellation.throwIfCancelled();return a::ToolResult{"Ready"};};tools->add(wait);
        options.sessionsDirectory=root.filePath("sessions");options.projectMemoryEnabled=true;options.skills.enabled=false;
        options.projectContext.enabled=false;options.compaction.automatic=false;options.toolSearch.enabled=false;
    }
    QString seed(a::Engine& engine,const QString& id,const QString& secret="MEMORY_RECALLED_741") {
        const auto path=QDir(engine.memory(id)["directory"].toString()).filePath("topic.md");
        auto written=engine.runMemoryTool(id,"Write",{{"path",path},{"content","---\ndescription: Project deployment constraints\ntype: project\n---\n"+secret}});
        require(!written.isError,"Memory fixture write failed");return path;
    }
};
QJsonObject api(a::Api& api,const QString& method,const QJsonObject& params={},QString token=QString(48,'a')) {return api.dispatch(method,params,token).result.get().toObject();}
QJsonObject exchange(iiLocalLLM::mcp::ServerSession& server,int id,const QString& method,QJsonObject params={}) {
    server.receive(QJsonObject{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}});
    for(int i=0;i<200;++i)for(const auto& item:server.takeMessages(20))if(item.toObject()["id"]==id)return item.toObject();
    throw std::runtime_error("MCP response timed out");
}
}
class MemoryRecallEngineTests:public QObject {
    Q_OBJECT
private slots:
    void automaticRecallSurfacesOnceAndClearCanRecallAgain() {
        Fixture f;a::Engine engine(f.model,f.tools,f.policy,f.options);auto id=engine.createSession("fixture",f.work).id;f.seed(engine,id);
        int recalled=0;auto events=[&](const a::Event& e){if(e.kind==a::EventKind::MemoryRecall&&e.data["attached_count"].toInt()>0)++recalled;};
        auto run=engine.run({id,"Recall the deployment constraints"},events).result.get();
        QVERIFY2(run.status==a::RunStatus::Completed,qPrintable(run.errorMessage));QVERIFY(run.text.contains("MEMORY_RECALLED_741"));
        QCOMPARE(recalled,1);QCOMPARE(run.usage.memoryRecallPromptTokens,11);QCOMPARE(run.usage.memoryRecallGeneratedTokens,7);
        auto messages=engine.session(id).messages;QCOMPARE(std::count_if(messages.begin(),messages.end(),[](const auto& m){return m.metadata.contains("iilocal.memory_recall");}),1);
        const auto calls=f.model->selectors.load();QVERIFY(engine.run({id,"Continue with those constraints"},events).result.get().text.contains("MEMORY_RECALLED_741"));
        QCOMPARE(f.model->selectors.load(),calls);QCOMPARE(recalled,1);
        const auto fork=engine.forkSession(id);QVERIFY(engine.run({fork.id,"Continue the same work"},events).result.get().text.contains("MEMORY_RECALLED_741"));
        QCOMPARE(f.model->selectors.load(),calls);
        const auto clear=engine.clearSession(id);QVERIFY(clear["complete"].toBool());
        auto fresh=engine.run({clear["session_id"].toString(),"Recall the deployment constraints"},events).result.get();
        QVERIFY(fresh.text.contains("MEMORY_RECALLED_741"));QCOMPARE(f.model->selectors.load(),calls+1);
    }
    void slowSelectorCannotHoldMainModelOrAppendAfterCompletion() {
        Fixture f;f.model->slow=true;a::Engine engine(f.model,f.tools,f.policy,f.options);auto id=engine.createSession("fixture",f.work).id;f.seed(engine,id);
        auto run=engine.run({id,"A prompt with enough context"}).result.get();QCOMPARE(run.status,a::RunStatus::Completed);QCOMPARE(run.text,"DONE");
        QVERIFY(f.model->started);QVERIFY(f.model->cancelled);QCOMPARE(f.model->mains.load(),1);
        const auto messages=engine.session(id).messages;QVERIFY(std::none_of(messages.begin(),messages.end(),[](const auto& m){return m.metadata.contains("iilocal.memory_recall");}));
    }
    void compactionRetainsTheRealQuestionAndResetsRecallDeduplication() {
        Fixture f;f.options.compaction.keepRecentGroups=1;a::Engine engine(f.model,f.tools,f.policy,f.options);
        const auto id=engine.createSession("fixture",f.work).id;f.seed(engine,id);
        auto run=engine.run({id,"Recall the deployment constraints"}).result.get();QVERIFY(run.text.contains("MEMORY_RECALLED_741"));
        auto before=engine.session(id);const auto user=std::find_if(before.messages.cbegin(),before.messages.cend(),[](const auto& m){return m.text=="Recall the deployment constraints";});
        QVERIFY(user!=before.messages.cend());a::CompactRequest compact;compact.sessionId=id;
        auto result=engine.compact(compact).result.get();QVERIFY2(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage));
        const auto stored=engine.session(id);QCOMPARE(stored.compactions.last().retainedUserMessageId,user->id);
        const auto visible=a::modelMessages(stored);QVERIFY(std::none_of(visible.cbegin(),visible.cend(),[](const auto& m){return m.metadata.contains("iilocal.memory_recall");}));
        const auto count=f.model->selectors.load();QVERIFY(engine.run({id,"Recall the deployment constraints again"}).result.get().text.contains("MEMORY_RECALLED_741"));
        QCOMPARE(f.model->selectors.load(),count+1);
    }
    void urgentInputCancelsTheOldSelectionBeforeStartingAnother() {
        Fixture f;std::atomic<bool> selectorStarted=false,mainStarted=false,oldCancelled=false;std::atomic<int> turns=0;
        f.model->action=[&](const a::ModelRequest& input,const CancellationToken& token)->a::ModelReply {
            if(input.responseSchema["properties"].toObject().contains("selected_memories")) {
                const auto query=QJsonDocument::fromJson(input.messages.first().text.toUtf8()).object()["query"].toString();
                if(query.startsWith("OLD")) {
                    selectorStarted=true;while(!token.isCancelled())QThread::msleep(1);oldCancelled=true;token.throwIfCancelled();
                }
                return {"{\"selected_memories\":[\"new.md\"]}",{},Usage{9,5}};
            }
            for(const auto& message:input.messages)if(message.metadata.contains("iilocal.memory_recall"))return {message.text,{}};
            if(std::any_of(input.messages.cbegin(),input.messages.cend(),[](const auto& m){return m.text.startsWith("NEW");}))
                return {{},{{"wait"+QString::number(++turns),"Yield",{}}}};
            mainStarted=true;while(!token.isCancelled())QThread::msleep(1);token.throwIfCancelled();return {};
        };
        a::Engine engine(f.model,f.tools,f.policy,f.options);const auto id=engine.createSession("fixture",f.work).id;f.seed(engine,id,"OLD_NOTE_412");
        const auto path=QDir(engine.memory(id)["directory"].toString()).filePath("new.md");
        QVERIFY(!engine.runMemoryTool(id,"Write",{{"path",path},{"content","---\ndescription: New requirement\n---\nNEW_NOTE_853"}}).isError);
        auto run=engine.run({id,"OLD deployment question"});QTRY_VERIFY_WITH_TIMEOUT(selectorStarted&&mainStarted,3000);
        engine.enqueueInput(id,{{"text","NEW deployment question"},{"priority","now"}});
        auto result=run.result.get();QVERIFY2(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage));
        QVERIFY(oldCancelled);QVERIFY(result.text.contains("NEW_NOTE_853"));QVERIFY(!result.text.contains("OLD_NOTE_412"));
    }
    void explicitRecallUsesPrivateApiOwnerAndMcpConnection() {
        Fixture f;a::ApiOptions options;options.stateDirectory=f.root.filePath("api");options.workingDirectory=f.work;
        options.engine=f.options;options.engine.sessionsDirectory.clear();options.clientTokens={{"society",QString(48,'a')},{"dreamscapes",QString(48,'b')}};
        a::Api host(f.model,f.tools,f.policy,options);
        const auto first=api(host,"agent.sessions.create",{{"model","fixture"}})["session_id"].toString();
        const auto second=api(host,"agent.sessions.create",{{"model","fixture"}},QString(48,'b'))["session_id"].toString();
        auto path=QDir(api(host,"agent.memory.get",{{"session_id",first}})["directory"].toString()).filePath("topic.md");
        QVERIFY(!api(host,"agent.memory.write",{{"session_id",first},{"path",path},{"content","---\ndescription: Deployment\n---\nOWNER_SECRET_52"}})["is_error"].toBool());
        auto answer=api(host,"agent.memory.recall",{{"session_id",first},{"query","Explain deployment constraints"}});
        QVERIFY(QJsonDocument(answer).toJson().contains("OWNER_SECRET_52"));
        auto isolated=api(host,"agent.memory.recall",{{"session_id",second},{"query","Explain deployment constraints"}},QString(48,'b'));
        QVERIFY(!QJsonDocument(isolated).toJson().contains("OWNER_SECRET_52"));QCOMPARE(isolated["status"].toString(),"no_candidates");
        QVERIFY_EXCEPTION_THROWN(api(host,"agent.memory.recall",{{"session_id",first},{"query","Recall this"}},QString(48,'b')),Error);
        auto engine=std::make_shared<a::Engine>(f.model,f.tools,f.policy,f.options);auto id=engine->createSession("fixture",f.work).id;f.seed(*engine,id,"MCP_SECRET_84");
        a::McpServerOptions mcp;mcp.engine=engine;mcp.model="fixture";mcp.workingDirectory=f.work;
        iiLocalLLM::mcp::ServerSession server(a::mcpServerOptions(f.tools,f.policy,mcp));
        auto initialized=exchange(server,1,"initialize",{{"protocolVersion","2025-03-26"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","fixture"},{"version","1"}}}});
        QVERIFY(initialized.contains("result"));server.receive(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/initialized"}});
        auto result=exchange(server,2,"tools/call",{{"name","iiLocalLLM.agent.memory.recall"},{"arguments",QJsonObject{{"query","Explain deployment constraints"}}}});
        QVERIFY2(QJsonDocument(result).toJson().contains("MCP_SECRET_84"),QJsonDocument(result).toJson().constData());
        auto forged=exchange(server,3,"tools/call",{{"name","iiLocalLLM.agent.memory.recall"},{"arguments",QJsonObject{{"query","Explain deployment constraints"},{"session_id",id}}}});
        QVERIFY(forged.contains("error")||forged["result"].toObject()["isError"].toBool());
    }
};
QTEST_GUILESS_MAIN(MemoryRecallEngineTests)
#include "memory_recall_engine_tests.moc"
