#include <agent/Api.h>
#include <QtCore/QDir>
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
    a::ModelReply generate(const a::ModelRequest& request, const CancellationToken& token, const TextCallback& delta) override {
        if (request.messages.last().text == "wait") {
            waiting = true;
            while (!token.isCancelled()) std::this_thread::sleep_for(1ms);
            cancelled = true; token.throwIfCancelled();
        }
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
