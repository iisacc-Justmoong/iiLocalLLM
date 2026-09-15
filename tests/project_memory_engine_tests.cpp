#include <agent/Engine.h>
#include <agent/Api.h>
#include <agent/McpServer.h>
#include <agent/Subagents.h>
#include <mcp/Server.h>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtTest/QtTest>
#include <mutex>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace m=iiLocalLLM::mcp;
namespace {
class Model final:public a::Model {
public:
    std::mutex mutex;QList<a::ModelRequest> requests;
    bool budgets=false;std::atomic_bool measuredWithMemory=false;
    std::function<a::ModelReply(const a::ModelRequest&)> reply;
    std::optional<ContextBudget> measure(const a::ModelRequest& request,const CancellationToken&)override {
        if(!budgets)return std::nullopt;qint64 count=100+request.systemPrompt.size();
        for(const auto& message:request.messages) {count+=message.text.size()+10;if(message.metadata.contains("iilocal.project_memory"))measuredWithMemory=true;}
        return ContextBudget{count,32768};
    }
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken&,const TextCallback&)override {
        std::lock_guard lock(mutex);requests.append(request);if(reply)return reply(request);
        if(request.summarizing)return {"Earlier requirements and observations remain relevant.",{},Usage{100,12,0,0}};
        for(const auto& message:request.messages)if(message.metadata.contains("iilocal.project_memory"))return {message.text,{}};
        return {"NO_MEMORY",{}};
    }
};
struct Fixture {
    QTemporaryDir root;QString workspace=root.filePath("workspace");
    std::shared_ptr<Model> model=std::make_shared<Model>();
    std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<const a::PermissionPolicy> policy=std::make_shared<a::RulePolicy>(a::PermissionMode::AcceptEdits);
    a::EngineOptions options;
    Fixture() {
        QDir().mkpath(workspace);a::registerWorkspaceTools(*registry,workspace,{},QStringList{root.filePath("state")});
        options.sessionsDirectory=root.filePath("state/sessions");options.projectMemoryEnabled=true;
        options.projectContext.enabled=false;options.skills.enabled=false;options.compaction.automatic=false;
    }
};
QJsonObject call(a::Api& api,const QString& method,QJsonObject args={},const QString& token=QString(48,'a')) {
    return api.dispatch(method,args,token).result.get().toObject();
}
QJsonObject exchange(m::ServerSession& session,int id,const QString& method,QJsonObject args) {
    session.receive({{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",args}});
    for(int i=0;i<200;++i)for(const auto& item:session.takeMessages(20)) {
        const auto response=item.toObject();if(response["id"]==id)return response;
    }
    throw std::runtime_error("MCP response timed out");
}
void initialize(m::ServerSession& session) {
    const auto response=exchange(session,1,"initialize",{{"protocolVersion","2025-11-25"},{"capabilities",QJsonObject{}},
        {"clientInfo",QJsonObject{{"name","memory-test"},{"version","1"}}}});
    if(response.contains("error"))throw std::runtime_error("MCP initialize failed");
    session.receive({{"jsonrpc","2.0"},{"method","notifications/initialized"}});
}
QJsonObject mcpCall(m::ServerSession& session,int id,const QString& tool,QJsonObject args={}) {
    const auto response=exchange(session,id,"tools/call",{{"name",tool},{"arguments",args}});
    if(response.contains("error"))throw std::runtime_error(QJsonDocument(response).toJson().constData());
    return response["result"].toObject();
}
}
class ProjectMemoryEngineTests final:public QObject {
    Q_OBJECT
private slots:
    void nativeToolsSaveAndRefreshMemoryAcrossSessionsAndRestart() {
        Fixture f;QString first,indexPath;
        {
            a::Engine engine(f.model,f.registry,f.policy,f.options);first=engine.createSession("fixture",f.workspace).id;
            indexPath=engine.memory(first)["index_path"].toString();int turn=0;
            f.model->reply=[&](const auto& request)->a::ModelReply {
                if(++turn==1)return {{},{{"save","Write",{{"path",indexPath},{"content","Persistent validation code: MEMORY_742"}}}}};
                for(const auto& message:request.messages)if(message.metadata.contains("iilocal.project_memory"))return {message.text,{}};
                return {"missing",{}};
            };
            auto result=engine.run({first,"Remember the validation code for future sessions."}).result.get();
            QCOMPARE(result.status,a::RunStatus::Completed);QVERIFY(result.text.contains("MEMORY_742"));
            const auto stored=engine.session(first);
            QVERIFY(std::none_of(stored.messages.begin(),stored.messages.end(),[](const auto& message){return message.metadata.contains("iilocal.project_memory");}));
            f.model->reply={};
            const auto fork=engine.forkSession(first);QCOMPARE(engine.memory(fork.id)["index_path"].toString(),indexPath);
            const auto cleared=engine.clearSession(first);QVERIFY(cleared["complete"].toBool());
            const auto newId=cleared["session_id"].toString();QVERIFY(engine.run({newId,"Recall the code."}).result.get().text.contains("MEMORY_742"));
        }
        a::Engine reopened(f.model,f.registry,f.policy,f.options);auto second=reopened.createSession("fixture",f.workspace).id;
        QCOMPARE(reopened.memory(second)["index_path"].toString(),indexPath);
        QVERIFY(reopened.run({second,"What code was saved?"}).result.get().text.contains("MEMORY_742"));
        QCOMPARE(reopened.memory(first)["index_path"].toString(),indexPath);
        auto read=reopened.runMemoryTool(second,"Read",{{"path",indexPath}});QVERIFY(!read.isError);QCOMPARE(read.data["sha256"].toString().size(),64);
    }
    void disablingMemoryDoesNotInjectOrGrantAccess() {
        Fixture f;QString index;
        {a::Engine enabled(f.model,f.registry,f.policy,f.options);auto id=enabled.createSession("fixture",f.workspace).id;
            index=enabled.memory(id)["index_path"].toString();QVERIFY(!enabled.runMemoryTool(id,"Write",{{"path",index},{"content","private note"}}).isError);}
        f.options.projectMemoryEnabled=false;a::Engine disabled(f.model,f.registry,f.policy,f.options);auto id=disabled.createSession("fixture",f.workspace).id;
        QVERIFY(!disabled.memory(id)["enabled"].toBool());QCOMPARE(disabled.run({id,"hello"}).result.get().text,"NO_MEMORY");
        QVERIFY_EXCEPTION_THROWN(disabled.runMemoryTool(id,"Read",{{"path",index}}),Error);
        f.model->reply=[&](const auto& request)->a::ModelReply {
            if(request.messages.last().role==a::MessageRole::Tool)return {request.messages.last().isError?"DENIED":"LEAKED",{}};
            return {{},{{"read","Read",{{"path",index}}}}};
        };
        QCOMPARE(disabled.run({id,"Read the note."}).result.get().text,"DENIED");
    }
    void compactionMeasuresMemoryAndReloadsItWithoutTranscriptDuplication() {
        Fixture f;f.model->budgets=true;a::Engine engine(f.model,f.registry,f.policy,f.options);
        const auto id=engine.createSession("fixture",f.workspace).id;const auto index=engine.memory(id)["index_path"].toString();
        QVERIFY(!engine.runMemoryTool(id,"Write",{{"path",index},{"content","CURRENT_MEMORY_103"}}).isError);
        {a::SessionStore store(f.options.sessionsDirectory);auto lease=store.acquire(id);
            for(int i=0;i<8;++i) {lease->append({{},a::MessageRole::User,"Requirement "+QString::number(i)+QString(512,'x')});lease->append({{},a::MessageRole::Assistant,"Observed requirement "+QString::number(i)});}}
        a::CompactRequest compact;compact.sessionId=id;auto result=engine.compact(compact).result.get();
        QVERIFY2(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage));QVERIFY(f.model->measuredWithMemory.load());
        QVERIFY(!engine.runMemoryTool(id,"Read",{{"path",index}}).isError);
        QVERIFY(!engine.runMemoryTool(id,"Write",{{"path",index},{"content","UPDATED_MEMORY_207"}}).isError);
        result=engine.run({id,"Continue with current memory."}).result.get();QVERIFY(result.text.contains("UPDATED_MEMORY_207"));
        QVERIFY(!result.text.contains("CURRENT_MEMORY_103"));
        const auto stored=engine.session(id);QVERIFY(!stored.compactions.isEmpty());
        QVERIFY(std::none_of(stored.messages.begin(),stored.messages.end(),[](const auto& message){return message.metadata.contains("iilocal.project_memory");}));
    }
    void readOnlyChildSharesMemoryButDoesNotGainWriteTools() {
        Fixture f;f.policy=std::make_shared<a::RulePolicy>(a::PermissionMode::AcceptEdits,QList<a::PermissionRule>{{"Agent",a::PermissionBehavior::Allow}});
        a::SubagentOptions options;options.workingDirectory=f.workspace;options.stateDirectory=f.root.filePath("children");
        a::SubagentDefinition role;role.name="recaller";role.tools={"Read","Glob","Grep"};role.readOnly=true;options.definitions={role};
        auto children=std::make_shared<a::Subagents>(f.model,f.registry,f.policy,f.options,options);a::Subagents::attach(f.options,children);
        a::Engine engine(f.model,f.registry,f.policy,f.options);const auto id=engine.createSession("fixture",f.workspace).id;
        QVERIFY(!engine.runMemoryTool(id,"Write",{{"path",engine.memory(id)["index_path"]},{"content","PARENT_MEMORY_498"}}).isError);
        const auto result=engine.runSubagentTool(id,"Agent",{{"prompt","Recall project context."},{"subagent_type","recaller"}});
        QVERIFY2(!result.isError,qPrintable(result.text));QVERIFY(result.data["result"].toObject()["text"].toString().contains("PARENT_MEMORY_498"));
        QVERIFY(std::none_of(f.model->requests.last().tools.begin(),f.model->requests.last().tools.end(),[](const auto& tool){return tool.name=="Write"||tool.name=="MemoryForget";}));
    }
    void authenticatedApiKeepsTwoAppMemoriesSeparateAndBoundsToolPaths() {
        Fixture f;a::ApiOptions options;options.workingDirectory=f.workspace;options.stateDirectory=f.root.filePath("api-state");
        options.engine=f.options;options.engine.sessionsDirectory.clear();options.clientTokens={{"society",QString(48,'a')},{"dreamscapes",QString(48,'b')}};
        a::Api api(f.model,f.registry,f.policy,options);
        const auto first=call(api,"agent.sessions.create",{{"model","fixture"}})["session_id"].toString();
        const auto second=call(api,"agent.sessions.create",{{"model","fixture"}},QString(48,'b'))["session_id"].toString();
        QVERIFY(call(api,"agent.info")["project_memory_enabled"].toBool());
        const auto one=call(api,"agent.memory.get",{{"session_id",first}}),two=call(api,"agent.memory.get",{{"session_id",second}},QString(48,'b'));
        QVERIFY(one["directory"]!=two["directory"]);const auto index=one["index_path"].toString();
        QVERIFY(!call(api,"agent.memory.write",{{"session_id",first},{"path",index},{"content","Society tenant note"}})["is_error"].toBool());
        const auto read=call(api,"agent.memory.read",{{"session_id",first},{"path",index}});QCOMPARE(read["text"].toString(),"Society tenant note");
        QVERIFY_EXCEPTION_THROWN(call(api,"agent.memory.get",{{"session_id",first}},QString(48,'b')),Error);
        QVERIFY_EXCEPTION_THROWN(call(api,"agent.memory.get",{{"session_id",first}},"invalid"),Error);
        QVERIFY_EXCEPTION_THROWN(call(api,"agent.memory.read",{{"session_id",second},{"path",index}},QString(48,'b')),Error);
        QVERIFY_EXCEPTION_THROWN(call(api,"agent.memory.write",{{"session_id",first},{"path",QDir(f.workspace).filePath("escape.md")},{"content","escape"}}),Error);
        QVERIFY(!QFileInfo::exists(QDir(f.workspace).filePath("escape.md")));
        const auto forgotten=call(api,"agent.memory.forget",{{"session_id",first},{"path",index},{"sha256",read["result"].toObject()["sha256"]}});
        QVERIFY(!forgotten["is_error"].toBool());QVERIFY(!QFileInfo::exists(index));
        QCOMPARE(call(api,"agent.memory.get",{{"session_id",second}},QString(48,'b'))["index"].toString(),QString());
    }
    void inputChangingHookCannotEscapeMemoryApi() {
        Fixture f;const auto escaped=QDir(f.workspace).filePath("escape.md");
        f.options.hooks.append([&](const a::HookInput& input,const CancellationToken&) {
            a::HookResult result;if(input.kind==a::HookKind::BeforeTool&&input.call.name=="Write") {
                auto args=input.call.arguments;args["path"]=escaped;result.updatedArguments=args;
            }return result;
        });
        a::Engine engine(f.model,f.registry,f.policy,f.options);auto id=engine.createSession("fixture",f.workspace).id;
        auto result=engine.runMemoryTool(id,"Write",{{"path",engine.memory(id)["index_path"]},{"content","escape"}});
        QVERIFY(result.isError);QVERIFY(!QFileInfo::exists(escaped));
    }
    void mcpPublishesMemoryAndRoutesNativeFilesToTheOwnedProject() {
        Fixture f;auto engine=std::make_shared<a::Engine>(f.model,f.registry,f.policy,f.options);
        a::McpServerOptions options;options.workingDirectory=f.workspace;options.engine=engine;options.model="fixture";
        m::ServerSession session(a::mcpServerOptions(f.registry,f.policy,options));initialize(session);
        const auto state=mcpCall(session,2,"iiLocalLLM.agent.memory.get")["structuredContent"].toObject();
        const auto index=state["index_path"].toString();QVERIFY(!index.isEmpty());
        auto written=mcpCall(session,3,"Write",{{"path",index},{"content","MCP_MEMORY_319"}});QVERIFY(!written["isError"].toBool());
        const auto read=mcpCall(session,4,"Read",{{"path",index}});QVERIFY(!read["isError"].toBool());
        QVERIFY(QJsonDocument(read).toJson().contains("MCP_MEMORY_319"));
        auto deleted=mcpCall(session,5,"MemoryForget",{{"path",index},{"sha256",read["structuredContent"].toObject()["sha256"]}});
        QVERIFY(!deleted["isError"].toBool());QVERIFY(!QFileInfo::exists(index));
        auto forged=exchange(session,6,"tools/call",{{"name","iiLocalLLM.agent.memory.get"},{"arguments",QJsonObject{{"session_id","foreign"}}}});
        QVERIFY(forged.contains("error")||forged["result"].toObject()["isError"].toBool());
    }
};
QTEST_GUILESS_MAIN(ProjectMemoryEngineTests)
#include "project_memory_engine_tests.moc"
