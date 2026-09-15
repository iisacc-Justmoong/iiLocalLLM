#include <agent/CommandHooks.h>
#include <agent/Engine.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <filesystem>
#include <iostream>
#include <thread>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
using namespace std::chrono_literals;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void write(const QString& path,const QByteArray& bytes) {
    QFile file(path);require(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size(),"Cannot write native fixture");
}
template<class F> void waitFor(F condition,int seconds=10) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
    while(!condition()){require(std::chrono::steady_clock::now()<deadline,"Native async hook deadline expired");std::this_thread::sleep_for(10ms);}
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);if(argc!=2)return 2;
    try {
        QTemporaryDir root(QDir::current().filePath("async-native-XXXXXX"));require(root.isValid(),"Cannot create native fixture");
        const auto models=root.filePath("models"),package=models+"/async-fixture";QDir().mkpath(package);
        ModelManifest manifest{"async-fixture","qwen2","gguf","Q4_K_M",32768,{"text-generation","chat"},"model.gguf",
            {{"model.gguf",491400032,"74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db"}}};
        std::error_code error;std::filesystem::create_hard_link(argv[1],(package+"/model.gguf").toStdString(),error);
        require(!error||QFile::copy(QString::fromLocal8Bit(argv[1]),package+"/model.gguf"),"Cannot provision model weights");
        write(package+"/manifest.json",QJsonDocument(manifestObject(manifest)).toJson());
        ServiceOptions serviceOptions;serviceOptions.modelsDirectory=models;Service service(serviceOptions);
        const auto uri=modelUri(manifest.id);(void)service.loadModel({uri,4096}).get();
        for(const bool rewake:{false,true}) {
            const auto mode=rewake?QString("rewake"):QString("context"),workspace=root.filePath(mode);QDir().mkpath(workspace);
            const auto code="CHECK_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(10);
            const auto text="Validation code: "+code;
            const auto output=rewake?"printf '%s' '"+text+"' >&2; exit 2"
                :"printf '%s\\n' '"+QString::fromUtf8(QJsonDocument(QJsonObject{{"systemMessage",text}}).toJson(QJsonDocument::Compact))+"'";
            const auto command="cat > input.json; touch ready; while [ ! -f release ]; do sleep .01; done; "+output;
            const QJsonObject settings{{"hooks",QJsonObject{{"Stop",QJsonArray{QJsonObject{{"hooks",QJsonArray{QJsonObject{
                {"type","command"},{"command",command},{rewake?"asyncRewake":"async",true},{"once",true},{"timeout",120}}}}}}}}}};
            a::CommandHookOptions hookOptions;hookOptions.workingDirectory=workspace;a::CommandHooks hooks(settings,hookOptions);
            a::EngineOptions options;options.sessionsDirectory=root.filePath(mode+"-sessions");options.projectContext.enabled=false;
            options.compaction.automatic=false;options.hooks={hooks.callback()};
            a::Engine engine(std::make_shared<a::ServiceModel>(service),std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(),options);
            const auto id=engine.createSession(uri,workspace,"When the background validation code arrives, output that code exactly. Never invent a code.").id;
            a::RunRequest request{id,"A background check will provide a validation code. Reply READY while waiting. Once the code arrives, reply with that code exactly."};
            request.generation.temperature=0;request.generation.maxTokens=128;request.maxTurns=4;
            const auto first=engine.run(request).result.get();require(first.status==a::RunStatus::Completed,"Initial native run failed");
            require(first.usage.generatedTokens>0,"No native generation evidence");write(workspace+"/release","go");
            QString answer;QJsonObject final;
            if(rewake) {
                waitFor([&]{const auto run=engine.hookStatus(id)["wake_run"].toObject();return !run.isEmpty()&&run["state"]!="running";},120);
                const auto wake=engine.hookStatus(id)["wake_run"].toObject();require(wake["state"]=="completed","Native wake failed");
                final=wake["result"].toObject();answer=final["text"].toString();
            }else {
                waitFor([&]{return engine.queuedInputs(id)["count"].toInt()==1;});
                request.prompt="Return exactly the validation code provided by the background check.";
                const auto second=engine.run(request).result.get();require(second.status==a::RunStatus::Completed,"Native context run failed");
                final=a::toJson(second);answer=second.text;
            }
            require(answer.contains(code),"Native model did not consume the hidden background validation code");
            engine.close();bool delivered=false;
            for(const auto& message:engine.session(id).messages)if(message.role==a::MessageRole::User&&message.text.contains(code))delivered=true;
            require(delivered,"Missing durable background context");
            std::cout<<QJsonDocument(QJsonObject{{"async_hooks",true},{"mode",mode},{"code",code},{"answer",answer},
                {"context_delivered",delivered},{"initial",a::toJson(first)},{"final",final}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
        }
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}
