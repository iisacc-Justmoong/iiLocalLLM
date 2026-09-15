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
    std::atomic<bool> search=false,slow=false,started=false;QJsonObject seen;
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken& token,const TextCallback&)override {
        started=true;while(slow&&!token.isCancelled())QThread::msleep(1);token.throwIfCancelled();
        if(!search)return {"Acknowledged."};
        if(request.messages.last().role==a::MessageRole::Tool){seen=request.messages.last().data;return {"History found."};}
        if(std::none_of(request.tools.begin(),request.tools.end(),[](const auto& t){return t.name=="SessionSearch";}))throw std::runtime_error("Missing eager SessionSearch");
        return {{},{{"search","SessionSearch",{{"query","DEPLOY_LABEL"}}}}};
    }
};
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("history-engine-XXXXXX")};QString work=root.filePath("work");
    std::shared_ptr<Model> model=std::make_shared<Model>();std::shared_ptr<a::ToolRegistry> tools=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::RulePolicy> policy=std::make_shared<a::RulePolicy>();a::EngineOptions options;
    Fixture(){QDir().mkpath(work);a::registerWorkspaceTools(*tools,work);options.sessionsDirectory=root.filePath("sessions");
        options.sessionHistoryEnabled=true;options.skills.enabled=false;options.projectContext.enabled=false;options.toolSearch.enabled=false;options.compaction.automatic=false;}
};
QJsonObject api(a::Api& host,const QString& method,QJsonObject params={},const QString& credential=QString(48,'a')) {
    return host.dispatch(method,params,credential).result.get().toObject();
}
QJsonObject rpc(iiLocalLLM::mcp::ServerSession& server,int id,const QString& method,QJsonObject params={}) {
    server.receive(QJsonObject{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}});
    for(int i=0;i<300;++i)for(const auto& item:server.takeMessages(10))if(item.toObject()["id"]==id)return item.toObject();
    throw std::runtime_error("MCP response timed out");
}
}
class SessionHistoryEngineTests:public QObject {
    Q_OBJECT
private slots:
    void modelSearchFindsSavedSessionAndDirectSearchDoesNotMutateHistory() {
        Fixture f;a::Engine engine(f.model,f.tools,f.policy,f.options);
        auto old=engine.createSession("fixture",f.work).id;QCOMPARE(engine.run({old,"DEPLOY_LABEL is ORCHID"}).result.get().status,a::RunStatus::Completed);
        const auto current=engine.createSession("fixture",f.work).id;f.model->search=true;
        const auto result=engine.run({current,"Find the previous deployment label."}).result.get();QCOMPARE(result.status,a::RunStatus::Completed);
        const auto found=f.model->seen["matches"].toArray();QCOMPARE(found.size(),1);QCOMPARE(found.first().toObject()["session_id"].toString(),old);
        const auto count=engine.session(current).messages.size();QVERIFY(!engine.runSessionSearch(current,{{"query","ORCHID"}}).isError);
        QCOMPARE(engine.session(current).messages.size(),count);QCOMPARE(engine.session(old).messages.size(),2);
    }
    void activeRunAllowsReadAndExplicitDenyWins() {
        Fixture f;a::Engine engine(f.model,f.tools,f.policy,f.options);const auto id=engine.createSession("fixture",f.work).id;
        f.model->slow=true;auto handle=engine.run({id,"active needle"});QTRY_VERIFY_WITH_TIMEOUT(f.model->started,3000);
        const auto searched=engine.runSessionSearch(id,{{"query","active needle"},{"session_ids",QJsonArray{id}}});QVERIFY(!searched.isError);QCOMPARE(searched.data["matches"].toArray().size(),1);
        handle.cancel();QCOMPARE(handle.result.get().status,a::RunStatus::Cancelled);f.model->slow=false;
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"SessionSearch",a::PermissionBehavior::Deny}});
        a::Engine denied(f.model,f.tools,policy,f.options);QVERIFY(denied.runSessionSearch(id,{{"query","active needle"},{"session_ids",QJsonArray{id}}}).isError);
        f.options.sessionHistoryEnabled=false;a::Engine disabled(f.model,f.tools,f.policy,f.options);QVERIFY(!disabled.sessionSearchTool());
        QVERIFY_THROWS_EXCEPTION(Error,disabled.runSessionSearch(id,{{"query","needle"}}));
    }
    void apiSeparatesClientsAndRejectsHostScopeArguments() {
        Fixture f;a::ApiOptions options;options.stateDirectory=f.root.filePath("api");options.workingDirectory=f.work;
        options.engine=f.options;options.engine.sessionsDirectory.clear();options.clientTokens={{"society",QString(48,'a')},{"dreamscapes",QString(48,'b')}};
        a::Api host(f.model,f.tools,f.policy,options);QVERIFY(api(host,"agent.info")["session_history_enabled"].toBool());
        const auto id=api(host,"agent.sessions.create",{{"model","fixture"}})["session_id"].toString();
        const auto other=api(host,"agent.sessions.create",{{"model","fixture"}},QString(48,'b'))["session_id"].toString();
        api(host,"agent.run",{{"session_id",id},{"prompt","SOC_SECRET"}});api(host,"agent.run",{{"session_id",other},{"prompt","DREAM_SECRET"}},QString(48,'b'));
        const auto page=api(host,"agent.sessions.search",{{"session_id",id},{"query","SECRET"},{"session_ids",QJsonArray{id}}});QVERIFY(!page["is_error"].toBool());QCOMPARE(page["result"].toObject()["matches"].toArray().size(),1);
        QVERIFY_THROWS_EXCEPTION(Error,api(host,"agent.sessions.search",{{"session_id",id},{"query","SECRET"}},QString(48,'b')));
        QVERIFY(api(host,"agent.sessions.search",{{"session_id",id},{"query","SECRET"},{"session_ids",QJsonArray{other}}})["is_error"].toBool());
        QVERIFY(api(host,"agent.sessions.search",{{"session_id",id},{"query","SECRET"},{"workspace",f.work}})["is_error"].toBool());
    }
    void mcpBindsSearchToConnectionAndAdvertisesDefinition() {
        Fixture f;int beforeTools=0;f.options.hooks.append([&](const a::HookInput& input,const auto&) {
            if(input.kind==a::HookKind::BeforeTool&&input.call.name=="SessionSearch")++beforeTools;return a::HookResult{};
        });auto engine=std::make_shared<a::Engine>(f.model,f.tools,f.policy,f.options);
        const auto old=engine->createSession("fixture",f.work).id;QCOMPARE(engine->run({old,"MCP_HISTORY_NEEDLE"}).result.get().status,a::RunStatus::Completed);
        a::McpServerOptions options;options.engine=engine;options.model="fixture";options.workingDirectory=f.work;
        options.tools.hooks=f.options.hooks;
        iiLocalLLM::mcp::ServerSession server(a::mcpServerOptions(f.tools,f.policy,options));
        const auto init=rpc(server,1,"initialize",{{"protocolVersion","2025-11-25"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","test"},{"version","1"}}}});
        QVERIFY(QJsonDocument(init).toJson().contains("iisacc/sessionHistory"));server.receive(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/initialized"}});
        const auto found=rpc(server,2,"tools/call",{{"name","SessionSearch"},{"arguments",QJsonObject{{"query","MCP_HISTORY_NEEDLE"}}}});
        QVERIFY2(!found.contains("error")&&!found["result"].toObject()["isError"].toBool(),QJsonDocument(found).toJson().constData());
        QVERIFY2(found["result"].toObject()["structuredContent"].toObject()["matches"].toArray().size()==1,QJsonDocument(found).toJson().constData());
        QCOMPARE(beforeTools,1);
        const auto forged=rpc(server,3,"tools/call",{{"name","SessionSearch"},{"arguments",QJsonObject{{"query","needle"},{"session_id",old}}}});
        QVERIFY(forged.contains("error")||forged["result"].toObject()["isError"].toBool());
    }
};
QTEST_GUILESS_MAIN(SessionHistoryEngineTests)
#include "session_history_engine_tests.moc"
