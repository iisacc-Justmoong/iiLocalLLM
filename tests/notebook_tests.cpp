#include <agent/Tools.h>
#include <agent/PermissionSettings.h>
#include <agent/Notebook.h>
#include <agent/Engine.h>
#include <agent/Api.h>
#include <agent/McpServer.h>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QProcess>
#include <QtCore/QUuid>
#include <QtTest/QTest>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void write(const QString& path,const QByteArray& bytes){QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())throw std::runtime_error("Fixture write failed");}
QByteArray read(const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))throw std::runtime_error("Fixture read failed");return f.readAll();}
QJsonObject cell(QString type,QString id,QString source){QJsonObject result{{"cell_type",type},{"id",id},{"source",source},{"metadata",QJsonObject{{"custom","preserved"}}}};
    if(type=="code"){result["outputs"]=QJsonArray{QJsonObject{{"output_type","stream"},{"name","stdout"},{"text","old output"}}};result["execution_count"]=9;}return result;}
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("notebook-test-XXXXXX")};QString path=root.filePath("book.ipynb");
    std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::RulePolicy> policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    a::ToolContext context{"owner","run",root.path(),root.filePath("artifacts")};
    Fixture(){a::registerWorkspaceTools(*registry,root.path());save({cell("code","first","print('old')"),cell("markdown","cell-0","# Existing")});}
    void save(QJsonArray cells,int minor=5){write(path,QJsonDocument(QJsonObject{{"nbformat",4},{"nbformat_minor",minor},{"metadata",QJsonObject{{"language_info",QJsonObject{{"name","python"}}},{"custom",42}}},{"cells",cells}}).toJson());}
    QJsonObject value()const{return QJsonDocument::fromJson(read(path)).object();}
    a::ToolResult call(QString name,QJsonObject args){return a::ToolRunner(registry,policy).run({"call",name,args},context);}
    a::ToolResult observe(){return call("Read",{{"path",path}});}
    a::ToolResult edit(QJsonObject args){args["notebook_path"]=path;return call("NotebookEdit",args);}
};
class QuietModel:public a::Model {public:a::ModelReply generate(const a::ModelRequest&,const CancellationToken&,const TextCallback&)override{return {"DONE",{},{1,1}};}};
a::EngineOptions engineOptions(const QString& state){a::EngineOptions result;result.sessionsDirectory=state;
    result.skills.enabled=false;result.projectContext.enabled=false;result.toolSearch.enabled=false;result.compaction.automatic=false;return result;}
}
class NotebookTests:public QObject {
 Q_OBJECT
private slots:
 void replacementRequiresReadAndPreservesUnrelatedData(){
    Fixture f;const auto original=read(f.path);const QJsonValue untouched=f.value()["cells"].toArray()[1];
    QVERIFY(f.edit({{"cell_id","first"},{"new_source","print('new')"}}).isError);QCOMPARE(read(f.path),original);
    QVERIFY(!f.observe().isError);auto result=f.edit({{"cell_id","first"},{"new_source","print('new')"}});QVERIFY2(!result.isError,qPrintable(result.text));
    const auto first=f.value()["cells"].toArray()[0].toObject();QCOMPARE(first["source"].toString(),"print('new')");
    QVERIFY(first["execution_count"].isNull());QVERIFY(first["outputs"].toArray().isEmpty());QCOMPARE(first["metadata"].toObject()["custom"].toString(),"preserved");
    QCOMPARE(f.value()["cells"].toArray()[1],untouched);QCOMPARE(f.value()["metadata"].toObject()["custom"].toInt(),42);
    QCOMPARE(read(result.data["backup_path"].toString()),original);QCOMPARE(result.data["cell_id"].toString(),"first");
    QVERIFY(!f.edit({{"cell_id","first"},{"new_source","print('again')"}}).isError);
 }
 void actualCellIdPrecedesFallbackIndexAndInsertionIsAfter(){
    Fixture f;QVERIFY(!f.observe().isError);auto result=f.edit({{"cell_id","cell-0"},{"new_source","# Target"}});QVERIFY2(!result.isError,qPrintable(result.text));
    QCOMPARE(f.value()["cells"].toArray()[0].toObject()["source"].toString(),"print('old')");
    QCOMPARE(f.value()["cells"].toArray()[1].toObject()["source"].toString(),"# Target");QCOMPARE(result.data["cell_type"].toString(),"markdown");
    result=f.edit({{"cell_id","first"},{"new_source","value = 1"},{"cell_type","code"},{"edit_mode","insert"}});QVERIFY2(!result.isError,qPrintable(result.text));
    auto inserted=f.value()["cells"].toArray()[1].toObject();QCOMPARE(inserted["source"].toString(),"value = 1");QVERIFY(!inserted["id"].toString().isEmpty());
    QVERIFY(inserted["outputs"].toArray().isEmpty());QVERIFY(inserted["execution_count"].isNull());
    result=f.edit({{"cell_id",inserted["id"]},{"new_source",""},{"edit_mode","delete"}});QVERIFY2(!result.isError,qPrintable(result.text));QCOMPARE(f.value()["cells"].toArray().size(),2);
 }
 void conversionProducesValidFieldsAndOldFormatsUseIndexSelectors(){
    Fixture f;QVERIFY(!f.observe().isError);auto result=f.edit({{"cell_id","first"},{"new_source","# Text"},{"cell_type","markdown"}});QVERIFY2(!result.isError,qPrintable(result.text));
    auto first=f.value()["cells"].toArray()[0].toObject();QVERIFY(!first.contains("outputs"));QVERIFY(!first.contains("execution_count"));
    first["attachments"]=QJsonObject{{"inline.txt",QJsonObject{{"text/plain","attachment"}}}};first.remove("id");f.save({first},4);QVERIFY(!f.observe().isError);
    result=f.edit({{"cell_id","cell-0"},{"new_source","answer = 42"},{"cell_type","code"}});QVERIFY2(!result.isError,qPrintable(result.text));
    first=f.value()["cells"].toArray()[0].toObject();QVERIFY(!first.contains("attachments"));QVERIFY(!first.contains("id"));QVERIFY(first["execution_count"].isNull());QVERIFY(first["outputs"].toArray().isEmpty());
    result=f.edit({{"new_source","# First"},{"cell_type","markdown"},{"edit_mode","insert"}});QVERIFY2(!result.isError,qPrintable(result.text));
    QCOMPARE(f.value()["cells"].toArray()[0].toObject()["source"].toString(),"# First");
 }
 void largeAttributionHasImmutableBeforeAndAfterFiles(){
    Fixture f;auto value=f.value();auto metadata=value["metadata"].toObject();metadata["large"]=QString(40000,'x');value["metadata"]=metadata;
    write(f.path,QJsonDocument(value).toJson());const auto before=read(f.path);QVERIFY(!f.observe().isError);
    auto result=f.edit({{"cell_id","first"},{"new_source","x = 1"}});QVERIFY2(!result.isError,qPrintable(result.text));
    QVERIFY(!result.data["files_inlined"].toBool());QVERIFY(!result.data.contains("original_file"));QVERIFY(!result.data.contains("updated_file"));
    const auto after=read(f.path);const auto snapshot=result.data["updated_file_path"].toString();QCOMPARE(read(snapshot),after);QCOMPARE(read(result.data["backup_path"].toString()),before);
    QVERIFY(!f.edit({{"cell_id","first"},{"new_source","x = 2"}}).isError);QCOMPARE(read(snapshot),after);
 }
 void invalidInputsAndDocumentsNeverWrite(){
    Fixture f;const auto original=read(f.path);QVERIFY(!f.observe().isError);
    for(const QJsonObject args:QList<QJsonObject>{
        {{"new_source","x"}},{{"cell_id","first"}},{{"cell_id","first"},{"new_source",42}},
        {{"cell_id","first"},{"new_source","x"},{"extra",true}},{{"new_source","x"},{"edit_mode","insert"}},
        {{"cell_id","cell-2"},{"new_source","x"}},{{"cell_id","cell-18446744073709551616"},{"new_source","x"}},
        {{"cell_id","first"},{"new_source","x"},{"cell_type","raw"}},{{"cell_id","first"},{"new_source","x"},{"edit_mode","append"}}}){
        QVERIFY(f.edit(args).isError);QCOMPARE(read(f.path),original);
    }
    QList<QByteArray> documents{"[1,2]","{broken",QByteArray("\xff",1)};
    for(int kind=0;kind<9;++kind){auto value=f.value();auto cells=value["cells"].toArray();auto first=cells[0].toObject();
        switch(kind){case 0:value["nbformat"]=5;break;case 1:value["nbformat_minor"]=-1;break;
        case 2:value["metadata"]=1;break;case 3:first.remove("id");break;case 4:first["id"]="cell-0";break;
        case 5:first["id"]="invalid id";break;case 6:first["source"]=QJsonArray{1};break;
        case 7:first["outputs"]=QJsonArray{1};break;case 8:first.remove("execution_count");break;}
        cells[0]=first;value["cells"]=cells;documents.append(QJsonDocument(value).toJson());}
    for(const auto& bytes:documents){write(f.path,bytes);f.observe();QVERIFY(f.edit({{"cell_id","first"},{"new_source","x"}}).isError);QCOMPARE(read(f.path),bytes);}
 }
 void rawCellsArraysAttachmentsAndFuturePayloadsArePreserved(){
    Fixture f;auto raw=cell("raw","raw","raw text");raw["source"]=QJsonArray{"line one\n","line two"};
    raw["attachments"]=QJsonObject{{"image",QJsonObject{{"image/png","payload"}}}};
    const QJsonObject future{{"cell_type","future-cell"},{"id","future"},{"payload",QJsonArray{42,"unknown"}}};
    f.save({cell("code","first","x = 1"),raw,future},6);QVERIFY(!f.observe().isError);
    QVERIFY(!f.edit({{"cell_id","first"},{"new_source","x = 2"}}).isError);
    QCOMPARE(f.value()["cells"].toArray()[1].toObject(),raw);QCOMPARE(f.value()["cells"].toArray()[2].toObject(),future);
    QVERIFY(f.edit({{"cell_id","future"},{"new_source",""},{"edit_mode","delete"}}).isError);
    const auto result=f.edit({{"cell_id","raw"},{"new_source","updated raw"}});QVERIFY2(!result.isError,qPrintable(result.text));
    QCOMPARE(result.data["cell_type"].toString(),"raw");QCOMPARE(f.value()["cells"].toArray()[1].toObject()["attachments"],raw["attachments"]);
 }
 void readsAreBoundToContentOwnerAndRevisions(){
    Fixture f;const QJsonObject args{{"cell_id","first"},{"new_source","x"}};
    QVERIFY(!f.call("Read",{{"path",f.path},{"limit",1}}).isError);QVERIFY(f.edit(args).isError);
    QVERIFY(!f.observe().isError);const auto original=read(f.path);write(f.path,original+" ");QVERIFY(f.edit(args).isError);
    QVERIFY(!f.observe().isError);f.context.sessionId="other";QVERIFY(f.edit(args).isError);
    f.context.sessionId="owner";++f.context.contextRevision;QVERIFY(f.edit(args).isError);
    QVERIFY(!f.observe().isError);++f.context.workspaceRevision;QVERIFY(f.edit(args).isError);
    QVERIFY(!f.observe().isError);QVERIFY(!f.edit(args).isError);
 }
 void permissionAliasesUseTheNotebookPathAndCanonicalTarget(){
    Fixture f;QVERIFY(!f.observe().isError);a::PermissionSettingsOptions settings;settings.workingDirectory=f.root.path();
    settings.fallbackMode=a::PermissionMode::Bypass;settings.inlineSettings={{"permissions",QJsonObject{{"deny",QJsonArray{"Edit(book.ipynb)"}}}}};
    const a::ToolCall call{"edit","NotebookEdit",{{"notebook_path",f.path},{"cell_id","first"},{"new_source","x"}}};
    auto policy=std::make_shared<a::SettingsPermissionPolicy>(settings);QVERIFY(a::ToolRunner(f.registry,policy).run(call,f.context).isError);
    settings.fallbackMode=a::PermissionMode::DontAsk;settings.inlineSettings={{"permissions",QJsonObject{{"allow",QJsonArray{"NotebookEdit(book.ipynb)"}}}}};
    policy=std::make_shared<a::SettingsPermissionPolicy>(settings);const auto allowed=a::ToolRunner(f.registry,policy).run(call,f.context);
    QVERIFY2(!allowed.isError,qPrintable(allowed.text+QString::fromUtf8(QJsonDocument(allowed.data).toJson())));
    auto plan=std::make_shared<a::RulePolicy>(a::PermissionMode::Plan);QVERIFY(a::ToolRunner(f.registry,plan).run(call,f.context).isError);
    auto accept=std::make_shared<a::RulePolicy>(a::PermissionMode::AcceptEdits);QVERIFY(!a::ToolRunner(f.registry,accept).run(call,f.context).isError);
    const auto second=f.root.filePath("second.ipynb"),link=f.root.filePath("alias.ipynb");write(second,read(f.path));QVERIFY(QFile::link(f.path,link));
    auto args=call.arguments;args["notebook_path"]=link;const auto prepared=f.registry->get("NotebookEdit").prepare(args,f.context);
    QVERIFY(QFile::remove(link));QVERIFY(QFile::link(second,link));QVERIFY_THROWS_EXCEPTION(Error,prepared.execute());
    f.context.protectedPaths.append(f.path);QVERIFY(f.edit({{"cell_id","first"},{"new_source","private"}}).isError);
 }
 void boundsCancellationAndFailedArtifactsLeaveOriginalIntact(){
    Fixture f;QVERIFY(!f.observe().isError);const auto original=read(f.path);
    QVERIFY(f.edit({{"cell_id","first"},{"new_source",QString(1024*1024,'x')}}).isError);QCOMPARE(read(f.path),original);
    const QJsonObject args{{"notebook_path",f.path},{"cell_id","first"},{"new_source","x"}};
    CancellationToken cancelled;cancelled.cancel();QVERIFY_THROWS_EXCEPTION(Error,a::editNotebook(original,args,cancelled));
    QVERIFY_THROWS_EXCEPTION(Error,a::editNotebook(QByteArray(1024*1024+1,' '),args));
    write(f.context.artifactsDirectory,"not a directory");QVERIFY(f.edit(args).isError);QCOMPARE(read(f.path),original);
 }
 void directControlsKeepTranscriptAndExpireObservationsAfterCompaction(){
    Fixture f;const auto options=engineOptions(f.root.filePath("state"));a::Engine engine(std::make_shared<QuietModel>(),f.registry,f.policy,options);
    QVERIFY(engine.notebookToolsEnabled());const auto id=engine.createSession("fixture",f.root.path()).id;
    const QJsonObject args{{"notebook_path","book.ipynb"},{"cell_id","first"},{"new_source","x = 1"}};
    QVERIFY(engine.runNotebookTool(id,"NotebookEdit",args).isError);
    QVERIFY(!engine.runNotebookTool(id,"Read",{{"notebook_path","book.ipynb"}}).isError);
    QVERIFY(!engine.runNotebookTool(id,"NotebookEdit",args).isError);QVERIFY(engine.session(id).messages.isEmpty());
    a::SessionStore store(options.sessionsDirectory);
    {auto lease=store.acquire(id);QVERIFY_THROWS_EXCEPTION(Error,engine.runNotebookTool(id,"Read",{{"notebook_path","book.ipynb"}}));
        lease->append({"u1",a::MessageRole::User,"old request"});lease->append({"a1",a::MessageRole::Assistant,"old answer"});lease->append({"u2",a::MessageRole::User,"new request"});
        a::Compaction checkpoint;checkpoint.id=QUuid::createUuid().toString(QUuid::WithoutBraces);checkpoint.atMessageId="u2";checkpoint.throughMessageId="a1";
        checkpoint.summary="old request answered";checkpoint.inputTokensBefore=100;checkpoint.inputTokensAfter=10;lease->compact(checkpoint);}
    QVERIFY(engine.runNotebookTool(id,"NotebookEdit",args).isError);
    QVERIFY(!engine.runNotebookTool(id,"Read",{{"notebook_path","book.ipynb"}}).isError);
    QVERIFY(!engine.runNotebookTool(id,"NotebookEdit",args).isError);QCOMPARE(engine.session(id).messages.size(),3);
    QVERIFY_THROWS_EXCEPTION(Error,engine.runNotebookTool(id,"Read",{{"notebook_path","book.ipynb"},{"extra",1}}));
    QVERIFY_THROWS_EXCEPTION(Error,engine.runNotebookTool(id,"Read",{{"notebook_path","ordinary.txt"}}));
 }
 void modelBatchReadsBeforeEditingAndNativeControlsFollowWorktrees(){
    class Model:public a::Model {public:int calls=0;a::ModelReply generate(const a::ModelRequest&,const CancellationToken&,const TextCallback&)override{
        if(calls++==0)return {{},{{"read","Read",{{"path","book.ipynb"}}},{"edit","NotebookEdit",{{"notebook_path","book.ipynb"},{"cell_id","first"},{"new_source","batch = 42"}}}},{1,1}};
        return {"DONE",{},{1,1}};}};
    Fixture f;auto git=[&](QStringList args){args.prepend(f.root.path());args.prepend("-C");return QProcess::execute("git",args);};
    QCOMPARE(git({"init","-q"}),0);QCOMPARE(git({"add","book.ipynb"}),0);
    QCOMPARE(git({"-c","user.name=Fixture","-c","user.email=fixture@example.invalid","commit","-qm","fixture"}),0);
    auto options=engineOptions(f.root.filePath("state"));options.worktrees.enabled=true;options.worktrees.fetchMissingBase=false;options.worktrees.directory=f.root.filePath("worktrees");
    a::Engine engine(std::make_shared<Model>(),f.registry,f.policy,options);const auto id=engine.createSession("fixture",f.root.path()).id;
    const auto run=engine.run({id,"Edit the notebook"}).result.get();QVERIFY2(run.status==a::RunStatus::Completed,qPrintable(run.errorMessage));
    for(const auto& message:engine.session(id).messages)if(message.role==a::MessageRole::Tool)QVERIFY2(!message.isError,qPrintable(message.text));
    QCOMPARE(f.value()["cells"].toArray()[0].toObject()["source"].toString(),"batch = 42");const auto original=read(f.path);
    const auto entered=engine.runWorktreeTool(id,"EnterWorktree",{{"name","notebook"}});QVERIFY2(!entered.isError,qPrintable(entered.text));
    const auto path=entered.data["worktreePath"].toString();
    QVERIFY(!engine.runNotebookTool(id,"Read",{{"notebook_path","book.ipynb"}}).isError);
    auto edited=engine.runNotebookTool(id,"NotebookEdit",{{"notebook_path","book.ipynb"},{"cell_id","first"},{"new_source","isolated = 1"}});QVERIFY2(!edited.isError,qPrintable(edited.text));
    QCOMPARE(edited.data["notebook_path"].toString(),path+"/book.ipynb");QCOMPARE(read(f.path),original);
    QVERIFY(engine.runNotebookTool(id,"Read",{{"notebook_path",f.path}}).isError);
    QVERIFY(!engine.runWorktreeTool(id,"ExitWorktree",{{"action","keep"}}).isError);
    QVERIFY(engine.runNotebookTool(id,"NotebookEdit",{{"notebook_path","book.ipynb"},{"cell_id","first"},{"new_source","stale"}}).isError);
 }
 void apiAndMcpExposeOwnedNativeNotebookOperations(){
    Fixture f;QTemporaryDir state(QDir::current().filePath("notebook-owners-XXXXXX"));auto model=std::make_shared<QuietModel>();a::ApiOptions options;options.engine=engineOptions({});options.workingDirectory=f.root.path();
    options.stateDirectory=state.filePath("api");options.clientTokens={{"society",QString(48,'a')},{"dreamscapes",QString(48,'b')}};
    a::Api api(model,f.registry,f.policy,options);auto call=[&](QString method,QJsonObject params={},QString token=QString(48,'a')){return api.dispatch(method,params,token).result.get().toObject();};
    QVERIFY(call("agent.info")["notebooks_enabled"].toBool());const auto id=call("agent.sessions.create",{{"model","fixture"}})["session_id"].toString();
    const QJsonObject observe{{"session_id",id},{"notebook_path","book.ipynb"}};
    QJsonObject edit=observe;edit["cell_id"]="first";edit["new_source"]="api = 1";
    QVERIFY(call("agent.notebooks.edit",edit)["is_error"].toBool());QVERIFY(!call("agent.notebooks.read",observe)["is_error"].toBool());
    QVERIFY(!call("agent.notebooks.edit",edit)["is_error"].toBool());QVERIFY_THROWS_EXCEPTION(Error,call("agent.notebooks.edit",edit,QString(48,'b')));
    QCOMPARE(call("agent.sessions.get",{{"session_id",id}})["message_count"].toInt(),0);
    a::McpServerOptions standalone;standalone.workingDirectory=f.root.path();QVERIFY(a::mcpServerOptions(f.registry,f.policy,standalone).experimentalCapabilities.contains("iisacc/notebooks"));
    auto host=std::make_shared<a::Engine>(model,f.registry,f.policy,engineOptions(state.filePath("mcp")));
    a::McpServerOptions mo;mo.engine=host;mo.model="fixture";mo.workingDirectory=f.root.path();
    mcp::ServerSession server(a::mcpServerOptions(f.registry,f.policy,mo));
    auto rpc=[&](int id,QString method,QJsonObject params){server.receive(QJsonObject{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}});
        for(int i=0;i<400;++i)for(const auto& value:server.takeMessages(10))if(value.toObject()["id"]==id)return value.toObject();throw std::runtime_error("MCP timeout");};
    const auto init=rpc(1,"initialize",{{"protocolVersion","2025-11-25"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","test"},{"version","1"}}}});
    QVERIFY(QJsonDocument(init).toJson().contains("iisacc/notebooks"));server.receive(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/initialized"}});
    edit.remove("session_id");QVERIFY(rpc(2,"tools/call",{{"name","NotebookEdit"},{"arguments",edit}})["result"].toObject()["isError"].toBool());
    QVERIFY(!rpc(3,"tools/call",{{"name","Read"},{"arguments",QJsonObject{{"path","book.ipynb"}}}})["result"].toObject()["isError"].toBool());
    const auto result=rpc(4,"tools/call",{{"name","NotebookEdit"},{"arguments",edit}})["result"].toObject();
    QVERIFY2(!result["isError"].toBool(),QJsonDocument(result).toJson().constData());QCOMPARE(result["structuredContent"].toObject()["cell_id"].toString(),"first");
 }
};
QTEST_GUILESS_MAIN(NotebookTests)
#include "notebook_tests.moc"
