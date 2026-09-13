#include <mcp/Server.h>
#include <agent/McpServer.h>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtTest/QTest>
#include <chrono>
#include <thread>
using namespace iiLocalLLM;
using namespace std::chrono_literals;
namespace m = iiLocalLLM::mcp;
namespace a = iiLocalLLM::agent;
namespace {
QJsonObject request(int id, QString method, QJsonObject params = {}) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}
QJsonObject tool(QString name) { return {{"name", name}, {"inputSchema", QJsonObject{{"type", "object"}}}}; }
m::ServerOptions options() {
    m::ServerOptions o;
    o.lists["tools/list"] = [](const auto&) { return QJsonArray{tool("echo")}; };
    o.handlers["tools/call"] = [](const QJsonObject& p, const m::ServerRequestContext& c) {
        c.cancellation.throwIfCancelled();
        c.progress({{"progress", 1}, {"total", 2}});
        c.progress({{"progress", 2}, {"total", 2}});
        return QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", p["arguments"].toObject()["text"].toString()}}}},
            {"structuredContent", p["arguments"].toObject()}, {"_meta", QJsonObject{{"app", "fixture"}}}};
    };
    o.requestTimeoutMs = 2000;
    return o;
}
QJsonObject next(m::ServerSession& s) {
    auto messages = s.takeMessages(2000);
    if (messages.size() != 1) throw std::runtime_error("Expected exactly one response");
    return messages[0].toObject();
}
void initialize(m::ServerSession& s, QJsonObject caps = {}, QString version = "2025-11-25") {
    s.receive(request(1, "initialize", {{"protocolVersion", version}, {"capabilities", caps},
        {"clientInfo", QJsonObject{{"name", "test-client"}, {"version", "1"}}}}));
    const auto result = next(s)["result"].toObject();
    if (result["protocolVersion"] != version) throw std::runtime_error("Handshake failed");
    s.receive({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
}
QJsonObject call(m::ServerSession& s, int id, QString name, QJsonObject args = {}) {
    s.receive(request(id, "tools/call", {{"name", name}, {"arguments", args}}));
    return next(s)["result"].toObject();
}
class HistoryModel final : public a::Model {
public:
    std::optional<ContextBudget> measure(const a::ModelRequest& r, const CancellationToken&) override {
        qint64 count = 100 + r.systemPrompt.size() + r.tools.size() * 40;
        for (const auto& message : r.messages) count += message.text.size() + 10;
        return ContextBudget{count, 16384};
    }
    a::ModelReply generate(const a::ModelRequest& r, const CancellationToken& token, const TextCallback&) override {
        token.throwIfCancelled();
        if (r.summarizing) return {"MCP conversation summary with preserved source observations", {}, {100, 10, 0, 0}};
        QStringList history;
        for (const auto& m : r.messages) if (m.role == a::MessageRole::User) history.append(m.text);
        return {history.join('|'), {}, {}};
    }
};
}
class McpServerTests : public QObject {
    Q_OBJECT
private slots:
    void lifecycleAndValidation() {
        m::ServerSession s(options());
        s.receive(request(9, "tools/list")); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32002);
        initialize(s); QVERIFY(s.isInitialized()); QCOMPARE(s.clientInfo()["name"].toString(), "test-client");
        s.receive(request(2, "tools/list")); QCOMPARE(next(s)["result"].toObject()["tools"].toArray().size(), 1);
        s.receive(request(3, "unknown")); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32601);
        auto invalid = request(4, "tools/call"); invalid["params"] = QJsonArray{};
        s.receive(invalid); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32602);
        s.receive(request(5, "initialize")); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32600);
    }
    void progressAndStructuredResult() {
        m::ServerSession s(options()); initialize(s);
        s.receive(request(2, "tools/call", {{"name", "echo"}, {"arguments", QJsonObject{{"text", "한글 값"}}},
            {"_meta", QJsonObject{{"progressToken", "caller-token"}}}}));
        QList<QJsonValue> messages;
        QElapsedTimer timer; timer.start();
        while (messages.size() < 3 && timer.elapsed() < 5000) messages += s.takeMessages(200);
        QCOMPARE(messages.size(), 3);
        QCOMPARE(messages[0].toObject()["params"].toObject()["progressToken"].toString(), "caller-token");
        QCOMPARE(messages[1].toObject()["params"].toObject()["progress"].toInt(), 2);
        QCOMPARE(messages[2].toObject()["result"].toObject()["structuredContent"].toObject()["text"].toString(), "한글 값");
        QCOMPARE(messages[2].toObject()["result"].toObject()["_meta"].toObject()["app"].toString(), "fixture");
    }
    void boundedRequestsAndCancellation() {
        auto o = options(); o.maxConcurrentRequests = 1; o.maxQueuedRequests = 0;
        std::atomic_bool started = false, stopped = false;
        o.handlers["test/wait"] = [&](const auto&, const m::ServerRequestContext& c) {
            started = true;
            while (!c.cancellation.isCancelled()) std::this_thread::sleep_for(1ms);
            stopped = true; c.cancellation.throwIfCancelled(); return QJsonObject{};
        };
        m::ServerSession s(o); initialize(s);
        s.receive(request(2, "test/wait")); QTRY_VERIFY(started.load());
        s.receive(request(3, "test/wait")); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32000);
        s.receive({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", QJsonObject{{"requestId", 2}}}});
        QTRY_VERIFY(stopped.load());
        s.receive(request(4, "ping")); QCOMPARE(next(s)["id"].toInt(), 4);
        QVERIFY(s.takeMessages().isEmpty());
    }
    void snapshotsAndSessionIsolation() {
        auto o = options(); o.listPageSize = 1;
        QJsonArray definitions{tool("one"), tool("two"), tool("three")};
        o.lists["tools/list"] = [&](const auto&) { return definitions; };
        m::ServerSession a(o), b(o); initialize(a); initialize(b);
        QVERIFY(a.id() != b.id());
        a.receive(request(2, "tools/list")); auto first = next(a)["result"].toObject();
        const auto cursor = first["nextCursor"].toString(); QVERIFY(!cursor.isEmpty());
        definitions = {tool("replacement")};
        b.receive(request(2, "tools/list", {{"cursor", cursor}})); QCOMPARE(next(b)["error"].toObject()["code"].toInt(), -32602);
        a.receive(request(3, "tools/list", {{"cursor", cursor}})); auto second = next(a)["result"].toObject();
        QCOMPARE(second["tools"].toArray()[0].toObject()["name"].toString(), "two");
        a.receive(request(4, "tools/list", {{"cursor", second["nextCursor"]}}));
        const auto last = next(a)["result"].toObject(); QVERIFY(!last.contains("nextCursor"));
        QCOMPARE(last["tools"].toArray()[0].toObject()["name"].toString(), "three");
    }
    void reverseRequestAndTimeout() {
        auto o = options();
        o.handlers["test/roots"] = [](const auto&, const m::ServerRequestContext& c) { return c.requestClient("roots/list", {}, 1000); };
        o.handlers["test/timeout"] = [](const auto&, const m::ServerRequestContext& c) { return c.requestClient("roots/list", {}, 30); };
        m::ServerSession s(o); initialize(s, {{"roots", QJsonObject{}}});
        s.receive(request(2, "test/roots")); const auto reverse = next(s);
        QCOMPARE(reverse["method"].toString(), "roots/list");
        s.receive({{"jsonrpc", "2.0"}, {"id", reverse["id"]}, {"result", QJsonObject{{"roots", QJsonArray{}}}}});
        QCOMPARE(next(s)["id"].toInt(), 2);
        s.receive(request(3, "test/timeout")); const auto pending = next(s); QVERIFY(pending.contains("method"));
        QList<QJsonValue> messages;
        QElapsedTimer timer; timer.start();
        while (messages.size() < 2 && timer.elapsed() < 5000) messages += s.takeMessages(200);
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages[0].toObject()["method"].toString(), "notifications/cancelled");
        QCOMPARE(messages[1].toObject()["id"].toInt(), 3); QVERIFY(messages[1].toObject().contains("error"));
    }
    void legacyBatchAndModernRejection() {
        m::ServerSession legacy(options()); initialize(legacy, {}, "2025-03-26");
        legacy.receiveBatch({request(2, "tools/list"), request(3, "ping"), 42,
            QJsonObject{{"jsonrpc", "2.0"}, {"method", "notifications/example"}}});
        const auto messages = legacy.takeMessages(2000); QCOMPARE(messages.size(), 1); QVERIFY(messages[0].isArray());
        const auto replies = messages[0].toArray(); QCOMPARE(replies.size(), 3);
        QSet<int> ids; for (const auto& value : replies) if (!value.toObject()["id"].isNull()) ids.insert(value.toObject()["id"].toInt());
        QCOMPARE(ids, (QSet<int>{2, 3})); QCOMPARE(legacy.takeNotifications().size(), 1);
        m::ServerSession modern(options()); initialize(modern);
        modern.receiveBatch({request(2, "ping")}); QCOMPARE(next(modern)["error"].toObject()["code"].toInt(), -32600);
        modern.receive(request(3, "ping")); QCOMPARE(next(modern)["id"].toInt(), 3);
    }
    void oversizedLegacyResponseKeepsBatchMembership() {
        auto o = options(); o.maxMessageBytes = 1024; o.maxQueuedBytes = 4096;
        o.handlers["test/large"] = [](const auto&, const auto&) {
            return QJsonObject{{"text", QString(2048, 'x')}};
        };
        m::ServerSession s(o); initialize(s, {}, "2025-03-26");
        s.receiveBatch({request(2, "test/large"), request(3, "ping")});
        const auto frames = s.takeMessages(2000);
        QCOMPARE(frames.size(), 1); QVERIFY(frames[0].isArray());
        const auto replies = frames[0].toArray(); QCOMPARE(replies.size(), 2);
        QSet<int> ids;
        for (const auto& value : replies) {
            const auto response = value.toObject(); ids.insert(response["id"].toInt());
            if (response["id"] == 2) QCOMPARE(response["error"].toObject()["code"].toInt(), -32000);
            else QVERIFY(response.contains("result"));
        }
        QCOMPARE(ids, (QSet<int>{2, 3}));
        s.receive(request(4, "ping")); QCOMPARE(next(s)["id"].toInt(), 4);
    }
    void exportedToolsKeepPolicyAndContent() {
        QTemporaryDir root;
        auto registry = std::make_shared<a::ToolRegistry>();
        a::Tool value; value.definition = {"value", "Read a value", {{"type", "object"}, {"additionalProperties", false}}, {}, true, true};
        value.execute = [](const auto&, const auto&) { return a::ToolResult{"hello", {{"number", 7}}, false,
            {QJsonObject{{"type", "image"}, {"data", "eA=="}, {"mimeType", "image/png"}}}, {{"private", "payload"}}}; };
        registry->add(value);
        a::Tool denied = value; denied.definition.name = "write"; denied.definition.readOnly = false; registry->add(denied);
        a::McpServerOptions config; config.workingDirectory = root.path(); config.appId = "com.iisacc.fixture";
        m::ServerSession s(a::mcpServerOptions(registry, std::make_shared<a::RulePolicy>(), config)); initialize(s);
        const auto result = call(s, 2, "value");
        QCOMPARE(result["structuredContent"].toObject()["number"].toInt(), 7);
        QCOMPARE(result["_meta"].toObject()["private"].toString(), "payload");
        const auto content = result["content"].toArray();
        QCOMPARE(content[0].toObject()["type"].toString(), "text");
        QCOMPARE(content[1].toObject()["type"].toString(), "image");
        QVERIFY(call(s, 3, "write")["isError"].toBool());
        QVERIFY(call(s, 4, "value", {{"extra", true}})["isError"].toBool());
        s.receive(request(5, "tools/call", {{"name", "missing"}})); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32602);
    }
    void manualCompactionIsConnectionBound() {
        QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>();
        a::EngineOptions engineConfig; engineConfig.sessionsDirectory = root.filePath("sessions");
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        auto engine = std::make_shared<a::Engine>(std::make_shared<HistoryModel>(), registry, policy, engineConfig);
        a::McpServerOptions config; config.workingDirectory = root.path(); config.engine = engine; config.model = "fixture";
        const auto serverOptions = a::mcpServerOptions(registry, policy, config);
        m::ServerSession first(serverOptions), second(serverOptions); initialize(first); initialize(second);
        call(first, 2, "iiLocalLLM.agent.run", {{"prompt", QString(1000, 'a')}});
        call(first, 3, "iiLocalLLM.agent.run", {{"prompt", QString(1000, 'b')}});
        call(first, 4, "iiLocalLLM.agent.run", {{"prompt", "Continue the task"}});
        const auto compacted = call(first, 5, "iiLocalLLM.agent.compact", {{"instructions", "Preserve unfinished work"}});
        QVERIFY(!compacted.value("isError").toBool());
        QCOMPARE(compacted.value("structuredContent").toObject().value("usage").toObject().value("compactions").toInt(), 1);
        const auto state = call(first, 6, "iiLocalLLM.agent.session").value("structuredContent").toObject();
        QCOMPARE(state.value("message_count").toInt(), 6); QCOMPARE(state.value("compaction_count").toInt(), 1);
        QVERIFY(call(second, 2, "iiLocalLLM.agent.compact").value("isError").toBool());
        QVERIFY(call(second, 3, "iiLocalLLM.agent.compact", {{"session_id", state.value("session_id")}}).value("isError").toBool());
    }
    void localAgentConversationsAreIsolated() {
        QTemporaryDir root;
        auto registry = std::make_shared<a::ToolRegistry>();
        a::EngineOptions engineConfig; engineConfig.sessionsDirectory = root.filePath("sessions");
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        auto engine = std::make_shared<a::Engine>(std::make_shared<HistoryModel>(), registry, policy, engineConfig);
        a::McpServerOptions config; config.workingDirectory = root.path(); config.engine = engine; config.model = "fixture";
        const auto serverOptions = a::mcpServerOptions(registry, policy, config);
        m::ServerSession first(serverOptions), second(serverOptions); initialize(first); initialize(second);
        const auto one = call(first, 2, "iiLocalLLM.agent.run", {{"prompt", "private-first"}});
        const auto two = call(second, 2, "iiLocalLLM.agent.run", {{"prompt", "private-second"}});
        const auto resumed = call(first, 3, "iiLocalLLM.agent.run", {{"prompt", "follow-up"}});
        QVERIFY(one["structuredContent"].toObject()["session_id"] != two["structuredContent"].toObject()["session_id"]);
        QCOMPARE(two["structuredContent"].toObject()["text"].toString(), "private-second");
        QCOMPARE(resumed["structuredContent"].toObject()["text"].toString(), "private-first|follow-up");
        QVERIFY(call(second, 3, "iiLocalLLM.agent.session", {{"session_id", one["structuredContent"].toObject()["session_id"]}})["isError"].toBool());
        const auto reset = call(first, 4, "iiLocalLLM.agent.run", {{"prompt", "new"}, {"new_session", true}});
        QCOMPARE(reset["structuredContent"].toObject()["text"].toString(), "new");
        QVERIFY(QDir().mkpath(root.filePath("src")));
        QFile rules(root.filePath("src/AGENTS.md")); QVERIFY(rules.open(QIODevice::WriteOnly));
        rules.write("Scoped MCP instructions"); rules.close();
        const auto scoped = call(first, 5, "iiLocalLLM.agent.run", {{"prompt", "scoped"}, {"context_paths", QJsonArray{"src/new.cpp"}}});
        QVERIFY(scoped["structuredContent"].toObject()["text"].toString().contains("Scoped MCP instructions"));
        const auto unrelated = call(second, 4, "iiLocalLLM.agent.run", {{"prompt", "unchanged"}});
        QVERIFY(!unrelated["structuredContent"].toObject()["text"].toString().contains("Scoped MCP instructions"));
        QVERIFY(call(first, 6, "iiLocalLLM.agent.run", {{"prompt", "invalid"}, {"context_paths", QJsonArray{"../outside"}}})["isError"].toBool());
    }
    void resourcesPromptsSubscriptionsAndLegacyContent() {
        auto o = options();
        o.lists["resources/list"] = [](const auto&) { return QJsonArray{QJsonObject{{"uri", "fixture://value"}, {"name", "value"}}}; };
        o.lists["resources/templates/list"] = [](const auto&) { return QJsonArray{QJsonObject{{"uriTemplate", "fixture://{key}"}, {"name", "template"}}}; };
        o.handlers["resources/read"] = [](const auto& p, const auto&) { return QJsonObject{{"contents", QJsonArray{QJsonObject{{"uri", p["uri"]}, {"text", "resource text"}}}}}; };
        o.handlers["resources/subscribe"] = [](const auto& p, const auto&) { if (p["uri"] != "fixture://value") throw m::RpcError(-32602, "Unknown resource"); return QJsonObject{}; };
        o.lists["prompts/list"] = [](const auto&) { return QJsonArray{QJsonObject{{"name", "summary"}}}; };
        o.handlers["prompts/get"] = [](const auto&, const auto&) { return QJsonObject{{"messages", QJsonArray{QJsonObject{{"role", "user"},
            {"content", QJsonObject{{"type", "text"}, {"text", "Summarize this"}}}}}}}; };
        m::ServerSession first(o), second(o); initialize(first); initialize(second);
        first.receive(request(2, "resources/list")); QCOMPARE(next(first)["result"].toObject()["resources"].toArray().size(), 1);
        first.receive(request(3, "resources/templates/list")); QCOMPARE(next(first)["result"].toObject()["resourceTemplates"].toArray().size(), 1);
        first.receive(request(4, "resources/read", {{"uri", "fixture://value"}})); QCOMPARE(next(first)["result"].toObject()["contents"].toArray().size(), 1);
        first.receive(request(5, "prompts/get", {{"name", "summary"}})); QCOMPARE(next(first)["result"].toObject()["messages"].toArray().size(), 1);
        first.receive(request(6, "resources/subscribe", {{"uri", "fixture://value"}})); QVERIFY(next(first).contains("result"));
        first.notify("notifications/resources/updated", {{"uri", "fixture://value"}});
        second.notify("notifications/resources/updated", {{"uri", "fixture://value"}});
        QCOMPARE(next(first)["params"].toObject()["uri"].toString(), "fixture://value"); QVERIFY(second.takeMessages().isEmpty());
        first.receive(request(7, "resources/unsubscribe", {{"uri", "fixture://value"}})); QVERIFY(next(first).contains("result"));
        first.notify("notifications/resources/updated", {{"uri", "fixture://value"}}); QVERIFY(first.takeMessages().isEmpty());
        o.handlers["tools/call"] = [](const auto&, const auto&) { return QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "resource_link"}, {"uri", "fixture://value"}, {"name", "value"}}}}, {"structuredContent", QJsonObject{{"n", 7}}}}; };
        m::ServerSession legacy(o); initialize(legacy, {}, "2025-03-26");
        const auto value = call(legacy, 2, "echo"); QVERIFY(!value.contains("structuredContent"));
        QCOMPARE(value["content"].toArray().size(), 2);
        for (const auto& block : value["content"].toArray()) QCOMPARE(block.toObject()["type"].toString(), "text");
    }
    void requestDeadlinesLimitsAndClose() {
        auto o = options(); o.requestTimeoutMs = 30; o.maxMessageBytes = 1024; o.maxQueuedBytes = 1024;
        std::atomic_int closed = 0;
        o.onClosed = [&](const QString&) { ++closed; };
        o.handlers["test/wait"] = [](const auto&, const m::ServerRequestContext& c) {
            while (!c.cancellation.isCancelled()) std::this_thread::sleep_for(1ms);
            c.cancellation.throwIfCancelled(); return QJsonObject{};
        };
        o.handlers["test/large"] = [](const auto&, const auto&) { return QJsonObject{{"text", QString(2048, 'x')}}; };
        m::ServerSession s(o); initialize(s);
        s.receive(request(2, "test/wait")); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32000);
        s.receive(request(3, "test/large")); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32000);
        s.receive(request(4, "ping")); QVERIFY(next(s).contains("result"));
        auto invalid = request(5, "ping"); invalid["id"] = QJsonValue::Null;
        s.receive(invalid); QCOMPARE(next(s)["error"].toObject()["code"].toInt(), -32600);
        try { s.receive(request(4, "ping")); QFAIL("Reused request ID accepted"); }
        catch (const Error& e) { QCOMPARE(int(e.code()), int(ErrorCode::ProtocolError)); }
        QVERIFY(s.isClosed()); s.close(); s.close(); QCOMPARE(closed.load(), 1);
    }
    void exportedReadHistoryDoesNotCrossConnections() {
        QTemporaryDir root; QFile file(root.filePath("existing.txt")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("original"); file.close();
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, root.path());
        a::McpServerOptions config; config.workingDirectory = root.path();
        const auto o = a::mcpServerOptions(registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass), config);
        m::ServerSession first(o), second(o); initialize(first); initialize(second);
        QVERIFY(!call(first, 2, "Read", {{"path", "existing.txt"}})["isError"].toBool());
        QVERIFY(call(second, 2, "Write", {{"path", "existing.txt"}, {"content", "other client"}})["isError"].toBool());
        QVERIFY(!call(first, 3, "Write", {{"path", "existing.txt"}, {"content", "owner"}})["isError"].toBool());
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("owner"));
    }
    void parallelReadersAndExclusiveWriter() {
        QTemporaryDir root; std::atomic_int active = 0; std::atomic_bool release = false, writer = false;
        auto registry = std::make_shared<a::ToolRegistry>();
        a::Tool read; read.definition = {"reader", "read", {{"type", "object"}}, {}, true, true};
        read.execute = [&](const auto&, const a::ToolContext& c) {
            ++active; while (!release && !c.cancellation.isCancelled()) std::this_thread::sleep_for(1ms); --active;
            c.cancellation.throwIfCancelled(); return a::ToolResult{"read"};
        }; registry->add(read);
        a::Tool write; write.definition = {"writer", "write", {{"type", "object"}}};
        write.execute = [&](const auto&, const auto&) { writer = true; return a::ToolResult{"write", {{"readers", active.load()}}}; }; registry->add(write);
        a::McpServerOptions config; config.workingDirectory = root.path();
        const auto o = a::mcpServerOptions(registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass), config);
        m::ServerSession first(o), second(o); initialize(first); initialize(second);
        first.receive(request(2, "tools/call", {{"name", "reader"}})); second.receive(request(2, "tools/call", {{"name", "reader"}}));
        QTRY_COMPARE(active.load(), 2);
        first.receive(request(3, "tools/call", {{"name", "writer"}}));
        QVERIFY(first.takeMessages(30).isEmpty()); QVERIFY(!writer.load()); release = true;
        QList<QJsonValue> messages; QElapsedTimer timer; timer.start();
        while (messages.size() < 2 && timer.elapsed() < 3000) messages += first.takeMessages(100);
        QCOMPARE(messages.size(), 2); QVERIFY(writer.load());
        for (const auto& message : messages) if (message.toObject()["id"] == 3)
            QCOMPARE(message.toObject()["result"].toObject()["structuredContent"].toObject()["readers"].toInt(), 0);
        QCOMPARE(next(second)["id"].toInt(), 2);
    }
};
QTEST_GUILESS_MAIN(McpServerTests)
#include "mcp_server_tests.moc"
