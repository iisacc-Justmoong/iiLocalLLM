#include <agent/Engine.h>
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
// Guided native acceptance: the host selects the next tool, but llama.cpp must
// generate every call and argument. No model replies or tool results are faked.
class Guided final:public a::Model {
    a::ServiceModel native;
public:
    int turn=0;QString planPath;QStringList used;
    explicit Guided(Service& service):native(service){}
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken& token,const TextCallback& output) override {
        auto next=request;next.enableThinking=false;
        for(const auto& message:request.messages)if(message.role==a::MessageRole::Tool)require(!message.isError,"A native planning tool failed");
        QString tool;
        if(turn==0){tool="EnterPlanMode";next.systemPrompt="Call EnterPlanMode now with an empty object.";}
        if(turn==1){tool="Write";next.systemPrompt="Call Write exactly once with path "+planPath+" and content: Inspect source and test changes. Do not add any other text.";}
        if(turn==2){tool="ExitPlanMode";next.systemPrompt="The plan file is written. Call ExitPlanMode now with an empty object. Do not supply plan content.";}
        if(turn==3){next.systemPrompt="Return exactly the validation code in the HOST APPROVED plan. Do not return the original draft or explain.";}
        require(turn<4,"Native planning flow exceeded four turns");++turn;
        next.tools.removeIf([&](const auto& value){return value.name!=tool;});next.toolChoice=tool.isEmpty()?"none":"required";
        if(tool=="ExitPlanMode") {auto schema=next.tools[0].inputSchema;schema["properties"]=QJsonObject{};next.tools[0].inputSchema=schema;}
        const auto reply=native.generate(next,token,output);
        if(!tool.isEmpty()){require(reply.toolCalls.size()==1&&reply.toolCalls[0].name==tool,"Native model did not generate the requested planning tool");used.append(tool);}
        return reply;
    }
};
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);if(argc!=3)return 2;
    try {
        QTemporaryDir root(QDir::current().filePath("plan-native-XXXXXX"));require(root.isValid(),"Cannot create native fixture");
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
        const auto code="PLAN_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(10);
        auto model=std::make_shared<Guided>(service);a::EngineOptions options;options.sessionsDirectory=root.filePath("sessions");
        options.planToolsEnabled=true;options.planToolsDeferred=false;options.skills.enabled=false;options.toolSearch.enabled=false;options.projectContext.enabled=false;options.compaction.automatic=false;
        int reviews=0;QString original;
        options.permissionResponse=[&](const auto& call,const auto&,const auto& c) {
            require(call.name=="ExitPlanMode","Unexpected native permission request");++reviews;
            QFile file(model->planPath);require(file.open(QIODevice::ReadOnly),"No model-written plan");original=QString::fromUtf8(file.readAll());
            require(!original.trimmed().isEmpty()&&!original.contains(code),"Draft contains a hidden host code");
            return a::PermissionResponse{a::PermissionBehavior::Allow,{},QJsonObject{{"plan","HOST APPROVED plan. Validation code: "+code}}};
        };
        a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(),options);const auto id=engine.createSession(uri,workspace).id;
        model->planPath=engine.planStatus(id)["plan_file_path"].toString();a::RunRequest request{id,"Prepare a plan, submit it for review, then return the host's validation code."};
        request.generation.temperature=0;request.generation.maxTokens=512;request.maxTurns=4;
        const auto result=engine.run(request,[](const a::Event& event){if(event.kind==a::EventKind::ToolFinished)std::cerr<<QJsonDocument(a::toJson(event)).toJson(QJsonDocument::Compact).constData()<<'\n';}).result.get();
        std::cerr<<QJsonDocument(a::toJson(result)).toJson(QJsonDocument::Compact).constData()<<"\n";
        require(result.status==a::RunStatus::Completed&&result.usage.generatedTokens>0,"Native plan run failed");
        require(model->used==QStringList{"EnterPlanMode","Write","ExitPlanMode"}&&reviews==1,"Missing native plan lifecycle");
        require(result.text.contains(code),"Native model did not consume the host-edited plan");const auto state=engine.planStatus(id);
        require(state["phase"]=="approved"&&state["approval_current"].toBool(),"Missing persisted native approval");
        std::cout<<QJsonDocument(QJsonObject{{"plan_mode",true},{"model",uri},{"guided_native_tools",QJsonArray::fromStringList(model->used)},
            {"code",code},{"draft",original},{"host_edit_consumed",true},{"reviews",reviews},{"result",a::toJson(result)},{"state",state}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
        engine.close();return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}
