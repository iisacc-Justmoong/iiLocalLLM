#include "agent/Engine.h"
#include <QtTest/QtTest>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <atomic>
#include <chrono>
#include <thread>
using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
void populate(a::SessionLease& lease) {
    lease.append({"u1", a::MessageRole::User, "Preserve the release requirements"});
    lease.append({"a1", a::MessageRole::Assistant, {}, {{"call1", "Read", {{"path", "a.txt"}}}}});
    lease.append({"t1", a::MessageRole::Tool, QString(3000, 'x'), {}, "call1", false, {{"actual", true}}});
    lease.append({"a2", a::MessageRole::Assistant, "Observed the requirements"});
    lease.append({"u2", a::MessageRole::User, "Now implement them exactly"});
}
a::Compaction checkpoint(const a::Session& s, bool summary = false) {
    a::Compaction c; c.id = uuid(); c.atMessageId = s.messages.last().id;
    if (!s.compactions.isEmpty()) c.previousId = s.compactions.last().id;
    if (summary) { c.throughMessageId = "t1"; c.summary = "Release requirements were read in a.txt (t1)."; }
    else c.clearedToolMessageIds = {"t1"};
    c.inputTokensBefore = 4000; c.inputTokensAfter = 400;
    return c;
}
}
class BudgetModel : public a::Model {
public:
    int window = 4096;
    bool waitOnSummary = false, invalidSummary = false, recall = false, oversizedSummary = false;
    std::atomic_bool summaryStarted = false;
    QList<a::ModelRequest> requests;
    std::optional<ContextBudget> measure(const a::ModelRequest& request, const CancellationToken& token) override {
        token.throwIfCancelled(); qint64 count = 100 + request.systemPrompt.size() + request.tools.size() * 40;
        for (const auto& m : request.messages) count += m.text.size() + 10;
        return ContextBudget{count, window};
    }
    a::ModelReply generate(const a::ModelRequest& request, const CancellationToken& token, const TextCallback&) override {
        const auto count = measure(request, token).value();
        if (count.inputTokens + request.generation.maxTokens > count.contextTokens) throw Error(ErrorCode::ContextOverflow, "uncompacted input");
        requests.append(request);
        if (request.summarizing) {
            summaryStarted = true;
            while (waitOnSummary) { token.throwIfCancelled(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            if (oversizedSummary) return {QString(window, 'z'), {}, {100, window, 0, 0}};
            if (invalidSummary) return {{}, {{"unexpected", "Write", {}}}};
            return {"Preserve the release requirements, observed results and unfinished work. Source: u1.", {}, {int(count.inputTokens), 20, 0, 0}};
        }
        if (recall && request.messages.last().role != a::MessageRole::Tool)
            return {{}, {{"recall1", "iiLocalLLM.session.read", {{"message_id", "t1"}, {"offset", 200}, {"limit", 128}}}}};
        return {"continued", {}};
    }
};
class CompactionTests : public QObject {
    Q_OBJECT
private slots:
    void automaticMicroCompactionAndRecall() {
        QTemporaryDir root; a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions");
        auto model = std::make_shared<BudgetModel>(); model->window = 2048; model->recall = true;
        a::Engine engine(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), options);
        auto session = engine.createSession("model://test", root.path());
        { a::SessionStore store(options.sessionsDirectory); auto lease = store.acquire(session.id); populate(*lease); }
        QList<a::EventKind> events;
        const auto result = engine.run({session.id, "continue"}, [&](const a::Event& e) { events.append(e.kind); }).result.get();
        QVERIFY2(result.status == a::RunStatus::Completed, qPrintable(result.errorMessage));
        QCOMPARE(result.usage.compactions, 1); QCOMPARE(result.usage.summaryGeneratedTokens, 0);
        QVERIFY(events.contains(a::EventKind::Compacted));
        session = engine.session(session.id); QCOMPARE(session.messages[2].text.size(), 3000);
        QCOMPARE(session.compactions.size(), 1); QVERIFY(session.compactions.first().summary.isEmpty());
        QVERIFY(model->requests.last().messages.last().text.contains(QString(100, 'x')));
        QVERIFY(a::pendingToolCalls(a::modelMessages(session)).isEmpty());
    }
    void rollingSummaryAndExactLatestInput() {
        QTemporaryDir root; a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions");
        auto model = std::make_shared<BudgetModel>();
        a::Engine engine(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), options);
        auto session = engine.createSession("model://test", root.path());
        { a::SessionStore store(options.sessionsDirectory); auto lease = store.acquire(session.id);
          lease->append({"u1", a::MessageRole::User, "Exact current requirement"});
          for (int i = 0; i < 6; ++i) lease->append({"a" + QString::number(i), a::MessageRole::Assistant, QString(1200, QChar(char('a' + i)))}); }
        const auto original = engine.session(session.id).messages;
        const auto result = engine.compact({session.id}).result.get();
        QVERIFY2(result.status == a::RunStatus::Completed, qPrintable(result.errorMessage));
        QCOMPARE(result.turns, 0); QCOMPARE(result.usage.compactions, 1);
        QVERIFY(result.usage.summaryGeneratedTokens >= 40); // Multiple bounded chunks, not silent head truncation.
        session = engine.session(session.id); QCOMPARE(session.messages, original);
        const auto visible = a::modelMessages(session); QCOMPARE(visible[1].text, "Exact current requirement");
        QVERIFY(visible.size() < original.size()); QCOMPARE(session.compactions.last().retainedUserMessageId, "u1");
        for (const auto& r : model->requests) {
            QVERIFY(r.summarizing); QVERIFY(r.tools.isEmpty());
            QVERIFY(r.messages.last().text.contains("Exact current requirement"));
        }
        const auto summaryCalls = model->requests.size();
        const auto repeated = engine.compact({session.id}).result.get();
        QCOMPARE(repeated.errorCode, ErrorCode::ContextOverflow);
        QCOMPARE(model->requests.size(), summaryCalls); // No raw prefix progress: do not spend another summary call.
        QCOMPARE(engine.session(session.id).compactions.size(), 1);
        const auto continued = engine.run({session.id, "Resume from the summary"}).result.get();
        QCOMPARE(continued.status, a::RunStatus::Completed); QCOMPARE(model->requests.last().messages.last().text, "Resume from the summary");
    }
    void summaryCancellationAndInvalidOutputDoNotCommit() {
        QTemporaryDir root; a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions");
        auto model = std::make_shared<BudgetModel>(); model->waitOnSummary = true;
        a::Engine engine(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), options);
        const auto session = engine.createSession("model://test", root.path());
        { a::SessionStore store(options.sessionsDirectory); auto lease = store.acquire(session.id); populate(*lease); }
        auto handle = engine.compact({session.id});
        QTRY_VERIFY_WITH_TIMEOUT(model->summaryStarted.load(), 3000);
        QCOMPARE(engine.compact({session.id}).result.get().errorCode, ErrorCode::ModelInUse);
        handle.cancel(); QCOMPARE(handle.result.get().status, a::RunStatus::Cancelled);
        QVERIFY(engine.session(session.id).compactions.isEmpty()); QCOMPARE(engine.session(session.id).messages.size(), 5);
        model->waitOnSummary = false; model->invalidSummary = true;
        QCOMPARE(engine.compact({session.id}).result.get().errorCode, ErrorCode::ProtocolError);
        QVERIFY(engine.session(session.id).compactions.isEmpty());
    }
    void compactedReadsCannotAuthorizeEdits() {
        QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, root.path());
        a::ToolRunner runner(registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass));
        a::ToolContext context; context.sessionId = "read-session"; context.workingDirectory = root.path(); context.artifactsDirectory = root.filePath("artifacts");
        QVERIFY(!runner.run({"w", "Write", {{"path", "file.txt"}, {"content", "before"}}}, context).isError);
        QVERIFY(!runner.run({"r", "Read", {{"path", "file.txt"}}}, context).isError);
        context.contextRevision = 1;
        QVERIFY(runner.run({"w2", "Write", {{"path", "file.txt"}, {"content", "after"}}}, context).isError);
        QVERIFY(!runner.run({"r2", "Read", {{"path", "file.txt"}}}, context).isError);
        QVERIFY(!runner.run({"w3", "Write", {{"path", "file.txt"}, {"content", "after"}}}, context).isError);
    }
    void automaticSummaryPreservesCurrentPrompt() {
        QTemporaryDir root; a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions");
        auto model = std::make_shared<BudgetModel>();
        a::Engine engine(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), options);
        const auto session = engine.createSession("model://test", root.path());
        { a::SessionStore store(options.sessionsDirectory); auto lease = store.acquire(session.id);
          lease->append({"u1", a::MessageRole::User, "Earlier requirement"});
          for (int i = 0; i < 6; ++i) lease->append({"a" + QString::number(i), a::MessageRole::Assistant, QString(1200, 'x')}); }
        const auto result = engine.run({session.id, "Exact current requirement"}).result.get();
        QVERIFY2(result.status == a::RunStatus::Completed, qPrintable(result.errorMessage));
        QCOMPARE(result.usage.compactions, 1); QVERIFY(result.usage.summaryGeneratedTokens > 0);
        QCOMPARE(model->requests.last().messages.last().text, "Exact current requirement");
        QCOMPARE(engine.session(session.id).messages.size(), 9);
    }
    void boundedFailuresHooksAndDisabledAutomatic() {
        QTemporaryDir root; a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions");
        bool reject = true; int beforeHooks = 0, afterHooks = 0;
        options.hooks.append([&](const a::HookInput& input, const CancellationToken&) {
            a::HookResult result;
            if (input.kind == a::HookKind::BeforeCompact) { ++beforeHooks; result.feedback = "Keep exact source identifiers"; }
            if (input.kind == a::HookKind::AfterCompact) { ++afterHooks; result.block = reject; }
            return result;
        });
        auto model = std::make_shared<BudgetModel>();
        a::Engine engine(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), options);
        const auto session = engine.createSession("model://test", root.path());
        { a::SessionStore store(options.sessionsDirectory); auto lease = store.acquire(session.id); populate(*lease); }
        QCOMPARE(engine.compact({session.id}).result.get().errorCode, ErrorCode::InvalidArgument);
        QCOMPARE(beforeHooks, 1); QCOMPARE(afterHooks, 1); QVERIFY(engine.session(session.id).compactions.isEmpty());
        reject = false; model->oversizedSummary = true;
        QCOMPARE(engine.compact({session.id}).result.get().errorCode, ErrorCode::ContextOverflow);
        QVERIFY(engine.session(session.id).compactions.isEmpty());
        model->oversizedSummary = false;
        auto config = options; config.compaction.automatic = false;
        a::Engine disabled(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), config);
        model->window = 2048;
        QCOMPARE(disabled.run({session.id, "continue"}).result.get().errorCode, ErrorCode::ContextOverflow);
        QVERIFY(engine.session(session.id).compactions.isEmpty());
        config.compaction.triggerFraction = std::numeric_limits<double>::quiet_NaN();
        QVERIFY_THROWS_EXCEPTION(Error, a::Engine(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), config));
    }
    void oldAdaptersAndOversizedCurrentInputFailExplicitly() {
        class Unmeasured : public a::Model {
        public: a::ModelReply generate(const a::ModelRequest&, const CancellationToken&, const TextCallback&) override { return {"answer", {}}; }
        };
        QTemporaryDir root; a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions");
        a::Engine engine(std::make_shared<Unmeasured>(), std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), options);
        const auto session = engine.createSession("model://test", root.path());
        QCOMPARE(engine.run({session.id, "hello"}).result.get().status, a::RunStatus::Completed);
        QCOMPARE(engine.compact({session.id}).result.get().errorCode, ErrorCode::RuntimeUnavailable);
        auto model = std::make_shared<BudgetModel>();
        a::Engine measured(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), options);
        QCOMPARE(measured.run({session.id, QString(5000, 'x')}).result.get().errorCode, ErrorCode::ContextOverflow);
        const auto restored = engine.session(session.id);
        QCOMPARE(restored.messages.last().text.size(), 5000); QVERIFY(restored.compactions.isEmpty());
    }
    void durableViewRetainsOriginalAndToolPairs() {
        QTemporaryDir root; a::SessionStore store(root.path()); auto s = store.create("model://test", {}, root.path());
        {
            auto lease = store.acquire(s.id); populate(*lease); const auto original = lease->session().messages;
            lease->compact(checkpoint(lease->session()));
            QCOMPARE(lease->session().messages, original);
            const auto visible = a::modelMessages(lease->session());
            QCOMPARE(visible.size(), original.size()); QCOMPARE(visible[2].toolCallId, "call1");
            QVERIFY(visible[2].text.contains("t1")); QVERIFY(visible[2].text.size() < 500);
            QVERIFY(visible[2].data.isEmpty()); QVERIFY(a::pendingToolCalls(visible).isEmpty());
        }
        s = store.load(s.id); QCOMPARE(s.compactions.size(), 1); QCOMPARE(s.messages[2].text.size(), 3000);
        const auto copy = store.fork(s.id); QCOMPARE(a::modelMessages(copy), a::modelMessages(s));
        const auto earlier = store.fork(s.id, "a2"); QVERIFY(earlier.compactions.isEmpty());
        QVERIFY_THROWS_EXCEPTION(Error, store.fork(s.id, "a1"));
    }
    void summaryBoundaryAndCheckpointValidation() {
        QTemporaryDir root; a::SessionStore store(root.path()); auto s = store.create("model://test", {}, root.path());
        auto lease = store.acquire(s.id); populate(*lease);
        auto c = checkpoint(lease->session(), true); c.throughMessageId = "a1";
        QVERIFY_THROWS_EXCEPTION(Error, lease->compact(c)); // Cannot split call1 from its result.
        c.throughMessageId = "t1"; c.atMessageId = "unknown";
        QVERIFY_THROWS_EXCEPTION(Error, lease->compact(c));
        c.atMessageId = "u2"; lease->compact(c);
        const auto visible = a::modelMessages(lease->session());
        QCOMPARE(visible.size(), 3); QVERIFY(visible.first().text.contains(c.summary));
        QCOMPARE(visible.last().text, "Now implement them exactly");
        QVERIFY(a::pendingToolCalls(visible).isEmpty());
        QVERIFY_THROWS_EXCEPTION(Error, lease->compact(c)); // Duplicate / stale previous checkpoint.
        auto bad = checkpoint(lease->session()); bad.clearedToolMessageIds = {"u2"};
        QVERIFY_THROWS_EXCEPTION(Error, lease->compact(bad));
    }
    void versionOneMigrationAndTornCheckpoint() {
        QTemporaryDir root; a::SessionStore store(root.path()); auto s = store.create("model://test", {}, root.path());
        { auto lease = store.acquire(s.id); populate(*lease); }
        QFile file(root.filePath(s.id + "/transcript.jsonl")); QVERIFY(file.open(QIODevice::ReadOnly));
        auto bytes = file.readAll(); file.close(); bytes.replace("\"version\":2", "\"version\":1");
        QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); file.close();
        { auto lease = store.acquire(s.id); lease->compact(checkpoint(lease->session(), true)); }
        QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(file.readLine().contains("\"version\":2")); file.close();
        QVERIFY(file.open(QIODevice::Append)); file.write("{\"type\":\"compaction\""); file.close();
        s = store.load(s.id); QCOMPARE(s.compactions.size(), 1); QCOMPARE(s.messages.size(), 5);
        QVERIFY(file.open(QIODevice::Append)); file.write("{\"type\":\"compaction\",\"checkpoint\":{}}\n"); file.close();
        QVERIFY_THROWS_EXCEPTION(Error, store.load(s.id)); // Complete corrupt records are never silently discarded.
    }
};
QTEST_GUILESS_MAIN(CompactionTests)
#include "compaction_tests.moc"
