#include "agent/PluginRuntime.h"
#include "agent/Api.h"
#include "agent/McpServer.h"
#include "mcp/Server.h"
#include <QtTest/QtTest>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void put(const QString& path,const QByteArray& bytes){
    if(!QDir().mkpath(QFileInfo(path).absolutePath()))qFatal("mkdir failed");
    QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())qFatal("write failed");
}
void json(const QString& path,const QJsonObject& o){put(path,QJsonDocument(o).toJson());}
QByteArray read(const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))qFatal("read failed");return f.readAll();}
class Model final:public a::Model{
public:
    QList<a::ModelRequest> requests;
    a::ModelReply generate(const a::ModelRequest& r,const CancellationToken&,const TextCallback&) override{requests.append(r);return {r.messages.last().text,{}};}
};
struct Fixture{
    QTemporaryDir root{QDir::current().filePath("plg-XXXXXX")};
    QString workspace=root.filePath("workspace"),package=root.filePath("package");
    a::PluginStore store{{root.filePath("store${UNSET_PLUGIN_VARIABLE}")}};
    Fixture(){QDir().mkpath(workspace);manifest();put(package+"/skills/inspect/SKILL.md","---\ndescription: Inspect plugin data\n---\nPLUGIN_SKILL $0 ${CLAUDE_PLUGIN_ROOT} ${CLAUDE_PLUGIN_DATA}\n");}
    void manifest(QJsonObject extra={}){extra["name"]="sample";if(!extra.contains("version"))extra["version"]="1.0.0";json(package+"/.claude-plugin/plugin.json",extra);}
    a::PluginSnapshot install(){store.install(package);return store.snapshot();}
    std::shared_ptr<a::PluginRuntime> runtime(const a::PluginSnapshot& s){a::CommandHookOptions o;o.workingDirectory=workspace;return std::make_shared<a::PluginRuntime>(s,o);}
    a::EngineOptions engineOptions(){a::EngineOptions e;e.sessionsDirectory=root.filePath("sessions");e.projectContext.enabled=false;e.compaction.automatic=false;return e;}
    a::AgentProfileOptions profiles(){a::AgentProfileOptions p;p.includeProject=false;p.includeBuiltins=false;return p;}
    a::McpConnectionOptions connections(){a::McpConnectionOptions m;m.workingDirectory=workspace;m.refreshIntervalMs=0;return m;}
};
}
class PluginTests final:public QObject{
    Q_OBJECT
private slots:
    void additiveNamespacedDocumentsAndPrivateStatus(){
        Fixture f;
        put(f.package+"/commands/nested/legacy.md","---\ndescription: Legacy command\n---\nLEGACY $ARGUMENTS");
        put(f.package+"/extra/task.md","---\ndescription: Extra command\n---\nEXTRA");
        put(f.package+"/agents/reviewer.md","---\nname: reviewer\ndescription: Plugin reviewer\ntools: [Read]\nskills: [inspect]\n---\nREVIEW ${CLAUDE_PLUGIN_ROOT}");
        f.manifest({{"commands",QJsonArray{"./commands","./extra/task.md"}},{"agents","./agents/reviewer.md"},{"settings",QJsonObject{{"private","PRIVATE_MARKER"}}}});
        const auto s=f.install();QCOMPARE(s.skills.size(),3);QCOMPARE(s.agents.size(),1);
        auto e=f.engineOptions();auto p=f.profiles();auto m=f.connections();a::PluginRuntime::attach(e,p,m,f.runtime(s));
        const auto catalog=a::discoverSkills(f.workspace,e.skills);QCOMPARE(catalog.skills.size(),3);
        const auto loaded=a::loadSkill(f.workspace,"sample:nested:legacy","value","session",a::SkillInvocationSource::User,e.skills);QVERIFY(loaded.text.contains("LEGACY value"));
        const auto profile=a::discoverAgentProfiles(f.workspace,p).find("sample:reviewer");QCOMPARE(profile.skills,QStringList{"sample:inspect"});QVERIFY(profile.systemPrompt.contains(s.plugins[0].root));
        const auto info=QJsonDocument(e.pluginSnapshot->toJson()).toJson();QVERIFY(!info.contains("PRIVATE_MARKER"));QVERIFY(!info.contains(f.root.path().toUtf8()));
        QVERIFY(s.plugins[0].unsupportedFeatures.contains("settings"));
        QVERIFY_THROWS_EXCEPTION(Error,a::PluginRuntime::attach(e,p,m,f.runtime(s)));
    }
    void revisionsAreStableAndDataSurvivesDisableUpdateAndRemoval(){
        Fixture f;const auto first=f.install();auto e=f.engineOptions();auto p=f.profiles();auto m=f.connections();a::PluginRuntime::attach(e,p,m,f.runtime(first));
        const auto old=a::loadSkill(f.workspace,"sample:inspect","first","s",a::SkillInvocationSource::User,e.skills).text;
        put(first.plugins[0].dataDirectory+"/keep.txt","KEEP");
        f.manifest({{"version","2.0.0"}});put(f.package+"/skills/inspect/SKILL.md","NEW_REVISION $ARGUMENTS");const auto newer=f.install();
        QVERIFY(first.revision!=newer.revision);QVERIFY(first.plugins[0].root!=newer.plugins[0].root);QCOMPARE(newer.plugins[0].dataDirectory,first.plugins[0].dataDirectory);
        QCOMPARE(a::loadSkill(f.workspace,"sample:inspect","first","s",a::SkillInvocationSource::User,e.skills).text,old);
        f.store.setEnabled("sample",false);QVERIFY(f.store.snapshot().skills.isEmpty());QCOMPARE(f.store.snapshot().plugins[0].status,"disabled");
        f.store.setEnabled("sample",true);QCOMPARE(f.store.snapshot().skills.size(),1);
        f.store.uninstall("sample");QVERIFY(f.store.list().isEmpty());QVERIFY(f.store.snapshot().plugins.isEmpty());
        QCOMPARE(read(first.plugins[0].dataDirectory+"/keep.txt"),QByteArray("KEEP"));QVERIFY(QFileInfo::exists(first.skills[0].path));
        QCOMPARE(a::loadSkill(f.workspace,"sample:inspect","first","s",a::SkillInvocationSource::User,e.skills).text,old);
    }
    void dependencyAvailabilityAndCyclesBlockConfiguration(){
        Fixture f;f.manifest({{"dependencies",QJsonArray{"base"}}});f.install();auto s=f.store.snapshot();
        QCOMPARE(s.plugins[0].status,"blocked");QVERIFY(s.skills.isEmpty());
        const auto base=f.root.filePath("base");json(base+"/.claude-plugin/plugin.json",{{"name","base"},{"version","1"}});
        f.store.install(base);s=f.store.snapshot();QCOMPARE(s.skills.size(),1);
        f.store.setEnabled("base",false);QVERIFY(f.store.snapshot().skills.isEmpty());f.store.setEnabled("base",true);
        json(base+"/.claude-plugin/plugin.json",{{"name","base"},{"version","1"},{"dependencies",QJsonArray{"sample"}}});
        f.store.install(base);s=f.store.snapshot();QVERIFY(s.skills.isEmpty());for(const auto& p:s.plugins)QCOMPARE(p.status,"blocked");
        f.manifest({{"dependencies",QJsonArray{"remote@marketplace"}}});f.install();QVERIFY(f.store.snapshot().skills.isEmpty());
    }
    void invalidUpdatesAreAtomicAndPackagesCannotEscape(){
        Fixture f;const auto first=f.install();const auto selected=f.store.list();
        f.manifest({{"commands","../outside.md"}});QVERIFY_THROWS_EXCEPTION(Error,f.store.install(f.package));QCOMPARE(f.store.list(),selected);
        f.manifest({{"skills",QJsonArray{QJsonArray{true}}}});QVERIFY_THROWS_EXCEPTION(Error,f.store.install(f.package));
        f.manifest();QVERIFY(QFile::link(first.skills[0].path,f.package+"/escape"));QVERIFY_THROWS_EXCEPTION(Error,f.store.install(f.package));
        QVERIFY(QFile::remove(f.package+"/escape"));a::PluginStoreOptions limited;limited.directory=f.root.filePath("limited");limited.maxFileBytes=8;
        a::PluginStore small(limited);QVERIFY_THROWS_EXCEPTION(Error,small.install(f.package));QVERIFY(small.list().isEmpty());
        CancellationToken cancelled;cancelled.cancel();QVERIFY_THROWS_EXCEPTION(Error,f.store.install(f.package,true,cancelled));QCOMPARE(f.store.list(),selected);
        QVERIFY_THROWS_EXCEPTION(Error,a::PluginStore({f.root.filePath("package/store")}).install(f.package));
    }
    void tamperedCacheCannotBecomeASelectedPrompt(){
        Fixture f;const auto s=f.install();auto e=f.engineOptions();auto p=f.profiles();auto m=f.connections();a::PluginRuntime::attach(e,p,m,f.runtime(s));
        put(s.skills[0].path,"TAMPERED");
        QVERIFY_THROWS_EXCEPTION(Error,f.store.snapshot());
        QVERIFY_THROWS_EXCEPTION(Error,a::loadSkill(f.workspace,"sample:inspect","","s",a::SkillInvocationSource::User,e.skills));
        QVERIFY_THROWS_EXCEPTION(Error,f.store.install(f.package));
    }
    void actualSkillAndSubagentUseThePluginSnapshot(){
        Fixture f;put(f.package+"/agents/reviewer.md","---\nname: reviewer\ndescription: Reviewer\ntools: [Read]\nskills: [inspect]\n---\nPLUGIN_AGENT ${CLAUDE_PLUGIN_ROOT}");
        put(f.package+"/commands/review.md","---\ndescription: Review in plugin child\ncontext: fork\nagent: reviewer\n---\nPLUGIN_FORK $ARGUMENTS");
        auto e=f.engineOptions();auto p=f.profiles();auto m=f.connections();a::PluginRuntime::attach(e,p,m,f.runtime(f.install()));
        auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,f.workspace);
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::SubagentOptions so;so.workingDirectory=f.workspace;so.stateDirectory=f.root.filePath("children");so.profiles=p;
        auto agents=std::make_shared<a::Subagents>(model,registry,policy,e,so);a::Subagents::attach(e,agents);
        a::Engine engine(model,registry,policy,e);const auto session=engine.createSession("fixture",f.workspace);
        a::RunRequest request{session.id};request.skill="sample:inspect";request.skillArguments="'two words'";
        const auto result=engine.run(request).result.get();QVERIFY2(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage));QVERIFY(result.text.contains("PLUGIN_SKILL two words"));
        const auto child=engine.runSubagentTool(session.id,"Agent",{{"subagent_type","sample:reviewer"},{"prompt","Review the file"}});
        QVERIFY2(!child.isError,qPrintable(child.text));QVERIFY(!agents->list(session.id).isEmpty());
        const auto& r=model->requests.last();QVERIFY(r.systemPrompt.contains("PLUGIN_AGENT"));
        QString history;for(const auto& message:r.messages)history+=message.text;QVERIFY(history.contains("PLUGIN_SKILL"));
        for(const auto& tool:r.tools)QVERIFY(tool.name=="Read"); // No plugin reattachment widens a child profile.
        a::RunRequest fork{session.id};fork.skill="sample:review";fork.skillArguments="fork argument";
        const auto forked=engine.run(fork).result.get();QVERIFY2(forked.status==a::RunStatus::Completed,qPrintable(forked.errorMessage));QVERIFY(forked.text.contains("PLUGIN_FORK fork argument"));
    }
    void unsupportedServersAndDisabledDiscoveryAreExplicit(){
        Fixture f;
        put(f.package+"/agents/nested/file.md","---\nname: semantic-name\n---\nAgent body.");
        f.manifest({{"mcpServers",QJsonObject{{"legacy",QJsonObject{{"type","sse"},{"url","https://example.invalid/"}}},
            {"oauth",QJsonObject{{"type","http"},{"url","https://example.invalid/"},{"oauth",QJsonObject{}}}}}},
            {"lspServers",QJsonObject{{"network",QJsonObject{{"command","unused"},{"transport","tcp"},{"extensionToLanguage",QJsonObject{{".cpp","cpp"}}}}}}}});
        const auto s=f.install();QVERIFY(s.mcpServers.isEmpty());QVERIFY(s.lsp.servers.isEmpty());QCOMPARE(s.agents[0].name,"sample:nested:semantic-name");
        QVERIFY(s.plugins[0].unsupportedFeatures.contains("mcpServers.legacy:transport"));QVERIFY(s.plugins[0].unsupportedFeatures.contains("mcpServers.oauth.oauth"));
        auto e=f.engineOptions();e.skills.enabled=false;auto p=f.profiles();p.enabled=false;auto m=f.connections();
        a::PluginRuntime::attach(e,p,m,f.runtime(s));QVERIFY(a::discoverSkills(f.workspace,e.skills).skills.isEmpty());
        for(const auto& profile:a::discoverAgentProfiles(f.workspace,p).profiles)QVERIFY(!profile.name.startsWith("sample:"));
    }
    void mcpProcessAndLspProtocolUsePackageResources(){
        Fixture f;QDir().mkpath(f.package+"/bin");QVERIFY(QFile::copy(PLUGIN_PEER,f.package+"/bin/peer"));QVERIFY(QFile::copy(LSP_FIXTURE,f.package+"/bin/lsp"));
        const QJsonObject peer{{"command","${CLAUDE_PLUGIN_ROOT}/bin/peer"},{"args",QJsonArray{"${CLAUDE_PLUGIN_ROOT}"}},{"env",QJsonObject{{"PLUGIN_SECRET","PRIVATE_MARKER"}}}};
        json(f.package+"/.mcp.json",{{"mcpServers",QJsonObject{{"process",peer}}}});
        json(f.package+"/.lsp.json",{{"language",QJsonObject{{"command","${CLAUDE_PLUGIN_ROOT}/bin/lsp"},{"args",QJsonArray{"","${CLAUDE_PLUGIN_DATA}/lsp.jsonl"}},{"extensionToLanguage",QJsonObject{{".cpp","cpp"}}}}}});
        const auto snapshot=f.install();auto e=f.engineOptions();auto p=f.profiles();auto m=f.connections();a::PluginRuntime::attach(e,p,m,f.runtime(snapshot));
        QCOMPARE(snapshot.lsp.servers[0].name,"plugin:sample:language");
        auto registry=std::make_shared<a::ToolRegistry>();a::McpConnections connections(registry,m);
        const auto client=connections.client("plugin:sample:process");QVERIFY(client);const auto result=client->callTool("echo",{});
        QCOMPARE(registry->definitions()[0].name,"mcp__plugin_sample_process__echo");
        QCOMPARE(a::mcpTools(client,{"host.name",{},false})[0].definition.name,"mcp__host.name__echo");
        const auto normalized=a::mcpTools(client,{"plugin:source.name:peer",{},false,true});
        QCOMPARE(normalized[0].definition.name,"mcp__plugin_source_name_peer__echo");
        QCOMPARE(normalized[0].definition.metadata["remote_name"],"echo");
        const auto observed=result["structuredContent"].toObject();QCOMPARE(observed["root"],snapshot.plugins[0].root);QCOMPARE(observed["argument"],snapshot.plugins[0].root);
        QCOMPARE(observed["data"],snapshot.plugins[0].dataDirectory);QCOMPARE(observed["secret"],"PRIVATE_MARKER");
        QVERIFY(!QJsonDocument(QJsonObject{{"status",connections.status()}}).toJson().contains("PRIVATE_MARKER"));
        put(f.workspace+"/main.cpp","int main() { return 0; }\n");a::Lsp lsp(e.lsp);
        a::ToolContext context;context.workingDirectory=f.workspace;context.sessionId="plugin-session";
        const auto hover=lsp.query({{"operation","hover"},{"filePath","main.cpp"},{"line",1},{"character",1}},context);
        QVERIFY2(!hover.isError,qPrintable(hover.text));QVERIFY(hover.text.contains("한글"));QVERIFY(QFileInfo::exists(snapshot.plugins[0].dataDirectory+"/lsp.jsonl"));
        lsp.close();connections.close();
    }
    void commandHooksReceivePluginEnvironmentWithoutPathReevaluation(){
#if defined(Q_OS_UNIX)
        Fixture f;put(f.package+"/hook.sh","#!/bin/sh\ncat > \"$CLAUDE_PLUGIN_DATA/hook.json\"\nprintf '{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\",\"additionalContext\":\"PLUGIN_HOOK\"}}'\n");
        QVERIFY(QFile::setPermissions(f.package+"/hook.sh",QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        json(f.package+"/hooks/hooks.json",{{"hooks",QJsonObject{{"PreToolUse",QJsonArray{QJsonObject{{"matcher","Read"},{"hooks",QJsonArray{QJsonObject{{"type","command"},{"command","\"${CLAUDE_PLUGIN_ROOT}/hook.sh\""}}}}}}}}}});
        const auto s=f.install();auto e=f.engineOptions();auto p=f.profiles();auto m=f.connections();a::PluginRuntime::attach(e,p,m,f.runtime(s));
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,f.workspace);put(f.workspace+"/input.txt","INPUT");
        a::ToolRunnerOptions options;options.hooks=e.hooks;a::ToolRunner runner(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),options);
        a::ToolContext context;context.workingDirectory=f.workspace;context.sessionId="hook-session";
        const auto result=runner.run({"read","Read",{{"path","input.txt"}}},context);QVERIFY2(!result.isError,qPrintable(result.text));
        const auto observed=QJsonDocument::fromJson(read(s.plugins[0].dataDirectory+"/hook.json")).object();QCOMPARE(observed["hook_event_name"],"PreToolUse");
#endif
    }
    void authenticatedApiAndMcpExposeTheSameRedactedSnapshot(){
        Fixture f;auto e=f.engineOptions();auto p=f.profiles();auto m=f.connections();const auto s=f.install();a::PluginRuntime::attach(e,p,m,f.runtime(s));
        auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>();
        a::ApiOptions options;options.workingDirectory=f.workspace;options.stateDirectory=f.root.filePath("api");options.engine=e;options.engine.sessionsDirectory.clear();options.clientTokens={{"society",QString(48,'a')},{"dreamscapes",QString(48,'b')}};
        a::Api api(model,registry,policy,options);
        const auto result=api.dispatch("agent.plugins.list",{},QString(48,'a')).result.get().toObject();QCOMPARE(result,s.toJson());
        QCOMPARE(api.dispatch("agent.plugins.list",{},QString(48,'b')).result.get().toObject(),result);
        QVERIFY_THROWS_EXCEPTION(Error,api.dispatch("agent.plugins.list",{},QString(48,'x')).result.get());
        QVERIFY_THROWS_EXCEPTION(Error,api.dispatch("agent.plugins.list",{{"install","unexpected"}},QString(48,'a')).result.get());
        auto engine=std::make_shared<a::Engine>(model,registry,policy,e);a::McpServerOptions mo;mo.engine=engine;mo.model="fixture";mo.workingDirectory=f.workspace;
        mcp::ServerSession server(a::mcpServerOptions(registry,policy,mo));
        auto rpc=[&](int id,QString method,QJsonObject params){server.receive(QJsonObject{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}});
            for(int n=0;n<400;++n)for(const auto& v:server.takeMessages(10))if(v.toObject()["id"]==id)return v.toObject();throw std::runtime_error("MCP timeout");};
        rpc(1,"initialize",{{"protocolVersion","2025-11-25"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","fixture"},{"version","1"}}}});
        server.receive(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/initialized"}});
        const auto response=rpc(2,"tools/call",{{"name","iiLocalLLM.agent.plugins.list"},{"arguments",QJsonObject{}}})["result"].toObject();
        QVERIFY(!response["isError"].toBool());QCOMPARE(response["structuredContent"].toObject(),result);api.close();
    }
    void cliLifecycleReturnsReviewableResults(){
        Fixture f;
        auto call=[&](const QStringList& args){QProcess p;p.setProgram(PLUGIN_CLI);p.setArguments(QStringList{"--store",f.store.directory()}+args);p.start();
            if(!p.waitForFinished(10000)||p.exitCode()!=0)throw std::runtime_error(p.readAllStandardError().toStdString());return QJsonDocument::fromJson(p.readAllStandardOutput()).object();};
        QCOMPARE(call({"install",f.package})["name"],"sample");QCOMPARE(call({"list"})["plugins"].toArray().size(),1);
        QVERIFY(call({"disable","sample"})["changed"].toBool());QCOMPARE(call({"inspect"})["plugins"].toArray()[0].toObject()["status"],"disabled");
        QVERIFY(call({"enable","sample"})["changed"].toBool());QVERIFY(call({"uninstall","sample"})["cache_and_data_retained"].toBool());QVERIFY(call({"list"})["plugins"].toArray().isEmpty());
    }
};
QTEST_GUILESS_MAIN(PluginTests)
#include "plugin_tests.moc"
