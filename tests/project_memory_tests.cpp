#include "agent/ProjectMemory.h"
#include "agent/Engine.h"
#include <QtTest/QtTest>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QCryptographicHash>
#include <future>

using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void put(const QString& path,const QByteArray& content) {
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));QFile f(path);QVERIFY(f.open(QIODevice::WriteOnly));QCOMPARE(f.write(content),content.size());
}
QString digest(const QByteArray& bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
struct Fixture {
    QTemporaryDir root;
    QString workspace=root.filePath("workspace"),state=root.filePath("state");
    a::ProjectMemoryOptions options{root.filePath("state/memory")};
    std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::ProjectMemory> memory;
    std::shared_ptr<const a::PermissionPolicy> policy=std::make_shared<a::RulePolicy>(a::PermissionMode::AcceptEdits);
    a::ToolContext context;
    Fixture() {
        QDir().mkpath(workspace);memory=std::make_shared<a::ProjectMemory>(options);
        a::registerWorkspaceTools(*registry,workspace,{},QStringList{state});memory->directory(workspace);
        memory->bindWorkspaceTools(*registry);registry->add(memory->forgetTool(false));
        context={"session","run",workspace,root.filePath("artifacts")};
    }
    QString path(const QString& name="MEMORY.md") {return QDir(memory->directory(workspace)).filePath(name);}
    a::ToolResult run(const QString& name,const QJsonObject& args) {return a::ToolRunner(registry,policy).run({"call",name,args},context);}
};
}
class ProjectMemoryTests final:public QObject {
    Q_OBJECT
private slots:
    void workspaceNotesPersistAcrossOwnersWithoutExposingAdjacentState() {
        Fixture f;put(f.root.filePath("state/secret.txt"),"secret");
        auto written=f.run("Write",{{"path",f.path()},{"content","Preferred language: Korean"}});QVERIFY2(!written.isError,qPrintable(written.text));
        a::ProjectMemory reopened(f.options);auto value=reopened.snapshot(f.workspace);
        QCOMPARE(value["index"].toString(),"Preferred language: Korean");QCOMPARE(value["index_sha256"].toString(),digest("Preferred language: Korean"));
        QVERIFY(f.run("Read",{{"path",f.root.filePath("state/secret.txt")}}).isError);
        const auto other=f.root.filePath("other-workspace");QDir().mkpath(other);
        QVERIFY(reopened.directory(other)!=f.memory->directory(f.workspace));
        QVERIFY(f.run("Read",{{"path",QDir(reopened.directory(other)).filePath("MEMORY.md")}}).isError);
        put(f.root.filePath("workspace/ordinary.md"),"workspace file");
        QCOMPARE(f.run("Read",{{"path","ordinary.md"}}).text,"workspace file");
    }
    void indexHasUtf8AndLineBoundsAndNeverBecomesAnInstruction() {
        Fixture f;auto options=f.options;options.maxIndexLines=2;options.maxIndexBytes=7;
        a::ProjectMemory memory(options);put(f.path(),QString::fromUtf8("한글입니다\nsecond\nthird").toUtf8());
        const auto state=memory.snapshot(f.workspace);QVERIFY(state["index_truncated"].toBool());
        QCOMPARE(state["index"].toString(),QString::fromUtf8("한글"));QVERIFY(state["index"].toString().toUtf8().size()<=7);
        const auto message=memory.message(f.workspace);QCOMPARE(message.role,a::MessageRole::User);
        QVERIFY(message.text.contains("untrusted"));QVERIFY(message.text.contains("truncated"));
        QVERIFY(message.metadata.contains("iilocal.project_memory"));
    }
    void topicDiscoverySearchAndInvalidFilesHaveExplicitBoundaries() {
        Fixture f;
        put(f.path("preferences.md"),"---\nname: Communication\ndescription: Korean responses\ntype: user\n---\nKeep replies concise.");
        put(f.path("build/testing.md"),"---\nname: Tests\ndescription: Build validation\ntype: feedback\n---\nBuild and run tests.");
        put(f.path("broken.md"),"---\nname: first\nname: second\n---\nInvalid YAML metadata");
        const auto result=f.memory->snapshot(f.workspace,"korean");
        QCOMPARE(result["files"].toArray().size(),1);QCOMPARE(result["files"].toArray().first().toObject()["path"].toString(),"preferences.md");
        QCOMPARE(result["files"].toArray().first().toObject()["type"].toString(),"user");
        QCOMPARE(result["diagnostics"].toArray().size(),1);
        QVERIFY(f.run("Glob",{{"path",f.memory->directory(f.workspace)},{"pattern","**/*.md"}}).text.contains("testing.md"));
        QVERIFY(f.run("Grep",{{"path",f.memory->directory(f.workspace)},{"pattern","Build and run"}}).text.contains("testing.md"));
    }
    void changedNotesNeedFreshReadsAndForgetNeedsCurrentHash() {
        Fixture f;const auto path=f.path("feedback.md");put(path,"before");
        QVERIFY(f.run("Write",{{"path",path},{"content","after"}}).isError);
        auto observed=f.run("Read",{{"path",path}});QVERIFY(!observed.isError);QCOMPARE(observed.data["sha256"].toString(),digest("before"));put(path,"external");
        QVERIFY(f.run("Edit",{{"path",path},{"old_string","before"},{"new_string","after"}}).isError);
        QVERIFY(f.run("MemoryForget",{{"path",path},{"sha256",digest("before")}}).isError);QVERIFY(QFileInfo::exists(path));
        auto result=f.run("MemoryForget",{{"path",path},{"sha256",digest("external")}});QVERIFY2(!result.isError,qPrintable(result.text));
        QVERIFY(!QFileInfo::exists(path));QVERIFY(result.data["removed"].toBool());QVERIFY(!result.data["backup_path"].toString().isEmpty());
        QVERIFY(f.run("MemoryForget",{{"path",f.root.filePath("workspace/ordinary.md")},{"sha256",digest("external")}}).isError);
    }
    void concurrentOwnersCannotOverwriteEachOthersReadSnapshot() {
        Fixture f;const auto path=f.path("concurrent.md");put(path,"initial");
        a::ProjectMemory second(f.options);auto registry=std::make_shared<a::ToolRegistry>();
        a::registerWorkspaceTools(*registry,f.workspace,{},QStringList{f.state});second.bindWorkspaceTools(*registry);
        a::ToolContext context=f.context;context.sessionId="second";
        a::ToolRunner runner(registry,f.policy);
        QVERIFY(!f.run("Read",{{"path",path}}).isError);QVERIFY(!runner.run({"read","Read",{{"path",path}}},context).isError);
        auto one=std::async(std::launch::async,[&]{return f.run("Write",{{"path",path},{"content","one"}});});
        auto two=std::async(std::launch::async,[&]{return runner.run({"write","Write",{{"path",path},{"content","two"}}},context);});
        const auto a=one.get(),b=two.get();QCOMPARE(int(!a.isError)+int(!b.isError),1);
        QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();QVERIFY(bytes=="one"||bytes=="two");
    }
    void replacingTheMemoryRootCannotRedirectAnApprovedWrite() {
        Fixture f;const auto directory=f.memory->directory(f.workspace),outside=f.root.filePath("outside");QDir().mkpath(outside);
        f.policy=std::make_shared<a::RulePolicy>();a::ToolRunnerOptions options;
        options.permission=[&](const auto&,const auto&,const auto&) {
            const auto moved=directory+"-moved";
            if(!QDir().rename(directory,moved)||!QFile::link(outside,directory))throw std::runtime_error("Cannot set up root replacement");
            return true;
        };
        auto result=a::ToolRunner(f.registry,f.policy,options).run({"write","Write",{{"path",QDir(directory).filePath("new.md")},{"content","unsafe"}}},f.context);
        QVERIFY(result.isError);QVERIFY(!QFileInfo::exists(QDir(outside).filePath("new.md")));
    }
    void publicWorkspaceSearchCannotDiscoverOtherProjectMemory() {
        QTemporaryDir root;const auto workspace=root.filePath("work"),other=root.filePath("other");
        QDir().mkpath(workspace);QDir().mkpath(other);
        a::ProjectMemory memory({QDir(workspace).filePath("state/memory")});
        const auto own=memory.directory(workspace),foreign=memory.directory(other);
        put(QDir(own).filePath("MEMORY.md"),"OWN_NOTE");put(QDir(foreign).filePath("MEMORY.md"),"FOREIGN_NOTE_943");
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,workspace);memory.bindWorkspaceTools(*registry);
        a::ToolRunner runner(registry,std::make_shared<a::RulePolicy>());a::ToolContext context{"owner","run",workspace};
        auto result=runner.run({"search","Grep",{{"path","."},{"pattern","FOREIGN_NOTE_943"}}},context);
        QVERIFY(!result.text.contains("FOREIGN_NOTE_943"));
        QVERIFY(runner.run({"read","Read",{{"path",QDir(foreign).filePath("MEMORY.md")}}},context).isError);
        QVERIFY(QFile::link(QDir(foreign).filePath("MEMORY.md"),QDir(workspace).filePath("alias.md")));
        QVERIFY(runner.run({"alias","Read",{{"path","alias.md"}}},context).isError);
        QCOMPARE(runner.run({"own","Read",{{"path",QDir(own).filePath("MEMORY.md")}}},context).text,"OWN_NOTE");
    }
    void symlinksCancellationLimitsAndExplicitPolicyDenialsApply() {
        Fixture f;put(f.root.filePath("outside.md"),"private");
        QVERIFY(QFile::link(f.root.filePath("outside.md"),f.path("link.md")));
        QVERIFY(f.run("Read",{{"path",f.path("link.md")}}).isError);
        QVERIFY(f.run("Write",{{"path",f.path("link.md")},{"content","overwrite"}}).isError);
        QVERIFY(!f.memory->snapshot(f.workspace)["diagnostics"].toArray().isEmpty());
        CancellationToken cancelled;cancelled.cancel();QVERIFY_EXCEPTION_THROWN(f.memory->snapshot(f.workspace,{},cancelled),Error);
        auto options=f.options;options.maxFiles=1;a::ProjectMemory bounded(options);
        put(f.path("a.md"),"alpha");put(f.path("b.md"),"beta");QVERIFY(bounded.snapshot(f.workspace)["catalog_truncated"].toBool());
        f.policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"Write",a::PermissionBehavior::Deny},{"MemoryForget",a::PermissionBehavior::Deny}});
        QVERIFY(f.run("Write",{{"path",f.path("new.md")},{"content","new"}}).isError);QVERIFY(!QFileInfo::exists(f.path("new.md")));
        QVERIFY(f.run("MemoryForget",{{"path",f.path("a.md")},{"sha256",digest("alpha")}}).isError);QVERIFY(QFileInfo::exists(f.path("a.md")));
    }
};
QTEST_GUILESS_MAIN(ProjectMemoryTests)
#include "project_memory_tests.moc"
