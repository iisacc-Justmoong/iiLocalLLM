#include "agent/AgentProfiles.h"
#include "agent/Subagents.h"
#include "tools/AgentProfileConfig.h"
#include <QtTest/QtTest>
#include <QtCore/QTemporaryDir>
#include <QtCore/QDir>
#include <QtCore/QFile>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void put(const QString& path,const QByteArray& bytes) {
    if(!QDir().mkpath(QFileInfo(path).absolutePath()))qFatal("mkdir failed");
    QFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size())qFatal("write failed");
}
QByteArray profile(const QByteArray& description,const QByteArray& extra={}) {
    return "---\nname: reviewer\ndescription: "+description+"\n"+extra+"---\nInspect the requested file and report observed findings.\n";
}
}
class AgentProfileTests final:public QObject {
    Q_OBJECT
private slots:
    void hostConfigValidationAndCatalogPrivacy() {
        QTemporaryDir root;const auto path=root.filePath("host.json");
        put(path,"{\"directories\":[\"profiles\"],\"include_builtins\":false,\"model_aliases\":{\"small\":\"local\"},\"allowed_models\":[\"other\"]}");
        QVERIFY(QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::WriteOwner));
        const auto configured=iiLocalLLMClient::profileConfig(path,false);
        QCOMPARE(configured.profiles.directories,QStringList{root.filePath("profiles")});QVERIFY(!configured.profiles.includeBuiltins);
        QCOMPARE(configured.modelAliases.value("small"),"local");QCOMPARE(configured.allowedModels,QStringList{"other"});
        QVERIFY_THROWS_EXCEPTION(Error,iiLocalLLMClient::profileConfig(path,true));
        for(const auto& invalid:{"{\"unknown\":true}","{\"overrides\":[]}","{\"model_aliases\":{\"small\":false}}","{\"include_project\":1}","{\"directories\":[\"https://invalid/\"]}"}) {
            put(path,invalid);QVERIFY_THROWS_EXCEPTION(Error,iiLocalLLMClient::profileConfig(path,false));
        }
        a::AgentProfileOptions options;options.projectBoundary=root.path();
        put(root.filePath(".claude/agents/read.md"),profile("unsupported","mcpServers:\n  remote:\n    env:\n      SECRET: PRIVATE_MARKER\n"));
        const auto catalog=a::discoverAgentProfiles(root.path(),options);QVERIFY(catalog.find("reviewer").unsupportedFeatures.contains("mcpServers"));
        QVERIFY(!QJsonDocument(catalog.toJson()).toJson().contains("PRIVATE_MARKER"));
        options.managedDirectory=root.filePath("managed");put(options.managedDirectory+"/read.md",profile("managed"));
        options.overrides={{"reviewer",false}};QCOMPARE(a::discoverAgentProfiles(root.path(),options).find("reviewer").source,"managed");
    }
    void missingPreloadLeavesNoChildAndProfileBackgroundIsHonored() {
        QTemporaryDir root;const auto workspace=root.filePath("workspace");QDir().mkpath(workspace);
        class Model final:public a::Model { a::ModelReply generate(const a::ModelRequest&,const CancellationToken&,const TextCallback&) override{return {"done",{}};} };
        auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions eo;eo.sessionsDirectory=root.filePath("parents");eo.projectContext.enabled=false;eo.compaction.automatic=false;
        a::SubagentOptions so;so.workingDirectory=workspace;so.stateDirectory=root.filePath("children");so.profiles.enabled=true;so.profiles.projectBoundary=workspace;
        auto agents=std::make_shared<a::Subagents>(model,registry,policy,eo,so);a::Subagents::attach(eo,agents);a::Engine engine(model,registry,policy,eo);const auto p=engine.createSession("local",workspace);
        const auto path=workspace+"/.claude/agents/review.md";put(path,profile("missing","skills: [missing]\n"));
        QVERIFY(engine.runSubagentTool(p.id,"Agent",{{"prompt","go"},{"subagent_type","reviewer"}}).isError);
        QVERIFY(agents->list(p.id).isEmpty());QVERIFY(a::SessionStore(so.stateDirectory+"/sessions").list().isEmpty());
        put(path,profile("background","background: true\n"));
        auto result=engine.runSubagentTool(p.id,"Agent",{{"prompt","go"},{"subagent_type","reviewer"},{"run_in_background",false}});
        QVERIFY(!result.isError);QCOMPARE(result.data["status"],"async_launched");
        const auto id=result.data["agentId"].toString();QCOMPARE(agents->output(p.id,id,true,2000)["status"],"completed");
        // A fatal catalog error cannot prevent lifecycle cleanup or inspection.
        QVERIFY(QFile::remove(path));QDir(workspace+"/.claude/agents").removeRecursively();
        QVERIFY(QFile::link(root.path(),workspace+"/.claude/agents"));
        engine.stopSubagents(p.id);QVERIFY(!engine.runSubagentTool(p.id,"AgentOutput",{{"agent_id",id}}).isError);
    }
    void actualExecutionFreezesPromptAndIntersectsCurrentScope() {
        QTemporaryDir root;const auto workspace=root.filePath("workspace");QDir().mkpath(workspace);
        class Model final:public a::Model {
        public:std::function<a::ModelReply(const a::ModelRequest&)> next;
            a::ModelReply generate(const a::ModelRequest& r,const CancellationToken&,const TextCallback&) override{return next(r);}
        };
        auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();
        registry->add({{"Read","read",{{"type","object"}}, {},true},[](const auto&,const auto&){return a::ToolResult{"observed"};}});
        registry->add({{"Write","write",{{"type","object"}}},[](const auto&,const auto&){throw std::runtime_error("write escaped");return a::ToolResult{};}});
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions eo;eo.sessionsDirectory=root.filePath("parents");eo.projectContext.enabled=false;eo.compaction.automatic=false;
        a::SubagentOptions so;so.workingDirectory=workspace;so.stateDirectory=root.filePath("children");so.profiles.enabled=true;so.profiles.projectBoundary=workspace;
        const auto path=workspace+"/.claude/agents/review.md";
        put(path,profile("original","tools: Read\ninitialPrompt: INITIAL_ONLY_ONCE\n"));
        auto agents=std::make_shared<a::Subagents>(model,registry,policy,eo,so);a::Subagents::attach(eo,agents);
        a::Engine engine(model,registry,policy,eo);const auto parent=engine.createSession("local",workspace);
        model->next=[](const auto& r){
            if(!r.systemPrompt.contains("Inspect the requested file"))throw std::runtime_error("missing profile prompt");
            if(!r.messages.last().text.contains("INITIAL_ONLY_ONCE"))throw std::runtime_error("missing initial prompt");
            for(const auto& t:r.tools)if(t.name!="Read")throw std::runtime_error("scope leak");
            return a::ModelReply{"first",{}};
        };
        auto result=engine.runSubagentTool(parent.id,"Agent",{{"prompt","review"},{"subagent_type","reviewer"}});QVERIFY2(!result.isError,qPrintable(result.text));
        const auto id=result.data["agentId"].toString();const auto original=agents->output(parent.id,id)["profile"].toObject();QVERIFY(!original["sha256"].toString().isEmpty());
        put(path,"---\nname: reviewer\ndescription: Changed\ntools: Write\n---\nREPLACED_PROMPT\n");
        QCOMPARE(agents->profiles().find("reviewer").description,"Changed");
        model->next=[](const auto& r){
            if(r.systemPrompt.contains("REPLACED_PROMPT")||!r.tools.isEmpty()||r.messages.last().text!="continue")throw std::runtime_error("resume changed frozen contract");
            return a::ModelReply{"resumed",{}};
        };
        result=engine.runSubagentTool(parent.id,"Agent",{{"prompt","continue"},{"resume",id}});QVERIFY2(!result.isError,qPrintable(result.text));
        QCOMPARE(agents->output(parent.id,id)["profile"].toObject(),original);
        put(path,profile("Unsupported","isolation: worktree\n"));
        const auto denied=engine.runSubagentTool(parent.id,"Agent",{{"prompt","run"},{"subagent_type","reviewer"}});QVERIFY(denied.isError);QCOMPARE(agents->list(parent.id).size(),1);
    }
    void liveModelCatalogPreloadAndModelAuthorization() {
        QTemporaryDir root;const auto workspace=root.filePath("workspace");QDir().mkpath(workspace);
        class Model final:public a::Model { public:std::function<a::ModelReply(const a::ModelRequest&)> next;
            a::ModelReply generate(const a::ModelRequest& r,const CancellationToken&,const TextCallback&) override{return next(r);} };
        auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions eo;eo.sessionsDirectory=root.filePath("parents");eo.projectContext.enabled=false;eo.compaction.automatic=false;
        a::SubagentOptions so;so.workingDirectory=workspace;so.stateDirectory=root.filePath("children");so.profiles.enabled=true;so.profiles.projectBoundary=workspace;so.modelAliases={{"small","authorized-local"}};
        auto agents=std::make_shared<a::Subagents>(model,registry,policy,eo,so);a::Subagents::attach(eo,agents);
        a::Engine engine(model,registry,policy,eo);const auto parent=engine.createSession("local",workspace);
        // Created after engine construction: the next turn and API must see it.
        put(workspace+"/.claude/agents/review.md",profile("Live agent","model: small\nskills: [inspect]\ntools: []\n"));
        put(workspace+"/.claude/skills/inspect/SKILL.md","---\nname: inspect\ndescription: Inspect data\n---\nPRELOADED_EVIDENCE ${CLAUDE_SESSION_ID}\n");
        int children=0;model->next=[&](const auto& r){
            if(r.contextId==parent.id){
                if(r.messages.last().role==a::MessageRole::Tool)return a::ModelReply{r.messages.last().data["result"].toObject()["text"].toString(),{}};
                bool found=false;for(const auto& t:r.tools)if(t.name=="Agent")found=t.description.contains("Live agent")&&t.inputSchema["properties"].toObject()["subagent_type"].toObject()["enum"].toArray().contains("reviewer");
                if(!found)throw std::runtime_error("stale profile catalog");
                return a::ModelReply{{},{{"delegate","Agent",{{"prompt","review"},{"subagent_type","reviewer"}}}}};
            }
            ++children;if(r.model!="authorized-local"||!r.tools.isEmpty())throw std::runtime_error("model or tool scope mismatch");
            bool found=false;for(const auto& m:r.messages)if(m.metadata.contains("iilocal.skill"))found|=m.text.contains("PRELOADED_EVIDENCE "+r.contextId);
            if(!found)throw std::runtime_error("profile skill missing");return a::ModelReply{"preloaded",{}};
        };
        const auto result=engine.run({parent.id,"delegate"}).result.get();QCOMPARE(result.status,a::RunStatus::Completed);QCOMPARE(result.text,"preloaded");QCOMPARE(children,1);
        put(workspace+"/.claude/agents/review.md",profile("Unauthorized model","model: arbitrary-model\n"));
        QVERIFY(engine.runSubagentTool(parent.id,"Agent",{{"prompt","run"},{"subagent_type","reviewer"}}).isError);QCOMPARE(agents->list(parent.id).size(),1);
        const auto catalog=engine.runSubagentTool(parent.id,"AgentProfiles");QVERIFY(!catalog.isError);QVERIFY(!catalog.data["profiles"].toArray().isEmpty());
    }
    void precedenceAndLiveFileUpdates() {
        QTemporaryDir root;const auto workspace=root.filePath("workspace");QDir().mkpath(workspace);
        a::AgentProfileOptions options;options.projectBoundary=workspace;
        options.userDirectory=root.filePath("user");options.managedDirectory=root.filePath("managed");
        options.pluginDirectories={root.filePath("plugin")};options.directories={root.filePath("flags")};
        const QStringList paths{options.pluginDirectories[0],options.userDirectory,workspace+"/.claude/agents",options.directories[0],options.managedDirectory};
        for(qsizetype i=0;i<paths.size();++i)put(paths[i]+"/review.md",profile(QByteArray::number(i)));
        auto catalog=a::discoverAgentProfiles(workspace,options);QCOMPARE(catalog.find("reviewer").description,"4");
        QCOMPARE(catalog.find("reviewer").source,"managed");QVERIFY(!catalog.find("reviewer").sha256.isEmpty());
        QVERIFY(catalog.shadowed.size()>=4);
        QVERIFY(QFile::remove(paths.last()+"/review.md"));
        catalog=a::discoverAgentProfiles(workspace,options);QCOMPARE(catalog.find("reviewer").description,"3");
        put(paths[3]+"/review.md",profile("changed","tools: [Read, Glob]\ndisallowedTools: [Glob]\n"));
        catalog=a::discoverAgentProfiles(workspace,options);QCOMPARE(catalog.find("reviewer").description,"changed");
        QCOMPARE(catalog.find("reviewer").tools,QStringList({"Read","Glob"}));
        QCOMPARE(catalog.find("reviewer").disallowedTools,QStringList({"Glob"}));
        for(const auto& item:catalog.toJson()["profiles"].toArray())QVERIFY(!item.toObject().contains("system_prompt"));
    }
    void metadataValidationAndUnsupportedExecutionAreVisible() {
        QTemporaryDir root;a::AgentProfileOptions options;options.projectBoundary=root.path();
        put(root.filePath(".claude/agents/read.md"),profile("Read specialist","tools: Read, Grep\nmodel: inherit\nmaxTurns: 3\nbackground: true\npermissionMode: plan\nskills: [inspect]\ninitialPrompt: Verify evidence first.\n"));
        auto catalog=a::discoverAgentProfiles(root.path(),options);auto p=catalog.find("reviewer");
        QCOMPARE(p.tools,QStringList({"Read","Grep"}));QVERIFY(p.model.isEmpty());QCOMPARE(p.maxTurns,3);
        QVERIFY(p.background);QCOMPARE(p.permissionMode,"plan");QCOMPARE(p.skills,QStringList{"inspect"});QCOMPARE(p.initialPrompt,"Verify evidence first.");
        put(root.filePath(".claude/agents/read.md"),profile("Unsupported isolation","isolation: worktree\n"));
        p=a::discoverAgentProfiles(root.path(),options).find("reviewer");QVERIFY(p.unsupportedFeatures.contains("isolation"));
        put(root.filePath(".claude/agents/read.md"),profile("bad","maxTurns: 0\n"));
        catalog=a::discoverAgentProfiles(root.path(),options);QVERIFY(!catalog.failedFiles.isEmpty());
        QVERIFY_THROWS_EXCEPTION(Error,catalog.find("reviewer"));
    }
    void projectHierarchyAndHostOverrides() {
        QTemporaryDir root;const auto workspace=root.filePath("repo/child");QDir().mkpath(workspace);
        a::AgentProfileOptions options;options.projectBoundary=root.filePath("repo");
        put(root.filePath("repo/.claude/agents/p.md"),profile("ancestor"));
        put(workspace+"/.claude/agents/p.md",profile("nearest"));
        QCOMPARE(a::discoverAgentProfiles(workspace,options).find("reviewer").description,"nearest");
        a::SubagentDefinition host;host.name="reviewer";host.description="host";
        QCOMPARE(a::discoverAgentProfiles(workspace,options,{host}).find("reviewer").description,"host");
        options.overrides={{"reviewer",QJsonObject{{"description","json"},{"prompt","JSON system"},{"tools",QJsonArray{}},{"maxTurns",2}}}};
        const auto p=a::discoverAgentProfiles(workspace,options,{host}).find("reviewer");QCOMPARE(p.description,"json");QVERIFY(p.tools.isEmpty());QCOMPARE(p.systemPrompt,"JSON system");
    }
    void scanConfinementCancellationAndFileLimits() {
        QTemporaryDir root;const auto workspace=root.filePath("workspace");QDir().mkpath(workspace+"/.claude/agents");
        a::AgentProfileOptions options;options.projectBoundary=workspace;
        put(root.filePath("outside.md"),profile("external"));QVERIFY(QFile::link(root.filePath("outside.md"),workspace+"/.claude/agents/link.md"));
        auto catalog=a::discoverAgentProfiles(workspace,options);QVERIFY(!catalog.failedFiles.isEmpty());
        QVERIFY_THROWS_EXCEPTION(Error,catalog.find("reviewer"));
        CancellationToken cancelled;cancelled.cancel();QVERIFY_THROWS_EXCEPTION(Error,a::discoverAgentProfiles(workspace,options,{},cancelled));
        QVERIFY(QFile::remove(workspace+"/.claude/agents/link.md"));put(workspace+"/.claude/agents/a.md",profile("valid"));
        options.maxTotalBytes=16;QVERIFY_THROWS_EXCEPTION(Error,a::discoverAgentProfiles(workspace,options));
    }
};
QTEST_GUILESS_MAIN(AgentProfileTests)
#include "agent_profiles_tests.moc"
