#include "agent/PermissionSettings.h"
#include "agent/Engine.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <future>
#include <barrier>
#ifdef Q_OS_UNIX
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void save(const QString& path,const QJsonObject& value) {
    QDir().mkpath(QFileInfo(path).absolutePath());QFile file(path);
    if(!file.open(QIODevice::WriteOnly)||file.write(QJsonDocument(value).toJson())<0)throw std::runtime_error("Fixture write failed");
}
QJsonObject read(const QString& path) {QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Fixture read failed");return QJsonDocument::fromJson(file.readAll()).object();}
QJsonObject rules(const QString& type,const QString& destination,const QString& behavior,const QString& tool,const QString& content={}) {
    QJsonObject rule{{"toolName",tool}};if(!content.isEmpty())rule["ruleContent"]=content;
    return {{"type",type},{"destination",destination},{"behavior",behavior},{"rules",QJsonArray{rule}}};
}
QJsonObject dirs(const QString& type,const QString& destination,const QString& directory) {return {{"type",type},{"destination",destination},{"directories",QJsonArray{directory}}};}
QJsonObject mode(const QString& destination,const QString& value){return {{"type","setMode"},{"destination",destination},{"mode",value}};}
struct Fixture {
    QTemporaryDir root;QString work=root.filePath("work");a::PermissionSettingsOptions options;
    Fixture(){QDir().mkpath(work);options.workingDirectory=work;options.userDirectory=root.filePath("user");options.managedDirectory=root.filePath("managed");}
    a::ToolContext context(const QString& id="one") const{return {id,{},work};}
};
a::PermissionBehavior decide(const a::SettingsPermissionPolicy& policy,const a::ToolContext& context,const QString& path) {
    return policy.decide({"Write","write",{{"type","object"}}},{{"path",path}},context).behavior;
}
class Model final:public a::Model {public:a::ModelReply generate(const a::ModelRequest&,const CancellationToken&,const TextCallback&)override{return {"DONE"};}};
}
class PermissionUpdatesTests final:public QObject {
    Q_OBJECT
private slots:
    void approvalAppliesDirectoryGrantBeforePreparingChangedInput() {
        for(bool valid:{true,false}) {
            Fixture f;const auto outside=f.root.filePath("outside");QDir().mkpath(outside);
            auto policy=std::make_shared<a::SettingsPermissionPolicy>(f.options);auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,f.work);
            a::ToolRunnerOptions options;options.permissionResponse=[&](const auto&,const auto&,const auto&){a::PermissionResponse result;result.behavior=a::PermissionBehavior::Allow;
                result.updatedArguments=QJsonObject{{"path",outside+"/created.txt"},{"content",valid?QJsonValue("APPROVED"):QJsonValue(7)}};
                result.updatedPermissions={dirs("addDirectories","session",outside)};return result;};
            const auto result=a::ToolRunner(registry,policy,options).run({"call","Write",{{"path","original.txt"},{"content","ORIGINAL"}}},f.context());
            QCOMPARE(result.isError,!valid);QCOMPARE(QFileInfo::exists(outside+"/created.txt"),valid);
            QCOMPARE(policy->workingDirectories(f.context()).contains(outside),valid);QVERIFY(!QFileInfo::exists(f.work+"/original.txt"));
        }
    }
    void approvalRemovalDropsCachedRootsAndRejectsSuppliedSnapshots() {
        for(bool suppliedSnapshot:{false,true}) {
            Fixture f;const auto outside=f.root.filePath("outside");QDir().mkpath(outside);f.options.additionalDirectories={outside};
            auto policy=std::make_shared<a::SettingsPermissionPolicy>(f.options);auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,f.work);
            a::ToolRunnerOptions options;options.permissionResponse=[&](const auto&,const auto&,const auto&){a::PermissionResponse response;response.behavior=a::PermissionBehavior::Allow;
                response.updatedPermissions={dirs("removeDirectories","cliArg",outside)};return response;};
            auto context=f.context();if(suppliedSnapshot)context.workingDirectories={outside};
            const auto result=a::ToolRunner(registry,policy,options).run({"write","Write",{{"path",outside+"/out.txt"},{"content","REMOVED"}}},context);
            QVERIFY(result.isError);QVERIFY(!QFileInfo::exists(outside+"/out.txt"));
            QVERIFY(!policy->workingDirectories(f.context()).contains(outside));
        }
    }
    void persistentUpdatesPreserveUnrelatedSettingsAndSurviveRestart() {
        Fixture f;const auto path=f.work+"/.claude/settings.json";
        save(path,{{"theme","USER_THEME"},{"permissions",QJsonObject{{"allow",QJsonArray{"Write(/old/**)"}},{"deny",QJsonArray{"Write(/guard/**)"}}}}});
        a::SettingsPermissionPolicy policy(f.options);const auto c=f.context();
        policy.applyUpdates({rules("addRules","projectSettings","allow","Write","/new/**"),rules("removeRules","projectSettings","allow","Write","/old/**"),
            rules("replaceRules","localSettings","ask","Write","/review/**"),rules("addRules","userSettings","allow","Bash","git status")},c);
        QCOMPARE(read(path)["theme"],"USER_THEME");QCOMPARE(read(path)["permissions"].toObject()["allow"].toArray(),QJsonArray{"Write(/new/**)"});
        QCOMPARE(decide(policy,c,"new/out"),a::PermissionBehavior::Allow);QCOMPARE(decide(policy,c,"old/out"),a::PermissionBehavior::Ask);
        QCOMPARE(decide(policy,c,"guard/out"),a::PermissionBehavior::Deny);QCOMPARE(decide(policy,c,"review/out"),a::PermissionBehavior::Ask);
        a::SettingsPermissionPolicy reopened(f.options);QCOMPARE(decide(reopened,c,"new/out"),a::PermissionBehavior::Allow);
        policy.applyUpdates({mode("localSettings","plan")},c);QCOMPARE(policy.describe(c)["mode"],"plan");
        QCOMPARE(a::SettingsPermissionPolicy(f.options).snapshot().mode,a::PermissionMode::Plan);
        policy.applyUpdates({rules("replaceRules","projectSettings","allow","Write","/replacement/**")},c);
        QCOMPARE(read(path)["permissions"].toObject()["deny"].toArray(),QJsonArray{"Write(/guard/**)"});
        QVERIFY(QFileInfo(path).permissions().testFlag(QFileDevice::ReadOwner));
    }
    void runtimeRulesCliOverridesAndModesAreIsolated() {
        Fixture f;a::SettingsPermissionPolicy policy(f.options,{{"Write(native/**)",a::PermissionBehavior::Allow}});const auto one=f.context(),two=f.context("two");
        QCOMPARE(decide(policy,one,"native/out"),a::PermissionBehavior::Allow);
        policy.applyUpdates({rules("addRules","session","allow","Write","/one/**"),rules("removeRules","cliArg","allow","Write","native/**")},one);
        QCOMPARE(decide(policy,one,"one/out"),a::PermissionBehavior::Allow);QCOMPARE(decide(policy,two,"one/out"),a::PermissionBehavior::Ask);
        QCOMPARE(decide(policy,one,"native/out"),a::PermissionBehavior::Ask);QCOMPARE(decide(policy,two,"native/out"),a::PermissionBehavior::Allow);
        policy.applyUpdates({rules("replaceRules","cliArg","deny","Write","/one/**")},one);
        QCOMPARE(decide(policy,one,"one/out"),a::PermissionBehavior::Deny);
        policy.applyUpdates({mode("session","plan")},one);QCOMPARE(policy.describe(one)["mode"],"plan");QCOMPARE(policy.describe(two)["mode"],"default");
        QVERIFY(!QFileInfo::exists(f.work+"/.claude/settings.json"));
        QCOMPARE(decide(a::SettingsPermissionPolicy(f.options),one,"one/out"),a::PermissionBehavior::Ask);
    }
    void directoriesRetainBindingsAcrossSessionsAndCanBeRemovedPerSession() {
        Fixture f;const auto outside=f.root.filePath("outside"),other=f.root.filePath("other"),link=f.root.filePath("link");QDir().mkpath(outside);QDir().mkpath(other);
        QVERIFY(QFile::link(outside,link));f.options.additionalDirectories={link};
        a::SettingsPermissionPolicy policy(f.options);auto one=f.context(),two=f.context("two");
        QVERIFY(policy.workingDirectories(one).contains(outside));
        policy.applyUpdates({dirs("removeDirectories","session",link)},one);
        QVERIFY(!policy.workingDirectories(one).contains(outside));QVERIFY(policy.workingDirectories(two).contains(outside));
        policy.applyUpdates({dirs("addDirectories","session",link)},one);QVERIFY(policy.workingDirectories(one).contains(outside));
        policy.inheritSession(one,f.context("child"));
        QVERIFY(QFile::remove(link));QVERIFY(QFile::link(other,link));
        QVERIFY(!policy.workingDirectories(f.context("child")).contains(other));QVERIFY(!policy.workingDirectories(two).contains(other));
        policy.applyUpdates({dirs("addDirectories","session",link)},one);QVERIFY(policy.workingDirectories(one).contains(other));
        QVERIFY(!policy.workingDirectories(f.context("child")).contains(other));
        policy.applyUpdates({dirs("addDirectories","localSettings",other)},one);
        QVERIFY(a::SettingsPermissionPolicy(f.options).workingDirectories(two).contains(other));
        policy.applyUpdates({dirs("removeDirectories","localSettings",other)},one);
        QVERIFY(!read(f.work+"/.claude/settings.local.json")["permissions"].toObject()["additionalDirectories"].toArray().contains(other));
    }
    void invalidBatchAndManagedPolicyCannotSilentlyGrant() {
        Fixture f;f.options.enabledSources={"local"};a::SettingsPermissionPolicy policy(f.options);const auto c=f.context();
        QVERIFY_THROWS_EXCEPTION(Error,policy.applyUpdates({rules("addRules","localSettings","allow","Write"),rules("addRules","userSettings","allow","Write")},c));
        QVERIFY(!QFileInfo::exists(f.work+"/.claude/settings.local.json"));
        QVERIFY_THROWS_EXCEPTION(Error,policy.applyUpdates({rules("addRules","session","allow","Bad(Name")},c));
        QCOMPARE(decide(policy,c,"out"),a::PermissionBehavior::Ask);
        save(f.options.managedDirectory+"/managed-settings.json",{{"allowManagedPermissionRulesOnly",true},{"permissions",QJsonObject{
            {"deny",QJsonArray{"Write(/guard/**)"}},{"disableBypassPermissionsMode","disable"}}}});
        QVERIFY_THROWS_EXCEPTION(Error,policy.applyUpdates({mode("session","bypassPermissions")},c));
        policy.applyUpdates({rules("addRules","session","allow","Write")},c);
        QCOMPARE(decide(policy,c,"out"),a::PermissionBehavior::Ask);QCOMPARE(decide(policy,c,"guard/out"),a::PermissionBehavior::Deny);
        CancellationToken token;token.cancel();auto cancelled=c;cancelled.cancellation=token;
        QVERIFY_THROWS_EXCEPTION(Error,policy.applyUpdates({mode("localSettings","plan")},cancelled));
        QVERIFY(!QFileInfo::exists(f.work+"/.claude/settings.local.json"));
    }
    void ruleRemovalNormalizesWholeToolAliasesAndEscapesContent() {
        Fixture f;const auto path=f.work+"/.claude/settings.local.json";
        save(path,{{"permissions",QJsonObject{{"allow",QJsonArray{"Bash(*)","Write()"}}}}});a::SettingsPermissionPolicy policy(f.options);
        policy.applyUpdates({rules("removeRules","localSettings","allow","Bash"),rules("removeRules","localSettings","allow","Write"),
            rules("addRules","localSettings","ask","Bash","python -c 'print(1)'" )},f.context());
        const auto p=read(path)["permissions"].toObject();QVERIFY(p["allow"].toArray().isEmpty());
        QCOMPARE(p["ask"].toArray().first(),"Bash(python -c 'print\\(1\\)')");
    }
    void concurrentWritersDoNotLoseRulesAndLinksAreRejected() {
        Fixture f;std::vector<std::future<void>> jobs;std::barrier start(8);
        for(int i=0;i<8;++i)jobs.push_back(std::async(std::launch::async,[&,i]{a::SettingsPermissionPolicy p(f.options);
            start.arrive_and_wait();
            p.applyUpdates({rules("addRules","localSettings","allow","Write","/path"+QString::number(i)+"/**")},f.context());}));
        for(auto& job:jobs)job.get();const auto path=f.work+"/.claude/settings.local.json";
        QCOMPARE(read(path)["permissions"].toObject()["allow"].toArray().size(),8);
        const auto external=f.root.filePath("protected.json");save(external,{{"untouched",true}});QVERIFY(QFile::remove(path));QVERIFY(QFile::link(external,path));
        a::SettingsPermissionPolicy p(f.options);QVERIFY_THROWS_EXCEPTION(Error,p.applyUpdates({mode("localSettings","plan")},f.context()));
        QCOMPARE(read(external),(QJsonObject{{"untouched",true}}));
    }
    void clearAndForkInheritTrustedRuntimeStateAndCapacityIsBounded() {
        Fixture f;auto policy=std::make_shared<a::SettingsPermissionPolicy>(f.options);
        a::EngineOptions options;options.sessionsDirectory=f.root.filePath("sessions");options.compaction.automatic=false;
        a::Engine engine(std::make_shared<Model>(),std::make_shared<a::ToolRegistry>(),policy,options);
        const auto old=engine.createSession("fixture",f.work).id;
        policy->applyUpdates({rules("addRules","session","allow","Write","/approved/**")},f.context(old));
        const auto fork=engine.forkSession(old).id;QCOMPARE(decide(*policy,f.context(fork),"approved/out"),a::PermissionBehavior::Allow);
        const auto cleared=engine.clearSession(old);QVERIFY(cleared["complete"].toBool());const auto next=cleared["session_id"].toString();
        QCOMPARE(decide(*policy,f.context(next),"approved/out"),a::PermissionBehavior::Allow);
        policy->applyUpdates({rules("removeRules","session","allow","Write","/approved/**")},f.context(next));
        QCOMPARE(decide(*policy,f.context(old),"approved/out"),a::PermissionBehavior::Allow);QCOMPARE(decide(*policy,f.context(next),"approved/out"),a::PermissionBehavior::Ask);
        f.options.maxRuntimeSessions=1;a::SettingsPermissionPolicy tiny(f.options);tiny.applyUpdates({mode("session","plan")},f.context());
        QVERIFY_THROWS_EXCEPTION(Error,tiny.applyUpdates({mode("session","plan")},f.context("two")));
        tiny.forgetSession(f.context());tiny.applyUpdates({mode("session","plan")},f.context("two"));
        QCOMPARE(tiny.describe(f.context("two"))["mode"],"plan");
    }
    void batchValidationKeepsFilesAndRuntimeUnchanged() {
        Fixture f;const auto path=f.work+"/.claude/settings.local.json";
        save(path,{{"theme","KEEP"}});f.options.maxFileBytes=1024;a::SettingsPermissionPolicy policy(f.options);
        QVERIFY_THROWS_EXCEPTION(Error,policy.applyUpdates({mode("session","plan"),
            rules("addRules","localSettings","allow","Write",QString(1100,'x'))},f.context()));
        QCOMPARE(read(path),(QJsonObject{{"theme","KEEP"}}));QCOMPARE(policy.describe(f.context())["mode"],"default");
    }
    void changedInputIsDeniedAfterTheApprovedSettingsArePublished() {
        Fixture f;auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,f.work);
        auto policy=std::make_shared<a::SettingsPermissionPolicy>(f.options);a::ToolRunnerOptions options;
        options.permissionResponse=[](const auto&,const auto&,const auto&){a::PermissionResponse response;response.behavior=a::PermissionBehavior::Allow;
            response.updatedArguments=QJsonObject{{"path","blocked.txt"},{"content","NO"}};
            response.updatedPermissions={rules("addRules","localSettings","deny","Write","/blocked.txt")};return response;};
        const auto result=a::ToolRunner(registry,policy,options).run({"write","Write",{{"path","original.txt"},{"content","ORIGINAL"}}},f.context());
        QVERIFY(result.isError);QVERIFY(!QFileInfo::exists(f.work+"/blocked.txt"));QVERIFY(!QFileInfo::exists(f.work+"/original.txt"));
        QCOMPARE(decide(a::SettingsPermissionPolicy(f.options),f.context(),"blocked.txt"),a::PermissionBehavior::Deny);
    }
    void lockedFileTimesOutOrCancelsWithoutPublishing() {
#ifdef Q_OS_UNIX
        Fixture f;const auto path=f.work+"/.claude/settings.local.json";save(path,{{"theme","KEEP"}});
        const auto lockPath=f.work+"/.claude/.settings.local.json.iillm-permissions.lock";
        struct Lock {int fd=-1;~Lock(){if(fd>=0)::close(fd);}} lock;
        lock.fd=::open(QFile::encodeName(lockPath).constData(),O_RDWR|O_CREAT|O_CLOEXEC,0600);QVERIFY(lock.fd>=0);QVERIFY(::flock(lock.fd,LOCK_EX|LOCK_NB)==0);
        f.options.updateLockTimeoutMs=30;a::SettingsPermissionPolicy timeout(f.options);
        try {timeout.applyUpdates({mode("localSettings","plan")},f.context());QFAIL("Expected lock timeout");}
        catch(const Error& error){QCOMPARE(error.code(),ErrorCode::Timeout);}
        f.options.updateLockTimeoutMs=5000;a::SettingsPermissionPolicy cancelled(f.options);auto context=f.context();
        auto job=std::async(std::launch::async,[&]{try{cancelled.applyUpdates({mode("localSettings","plan")},context);return ErrorCode::None;}catch(const Error& error){return error.code();}});
        QTest::qSleep(30);context.cancellation.cancel();QCOMPARE(job.get(),ErrorCode::Cancelled);
        QCOMPARE(read(path),(QJsonObject{{"theme","KEEP"}}));QVERIFY(::flock(lock.fd,LOCK_UN)==0);
        timeout.applyUpdates({mode("localSettings","plan")},f.context());QCOMPARE(timeout.describe(f.context())["mode"],"plan");
#else
        QSKIP("POSIX descriptor lock fixture");
#endif
    }
};
QTEST_GUILESS_MAIN(PermissionUpdatesTests)
#include "permission_updates_tests.moc"
