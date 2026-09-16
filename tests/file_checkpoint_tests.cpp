#include <agent/FileCheckpoints.h>
#include <agent/Engine.h>
#include <agent/Api.h>
#include <agent/McpServer.h>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <QtCore/QCryptographicHash>
#include <QtTest/QTest>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
QString uuid(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
void write(const QString& path,const QByteArray& bytes){QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())throw std::runtime_error("Fixture write failed");}
QByteArray read(const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))throw std::runtime_error("Fixture read failed");return f.readAll();}
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("checkpoint-test-XXXXXX")};
    QString work=root.filePath("work"),state=root.filePath("state"),path=work+"/file.txt";
    a::ToolContext context{uuid(),{},work};a::FileCheckpoints history{state};
    Fixture(){if(!QDir().mkpath(work))throw std::runtime_error("Fixture mkdir failed");write(path,"original\r\n");}
    QString checkpoint(){context.fileCheckpointId=uuid();history.checkpoint(context.fileCheckpointId,context);return context.fileCheckpointId;}
    void edit(const QString& target,const QByteArray& bytes){std::optional<QByteArray> before;if(QFileInfo::exists(target))before=read(target);history.track(target,before,context);write(target,bytes);}
};
class EditModel:public a::Model {public:
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken&,const TextCallback&)override{
        const auto last=request.messages.last();
        if(last.role==a::MessageRole::User)return {{},{{uuid(),"Read",{{"path","file.txt"}}}}, {1,1}};
        if(last.role==a::MessageRole::Tool&&last.metadata["name"]=="Read")return {{},{{uuid(),"Write",{{"path","file.txt"},{"content","edited"}}}}, {1,1}};
        // Tool messages carry their call identity; find the last assistant tool.
        for(auto it=request.messages.crbegin();it!=request.messages.crend();++it)if(!it->toolCalls.isEmpty()){
            if(it->toolCalls.last().name=="Read")return {{},{{uuid(),"Write",{{"path","file.txt"},{"content","edited"}}}}, {1,1}};break;}
        return {"DONE",{}, {1,1}};
    }
};
a::EngineOptions options(QString state){a::EngineOptions o;o.sessionsDirectory=state;o.fileCheckpointsEnabled=true;o.skills.enabled=false;o.projectContext.enabled=false;o.toolSearch.enabled=false;o.compaction.automatic=false;return o;}
std::shared_ptr<a::ToolRegistry> tools(const QString& path){auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,path);return registry;}
}
class FileCheckpointTests:public QObject {
 Q_OBJECT
private slots:
 void restoresFirstVersionWithinATurnAndLaterVersionsAcrossTurns(){
    Fixture f;const auto original=read(f.path);const auto first=f.checkpoint();f.edit(f.path,"one");f.edit(f.path,"two");
    const auto second=f.checkpoint();f.edit(f.path,"three");
    const auto preview=f.history.rewind(first,true,f.context);QCOMPARE(preview["filesChanged"].toArray().size(),1);QCOMPARE(read(f.path),"three");
    a::FileCheckpoints resumed(f.state);QVERIFY(resumed.rewind(first,false,f.context)["complete"].toBool());QCOMPARE(read(f.path),original);
    QVERIFY(resumed.rewind(second,false,f.context)["complete"].toBool());QCOMPARE(read(f.path),"two");
    QVERIFY(resumed.rewind(second,true,f.context)["filesChanged"].toArray().isEmpty());
 }
 void lateTrackedFilesUseFirstBackupAndEmptyNewFilesAreDeleted(){
    Fixture f;const auto first=f.checkpoint();const auto second=f.checkpoint();const auto created=f.work+"/empty";
    f.edit(f.path,"modified");f.edit(created,{});
    const auto preview=f.history.rewind(first,true,f.context);QCOMPARE(preview["filesChanged"].toArray().size(),2);
    const auto result=f.history.rewind(first,false,f.context);QVERIFY(result["complete"].toBool());QVERIFY(!QFileInfo::exists(created));QCOMPARE(read(f.path),"original\r\n");
    QVERIFY(f.history.rewind(second,false,f.context)["complete"].toBool());QVERIFY(!QFileInfo::exists(created));
 }
 void deletedTrackedFilesAndPermissionsAreRestored(){
    Fixture f;QVERIFY(QFile::setPermissions(f.path,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
    const auto mode=QFileInfo(f.path).permissions();const auto first=f.checkpoint();f.edit(f.path,"new");QVERIFY(QFile::remove(f.path));const auto deleted=f.checkpoint();
    QVERIFY(f.history.rewind(first,false,f.context)["complete"].toBool());QCOMPARE(read(f.path),"original\r\n");QCOMPARE(QFileInfo(f.path).permissions(),mode);
    QVERIFY(f.history.rewind(deleted,false,f.context)["complete"].toBool());QVERIFY(!QFileInfo::exists(f.path));
 }
 void preflightCorruptionAndChangedPreviewNeverOverwriteOtherFiles(){
    Fixture f;const auto first=f.checkpoint();f.edit(f.path,"new");const auto next=f.work+"/second";f.edit(next,"new file");
    const auto preview=f.history.rewind(first,true,f.context);write(f.path,"external");
    QVERIFY_THROWS_EXCEPTION(Error,f.history.rewind(first,false,f.context,preview["fingerprint"].toString()));QCOMPARE(read(next),"new file");QCOMPARE(read(f.path),"external");
    const auto digest=QString::fromLatin1(QCryptographicHash::hash("original\r\n",QCryptographicHash::Sha256).toHex());write(f.state+'/'+f.context.sessionId+'/'+digest+".blob","corrupt");
    QVERIFY_THROWS_EXCEPTION(Error,f.history.rewind(first,false,f.context));QCOMPARE(read(next),"new file");
 }
 void rejectsSymlinksPrivateRootsForeignScopesAndTamperedManifest(){
    Fixture f;const auto first=f.checkpoint();f.edit(f.path,"new");
    auto denied=f.context;denied.protectedPaths={f.path};QVERIFY_THROWS_EXCEPTION(Error,f.history.rewind(first,false,denied));
    auto revision=f.context;revision.workspaceRevision=1;QVERIFY_THROWS_EXCEPTION(Error,f.history.rewind(first,false,revision));
    const auto outside=f.root.filePath("outside");write(outside,"outside");QVERIFY(QFile::remove(f.path));QVERIFY(QFile::link(outside,f.path));
    QVERIFY_THROWS_EXCEPTION(Error,f.history.rewind(first,false,f.context));QCOMPARE(read(outside),"outside");
    const auto journal=f.state+'/'+f.context.sessionId+"/state.json";auto data=QJsonDocument::fromJson(read(journal)).object();data["snapshots"]=42;write(journal,QJsonDocument(data).toJson());
    QVERIFY_THROWS_EXCEPTION(Error,f.history.list(f.context));
 }
 void retentionKeepsFirstBackupsAndBoundsSnapshotCount(){
    Fixture f;QString retained;for(int i=0;i<104;++i){const auto id=f.checkpoint();if(i==4)retained=id;f.edit(f.path,QByteArray::number(i));}
    const auto list=f.history.list(f.context);QCOMPARE(list["snapshots"].toArray().size(),100);QCOMPARE(list["sequence"].toInt(),104);
    QVERIFY(f.history.rewind(retained,false,f.context)["complete"].toBool());QCOMPARE(read(f.path),"3");
    const auto blobs=QDir(f.state+'/'+f.context.sessionId).entryList({"*.blob"});QVERIFY(blobs.size()<=101);
 }
 void cancellationAndOversizeFailBeforeMutation(){
    Fixture f;const auto first=f.checkpoint();f.edit(f.path,"new");auto cancelled=f.context;cancelled.cancellation.cancel();
    QVERIFY_THROWS_EXCEPTION(Error,f.history.rewind(first,false,cancelled));QCOMPARE(read(f.path),"new");
    write(f.path,QByteArray(1024*1024+1,'x'));QVERIFY_THROWS_EXCEPTION(Error,f.history.rewind(first,false,f.context));QCOMPARE(QFileInfo(f.path).size(),1024*1024+1);
 }
 void partialIoFailureReportsRestoredFilesPrecisely(){
    Fixture f;const auto nested=f.work+"/z";QVERIFY(QDir().mkpath(nested));const auto path=nested+"/file";write(path,"second original");
    const auto first=f.checkpoint();f.edit(f.path,"new");f.edit(path,"second new");const auto mode=QFileInfo(nested).permissions();
    QVERIFY(QFile::setPermissions(nested,QFileDevice::ReadOwner|QFileDevice::ExeOwner));
    const auto result=f.history.rewind(first,false,f.context);const auto readable=read(path);
    const bool permissionsRestored=QFile::setPermissions(nested,mode);QVERIFY(permissionsRestored);
    QVERIFY(!result["complete"].toBool());QCOMPARE(result["filesRestored"].toArray(),QJsonArray{f.path});QCOMPARE(result["errors"].toArray().size(),1);
    QCOMPARE(read(f.path),"original\r\n");QCOMPARE(readable,"second new");
 }
 void engineCreatesMessageCheckpointsAndExpiresReadsAfterRewind(){
    Fixture f;auto registry=tools(f.work);auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);auto model=std::make_shared<EditModel>();
    a::Engine engine(model,registry,policy,options(f.root.filePath("sessions")));const auto id=engine.createSession("fixture",f.work).id;
    a::RunRequest request;request.sessionId=id;request.prompt="change";const auto run=engine.run(request).result.get();QVERIFY2(run.status==a::RunStatus::Completed,qPrintable(run.errorMessage));QCOMPARE(read(f.path),"edited");
    const auto messages=engine.session(id).messages;const auto first=messages.first().id;QCOMPARE(engine.fileCheckpoints(id)["snapshots"].toArray().first().toObject()["message_id"].toString(),first);
    const auto dry=engine.rewindFiles(id,first,true);QVERIFY2(!dry.isError,qPrintable(dry.text));QCOMPARE(read(f.path),"edited");
    const auto result=engine.rewindFiles(id,first);QVERIFY2(!result.isError,qPrintable(result.text));QCOMPARE(read(f.path),"original\r\n");QCOMPARE(engine.session(id).messages.size(),messages.size());
    a::ToolContext context{id,{},f.work};auto guard=engine.bindWorkspaceContext(context,true);
    QVERIFY(a::ToolRunner(registry,policy).run({uuid(),"Write",{{"path","file.txt"},{"content","stale read"}}},context).isError);
    a::ToolContext independent{id,{},f.work};QVERIFY(!engine.bindWorkspaceContext(independent,false));
    QVERIFY_THROWS_EXCEPTION(Error,engine.checkpointFiles(id));guard.reset();const auto fork=engine.forkSession(id);
    QCOMPARE(engine.fileCheckpoints(fork.id)["snapshots"],engine.fileCheckpoints(id)["snapshots"]);
 }
 void nativeControlsTrackNewFilesAndRespectCurrentWriteDeny(){
    Fixture f;auto registry=tools(f.work);auto allow=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);auto model=std::make_shared<EditModel>();const auto config=options(f.root.filePath("sessions"));QString id,point;
    {a::Engine engine(model,registry,allow,config);id=engine.createSession("fixture",f.work).id;point=engine.checkpointFiles(id)["message_id"].toString();
        a::ToolContext context{id,{},f.work};auto guard=engine.bindWorkspaceContext(context,true);
        QVERIFY(!a::ToolRunner(registry,allow).run({uuid(),"Write",{{"path","created"},{"content","new"}}},context).isError);}
    auto deny=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"Write(*)",a::PermissionBehavior::Deny}});
    a::Engine denied(model,registry,deny,config);QVERIFY(denied.rewindFiles(id,point).isError);QCOMPARE(read(f.work+"/created"),"new");
    a::Engine resumed(model,registry,allow,config);QVERIFY(!resumed.rewindFiles(id,point).isError);QVERIFY(!QFileInfo::exists(f.work+"/created"));
 }
 void approvalCannotRestoreIntoARevokedAdditionalDirectory(){
    class Policy:public a::PermissionPolicy {public:QString extra;bool granted=true;
        a::PermissionDecision decide(const a::ToolDefinition& tool,const QJsonObject&,const a::ToolContext&)const override{return {tool.name=="RewindFiles"?a::PermissionBehavior::Ask:a::PermissionBehavior::Allow,{}};}
        QStringList workingDirectories(const a::ToolContext& c)const override{return granted?QStringList{c.workingDirectory,extra}:QStringList{c.workingDirectory};}
    };
    Fixture f;auto registry=tools(f.work);auto policy=std::make_shared<Policy>();policy->extra=f.root.filePath("extra");QVERIFY(QDir().mkpath(policy->extra));
    auto config=options(f.root.filePath("sessions"));config.permission=[policy](const auto&,const auto&,const auto&){policy->granted=false;return true;};
    a::Engine engine(std::make_shared<EditModel>(),registry,policy,config);const auto id=engine.createSession("fixture",f.work).id;const auto point=engine.checkpointFiles(id)["message_id"].toString();
    const auto path=policy->extra+"/file";{a::ToolContext context{id,{},f.work};auto guard=engine.bindWorkspaceContext(context,true);
        QVERIFY(!a::ToolRunner(registry,policy).run({uuid(),"Write",{{"path",path},{"content","preserve"}}},context).isError);}
    const auto result=engine.rewindFiles(id,point);QVERIFY(result.isError);QCOMPARE(read(path),"preserve");
 }
 void projectMemoryDoesNotJoinWorkspaceHistory(){
    Fixture f;auto registry=tools(f.work);auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);auto config=options(f.root.filePath("sessions"));
    config.projectMemoryEnabled=true;config.memoryRecall.enabled=false;config.memoryExtraction.enabled=false;
    a::Engine engine(std::make_shared<EditModel>(),registry,policy,config);const auto id=engine.createSession("fixture",f.work).id;
    const auto point=engine.checkpointFiles(id)["message_id"].toString();const auto index=engine.memory(id)["index_path"].toString();
    const auto result=engine.runMemoryTool(id,"Write",{{"path",index},{"content","persistent note"}});QVERIFY2(!result.isError,qPrintable(result.text));
    QCOMPARE(engine.fileCheckpoints(id)["total_tracked_files"].toInt(),0);QVERIFY(!engine.rewindFiles(id,point).isError);QCOMPARE(read(index),"persistent note");
 }
 void apiAndMcpBindHistoryToAuthenticatedOwner(){
    Fixture f;auto registry=tools(f.work);auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);auto model=std::make_shared<EditModel>();
    a::ApiOptions config;config.engine=options({});config.workingDirectory=f.work;config.stateDirectory=f.root.filePath("api");config.clientTokens={{"society",QString(48,'a')},{"dreamscapes",QString(48,'b')}};
    a::Api api(model,registry,policy,config);auto call=[&](QString method,QJsonObject params={},QString token=QString(48,'a')){return api.dispatch(method,params,token).result.get().toObject();};
    QVERIFY(call("agent.info")["file_checkpoints_enabled"].toBool());const auto id=call("agent.sessions.create",{{"model","fixture"}})["session_id"].toString();
    const auto run=call("agent.run",{{"session_id",id},{"prompt","change"}});QCOMPARE(read(f.path),"edited");
    const auto history=call("agent.checkpoints.list",{{"session_id",id}});QVERIFY2(!history["snapshots"].toArray().isEmpty(),QJsonDocument(QJsonObject{{"history",history},{"run",run}}).toJson().constData());
    const auto point=history["snapshots"].toArray().first().toObject()["message_id"].toString();
    const auto child=call("agent.sessions.fork",{{"session_id",id}})["session_id"].toString();QVERIFY(child!=id&&!child.isEmpty());
    QVERIFY_THROWS_EXCEPTION(Error,call("agent.sessions.fork",{{"session_id",child}},QString(48,'b')));
    QVERIFY(!call("agent.checkpoints.rewind",{{"session_id",child},{"message_id",point}})["is_error"].toBool());QCOMPARE(read(f.path),"original\r\n");
    QVERIFY_THROWS_EXCEPTION(Error,call("agent.checkpoints.list",{{"session_id",id}},QString(48,'b')));
    QVERIFY_THROWS_EXCEPTION(Error,call("agent.checkpoints.rewind",{{"session_id",id},{"message_id",point},{"dry_run","false"}}));
    auto engine=std::make_shared<a::Engine>(model,registry,policy,options(f.root.filePath("mcp")));a::McpServerOptions mo;mo.engine=engine;mo.model="fixture";mo.workingDirectory=f.work;
    const auto serverOptions=a::mcpServerOptions(registry,policy,mo);QVERIFY(serverOptions.experimentalCapabilities.contains("iisacc/fileCheckpoints"));QVERIFY(serverOptions.experimentalCapabilities.contains("iisacc/sessionFork"));mcp::ServerSession server(serverOptions);
    int number=0;auto rpc=[&](QString method,QJsonObject params){const int n=++number;server.receive(QJsonObject{{"jsonrpc","2.0"},{"id",n},{"method",method},{"params",params}});
        for(int i=0;i<400;++i)for(const auto& v:server.takeMessages(10))if(v.toObject()["id"]==n)return v.toObject();throw std::runtime_error("MCP timeout");};
    rpc("initialize",{{"protocolVersion","2025-11-25"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","test"},{"version","1"}}}});server.receive(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/initialized"}});
    auto tool=[&](QString name,QJsonObject args={}){const auto response=rpc("tools/call",{{"name",name},{"arguments",args}});if(response.contains("error"))throw std::runtime_error(QJsonDocument(response).toJson().constData());return response["result"].toObject();};
    const auto checkpoint=tool("iiLocalLLM.agent.checkpoints.create")["structuredContent"].toObject()["message_id"].toString();
    QVERIFY(!tool("Write",{{"path","mcp.txt"},{"content","new"}})["isError"].toBool());QCOMPARE(read(f.work+"/mcp.txt"),"new");
    const auto old=tool("iiLocalLLM.agent.session")["structuredContent"].toObject()["session_id"].toString();
    QVERIFY(!tool("Read",{{"path","file.txt"}})["isError"].toBool());
    const auto edited=tool("Write",{{"path","file.txt"},{"content","mcp edit"}});QVERIFY(!edited["isError"].toBool());
    const auto backup=edited["structuredContent"].toObject()["backup_path"].toString();QVERIFY2(backup.contains('/'+old+"/artifacts/"),qPrintable(backup));
    QVERIFY(tool("iiLocalLLM.agent.fork",{{"session_id",id}})["isError"].toBool());
    const auto forked=tool("iiLocalLLM.agent.fork");QVERIFY2(!forked["isError"].toBool(),QJsonDocument(forked).toJson().constData());
    const auto newId=forked["structuredContent"].toObject()["session_id"].toString();QVERIFY(!newId.isEmpty()&&newId!=old);
    QCOMPARE(tool("iiLocalLLM.agent.session")["structuredContent"].toObject()["session_id"].toString(),newId);
    auto copied=backup;copied.replace('/'+old+"/artifacts/",'/'+newId+"/artifacts/");QCOMPARE(read(copied),"original\r\n");
    const auto restored=tool("iiLocalLLM.agent.checkpoints.rewind",{{"message_id",checkpoint}});QVERIFY2(!restored["isError"].toBool(),QJsonDocument(restored).toJson().constData());QVERIFY(!QFileInfo::exists(f.work+"/mcp.txt"));
    QCOMPARE(read(f.path),"original\r\n");
 }
};
QTEST_GUILESS_MAIN(FileCheckpointTests)
#include "file_checkpoint_tests.moc"
