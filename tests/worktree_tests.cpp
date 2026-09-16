#include <agent/Worktrees.h>
#include <agent/Engine.h>
#include <agent/PermissionSettings.h>
#include <agent/ShellTasks.h>
#include <QtCore/QJsonDocument>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
QByteArray git(const QString& root,QStringList args) {
    QProcess p;p.setWorkingDirectory(root);p.start("git",args);
    if(!p.waitForStarted(5000)||!p.waitForFinished(30000)||p.exitCode()!=0)
        throw std::runtime_error(("fixture git failed ("+args.join(' ').toUtf8()+"): "+p.errorString().toUtf8()+"; exit="+QByteArray::number(p.exitCode())+"; "+p.readAllStandardError()).constData());
    return p.readAllStandardOutput().trimmed();
}
void write(const QString& path,const QByteArray& bytes) {
    QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())throw std::runtime_error("fixture write");
}
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("worktree-test-XXXXXX")};
    QString repo=root.filePath("repo"),state=root.filePath("state");
    a::ToolContext context;a::WorktreeOptions options;
    Fixture(){QDir().mkpath(repo);git(repo,{"init","-q","-b","main"});git(repo,{"config","user.name","Fixture"});git(repo,{"config","user.email","fixture@example.invalid"});
        write(repo+"/main.cpp","int original=1;\n");git(repo,{"add","main.cpp"});git(repo,{"commit","-qm","initial"});
        context.sessionId="owner";context.workingDirectory=repo;options.directory=root.filePath("worktrees");options.fetchMissingBase=false;}
};
}
class WorktreeTests:public QObject {
 Q_OBJECT
private slots:
 void realGitEnterKeepAndResumePreserveTheOriginalCheckout(){
    Fixture f;const auto cwd=QDir::currentPath();a::Worktrees worktrees(f.state,f.options);
    const auto created=worktrees.enter({{"name","user/feature"}},f.context);QVERIFY(!created.isError);
    const auto path=created.data["worktreePath"].toString();QVERIFY(QFileInfo(path+"/.git").isFile());
    QCOMPARE(git(path,{"branch","--show-current"}),QByteArray("worktree-user+feature"));
    QCOMPARE(worktrees.view(f.context).directory,path);QCOMPARE(QDir::currentPath(),cwd);
    write(path+"/main.cpp","int isolated=2;\n");
    const auto kept=worktrees.exit({{"action","keep"}},f.context);QVERIFY(!kept.isError);QVERIFY(QFileInfo(path).isDir());
    QCOMPARE(worktrees.view(f.context).directory,f.repo);QCOMPARE(git(f.repo,{"status","--porcelain"}),QByteArray());
    const auto resumed=worktrees.enter({{"name","user/feature"}},f.context);QCOMPARE(resumed.data["worktreePath"].toString(),path);
    a::Worktrees reopened(f.state,f.options);QCOMPARE(reopened.view(f.context).directory,path);
 }
 void removeRequiresDiscardForFilesAndNewCommits(){
    Fixture f;a::Worktrees worktrees(f.state,f.options);const auto path=worktrees.enter({{"name","dirty"}},f.context).data["worktreePath"].toString();
    write(path+"/new.txt","valuable\n");QVERIFY_THROWS_EXCEPTION(Error,worktrees.exit({{"action","remove"}},f.context));QVERIFY(QFileInfo(path+"/new.txt").exists());
    git(path,{"add","new.txt"});git(path,{"commit","-qm","isolated change"});QVERIFY_THROWS_EXCEPTION(Error,worktrees.exit({{"action","remove"}},f.context));
    const auto removed=worktrees.exit({{"action","remove"},{"discard_changes",true}},f.context);
    QCOMPARE(removed.data["discardedCommits"].toInt(),1);QVERIFY(!QFileInfo(path).exists());
    QCOMPARE(git(f.repo,{"branch","--list","worktree-dirty"}),QByteArray());QCOMPARE(worktrees.view(f.context).directory,f.repo);
 }
 void ownershipCollisionsAndInvalidNamesCannotDeleteOtherWork(){
    Fixture f;a::Worktrees worktrees(f.state,f.options);
    for(const auto& name:{"../bad","/absolute","a//b","a+other","a/..","a..b","a.",""})QVERIFY_THROWS_EXCEPTION(Error,worktrees.enter({{"name",name}},f.context));
    git(f.repo,{"branch","worktree-existing"});QVERIFY_THROWS_EXCEPTION(Error,worktrees.enter({{"name","existing"}},f.context));
    const auto path=worktrees.enter({{"name","owner-only"}},f.context).data["worktreePath"].toString();
    auto other=f.context;other.sessionId="other";
    QVERIFY_THROWS_EXCEPTION(Error,worktrees.enter({{"name","owner-only"}},other));
    QVERIFY_THROWS_EXCEPTION(Error,worktrees.exit({{"action","remove"},{"discard_changes",true}},other));
    QVERIFY(QFileInfo(path).isDir());QVERIFY(!git(f.repo,{"branch","--list","worktree-existing"}).isEmpty());
 }
 void failedCreationAndMissingWorktreeCanReturnWithoutDeletingData(){
    Fixture f;auto options=f.options;options.create=[](const QString&,const a::ToolContext&)->QString{throw Error(ErrorCode::Cancelled,"creation cancelled");};
    a::Worktrees failing(f.root.filePath("failed-state"),options);QVERIFY_THROWS_EXCEPTION(Error,failing.enter({{"name","cancelled"}},f.context));
    const auto status=failing.status(f.context);QVERIFY(!status["active"].toBool());QCOMPARE(status["retained"].toArray().first().toObject()["phase"].toString(),"failed");
    a::Worktrees worktrees(f.state,f.options);const auto path=worktrees.enter({{"name","missing"}},f.context).data["worktreePath"].toString();
    git(f.repo,{"worktree","remove",path});QVERIFY(worktrees.status(f.context)["recoveryRequired"].toBool());
    QVERIFY(!worktrees.exit({{"action","keep"}},f.context).isError);QCOMPARE(worktrees.view(f.context).directory,f.repo);
 }
 void preparedOperationsCannotReuseChangedStateOrDiscardNewContents(){
    Fixture f;a::Worktrees worktrees(f.state,f.options);
    const auto pending=worktrees.enterTool().prepare({},f.context);QVERIFY(!pending.definition.metadata["worktree_name"].toString().isEmpty());
    const auto path=worktrees.enter({{"name","other"}},f.context).data["worktreePath"].toString();QVERIFY_THROWS_EXCEPTION(Error,pending.execute());
    const auto removal=worktrees.exitTool().prepare({{"action","remove"},{"discard_changes",true}},f.context);
    write(path+"/new.txt","arrived after preview");QVERIFY_THROWS_EXCEPTION(Error,removal.execute());QVERIFY(QFileInfo(path+"/new.txt").exists());
    git(f.repo,{"worktree","lock",path});QVERIFY_THROWS_EXCEPTION(Error,worktrees.exit({{"action","remove"},{"discard_changes",true}},f.context));
    QVERIFY(worktrees.status(f.context)["active"].toBool());git(f.repo,{"worktree","unlock",path});
 }
 void removalPreviewRequiresACompleteGitSnapshot(){
    Fixture f;a::Worktrees worktrees(f.state,f.options);const auto path=worktrees.enter({{"name","large"}},f.context).data["worktreePath"].toString();
    QFile large(path+"/large.bin");QVERIFY(large.open(QIODevice::WriteOnly));QVERIFY(large.resize(65LL*1024*1024));large.close();
    const auto status=worktrees.status(f.context);QVERIFY(!status["changesKnown"].toBool());QVERIFY(status["recoveryRequired"].toBool());
    QVERIFY_THROWS_EXCEPTION(Error,worktrees.exitTool().prepare({{"action","remove"},{"discard_changes",true}},f.context));
    QVERIFY(QFileInfo(path+"/large.bin").exists());QVERIFY(!worktrees.exitTool().prepare({{"action","keep"}},f.context).execute().isError);
    QCOMPARE(worktrees.view(f.context).directory,f.repo);QVERIFY(QFileInfo(path).isDir());
 }
 void sparseCheckoutAndWhitespacePathsRetainTheOriginalChanges(){
    Fixture f;const auto renamed=f.root.filePath("repo with space ");QVERIFY(QDir().rename(f.repo,renamed));f.repo=renamed;f.context.workingDirectory=f.repo;
    QDir().mkpath(f.repo+"/one");QDir().mkpath(f.repo+"/two");write(f.repo+"/one/a.txt","selected");write(f.repo+"/two/b.txt","excluded");
    git(f.repo,{"add","."});git(f.repo,{"commit","-qm","folders"});write(f.repo+"/main.cpp","original uncommitted work");write(f.repo+"/untracked.txt","original untracked work");
    f.options.sparsePaths={"one"};a::Worktrees worktrees(f.state,f.options);const auto path=worktrees.enter({{"name","sparse"}},f.context).data["worktreePath"].toString();
    QVERIFY(QFileInfo(path+"/one/a.txt").exists());QVERIFY(!QFileInfo(path+"/two/b.txt").exists());QVERIFY(!QFileInfo(path+"/untracked.txt").exists());
    QVERIFY(!worktrees.exit({{"action","remove"}},f.context).isError);QVERIFY(git(f.repo,{"status","--porcelain"}).contains("untracked.txt"));
 }
 void nonGitAdaptersAreOwnerBoundAndRemovalMustActuallySucceed(){
    Fixture f;const auto hookRoot=f.root.filePath("hook-work");QDir().mkpath(hookRoot);write(hookRoot+"/valuable","data");
    f.options.create=[hookRoot](const QString&,const a::ToolContext&){return hookRoot;};f.options.remove=[](const QString&,const a::ToolContext&){};
    a::Worktrees worktrees(f.state,f.options);worktrees.enter({{"name","hook"}},f.context);QVERIFY(!worktrees.status(f.context)["changesKnown"].toBool());
    QVERIFY_THROWS_EXCEPTION(Error,worktrees.exit({{"action","remove"}},f.context));QVERIFY_THROWS_EXCEPTION(Error,worktrees.exit({{"action","remove"},{"discard_changes",true}},f.context));
    QVERIFY(worktrees.status(f.context)["active"].toBool());QVERIFY(QFileInfo(hookRoot+"/valuable").exists());worktrees.exit({{"action","keep"}},f.context);
 }
 void cachedRemoteBaseAndChangedFileContentAreBound(){
    Fixture f;git(f.repo,{"update-ref","refs/remotes/origin/main","HEAD"});git(f.repo,{"symbolic-ref","refs/remotes/origin/HEAD","refs/remotes/origin/main"});
    write(f.repo+"/later.txt","local commit");git(f.repo,{"add","later.txt"});git(f.repo,{"commit","-qm","local ahead"});
    a::Worktrees worktrees(f.state,f.options);const auto path=worktrees.enter({{"name","remote"}},f.context).data["worktreePath"].toString();QVERIFY(!QFileInfo(path+"/later.txt").exists());
    write(path+"/main.cpp","before preview");const auto prepared=worktrees.exitTool().prepare({{"action","remove"},{"discard_changes",true}},f.context);
    write(path+"/main.cpp","after! preview");QVERIFY_THROWS_EXCEPTION(Error,prepared.execute());QVERIFY(QFileInfo(path).exists());
 }
 void boundScopesReloadSettingsAndExpireReadsWithoutGrantingTheOriginalRoot(){
    Fixture f;write(f.repo+"/host-private.txt","private");git(f.repo,{"add","host-private.txt"});git(f.repo,{"commit","-qm","private fixture"});
    QDir().mkpath(f.root.filePath("extra"));QDir().mkpath(f.options.directory+"/extra");
    auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,f.repo,{}, {f.repo+"/host-private.txt"});
    a::PermissionSettingsOptions config;config.workingDirectory=f.repo;config.additionalDirectories={"../extra"};config.fallbackMode=a::PermissionMode::Bypass;auto policy=std::make_shared<a::SettingsPermissionPolicy>(config);a::ToolRunner runner(registry,policy);
    QVERIFY(!runner.run({"read","Read",{{"path","main.cpp"}}},f.context).isError);
    a::Worktrees worktrees(f.state,f.options);const auto path=worktrees.enter({{"name","settings"}},f.context).data["worktreePath"].toString();
    auto scope=f.context;scope.workingDirectory=path;QVERIFY(runner.run({"unbound","Read",{{"path","main.cpp"}}},scope).isError);
    scope.originalWorkingDirectory=f.repo;scope.workspaceRevision=worktrees.view(f.context).revision;
    QVERIFY(!runner.run({"bound","Read",{{"path","main.cpp"}}},scope).isError);QVERIFY(runner.run({"outside","Read",{{"path",f.repo+"/main.cpp"}}},scope).isError);
    QVERIFY(runner.run({"private","Read",{{"path","host-private.txt"}}},scope).isError);
    QVERIFY(policy->workingDirectories(scope).contains(f.root.filePath("extra")));QVERIFY(!policy->workingDirectories(scope).contains(f.options.directory+"/extra"));
    QDir().mkpath(path+"/.claude");write(path+"/.claude/settings.json",R"json({"permissions":{"deny":["Read(main.cpp)"]}})json");
    QVERIFY(runner.run({"denied","Read",{{"path","main.cpp"}}},scope).isError);QCOMPARE(policy->describe(scope)["working_directories"].toArray().first().toString(),path);
    worktrees.exit({{"action","keep"}},f.context);scope.workingDirectory=f.repo;scope.workspaceRevision=worktrees.view(f.context).revision;
    QVERIFY(runner.run({"old-read","Write",{{"path","main.cpp"},{"content","must not write"}}},scope).isError);
    QVERIFY(!runner.run({"fresh","Read",{{"path","main.cpp"}}},scope).isError);
 }
 void engineSwitchesFilesShellPolicyAndPromptInsideOneToolBatch(){
    class Model:public a::Model {public:int turns=0;QString path;a::ModelReply generate(const a::ModelRequest&,const CancellationToken&,const std::function<bool(const QString&)>&)override {
        if(turns++==0)return {{},{{"enter","EnterWorktree",{{"name","batch"}}},{"read","Read",{{"path","main.cpp"}}},
            {"write","Write",{{"path","main.cpp"},{"content","isolated change"}}},{"shell","Bash",{{"command","pwd; cat main.cpp"}}},
            {"keep","ExitWorktree",{{"action","keep"}}},{"original","Write",{{"path","original-only.txt"},{"content","original workspace"}}}},{1,1}};
        return {"DONE",{},{1,1}};
    }};
    Fixture f;auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,f.repo);
    a::PermissionSettingsOptions settings;settings.workingDirectory=f.repo;settings.fallbackMode=a::PermissionMode::Bypass;
    auto policy=std::make_shared<a::SettingsPermissionPolicy>(settings);a::EngineOptions options;options.sessionsDirectory=f.state;options.worktrees=f.options;options.worktrees.enabled=true;options.worktrees.deferred=false;options.projectMemoryEnabled=true;options.memoryRecall.enabled=false;options.memoryExtraction.enabled=false;
    a::Engine engine(model,registry,policy,options);const auto session=engine.createSession("fixture",f.repo);const auto result=engine.run({session.id,"Create an isolated worktree, edit and leave keeping it."}).result.get();
    QVERIFY2(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage));QCOMPARE(result.text,"DONE");
    const auto history=engine.session(session.id);for(const auto& message:history.messages)if(message.role==a::MessageRole::Tool)QVERIFY2(!message.isError,qPrintable(message.text));
    QFile original(f.repo+"/main.cpp");QVERIFY(original.open(QIODevice::ReadOnly));QCOMPARE(original.readAll(),QByteArray("int original=1;\n"));
    const auto path=f.options.directory+"/batch";QFile changed(path+"/main.cpp");QVERIFY(changed.open(QIODevice::ReadOnly));QCOMPARE(changed.readAll(),QByteArray("isolated change"));
    bool shell=false;for(const auto& message:history.messages)if(message.toolCallId=="shell"){shell=true;QVERIFY(message.text.contains(path));QVERIFY(message.text.contains("isolated change"));}QVERIFY(shell);
    QVERIFY(QFileInfo(f.repo+"/original-only.txt").exists());QVERIFY(!QFileInfo(path+"/original-only.txt").exists());QCOMPARE(engine.sessionMetadata(session.id).workingDirectory,f.repo);
 }
 void sessionEndHooksObserveTheCurrentWorktree(){
    class Model:public a::Model {public:a::ModelReply generate(const a::ModelRequest&,const CancellationToken&,const std::function<bool(const QString&)>&)override{return {"READY",{},{1,1}};}};
    Fixture f;auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    a::EngineOptions options;options.sessionsDirectory=f.state;options.worktrees=f.options;options.worktrees.enabled=true;
    QString cwd,snapshot;options.hooks.append([&](const a::HookInput& input,const CancellationToken&){if(input.kind==a::HookKind::SessionEnd){cwd=input.context["cwd"].toString();if(input.modelContext&&input.modelContext->session)snapshot=input.modelContext->session->workingDirectory;}return a::HookResult{};});
    a::Engine engine(std::make_shared<Model>(),registry,policy,options);const auto id=engine.createSession("fixture",f.repo).id;
    QCOMPARE(engine.run({id,"Start"}).result.get().text,QString("READY"));const auto entered=engine.runWorktreeTool(id,"EnterWorktree",{{"name","end-hook"}});
    QVERIFY2(!entered.isError,qPrintable(entered.text));const auto path=entered.data["worktreePath"].toString();engine.endSession(id);
    QCOMPARE(cwd,path);QCOMPARE(snapshot,path);QVERIFY(engine.worktreeStatus(id)["active"].toBool());
 }
 void engineResumeAndBackgroundShellUseTheOwnedWorkspace(){
    class Model:public a::Model {public:a::ModelReply generate(const a::ModelRequest&,const CancellationToken&,const std::function<bool(const QString&)>&)override{return {"DONE",{},{1,1}};}};
    Fixture f;auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();auto shells=std::make_shared<a::ShellTasks>(f.repo,f.root.filePath("shells"));a::registerWorkspaceTools(*registry,f.repo,shells);
    auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);a::EngineOptions options;options.sessionsDirectory=f.state;options.worktrees=f.options;options.worktrees.enabled=true;
    QString id,path;
    {a::Engine engine(model,registry,policy,options);id=engine.createSession("fixture",f.repo).id;auto entered=engine.runWorktreeTool(id,"EnterWorktree",{{"name","persistent"}});QVERIFY2(!entered.isError,qPrintable(entered.text));path=entered.data["worktreePath"].toString();
     QCOMPARE(engine.session(id).workingDirectory,path);QCOMPARE(engine.sessionMetadata(id).workingDirectory,f.repo);QVERIFY_THROWS_EXCEPTION(Error,engine.clearSession(id));
     auto child=engine.forkSession(id);QCOMPARE(child.workingDirectory,f.repo);QVERIFY(!engine.worktreeStatus(child.id)["active"].toBool());
     a::ToolContext context{id,{},f.repo};auto scope=engine.bindWorkspaceContext(context);auto task=shells->start(context,"pwd",{},10000);scope.reset();
     const auto output=shells->output(id,task["task_id"].toString());QVERIFY2(QJsonDocument(output).toJson().contains(path.toUtf8()),QJsonDocument(output).toJson().constData());
     auto running=engine.runShellTool(id,"Bash",{{"command","sleep 30"},{"run_in_background",true}});QVERIFY2(!running.isError,qPrintable(running.text));
     auto removal=engine.runWorktreeTool(id,"ExitWorktree",{{"action","remove"}});QVERIFY(removal.isError);QVERIFY(QFileInfo(path).exists());
     QVERIFY(!engine.runShellTool(id,"TaskStop",{{"task_id",running.data["task_id"]}}).isError);}
    {a::Engine engine(model,registry,policy,options);QCOMPARE(engine.session(id).workingDirectory,path);auto result=engine.run({id,"Resume"}).result.get();QVERIFY(result.status==a::RunStatus::Completed);
     QVERIFY(!engine.runWorktreeTool(id,"ExitWorktree",{{"action","remove"}}).isError);QCOMPARE(engine.session(id).workingDirectory,f.repo);QVERIFY(!QFileInfo(path).exists());}
 }
 void ignoredFilesAndChangedBranchAreProtected(){
    Fixture f;a::Worktrees worktrees(f.state,f.options);const auto path=worktrees.enter({{"name","ignored"}},f.context).data["worktreePath"].toString();
    write(path+"/.gitignore","cache/\n");git(path,{"add",".gitignore"});git(path,{"commit","-qm","ignore cache"});
    QDir().mkpath(path+"/cache");write(path+"/cache/data.bin","keep me");
    QVERIFY(worktrees.status(f.context)["changedFiles"].toInt()>0);
    QVERIFY_THROWS_EXCEPTION(Error,worktrees.exit({{"action","remove"}},f.context));
    git(path,{"checkout","-qb","unrelated"});QVERIFY_THROWS_EXCEPTION(Error,worktrees.exit({{"action","remove"},{"discard_changes",true}},f.context));
    QVERIFY(QFileInfo(path+"/cache/data.bin").exists());
 }
};
QTEST_GUILESS_MAIN(WorktreeTests)
#include "worktree_tests.moc"
