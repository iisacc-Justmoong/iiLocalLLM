#include <agent/Teams.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <filesystem>
#include <iostream>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void put(const QString& path,const QByteArray& bytes){QDir().mkpath(QFileInfo(path).absolutePath());QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())throw std::runtime_error("Cannot write fixture");}
void print(const QJsonObject& value){std::cout<<QJsonDocument(value).toJson(QJsonDocument::Compact).constData()<<std::endl;}
bool used(const a::Session& session,const QString& tool,const QString& marker){
    QSet<QString> ids;for(const auto& m:session.messages){for(const auto& c:m.toolCalls)if(c.name==tool)ids.insert(c.id);
        if(m.role==a::MessageRole::Tool&&!m.isError&&ids.contains(m.toolCallId)&&m.text.contains(marker))return true;}return false;
}
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);if(argc!=3)return 2;
    try{
        QTemporaryDir root(QDir::current().filePath("teams-native-XXXXXX"));if(!root.isValid())return 1;
        const auto source=QDir(QString::fromLocal8Bit(argv[1])).filePath(modelId(QString::fromLocal8Bit(argv[2])));
        QFile file(source+"/manifest.json");if(!file.open(QIODevice::ReadOnly)||file.size()>1024*1024)throw std::runtime_error("Invalid manifest");
        auto manifest=parseModelManifest(QJsonDocument::fromJson(file.readAll()).object());if(modelUri(manifest.id)!=QString::fromLocal8Bit(argv[2]))throw std::runtime_error("Wrong model identity");
        manifest.id="team-fixture";const auto models=root.filePath("models"),package=models+"/"+manifest.id;
        for(const auto& f:manifest.files){const auto path=package+"/"+f.path;QDir().mkpath(QFileInfo(path).absolutePath());std::filesystem::create_hard_link((source+"/"+f.path).toStdString(),path.toStdString());}
        put(package+"/manifest.json",QJsonDocument(manifestObject(manifest)).toJson());const auto workspace=root.filePath("work");QDir().mkpath(workspace);
        ServiceOptions so;so.modelsDirectory=models;so.maxCachedContexts=1;so.maxCachedContextTokens=8192;Service service(so);const auto uri=modelUri(manifest.id);
        ModelLoadRequest load{uri,8192};load.options={{"enable_thinking",false},{"tool_grammar",true}};(void)service.loadModel(load).get();
        auto model=std::make_shared<a::ServiceModel>(service);auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,workspace);
        for(const auto& definition:registry->definitions())if(definition.name!="Read")registry->remove(definition.name);
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);a::EngineOptions eo;eo.sessionsDirectory=root.filePath("sessions");
        eo.projectContext.enabled=false;eo.skills.enabled=false;eo.toolSearch.enabled=false;eo.taskToolsEnabled=true;eo.taskToolsDeferred=false;eo.compaction.automatic=false;
        a::TeamsOptions config;config.workingDirectory=workspace;config.maxTurns=8;config.maxRuntimeMs=180000;config.generation.temperature=0;config.generation.maxTokens=1024;
        a::SubagentDefinition profile;profile.tools={"Read","TaskCreate","TaskList","SendMessage"};config.definitions={profile};
        auto teams=std::make_shared<a::Teams>(model,registry,policy,eo,config);a::Teams::attach(eo,teams);a::Engine engine(model,registry,policy,eo);
        const auto leader=engine.createSession(uri,workspace);a::ToolContext owner{leader.id,{},workspace};teams->create(owner,{{"team_name","native"}});
        QString member;bool all=true;print({{"qualification","teams"},{"model",QString::fromLocal8Bit(argv[2])},{"context_tokens",8192},{"max_cached_contexts",1},{"max_tokens",config.generation.maxTokens},{"load_options",load.options}});
        for(int phase=0;phase<2;++phase){
            const auto marker="TEAM_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(16);put(workspace+"/shared-note.txt",marker.toUtf8());
            const QString prompt="Use Read to read shared-note.txt now. Its entire contents are a marker. Call TaskCreate with subject exactly that observed marker and description 'Observed local team evidence'. Then call SendMessage with to 'team-lead', summary 'Local file observed', and message exactly the observed marker. Finally say done. Do not guess the marker.";
            if(!phase)member=teams->spawn(owner,{{"name","reader"},{"prompt",prompt}}).data["session_id"].toString();
            else teams->send(owner,{{"to","reader"},{"summary","Repeat against changed local bytes"},{"message","The file has changed since your previous assignment. Ignore every earlier marker and create a new task using only the value returned by a fresh Read in this assignment. "+prompt}});
            const auto state=teams->wait(leader.id,180000);const auto transcript=a::SessionStore(eo.sessionsDirectory+"/teams/sessions").load(member);
            bool shared=false,notified=false;for(const auto& v:engine.runTaskTool(leader.id,"TaskList").data["tasks"].toArray())shared|=v.toObject()["subject"]==marker;
            for(const auto& v:teams->inbox(leader.id)["messages"].toArray())notified|=v.toObject()["from"]=="reader"&&v.toObject()["message"]==marker;
            const bool read=used(transcript,"Read",marker),created=used(transcript,"TaskCreate",marker),sent=used(transcript,"SendMessage",marker),paired=a::pendingToolCalls(transcript.messages).isEmpty();
            bool completed=false;for(const auto& value:state["team"].toObject()["members"].toArray()){const auto m=value.toObject();if(m["name"]=="reader")completed=m["status"]=="idle"&&m["result"].toObject()["status"]=="completed";}
            const bool passed=state["idle"].toBool()&&completed&&read&&created&&sent&&paired&&shared&&notified;all&=passed;
            print({{"phase",phase?"followup":"initial"},{"passed",passed},{"completed",completed},{"read",read},{"task_created",created},{"message_sent",sent},{"shared_task_visible",shared},{"leader_received",notified},{"paired",paired},{"state",state}});
            if(!passed){QJsonArray messages;for(const auto& message:transcript.messages)messages.append(a::toJson(message));print({{"failed_phase_transcript",phase?"followup":"initial"},{"messages",messages}});}
        }
        const auto stopped=teams->stop(leader.id,"reader");const auto removed=teams->remove(owner);
        all&=stopped["stopped"].toBool()&&!removed.isError&&teams->status(leader.id)["team"].isNull();
        print({{"passed",all},{"stopped",stopped},{"removed",removed.data},{"leader_pending_inputs",engine.queuedInputs(leader.id)["count"]}});return all?0:1;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
