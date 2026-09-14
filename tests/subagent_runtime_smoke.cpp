#include "agent/Subagents.h"
#include <QtCore/QSet>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <filesystem>
#include <iostream>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void put(const QString& path,const QByteArray& bytes){QDir().mkpath(QFileInfo(path).absolutePath());QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())throw std::runtime_error("write failed");}
QString secret(){return "CHILD_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(16);}
void print(const QJsonObject& data){std::cout<<QJsonDocument(data).toJson(QJsonDocument::Compact).constData()<<std::endl;}
bool observed(const a::Session& session,const QString& value){
    QSet<QString> reads;
    for(const auto& m:session.messages) {
        if(m.role==a::MessageRole::Assistant)for(const auto& c:m.toolCalls)if(c.name=="Read")reads.insert(c.id);
        if(m.role==a::MessageRole::Tool&&!m.isError&&reads.contains(m.toolCallId)&&m.text.contains(value))return true;
    }
    return false;
}
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);if(argc!=3)return 2;
    try {
        QTemporaryDir root(QDir::current().filePath("subagents-native-XXXXXX"));if(!root.isValid())return 1;
        const auto source=QDir(QString::fromLocal8Bit(argv[1])).filePath(modelId(QString::fromLocal8Bit(argv[2])));
        QFile file(QDir(source).filePath("manifest.json"));if(!file.open(QIODevice::ReadOnly)||file.size()>1024*1024)throw std::runtime_error("Invalid model manifest");
        auto manifest=parseModelManifest(QJsonDocument::fromJson(file.readAll()).object());
        if(modelUri(manifest.id)!=QString::fromLocal8Bit(argv[2]))throw std::runtime_error("Wrong catalog identity");
        manifest.id="subagent-fixture";const auto models=root.filePath("models"),package=models+"/"+manifest.id;
        for(const auto& f:manifest.files){const auto to=QDir(package).filePath(f.path);QDir().mkpath(QFileInfo(to).absolutePath());std::filesystem::create_hard_link(QDir(source).filePath(f.path).toStdString(),to.toStdString());}
        put(package+"/manifest.json",QJsonDocument(manifestObject(manifest)).toJson());
        const auto workspace=root.filePath("workspace");QDir().mkpath(workspace);
        ServiceOptions serviceOptions;serviceOptions.modelsDirectory=models;serviceOptions.maxCachedContexts=1;
        Service service(serviceOptions);const auto uri=modelUri(manifest.id);
        ModelLoadRequest load{uri,8192};load.options={{"enable_thinking",false},{"tool_grammar",false}};
        print({{"qualification","subagents"},{"manifest",manifestObject(manifest)},{"context_tokens",8192},{"max_cached_contexts",1},{"load_options",load.options}});
        (void)service.loadModel(load).get();
        auto model=std::make_shared<a::ServiceModel>(service);auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,workspace);
        for(const auto& t:registry->definitions())if(t.name!="Read")registry->remove(t.name);
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{{"Agent",a::PermissionBehavior::Allow}});
        a::EngineOptions eo;eo.sessionsDirectory=root.filePath("parents");eo.maxConcurrentRuns=1;eo.projectContext.enabled=false;eo.compaction.automatic=false;
        a::SubagentOptions so;so.workingDirectory=workspace;so.stateDirectory=root.filePath("children");so.maxTurns=6;so.maxRuntimeMs=180000;so.generation.temperature=0;so.generation.maxTokens=2048;
        a::SubagentDefinition definition;definition.description="Read a local file and report its exact observed contents.";definition.tools={"Read"};definition.systemPrompt="Use Read to inspect the requested file, then report its exact contents. Do not guess.";so.definitions={definition};
        auto agents=std::make_shared<a::Subagents>(model,registry,policy,eo,so);eo.additionalTools=a::Subagents::tools(agents);
        a::Engine engine(model,registry,policy,eo);bool all=true;
        for(const bool fork:{false,true}) {
            const auto expected=secret();put(workspace+"/secret.txt",expected.toUtf8());const auto parent=engine.createSession(uri,workspace);
            if(fork){auto lease=a::SessionStore(eo.sessionsDirectory).acquire(parent.id);lease->append({{},a::MessageRole::User,"The target file is secret.txt. A delegated child should read that file."});}
            a::RunRequest request{parent.id};request.generation=so.generation;request.maxTurns=6;
            request.prompt=fork?"Call Agent with fork_context true and prompt: Read the target file identified in the inherited context and return its exact contents. Return the child's observed result as your final answer."
                :"Call Agent to delegate this task to a separate child: Use Read to read secret.txt and return its exact contents. Then return the child's observed result as your final answer.";
            bool delegated=false;
            const auto result=engine.run(request,[&](const a::Event& e){if(e.kind==a::EventKind::ToolStarted&&e.data["name"]=="Agent")delegated=true;if(e.kind!=a::EventKind::ModelDelta&&e.kind!=a::EventKind::ToolProgress)print(a::toJson(e));}).result.get();
            const auto list=agents->list(parent.id);bool read=false,childCompleted=false,context=false;
            for(const auto& item:list){const auto state=agents->output(parent.id,item.toObject()["agentId"].toString(),true,180000);
                const auto transcript=a::SessionStore(so.stateDirectory+"/sessions").load(state["session_id"].toString());
                read|=observed(transcript,expected);childCompleted|=state["status"]=="completed"&&state["result"].toObject()["text"].toString().contains(expected);
                if(fork)for(const auto& m:transcript.messages)context|=m.text=="The target file is secret.txt. A delegated child should read that file.";
                print({{"child",state}});
            }
            const bool pass=result.status==a::RunStatus::Completed&&delegated&&read&&childCompleted&&result.text.contains(expected)&&(!fork||context)&&a::pendingToolCalls(engine.session(parent.id).messages).isEmpty();
            all&=pass;print({{"phase",fork?"fork":"foreground"},{"passed",pass},{"delegated",delegated},{"observed",read},{"child_completed",childCompleted},{"fork_context_present",context},{"result",a::toJson(result)}});
        }
        const auto parent=engine.createSession(uri,workspace);a::ToolContext context{parent.id,{},workspace};context.sessionSnapshot=std::make_shared<a::Session>(parent);
        QString id;
        for(const bool resume:{false,true}) {
            const auto expected=secret();put(workspace+"/background.txt",expected.toUtf8());
            QJsonObject args{{"prompt","Use Read to read background.txt again now and return its exact current contents."},{"run_in_background",true}};if(resume)args["resume"]=id;
            const auto accepted=agents->run(context,args);id=accepted.data["agentId"].toString();const auto result=agents->output(parent.id,id,true,180000);
            const auto transcript=a::SessionStore(so.stateDirectory+"/sessions").load(result["session_id"].toString());
            const bool read=observed(transcript,expected);const auto queue=engine.queuedInputs(parent.id);
            const bool pass=accepted.data["status"]=="async_launched"&&result["status"]=="completed"&&read&&result["result"].toObject()["text"].toString().contains(expected)
                &&queue["count"].toInt()==(resume?2:1)&&a::pendingToolCalls(transcript.messages).isEmpty();
            all&=pass;print({{"phase",resume?"resume":"background"},{"passed",pass},{"observed",read},{"notification_count",queue["count"]},{"result",result}});
        }
        return all?0:1;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
