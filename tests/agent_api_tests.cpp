#include <agent/Api.h>
#include <agent/ShellTasks.h>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>
#include <chrono>
#include <thread>
using namespace iiLocalLLM;
using namespace std::chrono_literals;
namespace a = iiLocalLLM::agent;
namespace {
const QString firstToken(48, 'a'), secondToken(48, 'b');
class Model final : public a::Model {
public:
    std::atomic_bool waiting = false, cancelled = false;
    bool budgets = false, waitForSummary = false;
    std::optional<ContextBudget> measure(const a::ModelRequest& request, const CancellationToken& token) override {
        token.throwIfCancelled(); if (!budgets) return std::nullopt;
        qint64 count = 100 + request.systemPrompt.size() + request.tools.size() * 40;
        for (const auto& m : request.messages) count += m.text.size() + 10;
        return ContextBudget{count, 16384};
    }
    a::ModelReply generate(const a::ModelRequest& request, const CancellationToken& token, const TextCallback& delta) override {
        if (request.messages.last().text == "wait" || (request.summarizing && waitForSummary)) {
            waiting = true;
            while (!token.isCancelled()) std::this_thread::sleep_for(1ms);
            cancelled = true; token.throwIfCancelled();
        }
        if (request.summarizing) return {"API summary preserves the earlier request and observed response.", {}, {100, 12, 0, 0}};
        QStringList values;
        for (const auto& m : request.messages) if (m.role == a::MessageRole::User) values.append(m.text);
        const auto text = values.join('|'); if (delta) delta(text);
        return {text, {}, {1, 1, 0, 0}};
    }
};
a::ApiOptions options(QTemporaryDir& root) {
    a::ApiOptions o; o.workingDirectory = root.filePath("workspace"); QDir().mkpath(o.workingDirectory);
    o.stateDirectory = root.filePath("private"); o.clientTokens = {{"society", firstToken}, {"dreamscapes", secondToken}};
    return o;
}
QJsonObject call(a::Api& api, QString method, QJsonObject p = {}, QString token = firstToken) {
    auto request = api.dispatch(method, p, token);
    if (request.result.wait_for(3s) != std::future_status::ready) throw std::runtime_error("API request did not finish");
    return request.result.get().toObject();
}
template<class F> void error(F fn, ErrorCode expected) {
    try { fn(); QFAIL("Expected API error"); }
    catch (const Error& e) { QCOMPARE(e.code(), expected); }
}
}
class AgentApiTests : public QObject {
    Q_OBJECT
private slots:
    void inputsReachAnActiveRunWithoutAnAvailableRunWorker() {
        QTemporaryDir root; auto o = options(root); o.maxConcurrentRequests = 1; o.maxQueuedRequests = 0;
        auto model = std::make_shared<Model>();
        a::Api api(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        auto run = api.dispatch("agent.run", {{"session_id", id}, {"prompt", "wait"}}, firstToken);
        QTRY_VERIFY_WITH_TIMEOUT(model->waiting.load(), 3000);
        error([&] { call(api, "agent.inputs.enqueue", {{"session_id", id}, {"text", "wrong app"}}, secondToken); }, ErrorCode::NotFound);
        const auto input = call(api, "agent.inputs.enqueue", {{"session_id", id}, {"text", "urgent direction"}, {"priority", "now"}});
        QVERIFY(!input["input"].toObject()["id"].toString().isEmpty());
        QVERIFY(run.result.wait_for(3s) == std::future_status::ready);
        const auto answer = run.result.get().toObject(); QCOMPARE(answer["status"], "completed");
        QVERIFY(answer["text"].toString().endsWith("urgent direction"));
        QCOMPARE(call(api, "agent.inputs.list", {{"session_id", id}})["count"].toInt(), 0);
        auto queued = call(api, "agent.inputs.enqueue", {{"session_id", id}, {"text", "keep across restart"}})["input"].toObject();
        api.close();
        a::Api restored(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        QCOMPARE(call(restored, "agent.inputs.list", {{"session_id", id}})["inputs"].toArray()[0].toObject()["id"], queued["id"]);
        const auto fork = call(restored, "agent.sessions.fork", {{"session_id", id}}).value("session_id");
        QCOMPARE(call(restored, "agent.inputs.list", {{"session_id", fork}})["count"].toInt(), 0);
        QCOMPARE(call(restored, "agent.inputs.run", {{"session_id", id}})["status"], "completed");
        error([&] { call(restored, "agent.inputs.remove", {{"session_id", id}, {"input_id", queued["id"]}}); }, ErrorCode::NotFound);
    }
    void shellOutputHonorsApiDeadlineWithoutStoppingCommand() {
#if !defined(Q_OS_UNIX) || defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
        QSKIP("Background shell execution requires a desktop POSIX host");
#endif
        QTemporaryDir root; auto o = options(root); o.requestTimeoutMs = 200;
        auto shells = std::make_shared<a::ShellTasks>(o.workingDirectory, root.filePath("shell-state"));
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, o.workingDirectory, shells);
        a::Api api(std::make_shared<Model>(), registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass), o);
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        const auto task = call(api, "agent.shell.start", {{"session_id", id}, {"command", "sleep 30"}})["result"].toObject().value("backgroundTaskId");
        error([&] { call(api, "agent.shell.output", {{"session_id", id}, {"task_id", task}, {"timeout", 1000}}); }, ErrorCode::Timeout);
        QCOMPARE(shells->output(id.toString(), task.toString(), false, 0, 0, 1024)["task"].toObject()["status"], "running");
        shells->stop(id.toString(), task.toString());
    }
    void backgroundShellsAreAuthenticatedAndControlledDuringRuns() {
#if !defined(Q_OS_UNIX) || defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
        QSKIP("Background shell execution requires a desktop POSIX host");
#endif
        QTemporaryDir root; auto o = options(root); auto model = std::make_shared<Model>();
        auto shells = std::make_shared<a::ShellTasks>(o.workingDirectory, root.filePath("shell-state"));
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, o.workingDirectory, shells);
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::DontAsk,
            QList<a::PermissionRule>{{"Bash", a::PermissionBehavior::Allow}});
        a::Api api(model, registry, policy, o);
        QVERIFY(call(api, "agent.info")["background_tasks_enabled"].toBool());
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        auto run = api.dispatch("agent.run", {{"session_id", id}, {"prompt", "wait"}}, firstToken);
        QTRY_VERIFY_WITH_TIMEOUT(model->waiting.load(), 3000);
        const auto started = call(api, "agent.shell.start", {{"session_id", id}, {"command", "printf api-background; sleep 30"}});
        QVERIFY(!started["is_error"].toBool()); const auto task = started["result"].toObject().value("backgroundTaskId");
        QVERIFY(!task.toString().isEmpty());
        error([&] { call(api, "agent.shell.output", {{"session_id", id}, {"task_id", task}}, secondToken); }, ErrorCode::NotFound);
        error([&] { call(api, "agent.shell.list", {{"session_id", id}}, "bad-token"); }, ErrorCode::Unauthorized);
        const auto another = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        QVERIFY(call(api, "agent.shell.stop", {{"session_id", another}, {"task_id", task}})["is_error"].toBool());
        auto waiting = api.dispatch("agent.shell.output", {{"session_id", id}, {"task_id", task}, {"timeout", 30000}}, firstToken);
        const auto stop = call(api, "agent.shell.stop", {{"session_id", id}, {"task_id", task}});
        QVERIFY(!stop["is_error"].toBool());
        QCOMPARE(waiting.result.get().toObject()["result"].toObject()["task"].toObject()["status"], "killed");
        QCOMPARE(call(api, "agent.shell.list", {{"session_id", id}})["result"].toObject()["tasks"].toArray().size(), 1);
        call(api, "agent.cancel", {{"request_id", run.requestId}}); QCOMPARE(run.result.get().toObject()["status"], "cancelled");
    }
    void taskStateIsAuthenticatedAndAccessibleDuringRuns() {
        QTemporaryDir root; auto o = options(root); o.engine.taskToolsEnabled = true;
        auto model = std::make_shared<Model>();
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Default,
            QList<a::PermissionRule>{{"TaskUpdate", a::PermissionBehavior::Deny}});
        a::Api api(model, std::make_shared<a::ToolRegistry>(), policy, o);
        QVERIFY(call(api, "agent.info")["task_tools_enabled"].toBool());
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        auto run = api.dispatch("agent.run", {{"session_id", id}, {"prompt", "wait"}}, firstToken);
        QTRY_VERIFY_WITH_TIMEOUT(model->waiting.load(), 3000);
        const QJsonObject input{{"session_id", id}, {"subject", "Verify"}, {"description", "Check the actual result"}};
        const auto created = call(api, "agent.tasks.create", input);
        QVERIFY(!created["is_error"].toBool()); QCOMPARE(created["result"].toObject()["task"].toObject()["id"], "1");
        error([&] { call(api, "agent.tasks.list", {{"session_id", id}}, secondToken); }, ErrorCode::NotFound);
        error([&] { call(api, "agent.tasks.create", input, "invalid"); }, ErrorCode::Unauthorized);
        auto changed = call(api, "agent.tasks.update", {{"session_id", id}, {"taskId", "1"}, {"status", "completed"}});
        QVERIFY(changed["is_error"].toBool());
        QVERIFY(call(api, "agent.tasks.create", {{"session_id", id}, {"subject", "no description"}})["is_error"].toBool());
        QCOMPARE(call(api, "agent.tasks.list", {{"session_id", id}})["result"].toObject()["tasks"].toArray().size(), 1);
        const QJsonArray todos{QJsonObject{{"content", "Test"}, {"activeForm", "Testing"}, {"status", "pending"}}};
        QVERIFY(!call(api, "agent.todos.write", {{"session_id", id}, {"todos", todos}})["is_error"].toBool());
        QCOMPARE(call(api, "agent.todos.get", {{"session_id", id}})["result"].toObject()["todos"].toArray(), todos);
        call(api, "agent.cancel", {{"request_id", run.requestId}});
        QCOMPARE(run.result.get().toObject()["status"], "cancelled");
        api.close();
        a::Api reopened(model, std::make_shared<a::ToolRegistry>(), policy, o);
        QCOMPARE(call(reopened, "agent.tasks.list", {{"session_id", id}})["result"].toObject()["tasks"].toArray().size(), 1);
    }
    void manualCompactionUsesAuthenticatedRunLifecycle() {
        QTemporaryDir root; auto o = options(root); auto model = std::make_shared<Model>(); model->budgets = true;
        a::Api api(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        call(api, "agent.run", {{"session_id", id}, {"prompt", QString(1000, 'a')}});
        call(api, "agent.run", {{"session_id", id}, {"prompt", QString(1000, 'b')}});
        call(api, "agent.run", {{"session_id", id}, {"prompt", "current input"}});
        const QJsonObject p{{"session_id", id}, {"instructions", "Keep pending work and source IDs"}};
        error([&] { call(api, "agent.sessions.compact", p, secondToken); }, ErrorCode::NotFound);
        model->waitForSummary = true;
        auto pending = api.dispatch("agent.sessions.compact", p, firstToken);
        QTRY_VERIFY_WITH_TIMEOUT(model->waiting.load(), 3000);
        QCOMPARE(call(api, "agent.status", {{"request_id", pending.requestId}}).value("state").toString(), "running");
        call(api, "agent.cancel", {{"request_id", pending.requestId}});
        QCOMPARE(pending.result.get().toObject().value("status").toString(), "cancelled");
        QCOMPARE(call(api, "agent.sessions.get", {{"session_id", id}}).value("compaction_count").toInt(-1), 0);
        model->waitForSummary = false;
        const auto result = call(api, "agent.sessions.compact", p);
        QCOMPARE(result.value("status").toString(), "completed"); QCOMPARE(result.value("turns").toInt(-1), 0);
        QCOMPARE(result.value("usage").toObject().value("compactions").toInt(), 1);
        const auto session = call(api, "agent.sessions.get", {{"session_id", id}});
        QCOMPARE(session.value("message_count").toInt(), 6); QCOMPARE(session.value("compaction_count").toInt(), 1);
        QVERIFY(!session.value("compaction").toObject().value("summary").toString().isEmpty());
        error([&] { call(api, "agent.sessions.compact", {{"session_id", id}, {"workspace", root.path()}}); }, ErrorCode::InvalidArgument);
    }
    void projectContextIsAuthenticatedAndWorkspaceBound() {
        QTemporaryDir root; auto o = options(root); auto model = std::make_shared<Model>();
        auto put = [](const QString& path, const QByteArray& text) { QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(text), text.size()); };
        put(QDir(o.workingDirectory).filePath("AGENTS.md"), "Root rule");
        put(QDir(o.workingDirectory).filePath("src/AGENTS.md"), "Nested rule");
        a::Api api(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        const QJsonObject scope{{"session_id", id}, {"context_paths", QJsonArray{"src/new.cpp"}}};
        QCOMPARE(call(api, "agent.context.get", scope).value("files").toArray().size(), 2);
        error([&] { call(api, "agent.context.get", scope, secondToken); }, ErrorCode::NotFound);
        error([&] { call(api, "agent.context.get", scope, "wrong"); }, ErrorCode::Unauthorized);
        error([&] { call(api, "agent.context.get", {{"session_id", id}, {"context_paths", QJsonArray{"../private"}}}); }, ErrorCode::InvalidArgument);
        error([&] { call(api, "agent.context.get", {{"session_id", id}, {"context_paths", "src/new.cpp"}}); }, ErrorCode::InvalidArgument);
        error([&] { call(api, "agent.context.get", {{"session_id", id}, {"context_paths", QJsonArray{1}}}); }, ErrorCode::InvalidArgument);
        auto run = scope; run["prompt"] = "inspect rules";
        QVERIFY(call(api, "agent.run", run).value("text").toString().contains("Nested rule"));
        QCOMPARE(call(api, "agent.context.get", {{"session_id", id}}).value("files").toArray().size(), 2);
        auto invalid = o; invalid.engine.projectContext.rootDirectory = root.path();
        error([&] { a::Api rejected(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), invalid); }, ErrorCode::InvalidArgument);
    }
    void authenticationIsolationAndRestart() {
        QTemporaryDir root; auto o = options(root); auto model = std::make_shared<Model>();
        const auto registry = std::make_shared<a::ToolRegistry>(); auto policy = std::make_shared<a::RulePolicy>();
        QString id;
        {
            a::Api api(model, registry, policy, o);
            error([&] { call(api, "agent.info", {}, "wrong"); }, ErrorCode::Unauthorized);
            QCOMPARE(call(api, "agent.info")["client_id"].toString(), "society");
            id = call(api, "agent.sessions.create", {{"model", "fixture"}, {"system", "private instruction"}})["session_id"].toString();
            QVERIFY(!id.isEmpty());
            QCOMPARE(call(api, "agent.run", {{"session_id", id}, {"prompt", "first"}})["text"].toString(), "first");
            error([&] { call(api, "agent.sessions.get", {{"session_id", id}}, secondToken); }, ErrorCode::NotFound);
            QCOMPARE(call(api, "agent.sessions.list", {}, secondToken)["sessions"].toArray().size(), 0);
            error([&] { call(api, "agent.sessions.create", {{"model", "fixture"}, {"workspace", root.path()}}); }, ErrorCode::InvalidArgument);
        }
        a::Api restored(model, registry, policy, o);
        QCOMPARE(call(restored, "agent.sessions.list")["sessions"].toArray().size(), 1);
        QCOMPARE(call(restored, "agent.run", {{"session_id", id}, {"prompt", "second"}})["text"].toString(), "first|second");
    }
    void cancellationAndAdmission() {
        QTemporaryDir root; auto o = options(root); o.maxConcurrentRequests = 1; o.maxQueuedRequests = 0;
        auto model = std::make_shared<Model>();
        a::Api api(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        auto pending = api.dispatch("agent.run", {{"session_id", id}, {"prompt", "wait"}}, firstToken);
        QTRY_VERIFY(model->waiting.load());
        error([&] { call(api, "agent.info"); }, ErrorCode::QueueFull);
        error([&] { call(api, "agent.cancel", {{"request_id", pending.requestId}}, secondToken); }, ErrorCode::NotFound);
        QVERIFY(call(api, "agent.cancel", {{"request_id", pending.requestId}})["cancel_requested"].toBool());
        QVERIFY(pending.result.wait_for(3s) == std::future_status::ready);
        QVERIFY(model->cancelled.load());
        QCOMPARE(pending.result.get().toObject()["status"].toString(), "cancelled");
    }
    void forkPreservesCompleteBoundary() {
        QTemporaryDir root; a::SessionStore store(root.path());
        const auto original = store.create("fixture", "system", root.path());
        {
            auto lease = store.acquire(original.id);
            lease->append({"u", a::MessageRole::User, "read"});
            lease->append({"a", a::MessageRole::Assistant, {}, {{"call", "Read", {{"path", "value"}}}}});
            lease->append({"t", a::MessageRole::Tool, "observed", {}, "call"});
            lease->append({"done", a::MessageRole::Assistant, "answer"});
            error([&] { store.fork(original.id); }, ErrorCode::ModelInUse);
        }
        error([&] { store.fork(original.id, "a"); }, ErrorCode::InvalidArgument);
        error([&] { store.fork(original.id, "missing"); }, ErrorCode::NotFound);
        const auto fork = store.fork(original.id, "t");
        QVERIFY(fork.id != original.id); QCOMPARE(fork.messages.size(), 3);
        QVERIFY(a::pendingToolCalls(fork.messages).isEmpty()); QCOMPARE(store.list().size(), 2);
        QCOMPARE(store.load(original.id).messages.size(), 4); QCOMPARE(store.load(fork.id).messages, fork.messages);
        auto lease = store.acquire(fork.id); lease->append({"next", a::MessageRole::User, "continue"});
        QCOMPARE(store.load(original.id).messages.size(), 4);
    }
    void limitsQueueAndPrivateState() {
        QTemporaryDir root; auto o = options(root); auto model = std::make_shared<Model>();
        auto registry = std::make_shared<a::ToolRegistry>(); auto policy = std::make_shared<a::RulePolicy>();
        auto bad = o; bad.workingDirectory = QDir::rootPath();
        error([&] { a::Api api(model, registry, policy, bad); }, ErrorCode::InvalidArgument);
        bad = o; bad.stateDirectory = QDir(o.workingDirectory).filePath("state");
        error([&] { a::Api api(model, registry, policy, bad); }, ErrorCode::InvalidArgument);
        o.maxConcurrentRequests = 1; o.maxQueuedRequests = 1; o.maxSessionsPerClient = 2; o.requestTimeoutMs = 150;
        a::Api api(model, registry, policy, o);
        error([&] { a::Api duplicate(model, registry, policy, o); }, ErrorCode::AlreadyExists);
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        const auto fork = call(api, "agent.sessions.fork", {{"session_id", id}}).value("session_id"); QVERIFY(id != fork);
        error([&] { call(api, "agent.sessions.create", {{"model", "fixture"}}); }, ErrorCode::ResourceLimit);
        const auto page = call(api, "agent.sessions.list", {{"limit", 1}}); QCOMPARE(page["sessions"].toArray().size(), 1);
        const auto next = call(api, "agent.sessions.list", {{"cursor", page.value("next_cursor")}, {"limit", 1}});
        QCOMPARE(next["sessions"].toArray().size(), 1); QVERIFY(!next.contains("next_cursor"));
        auto active = api.dispatch("agent.run", {{"session_id", id}, {"prompt", "wait"}}, firstToken); QTRY_VERIFY(model->waiting.load());
        auto queued = api.dispatch("agent.sessions.create", {{"model", "fixture"}}, secondToken); queued.cancel();
        QCOMPARE(call(api, "agent.status", {{"request_id", active.requestId}})["state"].toString(), "running");
        error([&] { (void)active.result.get(); }, ErrorCode::Timeout);
        error([&] { (void)queued.result.get(); }, ErrorCode::Cancelled);
        QCOMPARE(call(api, "agent.sessions.list", {}, secondToken)["sessions"].toArray().size(), 0);
        api.close(); error([&] { call(api, "agent.info"); }, ErrorCode::ShuttingDown);
    }
    void forkRefusesArtifactsAndOversizedHeaderIsAtomic() {
        QTemporaryDir root; a::SessionStore store(root.path(), 1024);
        error([&] { store.create("fixture", QString(2048, 'x'), root.path()); }, ErrorCode::ResourceLimit);
        QVERIFY(store.list().isEmpty());
        QCOMPARE(QDir(root.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        const auto source = store.create("fixture", {}, root.path());
        { auto lease = store.acquire(source.id); QDir().mkpath(lease->artifactsDirectory());
            QFile file(QDir(lease->artifactsDirectory()).filePath("result.txt")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("observation"); }
        error([&] { store.fork(source.id); }, ErrorCode::RuntimeUnavailable);
        QCOMPARE(store.list().size(), 1);
    }
};
QTEST_GUILESS_MAIN(AgentApiTests)
#include "agent_api_tests.moc"
