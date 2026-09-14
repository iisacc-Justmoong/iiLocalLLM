#include "agent/PermissionRules.h"
#include "agent/Tools.h"
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QElapsedTimer>
#include <QtTest/QtTest>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
class PermissionRulesTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
#ifdef Q_OS_UNIX
        // Record the cold parser result separately from semantic assertions.
        // Paging/instrumentation can consume the production 100 ms budget;
        // one warm-up keeps later syntax tests from testing cold-start latency.
        a::ToolDefinition bash{"Bash","shell",{{"type","object"}}};a::ToolContext context;
        context.allowedTools={"Bash(printf:*)"};QElapsedTimer timer;timer.start();
        const auto decision=a::RulePolicy().decide(bash,{{"command","printf parser-warmup"}},context);
        qInfo().noquote()<<QString("cold_parser_behavior=%1 reason=%2 elapsed_ms=%3")
            .arg(int(decision.behavior)).arg(decision.reason).arg(timer.elapsed());
        QVERIFY(decision.behavior==a::PermissionBehavior::Allow||decision.behavior==a::PermissionBehavior::Ask);
#endif
    }
    void parsesListsWithoutSplittingArgumentPatterns() {
        QCOMPARE(a::parsePermissionRules({"Read, Bash(git status:*)", "Write(src/**) Skill(review:*)"}),
            QStringList({"Read","Bash(git status:*)","Write(src/**)","Skill(review:*)"}));
        QVERIFY_THROWS_EXCEPTION(Error,a::parsePermissionRules({"Bash(unclosed"}));
        QVERIFY_THROWS_EXCEPTION(Error,a::parsePermissionRules({"Bash(ok) trailing)"}));
    }
    void precedenceAndInvocationLifetime() {
        a::ToolDefinition tool{"Write","Write",{{"type","object"}}};a::ToolContext context;context.allowedTools={"Write"};
        QCOMPARE(a::RulePolicy().decide(tool,{},context).behavior,a::PermissionBehavior::Allow);
        QCOMPARE(a::RulePolicy(a::PermissionMode::DontAsk).decide(tool,{},context).behavior,a::PermissionBehavior::Allow);
        QCOMPARE(a::RulePolicy(a::PermissionMode::Plan).decide(tool,{},context).behavior,a::PermissionBehavior::Deny);
        for(auto behavior:{a::PermissionBehavior::Ask,a::PermissionBehavior::Deny})
            QCOMPARE(a::RulePolicy(a::PermissionMode::Bypass,{{"Write",behavior}}).decide(tool,{},context).behavior,behavior);
        QCOMPARE(a::RulePolicy().decide(tool,{},{}).behavior,a::PermissionBehavior::Ask);
    }
    void fileAndSkillRulesKeepTheirScope() {
        QTemporaryDir root;a::ToolContext context;context.workingDirectory=root.path();context.allowedTools={"Write(src/**)","Skill(review:*)","mcp__society"};
        a::RulePolicy policy;
        a::ToolDefinition write{"Write","write",{{"type","object"}}};
        QCOMPARE(policy.decide(write,{{"path","src/file.txt"}},context).behavior,a::PermissionBehavior::Allow);
        QCOMPARE(policy.decide(write,{{"path","src/../private.txt"}},context).behavior,a::PermissionBehavior::Ask);
        QCOMPARE(policy.decide(write,{{"path","elsewhere/file.txt"}},context).behavior,a::PermissionBehavior::Ask);
        a::ToolDefinition skill{"Skill","skill",{{"type","object"}}};
        QCOMPARE(policy.decide(skill,{{"skill","/review-pr"}},context).behavior,a::PermissionBehavior::Allow);
        QCOMPARE(policy.decide(skill,{{"skill","deploy"}},context).behavior,a::PermissionBehavior::Ask);
        QCOMPARE(policy.decide({"mcp__society__store","store",{{"type","object"}}},{},context).behavior,a::PermissionBehavior::Allow);
        QCOMPARE(policy.decide({"mcp__societyOther__store","store",{{"type","object"}}},{},context).behavior,a::PermissionBehavior::Ask);
    }
    void bashChecksEveryCommandAndFindsNestedDenials() {
#ifndef Q_OS_UNIX
        QSKIP("The non-POSIX executor uses cmd.exe; Bash content rules cannot authorize it");
#endif
        a::ToolDefinition bash{"Bash","shell",{{"type","object"}}};a::ToolContext context;
        context.allowedTools={"Bash(git status:*)","Bash(printf:*)"};a::RulePolicy policy;
        for(const auto& cmd:{"git status", "git status --short && printf done", "git status | printf done"}) {
            QElapsedTimer timer;timer.start();
            const auto decision=policy.decide(bash,{{"command",cmd}},context);
            QVERIFY2(decision.behavior==a::PermissionBehavior::Allow,qPrintable(QString("command=%1 behavior=%2 reason=%3 elapsed_ms=%4")
                .arg(cmd).arg(int(decision.behavior)).arg(decision.reason).arg(timer.elapsed())));
        }
        for(const auto& cmd:{"git status && rm file", "git status $(rm file)", "git status; unknown", "git status > /tmp/out", "git status\nunknown"})
            QVERIFY(policy.decide(bash,{{"command",cmd}},context).behavior!=a::PermissionBehavior::Allow);
        a::RulePolicy denied(a::PermissionMode::Bypass,{{"Bash(rm:*)",a::PermissionBehavior::Deny}});
        for(const auto& cmd:{"rm file", "printf x && rm file", "printf $(rm file)", "FOO=bar 'rm' file", "(rm file)"})
            QCOMPARE(denied.decide(bash,{{"command",cmd}},context).behavior,a::PermissionBehavior::Deny);
        QCOMPARE(a::RulePolicy(a::PermissionMode::Bypass,{{"Bash(printf [x])",a::PermissionBehavior::Deny}})
            .decide(bash,{{"command","printf [x]; printf done"}},context).behavior,a::PermissionBehavior::Deny);
    }
    void redirectsSymlinksAndUnanalysedShellDoNotExpandGrants() {
#ifndef Q_OS_UNIX
        QSKIP("Requires the POSIX Bash executor and filesystem symlinks");
#endif
        QTemporaryDir root,outside;QDir().mkpath(root.filePath("src"));QDir().mkpath(root.filePath("private"));
        a::ToolContext context;context.workingDirectory=root.path();context.allowedTools={"Write(src/**)","Bash(printf:*)"};
        QVERIFY(QFile::link(root.filePath("private"),root.filePath("src/link")));
        a::ToolDefinition write{"Write","write",{{"type","object"}}},bash{"Bash","shell",{{"type","object"}}};
        QCOMPARE(a::RulePolicy().decide(write,{{"path","src/link/secret"}},context).behavior,a::PermissionBehavior::Ask);
        QCOMPARE(a::RulePolicy(a::PermissionMode::Bypass,{{"Write(private/**)",a::PermissionBehavior::Deny}}).decide(write,{{"path","src/link/secret"}},context).behavior,a::PermissionBehavior::Deny);
        QCOMPARE(a::RulePolicy().decide(bash,{{"command","printf value > src/output"}},context).behavior,a::PermissionBehavior::Allow);
        QCOMPARE(a::RulePolicy(a::PermissionMode::Bypass,{{"Write(private/**)",a::PermissionBehavior::Deny}}).decide(bash,{{"command","printf value > src/link/secret"}},context).behavior,a::PermissionBehavior::Deny);
        for(const auto& cmd:{"printf $(unknown)","printf $DYNAMIC", "env FOO=bar printf x", "printf x &", "printf x; cd private; printf y > other", "printf x > $OUT", "printf 'unterminated"})
            QVERIFY(a::RulePolicy().decide(bash,{{"command",cmd}},context).behavior!=a::PermissionBehavior::Allow);
        for(const auto& cmd:{"eval 'rm file'","sh -c 'rm file'","timeout 1 rm file","env -i rm file","$TOOL file"})
            QCOMPARE(a::RulePolicy(a::PermissionMode::Bypass,{{"Bash(rm:*)",a::PermissionBehavior::Deny}}).decide(bash,{{"command",cmd}},context).behavior,a::PermissionBehavior::Deny);
        context.cancellation.cancel();QVERIFY_THROWS_EXCEPTION(Error,a::RulePolicy().decide(bash,{{"command","printf x"}},context));
    }
    void argumentRulesDoNotGrantUnknownToolsAndListsAreBounded() {
        a::ToolContext context;context.allowedTools={"custom(argument)","Agent(reader:*)"};
        QCOMPARE(a::RulePolicy().decide({"custom","custom",{{"type","object"}}},{},context).behavior,a::PermissionBehavior::Ask);
        QCOMPARE(a::RulePolicy().decide({"Agent","agent",{{"type","object"}}},{{"subagent_type","reader-fast"}},context).behavior,a::PermissionBehavior::Allow);
        QVERIFY_THROWS_EXCEPTION(Error,a::parsePermissionRules({QString(65537,'x')}));
        QStringList excessive;for(int i=0;i<257;++i)excessive.append("tool"+QString::number(i));
        QVERIFY_THROWS_EXCEPTION(Error,a::parsePermissionRules(excessive));
    }
    void whitespaceCommandNameDoesNotBecomeCommandAndArgument() {
#ifndef Q_OS_UNIX
        QSKIP("Requires the POSIX Bash executor");
#endif
        a::ToolDefinition bash{"Bash","shell",{{"type","object"}}};a::ToolContext context;
        context.allowedTools={"Bash(git status:*)"};
        for(const auto& command:{"'git status'", "git\\ status --short", "\"git status\""})
            QVERIFY(a::RulePolicy().decide(bash,{{"command",command}},context).behavior!=a::PermissionBehavior::Allow);
        QCOMPARE(a::RulePolicy().decide(bash,{{"command","'git' 'status' --short"}},context).behavior,a::PermissionBehavior::Allow);
    }
    void readWriteRedirectChecksBothAccessRules() {
#ifndef Q_OS_UNIX
        QSKIP("Requires the POSIX Bash executor");
#endif
        QTemporaryDir root;QVERIFY(QDir().mkpath(root.filePath("private")));
        a::ToolDefinition bash{"Bash","shell",{{"type","object"}}};a::ToolContext context;
        context.workingDirectory=root.path();context.allowedTools={"Bash(cat:*)"};
        for(const auto& rule:{"Read(private/**)","Write(private/**)"})
            for(auto behavior:{a::PermissionBehavior::Ask,a::PermissionBehavior::Deny})
                QCOMPARE(a::RulePolicy(a::PermissionMode::Bypass,{{rule,behavior}})
                    .decide(bash,{{"command","cat <> private/secret"}},context).behavior,behavior);
    }
    void trailingArgumentDoesNotReplaceRedirectTarget() {
#ifndef Q_OS_UNIX
        QSKIP("Requires the POSIX Bash executor");
#endif
        QTemporaryDir root;QVERIFY(QDir().mkpath(root.filePath("private")));
        a::ToolDefinition bash{"Bash","shell",{{"type","object"}}};a::ToolContext context;
        context.workingDirectory=root.path();context.allowedTools={"Bash(printf:*)"};
        for(const auto& command:{"printf value > private/secret extra", "printf > private/secret -- other"})
            QCOMPARE(a::RulePolicy(a::PermissionMode::Bypass,{{"Write(private/**)",a::PermissionBehavior::Deny}})
                .decide(bash,{{"command",command}},context).behavior,a::PermissionBehavior::Deny);
    }
    void filePermissionSnapshotRejectsPathReplacementBeforeExecution() {
#ifndef Q_OS_UNIX
        QSKIP("Requires filesystem symlinks");
#endif
        for(bool ask:{false,true}) {
            QTemporaryDir root;const auto workspace=QFileInfo(root.path()).canonicalFilePath();
            QVERIFY(QDir().mkpath(workspace+"/src"));QVERIFY(QDir().mkpath(workspace+"/private"));
            auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,workspace);
            auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{
                {"Write(src/**)",ask?a::PermissionBehavior::Ask:a::PermissionBehavior::Allow},{"Write(private/**)",a::PermissionBehavior::Deny}});
            auto replace=[&]{if(!QDir().rename(workspace+"/src",workspace+"/original-src")||!QFile::link(workspace+"/private",workspace+"/src"))throw std::runtime_error("replace test path");};
            a::ToolRunnerOptions options;if(ask)options.permission=[&](const auto&,const auto&,const auto&){replace();return true;};
            a::ToolRunner runner(registry,policy,options);a::ToolContext context{"session","run",workspace};
            const auto result=runner.run({"write","Write",{{"path","src/output"},{"content","FORBIDDEN"}}},context,[&](const a::Event& e){if(!ask&&e.kind==a::EventKind::ToolStarted)replace();});
            QVERIFY(result.isError);QVERIFY(!QFileInfo::exists(workspace+"/private/output"));
        }
    }
};
QTEST_GUILESS_MAIN(PermissionRulesTests)
#include "permission_rules_tests.moc"
