#include <agent/Engine.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <filesystem>
#include <iostream>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void write(const QString& path,const QByteArray& bytes){QFile file(path);require(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size(),"Cannot write fixture");}
QByteArray read(const QString& path){QFile file(path);require(file.open(QIODevice::ReadOnly),"Cannot read fixture");return file.readAll();}
void git(const QString& root,const QStringList& args){QProcess process;process.setWorkingDirectory(root);process.start("git",args);require(process.waitForStarted()&&process.waitForFinished(30000)&&process.exitCode()==0,"Fixture Git failed");}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);if(argc!=3)return 2;
    try {
        QTemporaryDir root(QDir::current().filePath("worktree-native-XXXXXX"));require(root.isValid(),"Cannot create fixture");
        const auto work=root.filePath("work");QDir().mkpath(work);git(work,{"init","-q","-b","main"});git(work,{"config","user.name","Fixture"});git(work,{"config","user.email","fixture@example.invalid"});
        const auto marker="owned_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(12);
        write(work+"/marker.txt",marker.toUtf8());git(work,{"add","."});git(work,{"commit","-qm","initial"});
        const auto uri=QString::fromLocal8Bit(argv[2]),catalog=QDir(QString::fromLocal8Bit(argv[1])).filePath(modelId(uri));
        const auto manifest=parseModelManifest(QJsonDocument::fromJson(read(catalog+"/manifest.json")).object());
        const auto models=root.filePath("models"),package=models+'/'+manifest.id;QDir().mkpath(package);
        for(const auto& item:manifest.files){const auto target=package+'/'+item.path;QDir().mkpath(QFileInfo(target).absolutePath());std::error_code error;
            std::filesystem::create_hard_link((catalog+'/'+item.path).toStdString(),target.toStdString(),error);require(!error||QFile::copy(catalog+'/'+item.path,target),"Cannot provision model");}
        write(package+"/manifest.json",QJsonDocument(manifestObject(manifest)).toJson());
        ServiceOptions so;so.modelsDirectory=models;Service service(so);ModelLoadRequest load{uri,8192};load.options={{"tool_grammar",true},{"enable_thinking",false}};service.loadModel(load).get();
        auto model=std::make_shared<a::ServiceModel>(service);auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,work);
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::DontAsk,QList<a::PermissionRule>{{"EnterWorktree",a::PermissionBehavior::Allow},{"ExitWorktree",a::PermissionBehavior::Allow}});
        a::EngineOptions eo;eo.sessionsDirectory=root.filePath("sessions");eo.worktrees.enabled=true;eo.worktrees.deferred=false;eo.worktrees.fetchMissingBase=false;eo.worktrees.directory=root.filePath("worktrees");
        eo.projectContext.enabled=false;eo.skills.enabled=false;eo.compaction.automatic=false;eo.toolSearch.enabled=false;
        eo.toolFilter=[](const a::ToolDefinition& tool){return QStringList{"EnterWorktree","ExitWorktree","Read"}.contains(tool.name);};
        QJsonArray calls;eo.hooks.append([&](const a::HookInput& input,const auto&){if(input.kind==a::HookKind::AfterTool)
            calls.append(QJsonObject{{"name",input.call.name},{"arguments",input.call.arguments},{"result",input.result.data},{"is_error",input.result.isError}});return a::HookResult{};});
        a::Engine engine(model,registry,policy,eo);const auto session=engine.createSession(uri,work,"Follow the user's requested worktree sequence with real tool calls. Read the marker only after entering the worktree. Do not guess the marker.");
        a::RunRequest request{session.id,"Create and enter a worktree named native using EnterWorktree. Then read marker.txt using Read. Then leave it using ExitWorktree with action keep. Finally return only the exact marker you read."};
        request.maxTurns=8;request.generation.maxTokens=1024;request.generation.temperature=0;
        const auto result=engine.run(request).result.get();std::cerr<<QJsonDocument(a::toJson(result)).toJson().constData()<<std::endl;
        require(result.status==a::RunStatus::Completed&&result.text.contains(marker),"Native model did not finish the worktree request with the observed marker");
        require(calls.size()==3,"Expected EnterWorktree, Read, ExitWorktree exactly once");
        const auto path=calls.first().toObject()["result"].toObject()["worktreePath"].toString();
        for(const auto& call:calls)require(!call.toObject()["is_error"].toBool(),"Native model tool failed");
        require(calls[0].toObject()["name"]=="EnterWorktree"&&calls[1].toObject()["name"]=="Read"&&calls[2].toObject()["name"]=="ExitWorktree","Native tool order was incorrect");
        require(calls[1].toObject()["result"].toObject()["path"]==path+"/marker.txt","Native Read used the wrong workspace");
        require(engine.session(session.id).workingDirectory==work&&!engine.worktreeStatus(session.id)["active"].toBool(),"Native model did not return to the original workspace");
        require(QFileInfo(path).isDir()&&read(work+"/marker.txt")==marker.toUtf8(),"keep did not preserve the worktree and original data");
        const auto modelCalls=calls;const auto messageCount=engine.session(session.id).messages.size();
        require(!engine.runWorktreeTool(session.id,"EnterWorktree",{{"name","native"}}).isError,"Host could not resume owned worktree");
        require(!engine.runWorktreeTool(session.id,"ExitWorktree",{{"action","remove"}}).isError&&!QFileInfo(path).exists(),"Host could not remove clean owned worktree");
        require(engine.session(session.id).messages.size()==messageCount,"Host worktree control changed transcript");
        const QJsonObject report{{"passed",true},{"model",uri},{"model_requests_unmodified",true},{"marker",marker},{"answer",a::toJson(result)},
            {"tools",modelCalls},{"original_preserved",true},{"host_resume_remove",true},{"host_transcript_unchanged",true}};
        std::cout<<QJsonDocument(report).toJson(QJsonDocument::Compact).constData()<<std::endl;
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}return 0;
}
