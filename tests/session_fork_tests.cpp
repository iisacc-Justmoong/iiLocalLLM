#include <agent/Engine.h>
#include <agent/SessionStore.h>
#include <agent/McpServer.h>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <QtCore/QCryptographicHash>
#include <QtCore/QLockFile>
#include <QtTest/QTest>
#include <thread>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void write(const QString& path,const QByteArray& bytes){if(!QDir().mkpath(QFileInfo(path).absolutePath()))throw std::runtime_error("mkdir");QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())throw std::runtime_error("write");}
QByteArray read(const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))throw std::runtime_error("read");return f.readAll();}
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("fork-test-XXXXXX")};
    QString work=root.filePath("work"),state=root.filePath("sessions");
    a::SessionStore store{state};
    Fixture(){if(!QDir().mkpath(work))throw std::runtime_error("work");}
    QString artifacts(const QString& id)const{return state+'/'+id+"/artifacts";}
};
class EditModel:public a::Model {public:
    a::ModelReply generate(const a::ModelRequest& r,const CancellationToken&,const TextCallback&)override{
        if(r.messages.last().role==a::MessageRole::User)return {{},{{"read-"+QString::number(r.messages.size()),"Read",{{"path","file"}}}}};
        for(auto it=r.messages.crbegin();it!=r.messages.crend();++it)if(!it->toolCalls.isEmpty()){
            if(it->toolCalls.last().name=="Read")return {{},{{"write-"+QString::number(r.messages.size()),"Write",{{"path","file"},{"content","edited"}}}}};break;}
        return {"done",{}};
    }
};
a::EngineOptions options(const Fixture& f){a::EngineOptions o;o.sessionsDirectory=f.state;o.fileCheckpointsEnabled=true;o.skills.enabled=false;o.toolSearch.enabled=false;o.projectContext.enabled=false;o.compaction.automatic=false;return o;}
std::shared_ptr<a::ToolRegistry> registry(const Fixture& f){auto r=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*r,f.work);return r;}
}
class SessionForkTests:public QObject {
 Q_OBJECT
private slots:
 void fullForkClonesBytesPermissionsAndNestedReferences(){
    Fixture f;const auto parent=f.store.create("fixture",{},f.work);const auto from=f.artifacts(parent.id);
    const auto bytes=QByteArray("raw\0bytes\r\n",11);write(from+"/nested/result",bytes);write(from+"/.unreferenced","direct control output");
    QVERIFY(QFile::setPermissions(from+"/nested/result",QFileDevice::ReadOwner|QFileDevice::WriteOwner));
    {auto lease=f.store.acquire(parent.id);a::Message user{"u",a::MessageRole::User,"Inspect "+from+"/nested/result"};
        user.data={{"path",from+"/nested/result"}};user.content={QJsonObject{{"type","text"},{"text",from+"/nested/result"}}};
        user.metadata={{"nested",QJsonArray{from+"/nested/result"}}};lease->append(user);}
    const auto fork=f.store.fork(parent.id);const auto to=f.artifacts(fork.id);QCOMPARE(fork.parentSessionId,parent.id);
    QCOMPARE(read(to+"/nested/result"),bytes);QCOMPARE(read(to+"/.unreferenced"),"direct control output");
    QCOMPARE(QFileInfo(to+"/nested/result").permissions(),QFileInfo(from+"/nested/result").permissions());
    const auto message=a::toJson(fork.messages.first());const auto json=QJsonDocument(message).toJson();QVERIFY(!json.contains(from.toUtf8()));QVERIFY(json.contains(to.toUtf8()));
    write(to+"/nested/result","child");QCOMPARE(read(from+"/nested/result"),bytes);
    write(from+"/nested/result","parent");QCOMPARE(read(to+"/nested/result"),"child");
    QCOMPARE(f.store.load(fork.id).messages,fork.messages);QVERIFY(f.store.load(parent.id).messages.first().text.contains(from));
 }
 void boundedForkDoesNotCopyFutureOrUnreferencedArtifacts(){
    Fixture f;const auto parent=f.store.create("fixture",{},f.work);const auto from=f.artifacts(parent.id);
    write(from+"/early","early");write(from+"/ear","short prefix");write(from+"/later","later");write(from+"/orphan","orphan");
    {auto lease=f.store.acquire(parent.id);lease->append({"early",a::MessageRole::User,from+"/early"});lease->append({"later",a::MessageRole::User,from+"/later"});}
    const auto fork=f.store.fork(parent.id,"early");QCOMPARE(fork.messages.size(),1);QCOMPARE(read(f.artifacts(fork.id)+"/early"),"early");
    QVERIFY(!QFileInfo::exists(f.artifacts(fork.id)+"/later"));QVERIFY(!QFileInfo::exists(f.artifacts(fork.id)+"/orphan"));
    QVERIFY(!QFileInfo::exists(f.artifacts(fork.id)+"/ear"));
 }
 void unsafeArtifactsLeaveNoPublishedOrPartialFork(){
    Fixture f;const auto parent=f.store.create("fixture",{},f.work);const auto from=f.artifacts(parent.id);
    write(from+"/a","safe");write(f.root.filePath("outside"),"private");QVERIFY(QFile::link(f.root.filePath("outside"),from+"/z"));
    QVERIFY_THROWS_EXCEPTION(Error,f.store.fork(parent.id));QCOMPARE(f.store.list(),QStringList{parent.id});
    QCOMPARE(QDir(f.state).entryList(QDir::Dirs|QDir::NoDotAndDotDot),QStringList{parent.id});QCOMPARE(read(f.root.filePath("outside")),"private");
 }
 void unreadableArtifactDirectoryCannotSilentlyLoseFiles(){
    Fixture f;const auto parent=f.store.create("fixture",{},f.work);const auto from=f.artifacts(parent.id);write(from+"/nested/result","evidence");
    for(const auto& directory:QStringList{from,from+"/nested"}){
        const auto permissions=QFileInfo(directory).permissions();QVERIFY(QFile::setPermissions(directory,{}));bool failed=false;
        try{(void)f.store.fork(parent.id);}catch(const Error&){failed=true;}
        const bool restored=QFile::setPermissions(directory,permissions);QVERIFY(restored);QVERIFY(failed);QCOMPARE(f.store.list(),QStringList{parent.id});
    }
 }
 void compactionAndToolReferencesUseCopiedArtifacts(){
    Fixture f;const auto parent=f.store.create("fixture",{},f.work);const auto from=f.artifacts(parent.id);write(from+"/result","evidence");
    {auto lease=f.store.acquire(parent.id);lease->append({"u",a::MessageRole::User,"inspect"});
        lease->append({"a",a::MessageRole::Assistant,{},{{"call","Read",{{"path",from+"/result"}}}}});
        lease->append({"t",a::MessageRole::Tool,"Full output: "+from+"/result",{},"call"});lease->append({"next",a::MessageRole::User,"continue"});
        a::Compaction c;c.id=QUuid::createUuid().toString(QUuid::WithoutBraces);c.atMessageId="next";c.throughMessageId="t";
        c.summary="Evidence in "+from+"/result";c.inputTokensBefore=200;c.inputTokensAfter=100;lease->compact(c);}
    const auto child=f.store.fork(parent.id);const auto to=f.artifacts(child.id);QCOMPARE(child.compactions.size(),1);
    QCOMPARE(child.messages[1].toolCalls.first().arguments["path"].toString(),to+"/result");QCOMPARE(child.compactions.first().summary,"Evidence in "+to+"/result");
    QVERIFY(a::pendingToolCalls(child.messages).isEmpty());QCOMPARE(f.store.load(child.id).compactions.first().summary,child.compactions.first().summary);
    const auto early=f.store.fork(parent.id,"t");QVERIFY(early.compactions.isEmpty());QCOMPARE(read(f.artifacts(early.id)+"/result"),"evidence");
 }
 void engineForkRetainsCheckpointAndParentRemovalDoesNotBreakChild(){
    Fixture f;write(f.work+"/file","original\r\n");auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    a::Engine engine(std::make_shared<EditModel>(),registry(f),policy,options(f));const auto parent=engine.createSession("fixture",f.work).id;
    const auto run=engine.run({parent,"edit"}).result.get();QVERIFY2(run.status==a::RunStatus::Completed,qPrintable(run.errorMessage));
    const auto message=engine.session(parent).messages.first().id;const auto manual=engine.checkpointFiles(parent)["message_id"].toString();
    const auto child=engine.forkSession(parent);QCOMPARE(engine.fileCheckpoints(child.id)["snapshots"].toArray().size(),2);
    auto disabled=options(f);disabled.fileCheckpointsEnabled=false;a::Engine paused(std::make_shared<EditModel>(),registry(f),policy,disabled);
    const auto preserved=paused.forkSession(parent);QCOMPARE(engine.fileCheckpoints(preserved.id)["snapshots"].toArray().size(),2);
    const auto prefix=engine.forkSession(parent,message);QCOMPARE(prefix.messages.size(),1);QCOMPARE(engine.fileCheckpoints(prefix.id)["snapshots"].toArray().size(),1);
    QVERIFY(QDir(f.state+"/file-checkpoints/"+parent).removeRecursively());QVERIFY(QDir(f.artifacts(parent)).removeRecursively());
    QVERIFY(!engine.rewindFiles(child.id,message).isError);QCOMPARE(read(f.work+"/file"),"original\r\n");
    QVERIFY(!engine.rewindFiles(child.id,manual).isError);QCOMPARE(read(f.work+"/file"),"edited");
    a::Engine reopened(std::make_shared<EditModel>(),registry(f),policy,options(f));QVERIFY(!reopened.rewindFiles(child.id,message).isError);QCOMPARE(read(f.work+"/file"),"original\r\n");
 }
 void oversizedArtifactsAndPrepublicationFailureAreAtomic(){
    Fixture f;const auto parent=f.store.create("fixture",{},f.work);const auto from=f.artifacts(parent.id);write(from+"/large",{});
    {QFile file(from+"/large");QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.resize(64*1024*1024+1));}
    QVERIFY_THROWS_EXCEPTION(Error,f.store.fork(parent.id));QCOMPARE(f.store.list(),QStringList{parent.id});
    write(from+"/large","small");QString proposed;
    QVERIFY_THROWS_EXCEPTION(Error,f.store.fork(parent.id,{},[&](const a::Session& child){proposed=child.id;
        if(f.store.list()!=QStringList{parent.id})throw std::runtime_error("published too early");
        if(read(f.artifacts(child.id)+"/large")!="small")throw std::runtime_error("copy not prepared");
        throw Error(ErrorCode::StorageFailure,"injected external-state failure");}));
    QVERIFY(!proposed.isEmpty());QVERIFY(!QFileInfo::exists(f.state+'/'+proposed));QCOMPARE(f.store.list(),QStringList{parent.id});
 }
 void boundedCheckpointsExcludeFutureOnlyFilesAndCorruptionRollsBack(){
    Fixture f;const auto parent=f.store.create("fixture",{},f.work);const auto first=QUuid::createUuid().toString(QUuid::WithoutBraces),later=QUuid::createUuid().toString(QUuid::WithoutBraces);
    {auto lease=f.store.acquire(parent.id);lease->append({first,a::MessageRole::User,"first"});lease->append({later,a::MessageRole::User,"later"});}
    a::FileCheckpoints history(f.state+"/file-checkpoints");a::ToolContext c{parent.id,{},f.work};c.fileCheckpointId=first;
    write(f.work+"/early","before");history.checkpoint(first,c);history.track(f.work+"/early",QByteArray("before"),c);write(f.work+"/early","after");
    c.fileCheckpointId=later;history.checkpoint(later,c);history.track(f.work+"/late",{},c);write(f.work+"/late","future");
    auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);a::Engine engine(std::make_shared<EditModel>(),registry(f),policy,options(f));
    const auto child=engine.forkSession(parent.id,first);QVERIFY(!engine.rewindFiles(child.id,first).isError);QCOMPARE(read(f.work+"/early"),"before");QCOMPARE(read(f.work+"/late"),"future");
    const auto digest=QString::fromLatin1(QCryptographicHash::hash("before",QCryptographicHash::Sha256).toHex());
    write(f.state+"/file-checkpoints/"+parent.id+'/'+digest+".blob","corrupt");const auto before=f.store.list();
    QVERIFY_THROWS_EXCEPTION(Error,engine.forkSession(parent.id));QCOMPARE(f.store.list(),before);
    QCOMPARE(QDir(f.state+"/file-checkpoints").entryList(QDir::Dirs|QDir::NoDotAndDotDot).size(),2);
    QVERIFY(!engine.rewindFiles(child.id,first).isError);
 }
 void failedPermissionInheritanceDoesNotPublishOrLeakBackups(){
    struct Policy:public a::PermissionPolicy{
        mutable QString child,forgotten;
        a::PermissionDecision decide(const a::ToolDefinition&,const QJsonObject&,const a::ToolContext&)const override{return {a::PermissionBehavior::Allow,{}};}
        void inheritSession(const a::ToolContext&,const a::ToolContext& c)const override{child=c.sessionId;throw Error(ErrorCode::ResourceLimit,"injected permission limit");}
        void forgetSession(const a::ToolContext& c)const override{forgotten=c.sessionId;}
    };
    Fixture f;auto policy=std::make_shared<Policy>();a::Engine engine(std::make_shared<EditModel>(),registry(f),policy,options(f));const auto parent=engine.createSession("fixture",f.work).id;
    engine.checkpointFiles(parent);write(f.artifacts(parent)+"/file","backup");QVERIFY_THROWS_EXCEPTION(Error,engine.forkSession(parent));
    QVERIFY(!policy->child.isEmpty());QCOMPARE(policy->forgotten,policy->child);QCOMPARE(engine.sessions(),QStringList{parent});
    QVERIFY(!QFileInfo::exists(f.state+'/'+policy->child));QVERIFY(!QFileInfo::exists(f.state+"/file-checkpoints/"+policy->child));QCOMPARE(read(f.artifacts(parent)+"/file"),"backup");
 }
 void checkpointLockAndCancellationLeaveDestinationAbsent(){
    Fixture f;auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);a::Engine engine(std::make_shared<EditModel>(),registry(f),policy,options(f));
    const auto parent=engine.createSession("fixture",f.work).id;engine.checkpointFiles(parent);write(f.artifacts(parent)+"/data","owned");
    QLockFile lock(f.state+"/file-checkpoints/"+parent+"/history.lock");lock.setStaleLockTime(0);QVERIFY(lock.tryLock(0));
    QVERIFY_THROWS_EXCEPTION(Error,engine.forkSession(parent));QCOMPARE(engine.sessions(),QStringList{parent});lock.unlock();
    const auto child=QUuid::createUuid().toString(QUuid::WithoutBraces);a::ToolContext c{parent,{},f.work};c.cancellation.cancel();
    a::FileCheckpoints history(f.state+"/file-checkpoints");QVERIFY_THROWS_EXCEPTION(Error,history.fork(c,child));QVERIFY(!QFileInfo::exists(f.state+"/file-checkpoints/"+child));
 }
 void mcpForkRejectsBusyOwnerAndHonorsHostDenial(){
    struct Waiting:public a::Model {std::atomic<bool> waiting=false,release=false;
        a::ModelReply generate(const a::ModelRequest&,const CancellationToken& token,const TextCallback&)override{
            waiting=true;while(!release){token.throwIfCancelled();std::this_thread::sleep_for(std::chrono::milliseconds(5));}return {"done",{}};
        }};
    Fixture f;auto model=std::make_shared<Waiting>();auto tools=registry(f);auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    auto engine=std::make_shared<a::Engine>(model,tools,policy,options(f));a::McpServerOptions config;config.engine=engine;config.model="fixture";config.workingDirectory=f.work;
    const auto server=a::mcpServerOptions(tools,policy,config);mcp::ServerRequestContext c;c.sessionId="connection";
    const auto call=[&](QString name,QJsonObject args=QJsonObject{}){return server.handlers.at("tools/call")({{"name",name},{"arguments",args}},c);};
    auto running=std::async(std::launch::async,[&]{return call("iiLocalLLM.agent.run",{{"prompt","wait"}});});
    struct Release{std::shared_ptr<Waiting> model;~Release(){model->release=true;}} release{model};
    QTRY_VERIFY_WITH_TIMEOUT(model->waiting.load(),5000);
    auto attempted=std::async(std::launch::async,[&]{return call("iiLocalLLM.agent.fork");});
    const bool ready=attempted.wait_for(std::chrono::seconds(5))==std::future_status::ready;model->release=true;
    const auto rejected=attempted.get();QVERIFY(!running.get()["isError"].toBool());QVERIFY(ready);QVERIFY(rejected["isError"].toBool());QCOMPARE(engine->sessions().size(),1);
    const auto child=call("iiLocalLLM.agent.fork");QVERIFY2(!child["isError"].toBool(),QJsonDocument(child).toJson().constData());QCOMPARE(engine->sessions().size(),2);
    auto denied=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"iiLocalLLM.agent.fork",a::PermissionBehavior::Deny}});
    const auto locked=a::mcpServerOptions(tools,denied,config);const auto deniedResult=locked.handlers.at("tools/call")({{"name","iiLocalLLM.agent.fork"},{"arguments",QJsonObject{}}},c);
    QVERIFY(deniedResult["isError"].toBool());QCOMPARE(engine->sessions().size(),2);
    server.onClosed(c.sessionId);
 }
};
QTEST_GUILESS_MAIN(SessionForkTests)
#include "session_fork_tests.moc"
