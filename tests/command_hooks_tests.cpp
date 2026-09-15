#include "agent/CommandHooks.h"
#include "agent/Engine.h"
#include "agent/Subagents.h"
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <future>
#include <thread>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
QJsonObject config(const QString& event,const QString& command,const QString& matcher="*") {
    return {{"hooks",QJsonObject{{event,QJsonArray{QJsonObject{{"matcher",matcher},
        {"hooks",QJsonArray{QJsonObject{{"type","command"},{"command",command}}}}}}}}}};
}
QJsonObject commands(const QString& event,const QJsonArray& hooks) {
    return {{"hooks",QJsonObject{{event,QJsonArray{QJsonObject{{"hooks",hooks}}}}}}};
}
QString output(const QJsonObject& object) {
    auto json=QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));json.replace("'","'\\''");
    return "printf '%s\\n' '"+json+"'";
}
QJsonObject pre(QString permission,QJsonObject input={}) {
    QJsonObject value{{"hookEventName","PreToolUse"},{"permissionDecision",permission}};
    if(!input.isEmpty())value["updatedInput"]=input;
    return {{"hookSpecificOutput",value}};
}
std::shared_ptr<a::ToolRegistry> files(const QString& root) {
    auto r=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*r,root);return r;
}
}
class CommandHooksTests final:public QObject {
    Q_OBJECT
private slots:
    void permissionRequestDecisionsKeepTheirOwnInputAndUpdates() {
        QTemporaryDir work;a::CommandHookOptions options;options.workingDirectory=work.path();
        const QJsonArray updates{QJsonObject{{"type","addRules"},{"destination","session"},{"behavior","allow"},{"rules",QJsonArray{QJsonObject{{"toolName","Write"}}}}}};
        auto reply=[](const QJsonObject& decision){return output({{"hookSpecificOutput",QJsonObject{{"hookEventName","PermissionRequest"},{"decision",decision}}}});};
        const auto allow=reply({{"behavior","allow"},{"updatedInput",QJsonObject{{"path","target.txt"},{"content","CONTENTS"}}},{"updatedPermissions",updates}});
        a::CommandHooks hooks(config("PermissionRequest","cat > request.json; "+allow,"Write|Edit"),options);
        a::HookInput input{a::HookKind::PermissionRequest,"session","run",{"id","Write",{{"path","source.txt"},{"content","ORIGINAL"}}},{},{},
            {{"permission_mode","default"},{"permission_suggestions",updates},{"transcript_path","/private/transcript.jsonl"}}};
        const auto result=hooks.callback()(input,{});QVERIFY(result.permissionResponse);QVERIFY(!result.block);
        QCOMPARE(result.permissionResponse->behavior,a::PermissionBehavior::Allow);QCOMPARE(result.permissionResponse->updatedPermissions,updates);
        QCOMPARE(result.permissionResponse->updatedArguments->value("content"),"CONTENTS");
        QFile payload(work.filePath("request.json"));QVERIFY(payload.open(QIODevice::ReadOnly));const auto body=QJsonDocument::fromJson(payload.readAll()).object();
        QCOMPARE(body["permission_suggestions"],updates);QCOMPARE(body["tool_input"],input.call.arguments);QCOMPARE(body["hook_event_name"],"PermissionRequest");
        // Different completion times, not configuration order. A later denial
        // must not mix into the earlier allow's input/update payload.
        a::CommandHooks race(commands("PermissionRequest",QJsonArray{
            QJsonObject{{"type","command"},{"command","sleep .15; "+reply({{"behavior","deny"},{"interrupt",true}})}},
            QJsonObject{{"type","command"},{"command",allow}}}),options);
        const auto first=race.callback()(input,{});QVERIFY(first.permissionResponse);QCOMPARE(first.permissionResponse->behavior,a::PermissionBehavior::Allow);
        QVERIFY(!first.stop&&!first.block&&!first.permissionResponse->interrupt);QCOMPARE(first.permissionResponse->updatedPermissions,updates);
    }
    void permissionRequestErrorsAndNonzeroExitCannotGrantAccess() {
        QTemporaryDir work;a::CommandHookOptions options;options.workingDirectory=work.path();
        auto reply=[](QJsonObject decision){return output({{"hookSpecificOutput",QJsonObject{{"hookEventName","PermissionRequest"},{"decision",decision}}}});};
        const a::HookInput input{a::HookKind::PermissionRequest,"s","r",{"id","Write",{}},{},{}};
        for(const QJsonObject decision:{QJsonObject{{"behavior","ask"}},QJsonObject{{"behavior","allow"},{"interrupt",true}},
            QJsonObject{{"behavior","deny"},{"updatedInput",QJsonObject{}}},QJsonObject{{"behavior","allow"},{"updatedInput",QJsonArray{}}},
            QJsonObject{{"behavior","allow"},{"updatedPermissions",QJsonArray{QJsonObject{{"type","unknown"}}}}},
            QJsonObject{{"behavior","allow"},{"updatedPermissions",QJsonArray{QJsonObject{{"type","setMode"},{"mode","auto"},{"destination","session"}}}}}}) {
            const auto result=a::CommandHooks(config("PermissionRequest",reply(decision)),options).callback()(input,{});
            QVERIFY(!result.permissionResponse);QCOMPARE(result.diagnostics.last().toObject()["outcome"],"non_blocking_error");
        }
        const auto denied=a::CommandHooks(config("PermissionRequest",reply({{"behavior","allow"}})+"; printf EXIT_DENIED >&2; exit 2"),options).callback()(input,{});
        QVERIFY(denied.permissionResponse);QCOMPARE(denied.permissionResponse->behavior,a::PermissionBehavior::Deny);QCOMPARE(denied.permissionResponse->message,"EXIT_DENIED");
        const auto failure=a::CommandHooks(config("PermissionRequest",reply({{"behavior","allow"}})+"; exit 1"),options).callback()(input,{});
        QVERIFY(!failure.permissionResponse);QCOMPARE(failure.diagnostics.last().toObject()["outcome"],"non_blocking_error");
        auto registry=files(work.path());a::ToolRunnerOptions runner;runner.hooks={a::CommandHooks(config("PermissionRequest",reply({{"behavior","allow"},
            {"updatedInput",QJsonObject{{"path","actual.txt"},{"content","ACTUAL"}}}})),options).callback()};
        const auto result=a::ToolRunner(registry,std::make_shared<a::RulePolicy>(),runner).run({"id","Write",{{"path","original.txt"},{"content","ORIGINAL"}}},{"s","r",work.path()});
        QVERIFY2(!result.isError,qPrintable(result.text));QVERIFY(!QFileInfo::exists(work.filePath("original.txt")));QFile actual(work.filePath("actual.txt"));QVERIFY(actual.open(QIODevice::ReadOnly));QCOMPARE(actual.readAll(),"ACTUAL");
    }
    void inputAndSessionLifecyclePayloadsPreserveContextAndControlBoundaries() {
        QTemporaryDir work;a::CommandHookOptions options;options.workingDirectory=work.path();
        const auto response=output({{"decision","block"},{"reason","IGNORED_START_REASON"},{"continue",false},
            {"hookSpecificOutput",QJsonObject{{"hookEventName","SessionStart"},{"additionalContext","START_CONTEXT"},{"initialUserMessage","INITIAL_PROMPT"}}}});
        a::CommandHooks start(config("SessionStart","cat > start.json; "+response,"startup|compact"),options);
        a::HookInput input{a::HookKind::SessionStart,"session","run",{}, {},{},{{"source","resume"},{"model","model://fixture"}}};
        QVERIFY(start.callback()(input,{}).diagnostics.isEmpty());
        input.context["source"]="startup";const auto result=start.callback()(input,{});
        QCOMPARE(result.feedback,"START_CONTEXT");QVERIFY(result.initialUserMessage);QCOMPARE(*result.initialUserMessage,"INITIAL_PROMPT");
        QFile saved(work.filePath("start.json"));QVERIFY(saved.open(QIODevice::ReadOnly));const auto body=QJsonDocument::fromJson(saved.readAll()).object();
        QCOMPARE(body["source"],"startup");QCOMPARE(body["model"],"model://fixture");QCOMPARE(body["hook_event_name"],"SessionStart");
        a::CommandHooks failed(config("SessionStart","printf 'START_ERROR' >&2; exit 2"),options);
        const auto error=failed.callback()(input,{});QVERIFY(!error.block);QVERIFY(error.feedback.isEmpty());
        QCOMPARE(error.diagnostics.last().toObject()["outcome"],"non_blocking_error");
        a::CommandHooks plain(config("UserPromptSubmit","cat > prompt.json; printf 'PROMPT_CONTEXT'"),options);
        input.kind=a::HookKind::UserPromptSubmit;input.text="/inspect 'two words'\n$(touch INJECTED)";
        QCOMPARE(plain.callback()(input,{}).feedback,"PROMPT_CONTEXT");
        QFile prompt(work.filePath("prompt.json"));QVERIFY(prompt.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(prompt.readAll()).object()["prompt"],input.text);QVERIFY(!QFileInfo::exists(work.filePath("INJECTED")));
        a::CommandHooks denied(config("UserPromptSubmit","printf 'PROMPT_DENIED' >&2; exit 2"),options);
        const auto blocked=denied.callback()(input,{});QVERIFY(blocked.block);QCOMPARE(blocked.feedback,"PROMPT_DENIED");
        a::CommandHooks stopped(config("UserPromptSubmit",output({{"continue",false},{"stopReason","PROMPT_STOPPED"}})),options);
        const auto stop=stopped.callback()(input,{});QVERIFY(stop.stop);QVERIFY(!stop.block);QCOMPARE(stop.stopReason,"PROMPT_STOPPED");
    }
    void receivesJsonOnStdinAndDoesNotInterpolateToolInput() {
        QTemporaryDir work;a::CommandHookOptions options;options.workingDirectory=work.path();
        const auto command="IFS= read -r payload; printf '%s' \"$payload\" > received.json; printf '%s' \"$CLAUDE_PROJECT_DIR\" > project.txt";
        a::CommandHooks hooks(config("PreToolUse",command,"Write|Edit"),options);
        const QString untrusted="$(touch INJECTED) `touch ALSO_INJECTED` ' \"\n한글";
        a::HookInput input{a::HookKind::BeforeTool,"session","run",{"call","Write",{{"path","out.txt"},{"content",untrusted}}},{},{}};
        auto result=hooks.callback()(input,{});QVERIFY(!result.block);
        QFile f(work.filePath("received.json"));QVERIFY(f.open(QIODevice::ReadOnly));const auto json=QJsonDocument::fromJson(f.readAll()).object();
        QCOMPARE(json["hook_event_name"].toString(),QString("PreToolUse"));QCOMPARE(json["session_id"].toString(),QString("session"));
        QCOMPARE(json["tool_use_id"].toString(),QString("call"));QCOMPARE(json["tool_input"].toObject()["content"].toString(),untrusted);
        QVERIFY(!QFileInfo::exists(work.filePath("INJECTED")));QVERIFY(!QFileInfo::exists(work.filePath("ALSO_INJECTED")));
        QFile p(work.filePath("project.txt"));QVERIFY(p.open(QIODevice::ReadOnly));QCOMPARE(QString::fromUtf8(p.readAll()),work.path());
        const auto described=QJsonDocument(hooks.describe()).toJson();QVERIFY(!described.contains("received.json"));
    }
    void changedInputIsValidatedAndHookAllowCannotOverrideHostRules() {
        QTemporaryDir work;a::CommandHookOptions options;options.workingDirectory=work.path();
        auto registry=files(work.path());auto rewrite=pre("allow",{{"path","changed.txt"},{"content","CHANGED"}});
        a::CommandHooks hooks(config("PreToolUse",output(rewrite)),options);
        a::ToolRunner runner(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::DontAsk),{{hooks.callback()}});
        a::ToolContext context{"session","run",work.path()};auto result=runner.run({"write","Write",{{"path","original.txt"},{"content","ORIGINAL"}}},context);
        QVERIFY2(!result.isError,qPrintable(result.text));QVERIFY(QFileInfo::exists(work.filePath("changed.txt")));QVERIFY(!QFileInfo::exists(work.filePath("original.txt")));
        for(auto behavior:{a::PermissionBehavior::Deny,a::PermissionBehavior::Ask}) {
            a::ToolRunner restricted(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::DontAsk,QList<a::PermissionRule>{{"Write",behavior}}),{{hooks.callback()}});
            QVERIFY(restricted.run({"blocked","Write",{{"path","other.txt"},{"content","forbidden"}}},context).isError);
        }
        auto invalid=pre("allow",{{"path",7},{"content","invalid"}});
        a::CommandHooks bad(config("PreToolUse",output(invalid)),options);
        a::ToolRunner invalidRunner(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),{{bad.callback()}});
        QVERIFY(invalidRunner.run({"invalid","Write",{{"path","safe.txt"},{"content","SAFE"}}},context).isError);
    }
    void exitTwoBlocksAndOtherExitIsNonBlocking() {
        QTemporaryDir work;a::CommandHookOptions options;options.workingDirectory=work.path();
        a::ToolContext context{"session","run",work.path()};auto registry=files(work.path());
        a::CommandHooks blocked(config("PreToolUse","printf 'POLICY_BLOCK' >&2; exit 2"),options);
        a::ToolRunner deny(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),{{blocked.callback()}});
        auto result=deny.run({"deny","Write",{{"path","blocked.txt"},{"content","X"}}},context);
        QVERIFY(result.isError);QVERIFY(result.text.contains("POLICY_BLOCK"));QVERIFY(!QFileInfo::exists(work.filePath("blocked.txt")));
        a::CommandHooks failure(config("PreToolUse","printf 'DIAGNOSTIC' >&2; exit 1"),options);
        a::ToolRunner allow(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),{{failure.callback()}});
        result=allow.run({"allow","Write",{{"path","allowed.txt"},{"content","X"}}},context);
        QVERIFY2(!result.isError,qPrintable(result.text));QVERIFY(QFileInfo::exists(work.filePath("allowed.txt")));
    }
    void unmatchedHookDoesNotSerializeLargeToolResults() {
        QTemporaryDir work;a::CommandHookOptions options;options.workingDirectory=work.path();options.maxInputBytes=32;
        a::CommandHooks hooks(config("PreToolUse","exit 0","Write"),options);
        a::HookInput input{a::HookKind::AfterTool,"session","run",{"read","Read",{{"path","large.txt"}}},{QString(10000,'x')},{}};
        a::HookResult result;
        try {result=hooks.callback()(input,{});}catch(const std::exception& e){QFAIL(e.what());}
        QVERIFY(result.diagnostics.isEmpty());
    }
    void onceAndParallelHooksHaveRealProcessSemantics() {
        QTemporaryDir work;a::CommandHookOptions options;options.workingDirectory=work.path();options.timeoutMs=2000;options.maxConcurrentProcesses=2;
        QJsonArray list;
        list.append(QJsonObject{{"type","command"},{"command","touch first; while [ ! -f second ]; do sleep .01; done; printf A >> counts"},{"once",true}});
        list.append(QJsonObject{{"type","command"},{"command","touch second; while [ ! -f first ]; do sleep .01; done; printf B >> counts"},{"once",true}});
        a::CommandHooks hooks(commands("PreToolUse",list),options);
        a::HookInput input{a::HookKind::BeforeTool,"session","run",{"call","Write",{{"path","out.txt"}}},{},{}};
        const auto result=hooks.callback()(input,{});QCOMPARE(result.diagnostics.size(),2);
        for(const auto& d:result.diagnostics)QCOMPARE(d.toObject()["outcome"].toString(),QString("success"));
        QVERIFY(hooks.callback()(input,{}).diagnostics.isEmpty());
        QFile count(work.filePath("counts"));QVERIFY(count.open(QIODevice::ReadOnly));QCOMPARE(count.readAll().size(),2);count.close();
        input.sessionId="another";QCOMPARE(hooks.callback()(input,{}).diagnostics.size(),2);
        QVERIFY(count.open(QIODevice::ReadOnly));QCOMPARE(count.readAll().size(),4);
    }
    void cancellationTimeoutAndOutputLimitStopOwnedProcesses() {
        QTemporaryDir work;a::CommandHookOptions options;options.workingDirectory=work.path();options.timeoutMs=60;
        a::HookInput input{a::HookKind::BeforeTool,"session","run",{"call","Write",{}},{},{}};
        a::CommandHooks timeout(config("PreToolUse","(sleep 1; printf LEAK > leaked.txt) & wait"),options);
        auto result=timeout.callback()(input,{});QCOMPARE(result.diagnostics.last().toObject()["error_code"].toString(),QString("timeout"));
        QTest::qWait(1100);QVERIFY(!QFileInfo::exists(work.filePath("leaked.txt")));
        options.timeoutMs=5000;options.maxOutputBytes=1024;
        a::CommandHooks flood(config("PreToolUse","yes OUTPUT"),options);
        result=flood.callback()(input,{});QCOMPARE(result.diagnostics.last().toObject()["error_code"].toString(),QString("resource_limit"));
        a::CommandHooks cancel(config("PreToolUse","touch started; sleep 30"),options);CancellationToken token;
        auto future=std::async(std::launch::async,[&]{return cancel.callback()(input,token);});
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(work.filePath("started")),3000);token.cancel();
        QVERIFY(future.wait_for(std::chrono::seconds(2))==std::future_status::ready);
        try {future.get();QFAIL("Cancelled hook completed normally");}catch(const Error& e){QCOMPARE(e.code(),ErrorCode::Cancelled);}
    }
    void jsonPrecedesExitFallbackAndInvalidConfigurationIsRejected() {
        QTemporaryDir work;a::CommandHookOptions options;options.workingDirectory=work.path();
        a::HookInput input{a::HookKind::BeforeTool,"session","run",{"call","Write",{}},{},{}};
        a::CommandHooks valid(config("PreToolUse",output(pre("allow"))+"; exit 2"),options);
        const auto result=valid.callback()(input,{});QVERIFY(!result.block);QVERIFY(result.permission);QCOMPARE(result.permission->behavior,a::PermissionBehavior::Allow);
        auto wrong=pre("allow");wrong["hookSpecificOutput"]=QJsonObject{{"hookEventName","Stop"},{"permissionDecision","allow"}};
        a::CommandHooks invalid(config("PreToolUse",output(wrong)),options);
        const auto failed=invalid.callback()(input,{});QVERIFY(!failed.permission);QCOMPARE(failed.diagnostics.last().toObject()["outcome"].toString(),QString("non_blocking_error"));
        QVERIFY_THROWS_EXCEPTION(Error,a::CommandHooks(config("Notification","exit 0"),options));
        QVERIFY_THROWS_EXCEPTION(Error,a::CommandHooks(config("PreToolUse","exit 0","["),options));
        QVERIFY_THROWS_EXCEPTION(Error,a::CommandHooks(commands("PreToolUse",{QJsonObject{{"type","command"},{"command","exit 0"},{"async",true}}}),options));
    }
    void invalidUtf8IsReportedWithoutApplyingControl() {
        QTemporaryDir work;a::CommandHookOptions options;options.workingDirectory=work.path();
        for(const auto command:{"printf '\\377'; exit 2","printf '\\342'; exit 2"}) {
            a::CommandHooks hooks(config("PreToolUse",command),options);
            const auto result=hooks.callback()({a::HookKind::BeforeTool,"session","run",{"call","Write",{}},{},{}},{});
            QVERIFY(!result.block);
            QCOMPARE(result.diagnostics.last().toObject()["error_code"].toString(),QString("invalid_argument"));
        }
    }
    void conditionalAllowIsLocalToOneInvocationAndRespectsPlan() {
        QTemporaryDir work;a::CommandHookOptions options;options.workingDirectory=work.path();
        auto settings=commands("PreToolUse",{QJsonObject{{"type","command"},{"command",output(pre("allow"))},{"if","Write(allowed.txt)"}}});
        a::CommandHooks hooks(settings,options);auto registry=files(work.path());a::ToolContext context{"session","run",work.path()};
        a::ToolRunner runner(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::DontAsk),{{hooks.callback()}});
        auto allowed=runner.run({"allowed","Write",{{"path","allowed.txt"},{"content","OK"}}},context);
        QVERIFY2(!allowed.isError,qPrintable(allowed.text));
        QVERIFY(runner.run({"denied","Write",{{"path","denied.txt"},{"content","NO"}}},context).isError);
        QVERIFY(!QFileInfo::exists(work.filePath("denied.txt")));QVERIFY(context.allowedTools.isEmpty());
        a::ToolRunner plan(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Plan),{{hooks.callback()}});
        QVERIFY(plan.run({"planned","Write",{{"path","allowed.txt"},{"content","NO"}}},context).isError);
        QFile file(work.filePath("allowed.txt"));QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("OK"));
    }
    void stopCommandsReviseThenCancelWithTheirOwnReason() {
        class Model final:public a::Model {
        public:
            int calls=0;
            a::ModelReply generate(const a::ModelRequest&,const CancellationToken&,const TextCallback&) override {
                return {++calls==1?"first":"revised",{}};
            }
        };
        QTemporaryDir root;QDir(root.path()).mkdir("work");const auto workspace=root.filePath("work");
        a::CommandHookOptions options;options.workingDirectory=workspace;
        const auto block=output({{"decision","block"},{"reason","REVISE_FROM_HOOK"}});
        a::CommandHooks hooks(config("Stop","cat >> stop-input.jsonl; if [ ! -f revised ]; then touch revised; "+block+"; fi"),options);
        auto model=std::make_shared<Model>();a::EngineOptions engineOptions;engineOptions.sessionsDirectory=root.filePath("state");engineOptions.hooks={hooks.callback()};
        a::Engine engine(model,files(workspace),std::make_shared<a::RulePolicy>(),engineOptions);
        const auto session=engine.createSession("model://test",workspace);
        const auto result=engine.run({session.id,"answer"}).result.get();
        QCOMPARE(result.status,a::RunStatus::Completed);QCOMPARE(result.text,QString("revised"));QCOMPARE(model->calls,2);
        QFile log(root.filePath("work/stop-input.jsonl"));QVERIFY(log.open(QIODevice::ReadOnly));
        const auto first=QJsonDocument::fromJson(log.readLine()).object(),second=QJsonDocument::fromJson(log.readLine()).object();
        QVERIFY(!first["stop_hook_active"].toBool());QVERIFY(second["stop_hook_active"].toBool());
        QCOMPARE(first["last_assistant_message"].toString(),QString("first"));
        QCOMPARE(first["transcript_path"].toString(),engine.transcriptPath(session.id));QVERIFY(QFileInfo::exists(engine.transcriptPath(session.id)));
        a::CommandHooks cancellation(config("Stop",output({{"continue",false},{"stopReason","EXPLICIT_STOP"},{"reason","UNRELATED"}})),options);
        engineOptions.hooks={cancellation.callback()};engineOptions.sessionsDirectory=root.filePath("cancel-state");
        a::Engine cancelled(model,files(workspace),std::make_shared<a::RulePolicy>(),engineOptions);
        const auto cancelSession=cancelled.createSession("model://test",workspace);
        const auto stopped=cancelled.run({cancelSession.id,"stop"}).result.get();
        QCOMPARE(stopped.status,a::RunStatus::Cancelled);QVERIFY(QJsonDocument(a::toJson(stopped)).toJson().contains("EXPLICIT_STOP"));
    }
    void subagentCommandsHaveChildIdentityAndForwardDiagnostics() {
        class Model final:public a::Model {
        public:a::ModelReply generate(const a::ModelRequest&,const CancellationToken&,const TextCallback&) override {return {"CHILD_DONE",{}};}
        };
        QTemporaryDir root;const auto workspace=root.filePath("work");QVERIFY(QDir().mkpath(workspace));
        auto settings=config("SubagentStart","cat >> child.jsonl","general-purpose");
        auto entries=settings["hooks"].toObject();entries["SubagentStop"]=config("SubagentStop","cat >> child.jsonl","general-purpose")["hooks"].toObject()["SubagentStop"];
        settings["hooks"]=entries;a::CommandHookOptions options;options.workingDirectory=workspace;a::CommandHooks hooks(settings,options);
        a::EngineOptions engineOptions;engineOptions.sessionsDirectory=root.filePath("parents");engineOptions.hooks={hooks.callback()};
        a::SubagentOptions subOptions;subOptions.workingDirectory=workspace;subOptions.stateDirectory=root.filePath("children");
        a::Subagents subagents(std::make_shared<Model>(),files(workspace),std::make_shared<a::RulePolicy>(),engineOptions,subOptions);
        const auto parent=a::SessionStore(engineOptions.sessionsDirectory).create("model://test","",workspace);
        a::ToolContext context{parent.id,"parent-run",workspace};context.sessionSnapshot=std::make_shared<a::Session>(parent);
        QStringList diagnostics;context.progress=[&](const QJsonObject& value) {
            const auto event=value["event"].toObject();if(event["event"]=="hook")diagnostics.append(event["data"].toObject()["hook_event_name"].toString());
        };
        const auto result=subagents.run(context,{{"prompt","answer"}});QVERIFY2(!result.isError,qPrintable(result.text));
        QVERIFY(diagnostics.contains("SubagentStart"));QVERIFY(diagnostics.contains("SubagentStop"));
        QFile file(workspace+"/child.jsonl");QVERIFY(file.open(QIODevice::ReadOnly));
        const auto start=QJsonDocument::fromJson(file.readLine()).object(),stop=QJsonDocument::fromJson(file.readLine()).object();
        QCOMPARE(start["hook_event_name"].toString(),QString("SubagentStart"));QCOMPARE(stop["hook_event_name"].toString(),QString("SubagentStop"));
        QCOMPARE(start["session_id"],stop["session_id"]);QVERIFY(start["session_id"].toString()!=parent.id);
        QCOMPARE(start["parent_session_id"].toString(),parent.id);QCOMPARE(start["transcript_path"],stop["transcript_path"]);
        QVERIFY(QFileInfo::exists(start["transcript_path"].toString()));QCOMPARE(stop["last_assistant_message"].toString(),QString("CHILD_DONE"));
    }
};
QTEST_GUILESS_MAIN(CommandHooksTests)
#include "command_hooks_tests.moc"
