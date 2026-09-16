#include <QtTest/QtTest>
#include "agent/Engine.h"
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QDir>
#include <atomic>
#include <chrono>
#include <thread>
#include <condition_variable>
using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
using namespace std::chrono_literals;

static QJsonObject schema() {
    return {{"type", "object"}, {"properties", QJsonObject{{"value", QJsonObject{{"type", "integer"}, {"minimum", 1}}}}},
        {"required", QJsonArray{"value"}}, {"additionalProperties", false}};
}
static a::Tool echoTool(QString name = "echo") {
    a::Tool tool;
    tool.definition = {name, "Return the supplied integer", schema(), schema(), true, true};
    tool.execute = [](const QJsonObject& input, const a::ToolContext&) { return a::ToolResult{QString::number(input["value"].toInt()), input}; };
    return tool;
}
class ScriptModel : public a::Model {
public:
    QList<a::ModelReply> replies;
    QList<a::ModelRequest> requests;
    std::function<void(const a::ModelRequest&)> observe;
    a::ModelReply generate(const a::ModelRequest& request, const CancellationToken& token,
            const std::function<bool(const QString&)>& delta) override {
        token.throwIfCancelled();
        requests.append(request);
        if (observe) observe(request);
        if (replies.isEmpty()) throw Error(ErrorCode::RuntimeFailure, "Unexpected model call");
        auto reply = replies.takeFirst();
        if (delta && !delta(reply.text)) token.throwIfCancelled();
        return reply;
    }
};
#include "agent_tests.h"
void AgentTests::singleToolLimitIsPassedToModels(){
    for(const int limit:{1,8}){
        QTemporaryDir root;auto model=std::make_shared<ScriptModel>();auto registry=std::make_shared<a::ToolRegistry>();registry->add(echoTool());
        model->replies={{{},{{"first","echo",{{"value",1}}}}},{"done",{}}};
        a::EngineOptions options;options.sessionsDirectory=root.filePath("sessions");options.maxToolCallsPerTurn=limit;
        a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(),options);const auto session=engine.createSession("fixture",root.path());
        QCOMPARE(engine.run({session.id,"Observe and finish"}).result.get().status,a::RunStatus::Completed);
        QCOMPARE(model->requests.size(),2);for(const auto& request:model->requests)QCOMPARE(request.parallelToolCalls,limit>1);
    }
}
void AgentTests::completionToolStopsBeforeLaterTools() {
    QTemporaryDir root;auto model=std::make_shared<ScriptModel>();auto registry=std::make_shared<a::ToolRegistry>();
    QList<int> executed;auto regular=echoTool();regular.execute=[&](const QJsonObject& args,const auto&) {
        executed.append(args["value"].toInt());return a::ToolResult{QString::number(args["value"].toInt()),args,false,{},{{"completes_run",true}}};
    };registry->add(regular);
    auto terminal=echoTool("StructuredOutput");terminal.completesRun=true;registry->add(terminal);
    a::ToolRunner runner(registry,std::make_shared<a::RulePolicy>());
    QVERIFY(!runner.concurrencySafe({"done","StructuredOutput",{{"value",2}}}));
    model->replies={{{},{{"before","echo",{{"value",1}}},{"done","StructuredOutput",{{"value",2}}},{"after","echo",{{"value",3}}}}}};
    a::EngineOptions options;options.sessionsDirectory=root.filePath("sessions");options.skills.enabled=false;options.projectContext.enabled=false;
    a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(),options);auto session=engine.createSession("model://test",root.path());
    const auto result=engine.run({session.id,"Return the verified result"}).result.get();
    QCOMPARE(result.status,a::RunStatus::Completed);QCOMPARE(result.text,"2");QCOMPARE(model->requests.size(),1);QCOMPARE(executed,QList<int>{1});
    QVERIFY(a::pendingToolCalls(engine.session(session.id).messages).isEmpty());
    terminal.execute=[](const QJsonObject&,const auto&){return a::ToolResult{"INVALID",{},true};};registry->remove("StructuredOutput");registry->add(terminal);
    model->replies={{{},{{"invalid","StructuredOutput",{{"value",2}}},{"regular","echo",{{"value",4}}}}},{"NEXT_TURN",{}}};
    session=engine.createSession("model://test",root.path());
    QCOMPARE(engine.run({session.id,"Retry an invalid result"}).result.get().text,"NEXT_TURN");QCOMPARE(executed,QList<int>({1,4}));
}
void AgentTests::verifierPermissionModeDoesNotInheritBypass() {
    auto tool=echoTool();tool.definition.readOnly=false;tool.definition.editsFiles=true;
    a::ToolContext verifier;verifier.permissionMode=a::PermissionMode::DontAsk;
    for(const auto mode:{a::PermissionMode::AcceptEdits,a::PermissionMode::Bypass}) {
        a::RulePolicy policy(mode);QCOMPARE(policy.decide(tool.definition,{},{}).behavior,a::PermissionBehavior::Allow);
        QCOMPARE(policy.decide(tool.definition,{},verifier).behavior,a::PermissionBehavior::Deny);
        QCOMPARE(policy.describe(verifier)["mode"],"dontAsk");QVERIFY(policy.describe({})["mode"]!="dontAsk");
        a::RulePolicy granted(mode,{{"echo",a::PermissionBehavior::Allow}});
        QCOMPARE(granted.decide(tool.definition,{},verifier).behavior,a::PermissionBehavior::Allow);
        a::RulePolicy denied(mode,{{"echo",a::PermissionBehavior::Allow},{"echo",a::PermissionBehavior::Deny}});
        QCOMPARE(denied.decide(tool.definition,{},verifier).behavior,a::PermissionBehavior::Deny);
    }
}
void AgentTests::registrySnapshotDuringExecution() {
    auto registry = std::make_shared<a::ToolRegistry>(); registry->add(echoTool());
    a::ToolRunnerOptions runnerOptions;
    runnerOptions.hooks.append([&](const a::HookInput& input, const CancellationToken&) {
        if (input.kind == a::HookKind::BeforeTool) registry->remove("echo");
        return a::HookResult{};
    });
    a::ToolRunner runner(registry, std::make_shared<a::RulePolicy>(), runnerOptions);
    const auto result = runner.run({"bound-call", "echo", {{"value", 7}}}, {});
    QVERIFY(!result.isError); QCOMPARE(result.text, "7"); QVERIFY(registry->definitions().isEmpty());
    registry->add(echoTool());
    auto model = std::make_shared<ScriptModel>();
    model->replies = {{{}, {{"frozen-call", "echo", {{"value", 9}}}}}, {"done", {}}};
    model->observe = [&](const a::ModelRequest&) {
        if (model->requests.size() == 1) {
            registry->remove("echo"); auto replacement = echoTool(); replacement.definition.readOnly = false;
            replacement.execute = [](const auto&, const auto&) { return a::ToolResult{"wrong replacement", {}, true}; };
            registry->add(std::move(replacement));
        }
    };
    QTemporaryDir root;
    a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions");
    a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
    const auto session = engine.createSession("model://test", root.path());
    QCOMPARE(engine.run({session.id, "call the registered tool"}).result.get().status, a::RunStatus::Completed);
    QCOMPARE(engine.session(session.id).messages[2].text, "9");
    QVERIFY(!model->requests.last().tools.first().readOnly); // Registry changes become visible next model turn.
}
void AgentTests::schemaAndRegistry() {
        a::ToolRegistry registry;
        registry.add(echoTool());
        registry.validateInput("echo", {{"value", 2}});
        QVERIFY_THROWS_EXCEPTION(Error, registry.validateInput("echo", {{"value", "2"}}));
        QVERIFY_THROWS_EXCEPTION(Error, registry.validateInput("echo", {{"value", 2}, {"extra", true}}));
        QVERIFY_THROWS_EXCEPTION(Error, registry.validateOutput("echo", {{"value", 0}}));
        QVERIFY_THROWS_EXCEPTION(Error, registry.add(echoTool()));
        auto conditional = echoTool("conditional");
        conditional.definition.inputSchema = QJsonDocument::fromJson(R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","allOf":[{"properties":{"value":{"$ref":"#/$defs/positive"}}}],"$defs":{"positive":{"type":"integer","minimum":1}},"required":["value"],"unevaluatedProperties":false})").object();
        registry.add(conditional);
        registry.validateInput("conditional", {{"value", 1}});
        QVERIFY_THROWS_EXCEPTION(Error, registry.validateInput("conditional", {{"value", 1}, {"x", 0}}));
    }
void AgentTests::permissionsAndHookRevalidation() {
        auto registry = std::make_shared<a::ToolRegistry>();
        int executions = 0;
        auto tool = echoTool();
        tool.execute = [&](const auto& input, const auto&) { ++executions; return a::ToolResult{"ok", input}; };
        registry->add(tool);
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,
            QList<a::PermissionRule>{{"*", a::PermissionBehavior::Allow}, {"echo", a::PermissionBehavior::Deny}});
        a::ToolRunner denied(registry, policy);
        QVERIFY(denied.run({"a", "echo", {{"value", 1}}}, {}).isError);
        QCOMPARE(executions, 0);
        a::ToolRunnerOptions options;
        options.hooks.append([](const a::HookInput& input, const CancellationToken&) {
            a::HookResult r;
            if (input.kind == a::HookKind::BeforeTool) r.updatedArguments = QJsonObject{{"value", -1}};
            return r;
        });
        a::ToolRunner invalid(registry, std::make_shared<a::RulePolicy>(), options);
        QVERIFY(invalid.run({"b", "echo", {{"value", 1}}}, {}).isError);
        QCOMPARE(executions, 0);
        tool.definition.readOnly = false;
        a::RulePolicy dontAsk(a::PermissionMode::DontAsk);
        QVERIFY(dontAsk.decide(tool.definition, {}, {}).behavior == a::PermissionBehavior::Deny);
    }
void AgentTests::transcriptInvariantsAndRecovery() {
        QTemporaryDir dir;
        a::SessionStore store(dir.path());
        const auto session = store.create("model://test", "system", dir.path());
        {
            auto lease = store.acquire(session.id);
            lease->append({{}, a::MessageRole::User, "hello"});
            lease->append({{}, a::MessageRole::Assistant, {}, {{"call-a", "echo", {{"value", 1}}}}});
            QVERIFY_THROWS_EXCEPTION(Error, store.acquire(session.id));
            QVERIFY_THROWS_EXCEPTION(Error, lease->append({{}, a::MessageRole::Tool, "bad", {}, "other"}));
        }
        QFile file(dir.filePath(session.id + "/transcript.jsonl"));
        QVERIFY(file.open(QIODevice::Append));
        file.write("{\"incomplete\":");
        file.close();
        const auto loaded = store.load(session.id);
        QCOMPARE(loaded.messages.size(), 2);
        QCOMPARE(a::pendingToolCalls(loaded.messages).size(), 1);
        QVERIFY_THROWS_EXCEPTION(Error, store.load("../outside"));
    }
void AgentTests::toolLoopAndPersistentResume() {
        QTemporaryDir dir;
        auto registry = std::make_shared<a::ToolRegistry>(); registry->add(echoTool());
        auto model = std::make_shared<ScriptModel>();
        model->replies = {{"", {{"c1", "echo", {{"value", 7}}}}}, {"seven", {}}};
        a::EngineOptions options; options.sessionsDirectory = dir.path();
        auto engine = std::make_unique<a::Engine>(model, registry, std::make_shared<a::RulePolicy>(), options);
        auto session = engine->createSession("model://test", dir.path());
        QList<a::EventKind> events;
        auto handle = engine->run({session.id, "Return seven"}, [&](const auto& e) { events.append(e.kind); });
        const auto result = handle.result.get();
        QVERIFY2(result.status == a::RunStatus::Completed, qPrintable(result.errorMessage));
        QCOMPARE(result.text, QString("seven"));
        QCOMPARE(result.turns, 2);
        QCOMPARE(model->requests.size(), 2);
        QCOMPARE(model->requests[1].messages.back().toolCallId, QString("c1"));
        QVERIFY(events.contains(a::EventKind::ToolStarted));
        QVERIFY(events.back() == a::EventKind::Finished);
        engine.reset();
        a::SessionStore store(dir.path());
        const auto restored = store.load(session.id);
        QCOMPARE(restored.messages.size(), 4);
        QVERIFY(a::pendingToolCalls(restored.messages).isEmpty());
    }
void AgentTests::interruptedToolIsNotExecutedAgain() {
        QTemporaryDir dir;
        a::SessionStore store(dir.path());
        auto session = store.create("model://test", {}, dir.path());
        {
            auto lease = store.acquire(session.id);
            lease->append({{}, a::MessageRole::User, "previous action"});
            lease->append({{}, a::MessageRole::Assistant, {}, {{"interrupted", "echo", {{"value", 9}}}}});
        }
        auto registry = std::make_shared<a::ToolRegistry>(); registry->add(echoTool());
        auto model = std::make_shared<ScriptModel>(); model->replies = {{"resumed", {}}};
        a::EngineOptions options; options.sessionsDirectory = dir.path();
        a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
        const auto result = engine.run({session.id, "continue"}).result.get();
        QVERIFY(result.status == a::RunStatus::Completed);
        const auto history = engine.session(session.id).messages;
        QCOMPARE(history[2].toolCallId, QString("interrupted"));
        QVERIFY(history[2].isError);
        QVERIFY(a::pendingToolCalls(history).isEmpty());
    }
void AgentTests::schemaFailureBecomesToolResult() {
        QTemporaryDir dir;
        auto registry = std::make_shared<a::ToolRegistry>(); registry->add(echoTool());
        auto model = std::make_shared<ScriptModel>();
        model->replies = {{"", {{"bad", "echo", {{"value", "wrong"}}}}}, {"handled", {}}};
        a::EngineOptions options; options.sessionsDirectory = dir.path();
        a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
        auto session = engine.createSession("model://test", dir.path());
        QVERIFY(engine.run({session.id, "test"}).result.get().status == a::RunStatus::Completed);
        QVERIFY(model->requests[1].messages.back().isError);
    }
void AgentTests::cancellationAndConsumerFailure() {
        class WaitingModel : public a::Model {
        public:
            std::atomic_bool started = false;
            a::ModelReply generate(const a::ModelRequest&, const CancellationToken& c,
                    const std::function<bool(const QString&)>&) override {
                started = true;
                for (;;) { c.throwIfCancelled(); std::this_thread::sleep_for(1ms); }
            }
        };
        QTemporaryDir dir;
        auto model = std::make_shared<WaitingModel>();
        auto registry = std::make_shared<a::ToolRegistry>();
        a::EngineOptions options; options.sessionsDirectory = dir.path();
        a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
        auto session = engine.createSession("model://test", dir.path());
        auto handle = engine.run({session.id, "wait"});
        QTRY_VERIFY_WITH_TIMEOUT(model->started.load(), 3000);
        handle.cancel();
        QVERIFY(handle.result.wait_for(2s) == std::future_status::ready);
        QVERIFY(handle.result.get().status == a::RunStatus::Cancelled);
        auto failing = engine.run({session.id, "fail"}, [](const auto&) { throw std::runtime_error("consumer"); });
        QVERIFY(failing.result.get().errorCode == ErrorCode::ConsumerFailure);
    }
void AgentTests::workspaceReadEditAndStaleGuard() {
        QTemporaryDir dir;
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, dir.path());
        a::ToolRunner runner(registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass));
        a::ToolContext context; context.sessionId = "test"; context.workingDirectory = dir.path();
        context.artifactsDirectory = dir.filePath("artifacts");
        auto call = [&](QString name, QJsonObject args) { return runner.run({name, name, args}, context); };
        QVERIFY(!call("Write", {{"path", "a.txt"}, {"content", "alpha\nbeta\n"}}).isError);
        QVERIFY(!call("Read", {{"path", "a.txt"}}).isError);
        QFile file(dir.filePath("a.txt")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("external\n"); file.close();
        QVERIFY(call("Edit", {{"path", "a.txt"}, {"old_string", "alpha"}, {"new_string", "changed"}}).isError);
        QVERIFY(!call("Read", {{"path", "a.txt"}}).isError);
        QVERIFY(!call("Edit", {{"path", "a.txt"}, {"old_string", "external"}, {"new_string", "changed"}}).isError);
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("changed\n")); file.close();
        QVERIFY(call("Read", {{"path", "../outside"}}).isError);
        QVERIFY(!call("Grep", {{"pattern", "changed"}}).isError);
    }
void AgentTests::parallelToolsAndExclusiveBarrier() {
    QTemporaryDir dir;
    auto registry = std::make_shared<a::ToolRegistry>();
    std::mutex mutex; std::condition_variable changed; int active = 0, arrived = 0, peak = 0; bool exclusive = false;
    for (const auto* name : {"first", "second", "exclusive"}) {
        auto tool = echoTool(name);
        const bool isExclusive = QString(name) == "exclusive";
        tool.definition.concurrencySafe = !isExclusive;
        tool.execute = [&, isExclusive](const QJsonObject& input, const a::ToolContext& c) {
            std::unique_lock lock(mutex);
            if (isExclusive) exclusive = active == 0 && arrived == 2;
            ++active; peak = std::max(peak, active);
            if (!isExclusive) {
                ++arrived; changed.notify_all();
                changed.wait_for(lock, 2s, [&] { return arrived == 2 || c.cancellation.isCancelled(); });
            }
            --active;
            return a::ToolResult{"ok", input};
        };
        registry->add(std::move(tool));
    }
    auto model = std::make_shared<ScriptModel>();
    model->replies = {{"", {{"one", "first", {{"value", 1}}}, {"two", "second", {{"value", 2}}}, {"three", "exclusive", {{"value", 3}}}}}, {"done", {}}};
    a::EngineOptions options; options.sessionsDirectory = dir.path();
    a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
    auto session = engine.createSession("model://test", dir.path());
    QVERIFY(engine.run({session.id, "parallel"}).result.get().status == a::RunStatus::Completed);
    QCOMPARE(peak, 2); QVERIFY(exclusive);
    QVERIFY(a::pendingToolCalls(engine.session(session.id).messages).isEmpty());
}
void AgentTests::stopHookAndTurnLimit() {
    QTemporaryDir dir;
    auto registry = std::make_shared<a::ToolRegistry>(); registry->add(echoTool());
    auto model = std::make_shared<ScriptModel>(); model->replies = {{"first", {}}, {"revised", {}}};
    a::EngineOptions options; options.sessionsDirectory = dir.path(); int stops = 0;
    options.hooks.append([&](const a::HookInput& input, const CancellationToken&) {
        a::HookResult r;
        if (input.kind == a::HookKind::Stop && ++stops == 1) { r.block = true; r.feedback = "Revise the answer"; }
        return r;
    });
    a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
    auto session = engine.createSession("model://test", dir.path());
    const auto revised = engine.run({session.id, "answer"}).result.get();
    QVERIFY(revised.status == a::RunStatus::Completed); QCOMPARE(revised.text, QString("revised")); QCOMPARE(revised.turns, 2);
    model->replies = {{"", {{"last-call", "echo", {{"value", 5}}}}}};
    a::RunRequest request{session.id, "one turn"}; request.maxTurns = 1;
    const auto limited = engine.run(request).result.get();
    QVERIFY(limited.status == a::RunStatus::TurnLimit);
    QVERIFY(a::pendingToolCalls(engine.session(session.id).messages).isEmpty());
}
void AgentTests::shellTimeoutAndExitStatus() {
#ifdef Q_OS_UNIX
    QTemporaryDir dir;
    auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, dir.path());
    a::ToolRunner runner(registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass));
    a::ToolContext c; c.workingDirectory = dir.path(); c.sessionId = "shell-test";
    auto success = runner.run({"ok", "Bash", {{"command", "printf 'native-shell'"}}}, c);
    QVERIFY(!success.isError); QCOMPARE(success.text, QString("native-shell"));
    auto error = runner.run({"err", "Bash", {{"command", "printf 'failed' >&2; exit 7"}}}, c);
    QVERIFY(error.isError); QCOMPARE(error.data["exit_code"].toInt(), 7);
    auto timeout = runner.run({"timeout", "Bash", {{"command", "sleep 30"}, {"timeout_ms", 30}}}, c);
    QVERIFY(timeout.isError); QCOMPARE(timeout.data["error_code"].toString(), QString("timeout"));
#endif
}
void AgentTests::invalidResultsAndDuplicateCalls() {
    auto registry = std::make_shared<a::ToolRegistry>();
    auto tool = echoTool(); tool.execute = [](const auto&, const auto&) { return a::ToolResult{"invalid", {{"value", -5}}}; };
    registry->add(tool);
    a::ToolRunner runner(registry, std::make_shared<a::RulePolicy>());
    QVERIFY(runner.run({"bad-result", "echo", {{"value", 1}}}, {}).isError);
    const auto duplicate = a::toJson(a::ToolCall{"same", "echo", {}});
    QVERIFY_THROWS_EXCEPTION(Error, a::replyFromJson(QJsonObject{{"tool_calls", QJsonArray{duplicate, duplicate}}}));
    QVERIFY_THROWS_EXCEPTION(Error, a::replyFromJson({{"text", "ok"}, {"unknown", true}}));
}
QTEST_GUILESS_MAIN(AgentTests)
