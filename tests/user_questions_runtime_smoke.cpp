#include <agent/Engine.h>
#include <agent/PermissionRequests.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
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
void write(const QString& path,const QByteArray& bytes){QFile file(path);require(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size(),"Cannot write native fixture");}
// The host selects the question tool, while the native model generates the
// question/choices and then consumes the answer received through the broker.
class Guided final:public a::Model {
    a::ServiceModel native;
public:
    int turn=0;QJsonObject asked;
    explicit Guided(Service& service):native(service){}
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken& token,const TextCallback& output) override {
        auto next=request;next.enableThinking=false;
        for(const auto& message:request.messages)if(message.role==a::MessageRole::Tool)require(!message.isError,"A native question tool failed");
        require(turn<2,"Native question flow exceeded two turns");
        if(turn++==0) {
            next.systemPrompt="Call AskUserQuestion once. Ask for the validation code. Use header Code and two choices: Provide, Skip. Do not answer the question yourself.";
            next.tools.removeIf([](const auto& value){return value.name!="AskUserQuestion";});next.toolChoice="required";
            auto schema=next.tools[0].inputSchema;auto properties=schema["properties"].toObject();
            properties.remove("answers");properties.remove("annotations");schema["properties"]=properties;next.tools[0].inputSchema=schema;
        } else {
            next.tools.clear();next.toolChoice="none";
            next.systemPrompt="Return exactly the validation code supplied in the user question response. Do not explain or add text.";
        }
        const auto reply=native.generate(next,token,output);
        if(turn==1){require(reply.toolCalls.size()==1&&reply.toolCalls[0].name=="AskUserQuestion","Native model did not generate the requested question");asked=reply.toolCalls[0].arguments;}
        return reply;
    }
};
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);if(argc!=3)return 2;
    try {
        QTemporaryDir root(QDir::current().filePath("question-native-XXXXXX"));require(root.isValid(),"Cannot create native fixture");
        const auto source=QDir(QString::fromLocal8Bit(argv[1])).filePath(modelId(QString::fromLocal8Bit(argv[2])));
        QFile manifestFile(source+"/manifest.json");require(manifestFile.open(QIODevice::ReadOnly),"Cannot read native model manifest");
        const auto manifest=parseModelManifest(QJsonDocument::fromJson(manifestFile.readAll()).object());
        require(modelUri(manifest.id)==QString::fromLocal8Bit(argv[2]),"Native model identity mismatch");
        const auto models=root.filePath("models"),package=models+"/"+manifest.id;QDir().mkpath(package);
        for(const auto& file:manifest.files) {
            const auto from=source+"/"+file.path,to=package+"/"+file.path;QDir().mkpath(QFileInfo(to).absolutePath());
            std::error_code error;std::filesystem::create_hard_link(from.toStdString(),to.toStdString(),error);
            require(!error||QFile::copy(from,to),"Cannot provision model weights");
        }
        write(package+"/manifest.json",QJsonDocument(manifestObject(manifest)).toJson());
        ServiceOptions serviceOptions;serviceOptions.modelsDirectory=models;Service service(serviceOptions);
        const auto uri=modelUri(manifest.id);ModelLoadRequest load{uri,8192};load.options["tool_grammar"]=true;load.options["enable_thinking"]=false;(void)service.loadModel(load).get();
        const auto workspace=root.filePath("work");QDir().mkpath(workspace);auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,workspace);
        const auto code="ANSWER_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(10);
        auto model=std::make_shared<Guided>(service);a::EngineOptions options;options.sessionsDirectory=root.filePath("sessions");
        options.userQuestionsEnabled=true;options.userQuestions.deferred=false;options.skills.enabled=false;options.toolSearch.enabled=false;options.projectContext.enabled=false;options.compaction.automatic=false;
        options.permissionRequests=std::make_shared<a::PermissionRequests>();int reviews=0;
        a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),options);
        const auto id=engine.createSession(uri,workspace).id;a::RunRequest request{id,"Ask me for a validation code, then return my answer."};
        request.generation.temperature=0;request.generation.maxTokens=512;request.maxTurns=2;
        const auto result=engine.run(request,[&](const a::Event& event) {
            if(event.kind==a::EventKind::PermissionRequested) {
                ++reviews;auto input=event.data["request"].toObject()["input"].toObject();
                require(!QJsonDocument(input).toJson().contains(code.toUtf8()),"Question contained a hidden answer");
                QJsonObject answers;for(const auto& q:input["questions"].toArray())answers[q.toObject()["question"].toString()]=code;
                require(!answers.isEmpty(),"Permission request omitted native questions");input["answers"]=answers;
                const auto accepted=options.permissionRequests->respond(event.data["request_id"].toString(),{{"behavior","allow"},{"updatedInput",input}});
                require(accepted["accepted"].toBool(),"Host answer was not accepted");
            }
            if(event.kind==a::EventKind::ToolFinished)std::cerr<<QJsonDocument(a::toJson(event)).toJson(QJsonDocument::Compact).constData()<<'\n';
        }).result.get();
        std::cerr<<QJsonDocument(a::toJson(result)).toJson(QJsonDocument::Compact).constData()<<"\n";
        require(result.status==a::RunStatus::Completed&&result.usage.generatedTokens>0&&reviews==1,"Native question run failed");
        require(result.text.trimmed()==code,"Native model did not consume the host answer exactly");
        std::cout<<QJsonDocument(QJsonObject{{"user_questions",true},{"model",uri},{"guided_native_tools",QJsonArray{"AskUserQuestion"}},
            {"code",code},{"question",model->asked},{"host_answer_consumed",true},{"reviews",reviews},{"result",a::toJson(result)}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
        engine.close();return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}
