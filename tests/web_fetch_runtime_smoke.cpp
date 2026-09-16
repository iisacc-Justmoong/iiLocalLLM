#include <agent/Engine.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include "../third_party/cpp-httplib/httplib.h"
#include <atomic>
#include <filesystem>
#include <iostream>
#include <thread>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
QByteArray read(const QString& path){QFile file(path);require(file.open(QIODevice::ReadOnly),"Cannot read model manifest");return file.readAll();}
struct Endpoint {
    httplib::Server server;std::thread worker;int port;std::atomic<int> calls=0;
    Endpoint(QString marker){server.Get("/document",[this,marker](const auto& request,auto& response){++calls;
        require(!request.has_header("Cookie")&&!request.has_header("Authorization"),"Unexpected web authentication");
        response.set_content(("<html><head><title>Fixture</title><script>IGNORED</script></head><body><h1>Deployment record</h1><p>"
            "The current deployment verification code is <strong>"+marker+"</strong>. This is the only verification code on this page.</p></body></html>").toStdString(),"text/html; charset=utf-8");});
        port=server.bind_to_any_port("127.0.0.1");require(port>0,"Cannot bind native web fixture");worker=std::thread([this]{server.listen_after_bind();});server.wait_until_ready();}
    ~Endpoint(){server.stop();worker.join();}
    QString origin()const{return QString("http://127.0.0.1:%1").arg(port);}
};
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);if(argc!=3)return 2;
    try {
        QTemporaryDir root(QDir::current().filePath("web-native-XXXXXX"));require(root.isValid(),"Cannot create native fixture");
        const auto uri=QString::fromLocal8Bit(argv[2]),source=QDir(QString::fromLocal8Bit(argv[1])).filePath(modelId(uri));
        const auto manifest=parseModelManifest(QJsonDocument::fromJson(read(source+"/manifest.json")).object());require(modelUri(manifest.id)==uri,"Model mismatch");
        const auto models=root.filePath("models"),package=models+'/'+manifest.id;QDir().mkpath(package);
        for(const auto& item:manifest.files){const auto target=package+'/'+item.path;QDir().mkpath(QFileInfo(target).absolutePath());std::error_code error;
            std::filesystem::create_hard_link((source+'/'+item.path).toStdString(),target.toStdString(),error);require(!error||QFile::copy(source+'/'+item.path,target),"Cannot provision model");}
        QFile manifestFile(package+"/manifest.json");require(manifestFile.open(QIODevice::WriteOnly),"Cannot write manifest");manifestFile.write(QJsonDocument(manifestObject(manifest)).toJson());manifestFile.close();
        ServiceOptions so;so.modelsDirectory=models;Service service(so);ModelLoadRequest load{uri,8192};load.options={{"tool_grammar",true},{"enable_thinking",false}};(void)service.loadModel(load).get();
        const auto marker="WEB_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(12);Endpoint endpoint(marker);
        const auto work=root.filePath("work");QDir().mkpath(work);auto model=std::make_shared<a::ServiceModel>(service);auto registry=std::make_shared<a::ToolRegistry>();
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{{"WebFetch(domain:127.0.0.1)",a::PermissionBehavior::Allow}});
        a::EngineOptions options;options.sessionsDirectory=root.filePath("sessions");options.webFetchEnabled=true;options.webFetch.deferred=false;
        options.webFetch.privateOrigins={endpoint.origin()};options.webFetch.maxTokens=512;options.webFetch.summaryTimeoutMs=120000;
        options.skills.enabled=false;options.projectContext.enabled=false;options.compaction.automatic=false;options.toolSearch.enabled=false;
        QJsonArray tools;options.hooks.append([&](const a::HookInput& input,const auto&){if(input.kind==a::HookKind::AfterTool){
            QJsonObject item{{"name",input.call.name},{"arguments",input.call.arguments},{"result",input.result.data},{"is_error",input.result.isError}};tools.append(item);
            std::cerr<<QJsonDocument(item).toJson(QJsonDocument::Compact).constData()<<std::endl;}return a::HookResult{};});
        a::Engine engine(model,registry,policy,options);const auto id=engine.createSession(uri,work,"Use WebFetch to read a requested page. After its result, answer with the exact verification code only. Do not guess codes.").id;
        a::RunRequest request{id,"Open "+endpoint.origin()+"/document with WebFetch. Extract the deployment verification code and answer with that code only."};request.maxTurns=4;request.generation.maxTokens=512;request.generation.temperature=0;
        const auto answer=engine.run(request).result.get();std::cerr<<QJsonDocument(a::toJson(answer)).toJson(QJsonDocument::Compact).constData()<<std::endl;
        require(answer.status==a::RunStatus::Completed&&answer.text.contains(marker),"Native agent did not return the code found only on the page");
        require(tools.size()==1&&tools[0].toObject()["name"]=="WebFetch"&&!tools[0].toObject()["is_error"].toBool(),"Expected one successful native WebFetch");
        const auto result=tools[0].toObject()["result"].toObject();require(result["result"].toString().contains(marker)&&result["usage"].toObject()["generated_tokens"].toInt()>0,"Native extractor did not read the code");
        const auto cached=engine.runWebFetch(id,{{"url",endpoint.origin()+"/document"},{"prompt","What is the exact deployment verification code? Reply only with that code."}});
        require(!cached.isError&&cached.text.contains(marker)&&cached.data["cached"].toBool()&&endpoint.calls==1,"Cached content did not use a fresh native extraction");
        options.webFetchEnabled=false;options.sessionsDirectory=root.filePath("disabled");a::Engine disabled(model,registry,policy,options);
        require(!disabled.webFetchTool(),"Disabled control exposed WebFetch");require(engine.session(id).messages.size()==4,"Direct WebFetch changed the transcript");
        std::cout<<QJsonDocument(QJsonObject{{"passed",true},{"model",uri},{"marker",marker},{"requests_unmodified",true},{"answer",a::toJson(answer)},
            {"tools",tools},{"cached_extraction",cached.data},{"http_requests",endpoint.calls.load()},{"disabled_control",true},{"main_message_count",4}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
    }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}return 0;
}
