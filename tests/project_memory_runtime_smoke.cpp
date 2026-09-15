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
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void write(const QString& path,const QByteArray& bytes) {
    QFile file(path);require(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size(),"Cannot write native memory fixture");
}
// Tool choice is guided, but every tool argument and recalled answer comes from
// the actual local model. The hidden code is never supplied by this adapter.
class Guided final:public a::Model {
    a::ServiceModel native;
public:
    bool save;int turns=0;QJsonArray generatedCalls;QJsonArray memoryInputs;
    Guided(Service& service,bool saving):native(service),save(saving){}
    a::ModelReply generate(const a::ModelRequest& input,const CancellationToken& token,const TextCallback& delta)override {
        auto request=input;request.enableThinking=false;request.generation.temperature=0;
        for(const auto& message:request.messages) {
            if(message.role==a::MessageRole::Tool)require(!message.isError,"Native memory tool failed");
            if(message.metadata.contains("iilocal.project_memory"))memoryInputs.append(message.text);
        }
        if(save&&turns<2) {
            const auto name=turns==0?QString("Read"):QString("Write");
            request.tools.removeIf([&](const auto& tool){return tool.name!=name;});require(request.tools.size()==1,"Expected native file tool");
            request.toolChoice="required";
            request.systemPrompt=turns==0?"Call Read once to read the user's source file. Use its absolute path exactly as supplied."
                :"Call Write once to save the validation code from the completed Read result into the supplied MEMORY.md path. Include the exact code in the saved text.";
        } else {
            request.tools.clear();request.toolChoice="none";
            request.systemPrompt=save?"The note was saved. Reply with SAVED."
                :"Return only the exact validation code in the provided persistent project memory. If the current context contains no code, return MISSING. Do not invent a code.";
        }
        ++turns;const auto reply=native.generate(request,token,delta);
        for(const auto& call:reply.toolCalls)generatedCalls.append(QJsonObject{{"name",call.name},{"arguments",call.arguments}});
        return reply;
    }
};
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);if(argc!=3)return 2;
    try {
        QTemporaryDir root(QDir::current().filePath("memory-native-XXXXXX"));require(root.isValid(),"Cannot create memory fixture");
        const auto uri=QString::fromLocal8Bit(argv[2]);const auto source=QDir(QString::fromLocal8Bit(argv[1])).filePath(modelId(uri));
        QFile manifestFile(source+"/manifest.json");require(manifestFile.open(QIODevice::ReadOnly),"Cannot open model manifest");
        const auto manifest=parseModelManifest(QJsonDocument::fromJson(manifestFile.readAll()).object());require(modelUri(manifest.id)==uri,"Model identity mismatch");
        const auto models=root.filePath("models"),package=models+'/'+manifest.id;QDir().mkpath(package);
        for(const auto& file:manifest.files) {
            const auto from=source+'/'+file.path,to=package+'/'+file.path;QDir().mkpath(QFileInfo(to).absolutePath());
            std::error_code error;std::filesystem::create_hard_link(from.toStdString(),to.toStdString(),error);
            require(!error||QFile::copy(from,to),"Cannot provision native model");
        }
        write(package+"/manifest.json",QJsonDocument(manifestObject(manifest)).toJson());
        ServiceOptions serviceOptions;serviceOptions.modelsDirectory=models;Service service(serviceOptions);
        ModelLoadRequest load{uri,8192};load.options["tool_grammar"]=true;load.options["enable_thinking"]=false;
        (void)service.loadModel(load).get();
        const auto workspace=root.filePath("work");QDir().mkpath(workspace);
        const auto sourceFile=QDir(workspace).filePath("validation.txt");
        const auto code="MEMORY_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(12);
        write(sourceFile,("Validation code: "+code).toUtf8());
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,workspace,{},QStringList{root.filePath("sessions")});
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::AcceptEdits);
        a::EngineOptions options;options.sessionsDirectory=root.filePath("sessions");options.projectMemoryEnabled=true;
        options.projectContext.enabled=false;options.skills.enabled=false;options.compaction.automatic=false;options.toolSearch.enabled=false;
        auto writer=std::make_shared<Guided>(service,true);a::RunResult saved,recalled,control;QString indexPath;
        {
            a::Engine engine(writer,registry,policy,options);const auto id=engine.createSession(uri,workspace).id;
            indexPath=engine.memory(id)["index_path"].toString();
            const auto prompt="Read "+sourceFile+" and remember its validation code by writing a concise note to "+indexPath+". Do not change the code.";
            require(!prompt.contains(code),"Initial prompt must not disclose the code");
            a::RunRequest request{id,prompt};request.maxTurns=3;request.generation.maxTokens=512;request.generation.temperature=0;
            saved=engine.run(request).result.get();
            std::cerr<<QJsonDocument(QJsonObject{{"phase","save"},{"result",a::toJson(saved)}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
            require(saved.status==a::RunStatus::Completed&&saved.usage.generatedTokens>0,"Native memory save failed");
            require(writer->generatedCalls.size()==2&&writer->generatedCalls[0].toObject()["name"]=="Read"
                &&writer->generatedCalls[1].toObject()["name"]=="Write","Native model did not read then write memory");
            require(writer->generatedCalls[1].toObject()["arguments"].toObject()["content"].toString().contains(code),"Native model did not write the observed code");
            require(engine.memory(id)["index"].toString().contains(code),"Saved index omitted code");
        }
        // Remove the original source; the second Engine receives a fresh empty
        // transcript and no file tools. It can only learn the code via memory.
        require(QFile::remove(sourceFile),"Cannot remove source before recall");
        auto reader=std::make_shared<Guided>(service,false);
        {
            a::Engine engine(reader,std::make_shared<a::ToolRegistry>(),policy,options);const auto id=engine.createSession(uri,workspace).id;
            require(engine.session(id).messages.isEmpty(),"Recall transcript was not empty");
            a::RunRequest request{id,"What validation code did we save? Return only the code."};request.maxTurns=1;request.generation.maxTokens=128;request.generation.temperature=0;
            recalled=engine.run(request).result.get();
            std::cerr<<QJsonDocument(QJsonObject{{"phase","recall"},{"result",a::toJson(recalled)}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
            require(recalled.status==a::RunStatus::Completed&&recalled.text.trimmed()==code,"New native session did not recall the exact memory code");
        }
        options.projectMemoryEnabled=false;auto isolated=std::make_shared<Guided>(service,false);
        {
            a::Engine engine(isolated,std::make_shared<a::ToolRegistry>(),policy,options);const auto id=engine.createSession(uri,workspace).id;
            a::RunRequest request{id,"What validation code did we save? Return only the code."};request.maxTurns=1;request.generation.maxTokens=128;request.generation.temperature=0;
            control=engine.run(request).result.get();
            std::cerr<<QJsonDocument(QJsonObject{{"phase","disabled"},{"result",a::toJson(control)}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
            require(control.status==a::RunStatus::Completed&&control.text.trimmed()=="MISSING"&&isolated->memoryInputs.isEmpty(),"Disabled-memory control leaked or invented the code");
        }
        std::cout<<QJsonDocument(QJsonObject{{"passed",true},{"project_memory",true},{"model",uri},{"guided",true},{"code",code},
            {"generated_calls",writer->generatedCalls},{"save",a::toJson(saved)},{"recall",a::toJson(recalled)},
            {"disabled_control",a::toJson(control)},{"source_removed_before_recall",true},{"fresh_empty_recall_session",true}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}
