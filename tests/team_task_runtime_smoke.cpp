#include <agent/Teams.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <filesystem>
#include <iostream>
#include <thread>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void put(const QString& path,const QByteArray& bytes){QDir().mkpath(QFileInfo(path).absolutePath());QFile f(path);
    if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())throw std::runtime_error("Cannot write fixture");}
void print(const QJsonObject& value){std::cout<<QJsonDocument(value).toJson(QJsonDocument::Compact).constData()<<std::endl;}
bool used(const QList<a::Message>& messages,const QString& tool,const QString& marker){
    QSet<QString> ids;for(const auto& m:messages){for(const auto& c:m.toolCalls)if(c.name==tool)ids.insert(c.id);
        if(m.role==a::MessageRole::Tool&&!m.isError&&ids.contains(m.toolCallId)&&m.text.contains(marker))return true;}return false;
}
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);if(argc<3)return 2;
    bool thinking=false;int contextTokens=12288;
    for(int i=3;i<argc;++i){const auto option=QString::fromLocal8Bit(argv[i]);
        if(option=="--thinking"&&!thinking)thinking=true;
        else if(option.startsWith("--context=")){bool valid=false;contextTokens=option.mid(10).toInt(&valid);if(!valid||contextTokens<4096||contextTokens>32768)return 2;}
        else return 2;
    }
    try{
        QTemporaryDir root(QDir::current().filePath("team-tasks-native-XXXXXX"));if(!root.isValid())return 1;
        const auto source=QDir(QString::fromLocal8Bit(argv[1])).filePath(modelId(QString::fromLocal8Bit(argv[2])));
        QFile file(source+"/manifest.json");if(!file.open(QIODevice::ReadOnly)||file.size()>1024*1024)throw std::runtime_error("Invalid manifest");
        auto manifest=parseModelManifest(QJsonDocument::fromJson(file.readAll()).object());if(modelUri(manifest.id)!=QString::fromLocal8Bit(argv[2]))throw std::runtime_error("Wrong model identity");
        manifest.id="team-task-fixture";const auto models=root.filePath("models"),package=models+"/"+manifest.id;
        for(const auto& f:manifest.files){const auto path=package+"/"+f.path;QDir().mkpath(QFileInfo(path).absolutePath());std::filesystem::create_hard_link((source+"/"+f.path).toStdString(),path.toStdString());}
        put(package+"/manifest.json",QJsonDocument(manifestObject(manifest)).toJson());const auto workspace=root.filePath("work");QDir().mkpath(workspace);
        ServiceOptions so;so.modelsDirectory=models;so.maxCachedContexts=1;so.maxCachedContextTokens=contextTokens;Service service(so);const auto uri=modelUri(manifest.id);
        ModelLoadRequest load{uri,contextTokens};load.options={{"enable_thinking",thinking},{"tool_grammar",true}};(void)service.loadModel(load).get();
        auto model=std::make_shared<a::ServiceModel>(service);auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,workspace);
        for(const auto& definition:registry->definitions())if(definition.name!="Read")registry->remove(definition.name);
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);a::EngineOptions eo;eo.sessionsDirectory=root.filePath("sessions");
        eo.projectContext.enabled=false;eo.skills.enabled=false;eo.toolSearch.enabled=false;eo.taskToolsEnabled=true;eo.taskToolsDeferred=false;eo.compaction.automatic=false;
        eo.maxToolCallsPerTurn=1;
        a::TeamsOptions config;config.workingDirectory=workspace;config.maxTurns=10;config.maxRuntimeMs=180000;config.generation.temperature=0;config.generation.maxTokens=2048;
        a::SubagentDefinition profile;profile.tools={"Read","TaskGet","TaskList","TaskUpdate","SendMessage"};
        profile.systemPrompt="Every new task message starts a fresh assignment. Call exactly ONE tool per response and WAIT for its result before deciding the next call. Complete assigned file-observation tasks in this order: TaskGet, Read, SendMessage, TaskUpdate. TaskGet returns TASK METADATA, never file contents. After TaskGet call Read with path shared-note.txt and WAIT for its result. Only that fresh Read result contains the file marker. Send exactly that marker as a STRING message to team-lead; never send a placeholder or task metadata. Then mark this task completed and say done. An ordinary task is not a shutdown request.";
        config.definitions={profile};
        auto teams=std::make_shared<a::Teams>(model,registry,policy,eo,config);a::Teams::attach(eo,teams);a::Engine engine(model,registry,policy,eo);
        const auto leader=engine.createSession(uri,workspace);a::ToolContext owner{leader.id,{},workspace};teams->create(owner,{{"team_name","automatic-native"}});
        QString member;bool all=true;print({{"qualification","team_automatic_tasks"},{"model",QString::fromLocal8Bit(argv[2])},{"context_tokens",contextTokens},
            {"max_cached_contexts",1},{"max_tokens",config.generation.maxTokens},{"max_turns",config.maxTurns},{"max_tool_calls_per_turn",eo.maxToolCallsPerTurn},{"load_options",load.options}});
        for(int phase=0;phase<2;++phase){
            const auto marker="TASK_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(16);put(workspace+"/shared-note.txt",marker.toUtf8());
            const qsizetype start=phase?a::SessionStore(eo.sessionsDirectory+"/teams/sessions").load(member).messages.size():0;
            const auto description=QString("First call TaskGet on this assigned task to confirm owner worker and status in_progress. Then use Read to read shared-note.txt now. Its entire contents are a marker. Call SendMessage with to 'team-lead', summary 'Assigned task observed', and message exactly the fresh observed marker. Finally call TaskUpdate on this task with status 'completed', then say done. The file changes between assignments: ignore every earlier marker and use only this assignment's fresh Read result.");
            const auto task=engine.runTaskTool(leader.id,"TaskCreate",{{"subject",phase?"Read the changed note":"Read the initial note"},{"description",description}}).data["task"].toObject()["id"].toString();
            if(!phase)member=teams->spawn(owner,{{"name","worker"},{"prompt","Use TaskList to find the in_progress task owned by worker. Read that task with TaskGet and follow its description exactly. Mark it completed using TaskUpdate when finished."}}).data["session_id"].toString();
            // No SendMessage or second spawn: the idle worker must discover this task.
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(180);QJsonObject state,record;
            for(;;){
                state=teams->status(leader.id);for(const auto& v:state["team"].toObject()["members"].toArray())if(v.toObject()["name"]=="worker")record=v.toObject();
                if(record["runs"].toInt()>=phase+1||record["status"]=="failed"||std::chrono::steady_clock::now()>=deadline)break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            const auto transcript=a::SessionStore(eo.sessionsDirectory+"/teams/sessions").load(member);const auto messages=transcript.messages.mid(start);
            const auto current=engine.runTaskTool(leader.id,"TaskGet",{{"taskId",task}}).data["task"].toObject();
            bool notified=false,claimedBeforeExecution=false,updated=false;
            for(const auto& v:teams->inbox(leader.id)["messages"].toArray())notified|=v.toObject()["from"]=="worker"&&v.toObject()["message"]==marker;
            QSet<QString> gets,updates;for(const auto& m:messages){for(const auto& call:m.toolCalls){
                    if(call.name=="TaskGet"&&call.arguments["taskId"]==task)gets.insert(call.id);
                    if(call.name=="TaskUpdate"&&call.arguments["taskId"]==task&&call.arguments["status"]=="completed")updates.insert(call.id);
                }
                if(m.role==a::MessageRole::Tool&&!m.isError&&gets.contains(m.toolCallId)){
                    const auto observed=QJsonDocument::fromJson(m.text.toUtf8()).object()["task"].toObject();
                    claimedBeforeExecution|=observed["id"]==task&&observed["owner"]=="worker"&&observed["status"]=="in_progress";
                }
                if(m.role==a::MessageRole::Tool&&!m.isError&&updates.contains(m.toolCallId)){
                    const auto observed=QJsonDocument::fromJson(m.text.toUtf8()).object()["task"].toObject();
                    updated|=observed["id"]==task&&observed["owner"]=="worker"&&observed["status"]=="completed";
                }
            }
            const bool read=used(messages,"Read",marker),sent=used(messages,"SendMessage",marker);
            const bool paired=a::pendingToolCalls(transcript.messages).isEmpty(),completed=current["status"]=="completed"&&current["owner"]=="worker";
            const bool passed=record["runs"].toInt()==phase+1&&record["status"]=="idle"&&record["result"].toObject()["status"]=="completed"&&claimedBeforeExecution&&read&&sent&&updated&&paired&&completed&&notified;all&=passed;
            print({{"phase",phase?"idle":"startup"},{"passed",passed},{"task_id",task},{"claimed_before_execution",claimedBeforeExecution},{"read",read},
                {"message_sent",sent},{"model_completed_task",updated},{"paired",paired},{"completed",completed},{"leader_received",notified},{"state",state}});
            if(!passed){QJsonArray json;for(const auto& message:messages)json.append(a::toJson(message));print({{"failed_phase_transcript",phase?"idle":"startup"},{"messages",json}});break;}
        }
        const auto stopped=teams->stop(leader.id,"worker");const auto removed=teams->remove(owner);
        all&=stopped["stopped"].toBool()&&!removed.isError&&engine.queuedInputs(leader.id)["count"].toInt()==0;
        print({{"passed",all},{"stopped",stopped},{"removed",removed.data},{"leader_pending_inputs",engine.queuedInputs(leader.id)["count"]}});return all?0:1;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
