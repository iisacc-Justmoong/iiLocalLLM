#include <agent/PluginRuntime.h>
#include <agent/Engine.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <filesystem>
#include <iostream>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
void put(const QString& path,const QByteArray& bytes){require(QDir().mkpath(QFileInfo(path).absolutePath()),"Cannot create fixture directory");QFile f(path);require(f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size(),"Cannot write fixture");}
QByteArray read(const QString& path){QFile f(path);require(f.open(QIODevice::ReadOnly),"Cannot read model manifest");return f.readAll();}
class FunctionModel final:public a::Model{
    a::ModelReply generate(const a::ModelRequest& r,const CancellationToken&,const TextCallback&) override{
        if(r.messages.last().role==a::MessageRole::Tool)return {r.messages.last().text,{}};
        return {{},{{"plugin-call","mcp__plugin_runtime_peer__echo",{}}}};
    }
};
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);if(argc!=2&&argc!=4)return 2;
    try{
        QTemporaryDir root(QDir::current().filePath("pln-XXXXXX"));require(root.isValid(),"Cannot create runtime fixture");
        const auto workspace=root.filePath("work"),package=root.filePath("source");QDir().mkpath(workspace);QDir().mkpath(package+"/bin");
        require(QFile::copy(QString::fromLocal8Bit(argv[1]),package+"/bin/peer"),"Cannot package the independent MCP peer");
        const auto marker="plugin_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
        const QJsonObject manifest{{"name","runtime"},{"version","1.0.0"},{"mcpServers",QJsonObject{{"peer",QJsonObject{
            {"command","${CLAUDE_PLUGIN_ROOT}/bin/peer"},{"env",QJsonObject{{"PLUGIN_SECRET",marker}}}}}}}};
        put(package+"/.claude-plugin/plugin.json",QJsonDocument(manifest).toJson());
        put(package+"/skills/inspect/SKILL.md","---\ndescription: Inspect the plugin peer\n---\nCall mcp__plugin_runtime_peer__echo with an empty object. Return only the exact secret field from its observed result. Do not guess or invent it.\n");
        a::PluginStore store({root.filePath("plugins")});store.install(package);const auto snapshot=store.snapshot();
        a::EngineOptions e;e.sessionsDirectory=root.filePath("sessions");e.compaction.automatic=false;e.projectContext.enabled=false;e.toolSearch.enabled=false;e.maxToolCallsPerTurn=1;
        a::AgentProfileOptions profiles;profiles.includeProject=false;profiles.includeBuiltins=false;
        a::McpConnectionOptions m;m.workingDirectory=workspace;m.refreshIntervalMs=0;m.deferTools=false;
        a::CommandHookOptions hooks;hooks.workingDirectory=workspace;a::PluginRuntime::attach(e,profiles,m,std::make_shared<a::PluginRuntime>(snapshot,hooks));
        auto registry=std::make_shared<a::ToolRegistry>();a::McpConnections connections(registry,m);
        require(bool(connections.client("plugin:runtime:peer")),"Plugin MCP did not connect");
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        std::unique_ptr<Service> service;std::shared_ptr<a::Model> model;QString uri="fixture";
        if(argc==4){
            uri=QString::fromLocal8Bit(argv[3]);const auto source=QDir(QString::fromLocal8Bit(argv[2])).filePath(modelId(uri));
            const auto manifest=parseModelManifest(QJsonDocument::fromJson(read(source+"/manifest.json")).object());
            const auto models=root.filePath("models"),target=models+'/'+manifest.id;QDir().mkpath(target);
            for(const auto& file:manifest.files){const auto path=target+'/'+file.path;QDir().mkpath(QFileInfo(path).absolutePath());std::error_code error;
                std::filesystem::create_hard_link((source+'/'+file.path).toStdString(),path.toStdString(),error);require(!error||QFile::copy(source+'/'+file.path,path),"Cannot provision model fixture");}
            put(target+"/manifest.json",QJsonDocument(manifestObject(manifest)).toJson());
            ServiceOptions options;options.modelsDirectory=models;options.defaultContextTokens=8192;options.maxCachedContextTokens=8192;options.maxCachedContexts=1;
            service=std::make_unique<Service>(options);ModelLoadRequest load{uri,8192};load.options={{"tool_grammar",true},{"enable_thinking",false}};service->loadModel(load).get();
            model=std::make_shared<a::ServiceModel>(*service);
        }else model=std::make_shared<FunctionModel>();
        QJsonArray calls;e.hooks.append([&](const a::HookInput& input,const auto&){if(input.kind==a::HookKind::AfterTool)calls.append(QJsonObject{
            {"name",input.call.name},{"arguments",input.call.arguments},{"is_error",input.result.isError}});return a::HookResult{};});
        a::Engine engine(model,registry,policy,e);const auto session=engine.createSession(uri,workspace,"Complete the selected skill using the available tools.");
        a::RunRequest request{session.id};request.skill="runtime:inspect";request.maxTurns=4;request.generation.maxTokens=1024;
        request.generation.temperature=0;request.generation.topP=0.9;request.generation.topK=40;request.generation.minP=0;request.generation.seed=0;
        const auto run=engine.run(request);if(run.result.wait_for(std::chrono::seconds(120))!=std::future_status::ready)run.cancel();
        const auto result=run.result.get();const auto saved=engine.session(session.id);
        const bool passed=result.status==a::RunStatus::Completed&&result.text.contains(marker)&&calls.size()==1
            &&calls[0].toObject()["name"]=="mcp__plugin_runtime_peer__echo"&&!calls[0].toObject()["is_error"].toBool()&&a::pendingToolCalls(saved.messages).isEmpty();
        QJsonObject report{{"passed",passed},{"native_inference",argc==4},{"model",uri},{"marker",marker},{"run",a::toJson(result)},{"calls",calls},
            {"plugins",engine.plugins()},{"marker_only_in_peer_environment",true},{"prompt_and_success_criteria_unchanged",true}};
        std::cout<<QJsonDocument(report).toJson(QJsonDocument::Compact).constData()<<std::endl;
        engine.endSession(session.id);connections.close();return passed?0:1;
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}
