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
        QTemporaryDir root(QDir::current().filePath("history-native-XXXXXX"));require(root.isValid(),"Cannot create fixture");
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
        const auto sessions=root.filePath("sessions");a::SessionStore store(sessions);
        const auto old=store.create(uri,"Old conversation",work).id;
        {
            auto lease=store.acquire(old);
            for(int i=0;i<8;++i)lease->append({{},a::MessageRole::User,"Build log "+QString::number(i)+": "+QString(160000,'x')});
            lease->append({{},a::MessageRole::User,"The release alias is "+marker+". Keep this exact spelling."});
        }
        auto model=std::make_shared<a::ServiceModel>(service);auto tools=std::make_shared<a::ToolRegistry>();
        auto policy=std::make_shared<a::RulePolicy>();a::EngineOptions options;options.sessionsDirectory=sessions;options.sessionHistoryEnabled=true;
        options.projectContext.enabled=false;options.skills.enabled=false;options.toolSearch.enabled=false;options.compaction.automatic=false;
        a::Engine engine(model,tools,policy,options);const auto id=engine.createSession(uri,work).id;
        a::RunRequest request{id,"Find the release alias in our previous conversations. Use SessionSearch with the literal query release alias. Reply with the exact alias from the matching message."};
        request.maxTurns=5;request.generation.maxTokens=768;request.generation.temperature=0;
        const auto answer=engine.run(request).result.get();require(answer.status==a::RunStatus::Completed,"Native history run did not complete");
        require(answer.text.contains(marker),"Native answer omitted the saved alias");
        bool observed=false;QJsonArray calls;int errors=0;
        for(const auto& message:engine.session(id).messages) {
            for(const auto& call:message.toolCalls){calls.append(QJsonObject{{"name",call.name},{"arguments",call.arguments}});require(call.name=="SessionSearch","Unexpected tool");}
            if(message.role!=a::MessageRole::Tool)continue;errors+=message.isError;
            for(const auto& value:message.data["matches"].toArray()) {
                const auto hit=value.toObject();observed|=hit["session_id"]==old&&hit["snippet"].toString().contains(marker)&&hit["byte_offset"].toInteger()>1024*1024;
            }
        }
        require(observed&&!errors,"Native search did not retrieve the owned large transcript tail");
        auto disabledOptions=options;disabledOptions.sessionHistoryEnabled=false;
        a::Engine disabled(model,tools,policy,disabledOptions);require(!disabled.sessionSearchTool(),"Disabled history remains advertised");
        std::cout<<QJsonDocument(QJsonObject{{"passed",true},{"model",uri},{"marker",marker},{"model_requests_unmodified",true},
            {"answer",a::toJson(answer)},{"calls",calls},{"tool_errors",errors},{"large_history_tail_found",observed},{"disabled_control",true}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}
