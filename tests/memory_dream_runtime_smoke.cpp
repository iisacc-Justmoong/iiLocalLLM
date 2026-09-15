#include <agent/Engine.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <filesystem>
#include <iostream>
#include <mutex>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void write(const QString& path,const QByteArray& bytes){QFile file(path);require(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size(),"Cannot write fixture");}
QByteArray read(const QString& path){QFile file(path);require(file.open(QIODevice::ReadOnly),"Cannot read fixture");return file.readAll();}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);if(argc!=3)return 2;
    try {
        QTemporaryDir root(QDir::current().filePath("dream-native-XXXXXX"));require(root.isValid(),"Cannot create fixture");
        const auto uri=QString::fromLocal8Bit(argv[2]),source=QDir(QString::fromLocal8Bit(argv[1])).filePath(modelId(uri));
        const auto manifest=parseModelManifest(QJsonDocument::fromJson(read(source+"/manifest.json")).object());require(modelUri(manifest.id)==uri,"Wrong model identity");
        const auto models=root.filePath("models"),package=models+'/'+manifest.id;QDir().mkpath(package);
        for(const auto& item:manifest.files) {
            const auto target=package+'/'+item.path;QDir().mkpath(QFileInfo(target).absolutePath());std::error_code error;
            std::filesystem::create_hard_link((source+'/'+item.path).toStdString(),target.toStdString(),error);
            require(!error||QFile::copy(source+'/'+item.path,target),"Cannot provision model");
        }
        write(package+"/manifest.json",QJsonDocument(manifestObject(manifest)).toJson());
        ServiceOptions serviceOptions;serviceOptions.modelsDirectory=models;Service service(serviceOptions);
        ModelLoadRequest load{uri,8192};load.options["tool_grammar"]=true;load.options["enable_thinking"]=false;(void)service.loadModel(load).get();
        const auto work=root.filePath("work"),sessions=root.filePath("sessions");QDir().mkpath(work);a::SessionStore store(sessions);
        for(int i=0;i<5;++i){const auto id=store.create(uri,"Prior conversation",work).id;store.acquire(id)->append({{},a::MessageRole::User,"We discussed report language preferences in this project."});}
        auto model=std::make_shared<a::ServiceModel>(service);auto tools=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*tools,work);
        auto policy=std::make_shared<a::RulePolicy>();a::EngineOptions options;options.sessionsDirectory=sessions;
        options.projectMemoryEnabled=true;options.sessionHistoryEnabled=true;options.memoryRecall.enabled=false;options.memoryExtraction.enabled=false;
        options.memoryDream.automatic=true;options.projectContext.enabled=false;options.skills.enabled=false;options.toolSearch.enabled=false;options.compaction.automatic=false;
        QJsonArray calls;std::mutex callsMutex;
        options.hooks.append([&](const a::HookInput& input,const auto&) {
            if(input.kind==a::HookKind::AfterTool){std::lock_guard lock(callsMutex);QJsonObject call{{"session_id",input.sessionId},
                {"name",input.call.name},{"arguments",input.call.arguments},{"is_error",input.result.isError},{"result",input.result.text}};
                calls.append(call);std::cerr<<QJsonDocument(call).toJson(QJsonDocument::Compact).constData()<<std::endl;}return a::HookResult{};
        });
        a::Engine engine(model,tools,policy,options);const auto id=engine.createSession(uri,work,
            "For a conversational acknowledgement, output ACK only and do not use tools. When a separate memory maintenance instruction is appended, execute that maintenance using its permitted native tools.").id;
        const auto memory=engine.memory(id);const auto directory=memory["directory"].toString(),index=memory["index_path"].toString();
        const auto topic=QDir(directory).filePath("preferences.md");
        const QByteArray old="---\nname: Report preferences\ndescription: Standing language and heading preferences for reports\ntype: feedback\n---\nReports must use English only.\n";
        write(topic,old);write(index,"# Project memory\n- [Report preferences](preferences.md)\n- [Duplicate report preferences](preferences.md)\n");
        const auto marker="REPORT_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(10);
        a::RunRequest request{id,"My permanent reporting preference has changed: use Korean, and use "+marker+" as the section heading in every future report. This supersedes my previous English-only reporting preference. Please simply acknowledge it."};
        request.maxTurns=4;request.generation.maxTokens=1536;request.generation.temperature=0;
        QJsonArray events;const auto answer=engine.run(request,[&](const a::Event& event){if(event.kind==a::EventKind::MemoryDream)events.append(event.data);}).result.get();
        std::cerr<<QJsonDocument(a::toJson(answer)).toJson(QJsonDocument::Compact).constData()<<std::endl;
        require(answer.status==a::RunStatus::Completed,"Native parent response failed");require(engine.drainMemoryDreams(360000),"Native dream did not drain");
        const auto state=engine.memoryDreamStatus(id);std::cerr<<QJsonDocument(state).toJson(QJsonDocument::Compact).constData()<<std::endl;
        const auto records=state["records"].toArray();require(!records.isEmpty(),"Automatic native dream was not admitted");const auto dream=records.last().toObject();
        require(dream["status"]=="completed"&&dream["tool_errors"].toInt()==0,"Native dream did not complete cleanly");
        require(dream["usage"].toObject()["generated_tokens"].toInt()>0&&dream["last_consolidated_ms"].toInteger()>0,"No native dream usage or successful timestamp");
        require(dream["sessions_reviewing"].toInt()==5,"Default prior-session gate did not apply");
        const auto consolidated=read(topic),pruned=read(index);
        require(consolidated!=old&&consolidated.contains(marker.toUtf8())&&!consolidated.contains("Reports must use English only."),"Native dream did not replace the obsolete preference");
        require(pruned.count("preferences.md")==1&&pruned.count('\n')<=200&&pruned.size()<=25000,"Native dream did not deduplicate the memory index");
        require(engine.session(id).messages.size()==2&&engine.queuedInputs(id)["count"].toInt()==0,"Dream polluted main conversation");
        require(dream["written_paths"].toArray().contains(topic)&&dream["written_paths"].toArray().contains(index),"Missing successful native file changes");
        const auto next=engine.createSession(uri,work).id;require(!engine.memory(next,marker)["files"].toArray().isEmpty(),"New session cannot find consolidated preference");
        std::cout<<QJsonDocument(QJsonObject{{"passed",true},{"model",uri},{"marker",marker},{"model_requests_unmodified",true},
            {"answer",a::toJson(answer)},{"events",events},{"dream",dream},{"calls",calls},{"consolidated_topic",QString::fromUtf8(consolidated)},
            {"pruned_index",QString::fromUtf8(pruned)},{"new_session_memory",true},{"main_message_count",2}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}
