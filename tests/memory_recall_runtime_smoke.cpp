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
class Guided final:public a::Model {
    a::ServiceModel native;
public:
    bool control=false;
    explicit Guided(Service& service):native(service){}
    a::ModelReply generate(const a::ModelRequest& input,const CancellationToken& token,const TextCallback& delta)override {
        // The relevance selector is unmodified. Only the main conversation's
        // tool choice is guided to exercise the asynchronous collection point.
        if(input.responseSchema["properties"].toObject().contains("selected_memories"))return native.generate(input,token,delta);
        auto request=input;request.enableThinking=false;request.generation.temperature=0;
        const bool recalled=std::any_of(input.messages.cbegin(),input.messages.cend(),[](const auto& message){return message.metadata.contains("iilocal.memory_recall");});
        if(!control&&!recalled) {
            request.tools.removeIf([](const auto& tool){return tool.name!="Read";});request.toolChoice="required";
            request.systemPrompt="Call Read once on the absolute validation.txt path supplied by the user. Reading it again is allowed while project context is being prepared.";
        } else {
            request.tools.clear();request.toolChoice="none";
            request.systemPrompt="Return only the exact Aurora release validation code from the provided recalled project notes. If no recalled note contains the code, return MISSING. Do not invent a code.";
        }
        return native.generate(request,token,delta);
    }
};
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);if(argc!=3)return 2;
    try {
        QTemporaryDir root(QDir::current().filePath("recall-native-XXXXXX"));require(root.isValid(),"Cannot create fixture");
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
        const auto work=root.filePath("work");QDir().mkpath(work);const auto sourceFile=QDir(work).filePath("validation.txt");
        write(sourceFile,"Aurora release validation code is stored in project notes.");
        const auto code="RECALL_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(12);
        auto model=std::make_shared<Guided>(service);auto tools=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*tools,work);
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::AcceptEdits);
        a::EngineOptions options;options.sessionsDirectory=root.filePath("sessions");options.projectMemoryEnabled=true;
        options.projectContext.enabled=false;options.skills.enabled=false;options.toolSearch.enabled=false;options.compaction.automatic=false;
        a::RunResult automatic,control;QJsonArray events;QJsonObject manual;
        {
            a::Engine engine(model,tools,policy,options);const auto id=engine.createSession(uri,work).id;
            const auto directory=engine.memory(id)["directory"].toString();
            auto save=[&](const QString& name,const QString& text){require(!engine.runMemoryTool(id,"Write",{{"path",QDir(directory).filePath(name)},{"content",text}}).isError,"Cannot seed topic");};
            save("release.md","---\nname: Aurora release\ndescription: Aurora release validation code\ntype: project\n---\nAurora release validation code: "+code);
            save("typography.md","---\nname: Typography\ndescription: Marketing typography preferences\ntype: user\n---\nPrefer large headings.");
            require(engine.memory(id)["index"].toString().isEmpty(),"Index must not disclose the code");
            const auto prompt="Read "+sourceFile+" then recall the Aurora release validation code from saved project notes. Return only the code.";
            require(!prompt.contains(code),"Prompt disclosed code");a::RunRequest request{id,prompt};request.maxTurns=6;request.generation.maxTokens=512;
            automatic=engine.run(request,[&](const a::Event& event){if(event.kind==a::EventKind::MemoryRecall)events.append(event.data);}).result.get();
            std::cerr<<QJsonDocument(a::toJson(automatic)).toJson(QJsonDocument::Compact).constData()<<std::endl;
            require(automatic.status==a::RunStatus::Completed&&automatic.text.trimmed()==code,"Automatic native recall failed");
            require(automatic.usage.memoryRecallGeneratedTokens>0,"No native selector tokens recorded");
            const auto stored=engine.session(id);require(std::any_of(stored.messages.cbegin(),stored.messages.cend(),[](const auto& message){return message.metadata.contains("iilocal.memory_recall");}),"No recalled transcript attachment");
            const auto other=engine.createSession(uri,work).id;manual=engine.recallMemory(other,"What is the Aurora release validation code?");
            std::cerr<<QJsonDocument(QJsonObject{{"phase","explicit_recall"},{"result",manual}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
            require(manual["status"]=="completed"&&QJsonDocument(manual).toJson().contains(code.toUtf8()),"Explicit native recall failed");
            require(engine.session(other).messages.isEmpty(),"Explicit recall modified the transcript");
        }
        options.memoryRecall.enabled=false;model->control=true;
        {
            a::Engine engine(model,std::make_shared<a::ToolRegistry>(),policy,options);const auto id=engine.createSession(uri,work).id;
            a::RunRequest request{id,"What is the Aurora release validation code?"};request.generation.maxTokens=128;request.maxTurns=1;
            control=engine.run(request).result.get();
            std::cerr<<QJsonDocument(QJsonObject{{"phase","disabled_control"},{"result",a::toJson(control)}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
            require(control.status==a::RunStatus::Completed&&control.text.trimmed()=="MISSING","Disabled recall control failed");
            require(control.usage.memoryRecallGeneratedTokens==0,"Disabled selector ran");
        }
        std::cout<<QJsonDocument(QJsonObject{{"passed",true},{"model",uri},{"code",code},{"main_tool_choice_guided",true},{"selector_unmodified",true},
            {"index_empty",true},{"automatic",a::toJson(automatic)},{"recall_events",events},{"explicit",manual},{"disabled_control",a::toJson(control)}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}
