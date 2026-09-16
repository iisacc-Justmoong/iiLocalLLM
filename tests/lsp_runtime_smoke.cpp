#include <agent/Engine.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QUuid>
#include <filesystem>
#include <iostream>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void write(const QString& path,const QByteArray& bytes){QFile file(path);require(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size(),"Cannot write fixture");}
QByteArray read(const QString& path){QFile file(path);require(file.open(QIODevice::ReadOnly),"Cannot read fixture");return file.readAll();}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);if(argc!=2&&argc!=4)return 2;
    try {
        QTemporaryDir root(QDir::current().filePath("lsp-native-XXXXXX"));require(root.isValid(),"Cannot create fixture");const auto work=root.filePath("work");QDir().mkpath(work);
        require(QProcess::execute("git",{"init","-q",work})==0,"Cannot initialize fixture repository");
        const auto marker="symbol_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(12);
        const QByteArray source=("int "+marker+"(int value) { return value + 3; }\nint caller() { return "+marker+"(7); }\nstruct Base { virtual int run() = 0; };\nstruct Impl : Base { int run() override { return caller(); } };\nint main() { Impl x; return x.run(); }\n").toUtf8();write(work+"/main.cpp",source);
        write(work+"/compile_flags.txt","-xc++\n-std=c++20\n-Wno-missing-prototypes\n");
        a::LspServerOptions config;config.name="clangd";config.command=QString::fromLocal8Bit(argv[1]);config.arguments={"--background-index=false","--pch-storage=memory","--log=error","--enable-config=false","--compile-commands-dir="+work};config.extensions={{".cpp","cpp"}};
        a::LspOptions options;options.servers={config};a::Lsp lsp(options);a::ToolContext context;context.sessionId="native";context.workingDirectory=work;
        auto args=[](QString op,int line,int column,QString search=QString()){QJsonObject result{{"operation",op},{"filePath","main.cpp"},{"line",line},{"character",column}};if(!search.isNull())result["query"]=search;return result;};
        QJsonObject results;
        for(const auto& op:{"goToDefinition","findReferences","hover","documentSymbol","workspaceSymbol","prepareCallHierarchy","incomingCalls","outgoingCalls","goToImplementation"}) {
            int line=1,col=5;if(QString(op)=="goToDefinition"){line=2;col=source.split('\n')[1].indexOf(marker.toUtf8())+1;}else if(QString(op)=="outgoingCalls"){line=2;col=5;}else if(QString(op)=="goToImplementation"){line=3;col=27;}
            auto request=args(op,line,col);if(QString(op)=="workspaceSymbol")request["query"]=marker;
            const auto result=lsp.query(request,context);std::cerr<<op<<": "<<result.text.toStdString()<<std::endl;
            require(!result.isError&&result.data["resultCount"].toInt()>0,"clangd returned no required result");results[op]=result.data;
        }
        require(results["goToDefinition"].toObject()["data"].toArray().first().toObject()["range"].toObject()["start"].toObject()["line"].toInt(-1)==0,"Definition location is wrong");
        write(work+"/main.cpp",source+"int broken() { return missing_symbol; }\n");lsp.query(args("documentSymbol",1,1),context);QJsonObject diagnostics;
        for(int i=0;i<100;++i){diagnostics=lsp.status(context);if(QJsonDocument(diagnostics).toJson().contains("missing_symbol"))break;QThread::msleep(20);}
        require(QJsonDocument(diagnostics).toJson().contains("missing_symbol"),"clangd did not publish changed-file diagnostics");
        write(work+"/main.cpp",source);lsp.query(args("documentSymbol",1,1),context);QJsonObject repaired;
        for(int i=0;i<100;++i){repaired=lsp.status(context);if(!repaired["diagnostics"].toArray().isEmpty()&&repaired["diagnostics"].toArray().first().toObject()["diagnostics"].toArray().isEmpty())break;QThread::msleep(20);}
        std::cerr<<"Changed diagnostics: "<<QJsonDocument(diagnostics).toJson(QJsonDocument::Compact).constData()<<"\nRepaired diagnostics: "<<QJsonDocument(repaired).toJson(QJsonDocument::Compact).constData()<<std::endl;
        require(!repaired["diagnostics"].toArray().isEmpty()&&repaired["diagnostics"].toArray().first().toObject()["version"].toInt()==3&&repaired["diagnostics"].toArray().first().toObject()["diagnostics"].toArray().isEmpty(),"clangd did not publish an empty current-version diagnostic set after repair");lsp.close();
        QJsonObject report{{"passed",true},{"marker",marker},{"operations",results},{"diagnostics_before_repair",diagnostics},{"diagnostics_after_repair",repaired},{"model_tested",argc==4}};
        if(argc==4) {
            const auto uri=QString::fromLocal8Bit(argv[3]),catalog=QDir(QString::fromLocal8Bit(argv[2])).filePath(modelId(uri));const auto manifest=parseModelManifest(QJsonDocument::fromJson(read(catalog+"/manifest.json")).object());
            const auto models=root.filePath("models"),package=models+'/'+manifest.id;QDir().mkpath(package);
            for(const auto& item:manifest.files){const auto target=package+'/'+item.path;QDir().mkpath(QFileInfo(target).absolutePath());std::error_code error;std::filesystem::create_hard_link((catalog+'/'+item.path).toStdString(),target.toStdString(),error);require(!error||QFile::copy(catalog+'/'+item.path,target),"Cannot provision model");}
            write(package+"/manifest.json",QJsonDocument(manifestObject(manifest)).toJson());ServiceOptions so;so.modelsDirectory=models;Service service(so);ModelLoadRequest load{uri,8192};load.options={{"tool_grammar",true},{"enable_thinking",false}};service.loadModel(load).get();
            auto model=std::make_shared<a::ServiceModel>(service);auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>();
            a::EngineOptions eo;eo.sessionsDirectory=root.filePath("sessions");eo.lsp=options;eo.lsp.deferred=false;eo.projectContext.enabled=false;eo.skills.enabled=false;eo.compaction.automatic=false;eo.toolSearch.enabled=false;QJsonArray tools;
            eo.hooks.append([&](const a::HookInput& input,const auto&){if(input.kind==a::HookKind::AfterTool){tools.append(QJsonObject{{"name",input.call.name},{"arguments",input.call.arguments},{"result",input.result.data},{"is_error",input.result.isError}});}return a::HookResult{};});
            a::Engine engine(model,registry,policy,eo);auto session=engine.createSession(uri,work,"Use LSP documentSymbol to inspect the requested source file. Return the exact function identifier beginning with symbol_. Do not guess names.");
            a::RunRequest request{session.id,"Inspect main.cpp using LSP documentSymbol with line 1 and character 1. Return only the exact function name that begins with symbol_."};request.maxTurns=4;request.generation.maxTokens=512;request.generation.temperature=0;
            const auto result=engine.run(request).result.get();std::cerr<<QJsonDocument(a::toJson(result)).toJson().constData()<<std::endl;
            require(result.status==a::RunStatus::Completed&&result.text.contains(marker),"Native model did not return the symbol found only in clangd's results");
            require(tools.size()==1&&tools.first().toObject()["name"]=="LSP"&&!tools.first().toObject()["is_error"].toBool(),"Expected one successful model LSP tool call");
            require(engine.session(session.id).messages.size()==4,"Unexpected main transcript");engine.runLsp(session.id,args("hover",1,5));require(engine.session(session.id).messages.size()==4,"Host LSP query changed transcript");
            report["answer"]=a::toJson(result);report["tools"]=tools;report["model_requests_unmodified"]=true;report["server_status"]=engine.lspStatus(session.id);engine.endSession(session.id);require(engine.lspStatus(session.id)["servers"].toArray().isEmpty(),"Session end retained LSP process");report["session_cleanup"]=true;
        }
        std::cout<<QJsonDocument(report).toJson(QJsonDocument::Compact).constData()<<std::endl;
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}return 0;
}
