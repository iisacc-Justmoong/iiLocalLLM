#include <agent/Engine.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <filesystem>
#include <iostream>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void write(const QString& path,const QByteArray& bytes){QFile file(path);require(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size(),"Cannot write fixture");}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);if(argc!=3)return 2;
    try {
        QTemporaryDir root(QDir::current().filePath("extraction-native-XXXXXX"));require(root.isValid(),"Cannot create fixture");
        const auto uri=QString::fromLocal8Bit(argv[2]),source=QDir(QString::fromLocal8Bit(argv[1])).filePath(modelId(uri));
        QFile file(source+"/manifest.json");require(file.open(QIODevice::ReadOnly),"Cannot read manifest");
        const auto manifest=parseModelManifest(QJsonDocument::fromJson(file.readAll()).object());require(modelUri(manifest.id)==uri,"Wrong model identity");
        const auto models=root.filePath("models"),package=models+'/'+manifest.id;QDir().mkpath(package);
        for(const auto& item:manifest.files) {
            const auto target=package+'/'+item.path;QDir().mkpath(QFileInfo(target).absolutePath());std::error_code error;
            std::filesystem::create_hard_link((source+'/'+item.path).toStdString(),target.toStdString(),error);
            require(!error||QFile::copy(source+'/'+item.path,target),"Cannot provision model");
        }
        write(package+"/manifest.json",QJsonDocument(manifestObject(manifest)).toJson());
        ServiceOptions serviceOptions;serviceOptions.modelsDirectory=models;Service service(serviceOptions);
        ModelLoadRequest load{uri,8192};load.options["tool_grammar"]=true;load.options["enable_thinking"]=false;(void)service.loadModel(load).get();
        const auto work=root.filePath("work");QDir().mkpath(work);
        const auto marker="LABEL_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(10);
        auto model=std::make_shared<a::ServiceModel>(service);auto tools=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*tools,work);
        auto policy=std::make_shared<a::RulePolicy>();a::EngineOptions options;options.sessionsDirectory=root.filePath("sessions");
        options.projectMemoryEnabled=true;options.memoryRecall.enabled=false;options.memoryExtraction.enabled=true;options.memoryExtraction.timeoutMs=180000;
        options.projectContext.enabled=false;options.skills.enabled=false;options.toolSearch.enabled=false;options.compaction.automatic=false;
        a::Engine engine(model,tools,policy,options);const auto id=engine.createSession(uri,work,
            "Respond briefly to conversational requests. In the main conversation, do not use file tools unless the user explicitly asks to inspect or change a file.").id;
        a::RunRequest request{id,"For all my future deployment reports, use "+marker+" as the section heading. This is my standing reporting preference. Please simply acknowledge it."};
        request.maxTurns=4;request.generation.maxTokens=768;request.generation.temperature=0;
        QJsonArray events;const auto answer=engine.run(request,[&](const a::Event& event){if(event.kind==a::EventKind::MemoryExtraction)events.append(event.data);}).result.get();
        std::cerr<<QJsonDocument(a::toJson(answer)).toJson(QJsonDocument::Compact).constData()<<std::endl;
        require(answer.status==a::RunStatus::Completed,"Native parent response failed");
        require(engine.drainMemoryExtractions(240000),"Native extraction did not drain");
        const auto state=engine.memoryExtractionStatus(id);std::cerr<<QJsonDocument(state).toJson(QJsonDocument::Compact).constData()<<std::endl;
        const auto records=state["records"].toArray();require(!records.isEmpty(),"No automatic native extraction was admitted");
        const auto extraction=records.last().toObject();require(extraction["status"]=="completed","Native extraction did not complete");
        require(extraction["usage"].toObject()["generated_tokens"].toInt()>0,"No native extraction usage");
        require(!extraction["saved_topics"].toArray().isEmpty(),"Native extraction saved no topic");
        bool found=false;for(const auto& value:extraction["saved_topics"].toArray()) {
            QFile note(value.toString());require(note.open(QIODevice::ReadOnly),"Saved topic cannot be read");found|=note.readAll().contains(marker.toUtf8());
        }
        require(found,"Extracted topic omitted the reporting preference");
        const auto conversation=engine.session(id);require(conversation.messages.size()==2,"Extraction leaked into main transcript or main wrote memory directly");
        require(engine.extractMemory(id)["status"]=="up_to_date","Completed parent context was extracted twice");
        const auto fresh=engine.createSession(uri,work).id;require(!engine.memory(fresh,marker)["files"].toArray().isEmpty(),"New session cannot find extracted memory");
        auto disabledOptions=options;disabledOptions.memoryExtraction.enabled=false;disabledOptions.sessionsDirectory=root.filePath("disabled");
        a::Engine disabled(model,tools,policy,disabledOptions);const auto disabledId=disabled.createSession(uri,work).id;
        a::RunRequest control{disabledId,"Please acknowledge that I prefer concise reports."};control.generation.maxTokens=128;control.generation.temperature=0;
        require(disabled.run(control).result.get().status==a::RunStatus::Completed,"Disabled control failed");
        require(disabled.memoryExtractionStatus(disabledId)["records"].toArray().isEmpty(),"Disabled extraction ran");
        std::cout<<QJsonDocument(QJsonObject{{"passed",true},{"model",uri},{"marker",marker},{"model_requests_unmodified",true},
            {"answer",a::toJson(answer)},{"events",events},{"extraction",extraction},{"new_session_memory",true},{"disabled_control",true}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}
