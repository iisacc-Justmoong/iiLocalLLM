#include "agent/PermissionSettings.h"
#include <QtCore/QTemporaryDir>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtTest/QtTest>
using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
namespace {
void save(const QString& path, const QJsonObject& object) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) throw std::runtime_error("fixture directory");
    QFile f(path); if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(object).toJson()) < 0) throw std::runtime_error("fixture settings");
}
QJsonObject permissions(const char* behavior, const QStringList& rules) {
    return {{"permissions", QJsonObject{{behavior, QJsonArray::fromStringList(rules)}}}};
}
a::ToolDefinition writeTool() { return {"Write", "write", {{"type", "object"}}}; }
}
class PermissionSettingsTests final : public QObject {
    Q_OBJECT
private slots:
    void matchesIndependentNodeIgnoreCorpus() {
        QFile file(QFINDTESTDATA("permission_settings_patterns.json"));QVERIFY(file.open(QIODevice::ReadOnly));
        const auto data=QJsonDocument::fromJson(file.readAll()).object();QVERIFY(data["cases"].toArray().size()>=40);
        QTemporaryDir root;a::ToolContext c;c.workingDirectory=root.path();
        a::ToolDefinition read{"Read","read",{{"type","object"}},{},true};
        for(const auto& value:data["cases"].toArray()) {
            const auto row=value.toObject();QStringList rules;for(const auto& pattern:row["patterns"].toArray())rules.append("Read("+pattern.toString()+")");
            a::PermissionSettingsOptions options;options.workingDirectory=root.path();options.inlineSettings=permissions("deny",rules);
            a::SettingsPermissionPolicy policy(options);
            const bool denied=policy.decide(read,{{"path",row["path"]}},c).behavior==a::PermissionBehavior::Deny;
            QVERIFY2(denied==row["denied"].toBool(),qPrintable(QJsonDocument(row).toJson(QJsonDocument::Compact)));
        }
    }
    void mergesSourcesAndRefreshesRemoval() {
        QTemporaryDir root; const auto work = root.filePath("work"); QVERIFY(QDir().mkpath(work));
        a::PermissionSettingsOptions o; o.workingDirectory = work; o.userDirectory = root.filePath("user"); o.managedDirectory = root.filePath("managed");
        o.flagFiles = {root.filePath("flag.json")}; o.inlineSettings = permissions("ask", {"Bash(rm:*)"});
        save(o.userDirectory+"/settings.json", permissions("allow", {"Write(/user/**)"}));
        save(work+"/.claude/settings.json", permissions("allow", {"Write(/src/**)"}));
        save(work+"/.claude/settings.local.json", permissions("deny", {"Write(/src/private/**)"}));
        save(o.flagFiles[0], permissions("allow", {"Skill(review)"}));
        save(o.managedDirectory+"/managed-settings.json", {{"permissions", QJsonObject{{"defaultMode", "dontAsk"}}}});
        a::SettingsPermissionPolicy policy(o); a::ToolContext c; c.workingDirectory = work;
        QCOMPARE(policy.decide(writeTool(), {{"path", "src/code.cpp"}}, c).behavior, a::PermissionBehavior::Allow);
        QCOMPARE(policy.decide(writeTool(), {{"path", "src/private/secret"}}, c).behavior, a::PermissionBehavior::Deny);
        auto snapshot = policy.snapshot(); QCOMPARE(snapshot.mode, a::PermissionMode::DontAsk); QCOMPARE(snapshot.sources.size(), 6);
        QVERIFY(snapshot.toJson().contains("rules"));
        save(work+"/.claude/settings.local.json", {});
        QCOMPARE(policy.decide(writeTool(), {{"path", "src/private/secret"}}, c).behavior, a::PermissionBehavior::Allow);
        save(work+"/.claude/settings.json", {});
        QCOMPARE(policy.decide(writeTool(), {{"path", "src/code.cpp"}}, c).behavior, a::PermissionBehavior::Deny);
    }
    void managedDropInsAndSourceSelection() {
        QTemporaryDir root; const auto work = root.filePath("work"); QVERIFY(QDir().mkpath(work));
        a::PermissionSettingsOptions o; o.workingDirectory=work; o.managedDirectory=root.filePath("managed"); o.enabledSources={};
        save(work+"/.claude/settings.json", permissions("deny", {"Write"}));
        save(o.managedDirectory+"/managed-settings.json", {{"allowManagedPermissionRulesOnly",true},{"permissions",QJsonObject{{"defaultMode","dontAsk"}}}});
        save(o.managedDirectory+"/managed-settings.d/10-base.json", permissions("allow",{"Write(/managed/**)"}));
        save(o.managedDirectory+"/managed-settings.d/20-mode.json", {{"permissions",QJsonObject{{"defaultMode","plan"}}}});
        save(o.managedDirectory+"/managed-settings.d/.hidden.json", permissions("deny",{"Write"}));
        a::SettingsPermissionPolicy policy(o, {{"Write",a::PermissionBehavior::Allow}});
        auto s=policy.snapshot(); QVERIFY(s.managedRulesOnly); QCOMPARE(s.mode,a::PermissionMode::Plan); QCOMPARE(s.sources.size(),3);
        QCOMPARE(s.rules.first().source,QString("policySettings"));
        for(const auto& rule:s.rules)QVERIFY(rule.source=="policySettings"||rule.source=="host.settingsProtection");
        o.modeOverride=a::PermissionMode::Default; a::SettingsPermissionPolicy normal(o,{{"Write",a::PermissionBehavior::Allow}});
        a::ToolContext c; c.workingDirectory=work;c.allowedTools={"Skill(review)"};
        QCOMPARE(normal.decide(writeTool(),{{"path","elsewhere"}},c).behavior,a::PermissionBehavior::Ask);
        QCOMPARE(normal.decide(writeTool(),{{"path","managed/file"}},c).behavior,a::PermissionBehavior::Allow);
    }
    void anchoredBasenamesNegationAndSourceRoots() {
        QTemporaryDir root; const auto work=root.filePath("work"); QVERIFY(QDir().mkpath(work+"/sub"));
        a::PermissionSettingsOptions o; o.workingDirectory=work; o.homeDirectory=root.path();o.userDirectory=work+"/config";
        save(o.userDirectory+"/settings.json", permissions("allow",{"Write(/only/**)"}));
        save(work+"/.claude/settings.json",permissions("deny",{"Read(.env)","Read([Ss]ecret?.txt)","Read(/locked/*)","Read(!/locked/public.txt)"}));
        a::SettingsPermissionPolicy p(o);a::ToolContext c;c.workingDirectory=work;
        a::ToolDefinition read{"Read","read",{{"type","object"}},{},true};
        for(const auto& path:{".env","sub/.env","sub/Secret1.txt","locked/private.txt"})
            QCOMPARE(p.decide(read,{{"path",path}},c).behavior,a::PermissionBehavior::Deny);
        QCOMPARE(p.decide(read,{{"path","locked/public.txt"}},c).behavior,a::PermissionBehavior::Allow);
        QCOMPARE(p.decide(writeTool(),{{"path","config/only/file"}},c).behavior,a::PermissionBehavior::Allow);
        QCOMPARE(p.decide(writeTool(),{{"path","only/file"}},c).behavior,a::PermissionBehavior::Ask);
    }
    void disabledBypassAndInvalidSettingsDoNotGrant() {
        QTemporaryDir root; a::PermissionSettingsOptions o;o.workingDirectory=root.path();o.modeOverride=a::PermissionMode::Bypass;
        save(root.filePath(".claude/settings.json"),{{"permissions",QJsonObject{{"defaultMode","plan"},{"disableBypassPermissionsMode","disable"}}}});
        a::SettingsPermissionPolicy p(o);QCOMPARE(p.snapshot().mode,a::PermissionMode::Plan);
        save(root.filePath(".claude/settings.json"),{{"permissions",QJsonObject{{"deny",QJsonArray{42}}}}});
        QVERIFY_THROWS_EXCEPTION(Error,p.snapshot());
        a::ToolContext c;c.workingDirectory=root.path();QVERIFY_THROWS_EXCEPTION(Error,p.decide(writeTool(),{{"path","file"}},c));
    }
    void unicodeDirectoryParentsAndDuplicateNegations() {
        QTemporaryDir root;a::PermissionSettingsOptions o;o.workingDirectory=root.path();
        o.inlineSettings=permissions("deny",{"Read(비밀?.txt)","Read([가-힣]문서.txt)","Read(ÉTUDE.txt)","Read(/private/)","Read(!/private/visible.txt)",
            "Read(/flat/*)","Read(!/flat/public.txt)","Read(/flat/*)"});
        a::SettingsPermissionPolicy p(o);a::ToolContext c;c.workingDirectory=root.path();
        a::ToolDefinition read{"Read","read",{{"type","object"}},{},true};
        for(const auto& path:{"sub/비밀가.txt","한문서.txt","étude.txt","private/visible.txt","flat/secret"})
            QCOMPARE(p.decide(read,{{"path",path}},c).behavior,a::PermissionBehavior::Deny);
        for(const auto& path:{"비밀가나.txt","public.txt","flat/public.txt"})
            QCOMPARE(p.decide(read,{{"path",path}},c).behavior,a::PermissionBehavior::Allow);
    }
    void filesCannotSilentlyDropRulesOrRewriteAuthority() {
        QTemporaryDir root;a::PermissionSettingsOptions o;o.workingDirectory=root.path();o.fallbackMode=a::PermissionMode::DontAsk;
        const auto path=root.filePath(".claude/settings.json");save(path,permissions("allow",{"Edit"}));
        a::SettingsPermissionPolicy p(o);a::ToolContext c;c.workingDirectory=root.path();
        QCOMPARE(p.decide(writeTool(),{{"path","ordinary.txt"}},c).behavior,a::PermissionBehavior::Allow);
        QCOMPARE(p.decide(writeTool(),{{"path",".claude/./settings.json"}},c).behavior,a::PermissionBehavior::Deny);
        QCOMPARE(p.decide(writeTool(),{{"path","nested/.claude/settings.local.json"}},c).behavior,a::PermissionBehavior::Deny);
        QVERIFY(QFile::remove(path));QVERIFY(QFile::link(root.filePath("missing.json"),path));QVERIFY_THROWS_EXCEPTION(Error,p.snapshot());
        QVERIFY(QFile::remove(path));QVERIFY(QDir().mkdir(path));QVERIFY_THROWS_EXCEPTION(Error,p.snapshot());QVERIFY(QDir().rmdir(path));
        save(path,permissions("deny",{"Write"}));
#ifdef Q_OS_UNIX
        QVERIFY(QFile::setPermissions(path,{}));QVERIFY_THROWS_EXCEPTION(Error,p.snapshot());QVERIFY(QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::WriteOwner));
#endif
        o.maxFileBytes=16;a::SettingsPermissionPolicy tiny(o);QVERIFY_THROWS_EXCEPTION(Error,tiny.snapshot());
        o.maxFileBytes=1024;o.inlineSettings={{"permissions",QJsonObject{{"unknownDirectories",QJsonArray{root.filePath("outside")}}}}};
        a::SettingsPermissionPolicy unsupported(o);QVERIFY(unsupported.snapshot().unsupportedFeatures.contains("permissions.unknownDirectories"));
        QVERIFY_THROWS_EXCEPTION(Error,unsupported.decide(writeTool(),{{"path","file"}},c));
    }
    void finalComponentDirectoryMetadataIsRefreshedBeforeMatching() {
        QTemporaryDir root;a::PermissionSettingsOptions o;o.workingDirectory=root.path();
        o.inlineSettings=permissions("deny",{"Read(target/)"});
        a::SettingsPermissionPolicy policy(o);a::ToolContext context;context.workingDirectory=root.path();
        a::ToolDefinition read{"Read","read",{{"type","object"}},{},true};
        const QJsonObject input{{"path","target"}};
        QVERIFY(QDir(root.path()).mkdir("target"));
        QCOMPARE(policy.decide(read,input,context).behavior,a::PermissionBehavior::Deny);
        QVERIFY(QDir(root.path()).rmdir("target"));
        QFile file(root.filePath("target"));QVERIFY(file.open(QIODevice::WriteOnly));file.close();
        QCOMPARE(policy.decide(read,input,context).behavior,a::PermissionBehavior::Allow);
        QVERIFY(file.remove());
        QCOMPARE(policy.decide(read,input,context).behavior,a::PermissionBehavior::Allow);
    }
    void hostilePatternsAreBoundedAndCancellationPropagates() {
        QTemporaryDir root;a::PermissionSettingsOptions o;o.workingDirectory=root.path();
        o.inlineSettings=permissions("deny",{"Read("+QString("*a").repeated(160)+"b)"});
        a::SettingsPermissionPolicy p(o);a::ToolContext c;c.workingDirectory=root.path();
        a::ToolDefinition read{"Read","read",{{"type","object"}},{},true};
        QElapsedTimer elapsed;elapsed.start();
        try { p.decide(read,{{"path",QString(240,'a')+'b'}},c);QFAIL("Pattern exhaustion must fail the decision"); }
        catch(const Error& error) { QCOMPARE(error.code(),ErrorCode::ResourceLimit); }
        QVERIFY(elapsed.elapsed()<2000);
        c.cancellation.cancel();
        try { p.decide(read,{{"path","file"}},c);QFAIL("Expected cancellation"); }
        catch(const Error& error) { QCOMPARE(error.code(),ErrorCode::Cancelled); }
    }
};
QTEST_GUILESS_MAIN(PermissionSettingsTests)
#include "permission_settings_tests.moc"
