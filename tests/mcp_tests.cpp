#include <mcp/Client.h>
#include <agent/McpTools.h>
#include <agent/Engine.h>
#include <QtTest/QTest>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUrl>
#include <chrono>
#include <thread>

using namespace iiLocalLLM;
using namespace std::chrono_literals;
namespace m = iiLocalLLM::mcp;
namespace {
m::StdioOptions options(QString mode = "normal") {
    m::StdioOptions o; o.program = MCP_TEST_PYTHON; o.arguments = {"-B", MCP_TEST_PEER, mode};
    o.requestTimeoutMs = 2000; return o;
}
template<class F> void checkError(F f, ErrorCode code) {
    try { f(); QFAIL("Expected an iiLocalLLM error"); }
    catch (const Error& e) { QCOMPARE(int(e.code()), int(code)); }
}
}
class McpTests : public QObject {
    Q_OBJECT
private slots:
    void lifecycleAndDiscovery() {
        m::StdioClient client(options("split"));
        QVERIFY(client.isConnected()); QCOMPARE(client.protocolVersion(), "2025-11-25");
        QCOMPARE(client.serverInfo()["name"].toString(), "independent-peer");
        QCOMPARE(client.listTools().size(), 2);
        QCOMPARE(client.listResources().size(), 1); QCOMPARE(client.listResourceTemplates().size(), 1);
        QCOMPARE(client.readResource("test://value")["contents"].toArray()[0].toObject()["text"].toString(), "관측 값");
        client.subscribeResource("test://value"); client.unsubscribeResource("test://value");
        QCOMPARE(client.listPrompts().size(), 1);
        QCOMPARE(client.getPrompt("summarize", {{"topic", "한글"}})["messages"].toArray().size(), 1);
        const auto result = client.callTool("echo", {{"value", "다른 프로세스의 응답"}});
        QCOMPARE(result["content"].toArray().size(), 2);
        QCOMPARE(result["structuredContent"].toObject()["value"].toString(), "다른 프로세스의 응답");
        QVERIFY(client.stderrTail().contains("diagnostic"));
        client.request("test/notify");
        QCOMPARE(client.takeNotifications().size(), 1); QVERIFY(client.takeNotifications().isEmpty());
        client.close(); QVERIFY(!client.isConnected());
    }
    void negotiationAndCapabilities() {
        m::StdioClient older(options("older")); QCOMPARE(older.protocolVersion(), "2025-06-18");
        checkError([] { m::StdioClient invalid(options("bad-version")); }, ErrorCode::ProtocolError);
        m::StdioClient unavailable(options("no-capabilities"));
        checkError([&] { unavailable.listTools(); }, ErrorCode::ProtocolError);
        QVERIFY(unavailable.request("ping").isEmpty());
    }
    void concurrentOutOfOrderRequests() {
        m::StdioClient client(options());
        auto first = std::async(std::launch::async, [&] { return client.request("test/slow", {{"delay", .15}, {"value", "first"}}); });
        auto second = std::async(std::launch::async, [&] { return client.request("test/slow", {{"delay", .01}, {"value", "second"}}); });
        try {
            QCOMPARE(second.get()["value"].toString(), "second");
            QCOMPARE(first.get()["value"].toString(), "first");
        } catch (...) { qWarning().noquote() << client.stderrTail(); throw; }
        try { client.request("test/error"); QFAIL("Expected remote error"); }
        catch (const m::RpcError& e) { QCOMPARE(e.rpcCode(), -32602); QCOMPARE(e.data().toObject()["field"].toString(), "x"); }
    }
    void legacyBatchesGroupAsyncReverseResponses() {
        m::ClientOptions config; config.capabilities = {{"sampling", QJsonObject{}}};
        config.requestHandlers["sampling/createMessage"] = [](const auto&, const CancellationToken& token) {
            std::this_thread::sleep_for(30ms); token.throwIfCancelled();
            return QJsonObject{{"model", "fixture"}, {"role", "assistant"}, {"content", QJsonObject{{"type", "text"}, {"text", "sampled"}}}};
        };
        m::StdioClient client(options("legacy"), config);
        QCOMPARE(client.protocolVersion(), "2025-03-26"); QVERIFY(client.request("test/batch")["batch"].toBool());
        QTRY_COMPARE(client.request("test/state")["batchResponses"].toArray().size(), 1);
        const auto state = client.request("test/state");
        const auto replies = state["batchResponses"].toArray()[0].toArray(); QCOMPARE(replies.size(), 3);
        QSet<int> ids; for (const auto& v : replies) ids.insert(v.toObject()["id"].toInt());
        QCOMPARE(ids, (QSet<int>{901, 902, 903}));
        QCOMPARE(state["unexpectedResponses"].toInt(), 0); QCOMPARE(client.takeNotifications().size(), 1);
    }
    void cancellationAndTimeout() {
        m::StdioClient client(options()); CancellationToken token;
        std::promise<void> ready; auto notification = ready.get_future();
        auto cancelled = std::async(std::launch::async, [&] {
            checkError([&] { client.request("test/slow", {{"delay", .15}}, token,
                [&](const QJsonObject& p) { QCOMPARE(p["message"].toString(), "준비 완료"); ready.set_value(); }); }, ErrorCode::Cancelled);
        });
        QVERIFY(notification.wait_for(2s) == std::future_status::ready); token.cancel(); cancelled.get();
        try {
            client.request("test/slow", {{"delay", .15}, {"value", "private-argument"}}, {}, {}, 30);
            QFAIL("Expected request timeout");
        } catch (const Error& e) {
            QCOMPARE(e.code(), ErrorCode::Timeout);
            QVERIFY(QString::fromUtf8(e.what()).contains("test/slow"));
            QVERIFY(!QString::fromUtf8(e.what()).contains("private-argument"));
        }
        QCOMPARE(client.request("test/state")["cancelled"].toInt(), 2);
        std::this_thread::sleep_for(200ms); // The independent peer deliberately sends late replies.
        QVERIFY(client.request("ping").isEmpty()); QVERIFY(client.isConnected());
    }
    void rootsAndReverseRequests() {
        QTemporaryDir directory; m::ClientOptions config;
        config.roots = {QJsonObject{{"uri", QUrl::fromLocalFile(directory.path()).toString()}, {"name", "project"}}};
        config.capabilities = {{"elicitation", QJsonObject{{"form", QJsonObject{}}}}};
        config.requestHandlers["elicitation/create"] = [](const QJsonObject& p, const CancellationToken& token) {
            token.throwIfCancelled(); return QJsonObject{{"action", "accept"}, {"content", QJsonObject{{"answer", p["message"]}}}};
        };
        m::StdioClient client(options(), config);
        const auto roots = client.request("test/reverse", {{"method", "roots/list"}})["response"].toObject()["result"].toObject()["roots"].toArray();
        QCOMPARE(roots, config.roots);
        client.setRoots({});
        QCOMPARE(client.request("test/state")["rootsChanges"].toInt(), 1);
        const auto response = client.request("test/reverse", {{"method", "elicitation/create"}, {"params", QJsonObject{{"message", "host answer"}}}})["response"].toObject();
        QCOMPARE(response["result"].toObject()["content"].toObject()["answer"].toString(), "host answer");
        const auto unsupported = client.request("test/reverse", {{"method", "sampling/createMessage"}})["response"].toObject();
        QCOMPARE(unsupported["error"].toObject()["code"].toInt(), -32601);
        checkError([&] { client.setRoots({QJsonObject{{"uri", "https://example.com"}}}); }, ErrorCode::InvalidArgument);
        checkError([&] { client.setRoots({QJsonObject{{"uri", "file:///tmp/../private"}}}); }, ErrorCode::InvalidArgument);
    }
    void reverseRequestCancellationAndReentry() {
        m::StdioClient* connection = nullptr; m::ClientOptions config;
        config.capabilities = {{"elicitation", QJsonObject{{"form", QJsonObject{}}}}};
        std::promise<void> entered, stopped; auto waiting = entered.get_future(); auto done = stopped.get_future();
        config.requestHandlers["elicitation/create"] = [&](const QJsonObject&, const CancellationToken& token) {
            connection->request("ping"); // The I/O thread must stay free while a host callback runs.
            entered.set_value();
            while (!token.isCancelled()) std::this_thread::sleep_for(2ms);
            stopped.set_value();
            return QJsonObject{{"action", "cancel"}};
        };
        m::StdioClient client(options(), config); connection = &client;
        auto pending = std::async(std::launch::async, [&] { return client.request("test/reverse", {{"method", "elicitation/create"}}); });
        QVERIFY(waiting.wait_for(2s) == std::future_status::ready);
        client.request("test/cancel-host");
        QVERIFY(pending.get()["cancelled"].toBool()); QVERIFY(done.wait_for(2s) == std::future_status::ready);
        client.request("ping"); QCOMPARE(client.request("test/state")["unexpectedResponses"].toInt(), 0);
    }
    void progressConsumerFailureAndInputValidation() {
        m::StdioClient client(options());
        checkError([&] { client.request("ping", {{"_meta", QJsonValue::Null}}); }, ErrorCode::InvalidArgument);
        checkError([&] { client.request("test/slow", {}, {}, [](const QJsonObject&) { throw std::runtime_error("observer failed"); }); }, ErrorCode::ConsumerFailure);
        client.request("ping");
        QCOMPARE(client.request("test/state")["cancelled"].toInt(), 1);
        CancellationToken token; token.cancel();
        checkError([&] { client.request("test/slow", {}, token); }, ErrorCode::Cancelled);
        QVERIFY(client.isConnected());
    }
    void malformedPeer_data() {
        QTest::addColumn<QString>("kind");
        for (const auto& kind : {"json", "batch", "utf8", "oversize", "invalid-id"}) QTest::newRow(kind) << QString(kind);
    }
    void malformedPeer() {
        QFETCH(QString, kind); auto o = options(); o.maxMessageBytes = 4096;
        m::StdioClient client(o);
        try { client.request("test/malformed", {{"kind", kind}}); QFAIL("Invalid peer frame accepted"); }
        catch (const Error& e) { QVERIFY(e.code() == ErrorCode::ProtocolError || e.code() == ErrorCode::ResourceLimit); }
        QVERIFY(!client.isConnected());
    }
    void badListsAndExit() {
        for (const auto& mode : {"duplicates", "cursor-cycle"}) {
            m::StdioClient client(options(mode));
            checkError([&] { client.listTools(); }, ErrorCode::ProtocolError);
        }
        m::StdioClient client(options());
        checkError([&] { client.request("test/exit"); }, ErrorCode::RuntimeFailure);
        QVERIFY(!client.isConnected());
        checkError([&] { client.request("ping"); }, ErrorCode::ShuttingDown);
    }
    void aggregateListLimit() {
        auto o = options("large-list"); o.maxMessageBytes = 1024; o.maxQueuedBytes = 1024;
        m::StdioClient client(o);
        checkError([&] { client.listTools(); }, ErrorCode::ResourceLimit);
        QVERIFY(client.request("ping").isEmpty());
    }
    void boundedQueueAndClose() {
        auto o = options(); o.maxPendingRequests = 1; m::StdioClient client(o);
        std::promise<void> ready; auto started = ready.get_future();
        auto pending = std::async(std::launch::async, [&] {
            checkError([&] { client.request("test/slow", {{"delay", 5}}, {}, [&](const QJsonObject&) { ready.set_value(); }); }, ErrorCode::ShuttingDown);
        });
        QVERIFY(started.wait_for(2s) == std::future_status::ready);
        checkError([&] { client.request("ping"); }, ErrorCode::QueueFull);
        client.close(); pending.get(); QVERIFY(!client.isConnected());
    }
    void agentBridgePreservesResultsAndPolicy() {
        auto client = std::make_shared<m::StdioClient>(options());
        auto registry = std::make_shared<agent::ToolRegistry>();
        for (auto tool : agent::mcpTools(client, {"app", "com.iisacc.fixture"})) registry->add(std::move(tool));
        const auto definition = registry->get("mcp__app__echo").definition;
        QVERIFY(!definition.readOnly); QVERIFY(!definition.concurrencySafe);
        QVERIFY(definition.metadata["remote_definition"].toObject()["annotations"].toObject()["readOnlyHint"].toBool());
        QTemporaryDir directory; agent::ToolContext context{"session", "run", directory.path(), directory.path(), {}, {}};
        auto denied = agent::ToolRunner(registry, std::make_shared<agent::RulePolicy>(agent::PermissionMode::DontAsk));
        QVERIFY(denied.run({"c1", "mcp__app__echo", {{"value", "denied"}}}, context).isError);
        auto runner = agent::ToolRunner(registry, std::make_shared<agent::RulePolicy>(agent::PermissionMode::Bypass));
        auto output = runner.run({"c2", "mcp__app__echo", {{"value", "observed"}}}, context);
        QVERIFY(!output.isError); QCOMPARE(output.content.size(), 2); QCOMPARE(output.metadata["app"].toString(), "fixture");
        QCOMPARE(output.data["value"].toString(), "observed");
        QVERIFY(output.text.contains("\"value\":\"observed\""));
        QVERIFY(runner.run({"c3", "mcp__app__echo", {{"value", "bad-output"}}}, context).isError);
        QVERIFY(runner.run({"c4", "mcp__app__echo", {{"value", "tool-error"}}}, context).isError);
        struct Model final : agent::Model {
            agent::ModelReply generate(const agent::ModelRequest& request, const CancellationToken&, const std::function<bool(const QString&)>&) override {
                if (request.messages.back().role == agent::MessageRole::User)
                    return {{}, {{"remote-call", "mcp__app__echo", {{"value", request.messages.back().text}}}}, {}};
                const auto& result = request.messages.back();
                if (result.content.size() != 2 || result.metadata["app"] != "fixture") throw std::runtime_error("Lost MCP content");
                return {result.data["value"].toString(), {}, {}};
            }
        };
        agent::EngineOptions engineOptions; engineOptions.sessionsDirectory = directory.path() + "/sessions";
        agent::Engine engine(std::make_shared<Model>(), registry,
            std::make_shared<agent::RulePolicy>(agent::PermissionMode::Bypass), engineOptions);
        const auto session = engine.createSession("fixture", directory.path());
        const auto result = engine.run({session.id, "fresh value 83"}).result.get();
        QCOMPARE(int(result.status), int(agent::RunStatus::Completed)); QCOMPARE(result.text, "fresh value 83");
        const auto stored = engine.session(session.id).messages[2];
        QCOMPARE(stored.content.size(), 2); QCOMPARE(agent::messageFromJson(agent::toJson(stored)), stored);
    }
    void nativeAdapterDoesNotDropMedia() {
        QTemporaryDir directory; ServiceOptions options; options.modelsDirectory = directory.path();
        Service service(options); agent::ServiceModel model(service); agent::ModelRequest request;
        request.model = "not-loaded";
        agent::Message message; message.role = agent::MessageRole::Tool;
        message.content = {QJsonObject{{"type", "image"}, {"mimeType", "image/png"}, {"data", "AA=="}}};
        request.messages.append(message);
        checkError([&] { model.generate(request, {}, {}); }, ErrorCode::RuntimeUnavailable);
    }
};
QTEST_GUILESS_MAIN(McpTests)
#include "mcp_tests.moc"
