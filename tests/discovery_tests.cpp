#include "agent/ToolSearch.h"
#include "agent/Engine.h"
#include "agent/McpConnections.h"
#include "mcp/HttpServer.h"
#include <QtTest/QtTest>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <atomic>
#include <algorithm>

using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
namespace m = iiLocalLLM::mcp;
namespace {
a::Tool tool(QString name, QString description, bool deferred = true) {
    a::Tool value;
    value.definition = {name, description, {{"type", "object"}}, {}, true, true, false, deferred};
    value.execute = [name](const QJsonObject&, const a::ToolContext&) { return a::ToolResult{name}; };
    return value;
}
bool visible(const a::ModelRequest& request, const QString& name) {
    return std::any_of(request.tools.begin(), request.tools.end(), [&](const auto& t) { return t.name == name; });
}
class Model final : public a::Model {
public:
    QList<a::ModelReply> replies;
    QList<a::ModelRequest> requests;
    std::function<void(int)> afterRequest;
    a::ModelReply generate(const a::ModelRequest& r, const CancellationToken&, const TextCallback&) override {
        requests.append(r);
        if (afterRequest) afterRequest(requests.size());
        if (replies.isEmpty()) throw Error(ErrorCode::RuntimeFailure, "Unexpected test model request");
        return replies.takeFirst();
    }
};
void write(const QString& path, const QJsonObject& object) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) throw std::runtime_error("Fixture write failed");
    file.write(QJsonDocument(object).toJson());
}
a::EngineOptions engineOptions(const QString& state) {
    a::EngineOptions o; o.sessionsDirectory = state; o.projectContext.enabled = false;
    o.compaction.automatic = false; return o;
}
}
class DiscoveryTests : public QObject {
    Q_OBJECT
private slots:
    void keywordAndDirectSearch();
    void selectionIsScopedAndPersisted();
    void sameTurnAndChangedSchemaCannotBypassDiscovery();
    void atomicReplacementPreservesSnapshots();
    void configuredHttpToolsAndNotificationRefresh();
    void configErrorsAndCredentialsStayIsolated();
    void configuredStdioReloadAndRecovery();
    void failedConnectionAndDiscoveryDiagnostics();
    void perServerDeadlinesAndReload();
    void validationBeforeEffects();
    void selectionBoundsPolicyAndCompaction();
    void closeDoesNotCancelTheHostsToken();
};
void DiscoveryTests::keywordAndDirectSearch() {
    QList<a::ToolDefinition> catalog{
        tool("mcp__society__list_files", "List project files 파일 목록").definition,
        tool("mcp__dreamscapes__generate", "Generate an image 이미지 생성").definition,
        tool("Read", "Read local text", false).definition};
    const auto found = a::searchTools(catalog, "+society files", 1);
    QCOMPARE(found.data["matches"].toArray(), QJsonArray{"mcp__society__list_files"});
    QCOMPARE(found.data["tools"].toArray().first().toObject()["inputSchema"].toObject(), catalog.first().inputSchema);
    QCOMPARE(a::searchTools(catalog, "이미지", 5).data["matches"].toArray(), QJsonArray{"mcp__dreamscapes__generate"});
    QCOMPARE(a::searchTools(catalog, "mcp__society", 5).data["matches"].toArray(), QJsonArray{"mcp__society__list_files"});
    const auto selected = a::searchTools(catalog, "select:Read,mcp__dreamscapes__generate,missing", 5);
    QCOMPARE(selected.data["matches"].toArray(), (QJsonArray{"Read", "mcp__dreamscapes__generate"}));
    QCOMPARE(selected.data["missing"].toArray(), QJsonArray{"missing"});
    QVERIFY_THROWS_EXCEPTION(Error, a::searchTools(catalog, "", 1));
    QVERIFY_THROWS_EXCEPTION(Error, a::searchTools(catalog, "files", 0));
}
void DiscoveryTests::selectionIsScopedAndPersisted() {
    QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>();
    registry->add(tool("remote", "Remote action"));
    auto model = std::make_shared<Model>();
    model->replies = {{{}, {{"search", "ToolSearch", {{"query", "select:remote"}}}}},
        {{}, {{"call", "remote", {}}}}, {"done", {}}};
    const auto options = engineOptions(root.filePath("sessions")); QString id, forkId, emptyFork;
    {
        a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
        id = engine.createSession("model://test", root.path()).id;
        QCOMPARE(engine.run({id, "Find remote and call it"}).result.get().status, a::RunStatus::Completed);
        QVERIFY(visible(model->requests[0], "ToolSearch")); QVERIFY(!visible(model->requests[0], "remote"));
        QVERIFY(visible(model->requests[1], "remote"));
        QCOMPARE(engine.session(id).messages[4].text, "remote");
        forkId = engine.forkSession(id).id;
        emptyFork = engine.forkSession(id, engine.session(id).messages.first().id).id;
        const auto other = engine.createSession("model://test", root.path());
        model->replies = {{"other", {}}};
        QCOMPARE(engine.run({other.id, "No prior search"}).result.get().status, a::RunStatus::Completed);
        QVERIFY(!visible(model->requests.last(), "remote"));
    }
    a::Engine resumed(model, registry, std::make_shared<a::RulePolicy>(), options);
    for (const auto& session : {id, forkId, emptyFork}) {
        model->replies = {{"resumed", {}}};
        QCOMPARE(resumed.run({session, "Continue"}).result.get().status, a::RunStatus::Completed);
        QCOMPARE(visible(model->requests.last(), "remote"), session != emptyFork);
    }
}
void DiscoveryTests::sameTurnAndChangedSchemaCannotBypassDiscovery() {
    QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>(); registry->add(tool("remote", "Remote action"));
    auto model = std::make_shared<Model>();
    model->replies = {{{}, {{"search", "ToolSearch", {{"query", "select:remote"}}}, {"premature", "remote", {}}}}, {"done", {}}};
    a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), engineOptions(root.filePath("sessions")));
    const auto id = engine.createSession("model://test", root.path()).id;
    QCOMPARE(engine.run({id, "Search then call in the same batch"}).result.get().status, a::RunStatus::Completed);
    QVERIFY(engine.session(id).messages[3].isError);
    QVERIFY(visible(model->requests.last(), "remote"));
    auto changed = tool("remote", "Changed action");
    registry->replace({"remote"}, {changed});
    model->replies = {{{}, {{"stale", "remote", {}}}}, {"done", {}}};
    QCOMPARE(engine.run({id, "Try old schema"}).result.get().status, a::RunStatus::Completed);
    QVERIFY(!visible(model->requests.last(), "remote"));
    QVERIFY(engine.session(id).messages[7].isError);
}
void DiscoveryTests::atomicReplacementPreservesSnapshots() {
    a::ToolRegistry registry; registry.add(tool("owned", "Original")); registry.add(tool("other", "Host tool"));
    auto snapshot = registry.snapshot();
    QVERIFY_THROWS_EXCEPTION(Error, registry.replace({"owned"}, {tool("other", "Collision")}));
    QCOMPARE(registry.get("owned").definition.description, "Original");
    registry.replace({"owned"}, {tool("new", "Replacement")});
    QVERIFY_THROWS_EXCEPTION(Error, registry.get("owned"));
    QCOMPARE(snapshot->get("owned").definition.description, "Original");
    QCOMPARE(registry.definitions().size(), 2);
}
void DiscoveryTests::configuredHttpToolsAndNotificationRefresh() {
    QTemporaryDir root; auto exported = std::make_shared<a::ToolRegistry>(); exported->add(tool("one", "First", false));
    m::HttpServerOptions transport; transport.authenticate = [](const QByteArray& token) { return token == "valid-token" ? "app" : QString{}; };
    m::HttpServer server([exported](const QString&) {
        m::ServerOptions o; o.lists["tools/list"] = [exported](const m::ServerRequestContext&) { QJsonArray out; for (const auto& t : exported->definitions()) out.append(a::toJson(t)); return out; };
        o.handlers["tools/call"] = [](const QJsonObject& params, const m::ServerRequestContext&) {
            return QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", params["name"]}}}}};
        }; return o;
    }, transport);
    QVERIFY(server.listen());
    const auto config = root.filePath(".mcp.json");
    write(config, {{"mcpServers", QJsonObject{{"app", QJsonObject{{"type", "http"}, {"url", server.endpoint().toString()},
        {"headers", QJsonObject{{"Authorization", "Bearer ${TOKEN}"}}}, {"appId", "com.iisacc.example"}}}}}});
    auto registry = std::make_shared<a::ToolRegistry>(); registry->add(tool("local", "Host", false));
    a::McpConnectionOptions o; o.workingDirectory = root.path(); o.configFiles = {config}; o.environment.insert("TOKEN", "valid-token"); o.refreshIntervalMs = 25;
    a::McpConnections connections(registry, o);
    QVERIFY2(connections.status().first().toObject()["state"] == "ready", QJsonDocument(connections.status()).toJson().constData());
    const auto remote = registry->get("mcp__app__one"); QVERIFY(remote.definition.deferred);
    QCOMPARE(remote.definition.metadata["app_id"].toString(), "com.iisacc.example");
    QVERIFY(!remote.definition.readOnly); // Remote hints never grant local permission.
    auto frozen = registry->snapshot();
    exported->replace({"one"}, {tool("two", "Second", false)});
    const auto ids = server.sessionIds("app"); QCOMPARE(ids.size(), 1);
    server.notify(ids.first(), "notifications/tools/list_changed");
    QTRY_VERIFY_WITH_TIMEOUT(registry->definitions().size() == 2 && registry->definitions().last().name == "mcp__app__two", 5000);
    QCOMPARE(frozen->get("mcp__app__one").definition.name, "mcp__app__one");
    connections.close(); QCOMPARE(registry->definitions().size(), 1);
}
void DiscoveryTests::configErrorsAndCredentialsStayIsolated() {
    QTemporaryDir root; const auto path = root.filePath(".mcp.json");
    write(path, {{"mcpServers", QJsonObject{{"unavailable", QJsonObject{{"type", "http"}, {"url", "http://127.0.0.1:1/mcp"},
        {"headers", QJsonObject{{"Authorization", "Bearer very-secret-token"}}}}}}}});
    a::McpConnectionOptions o; o.workingDirectory = root.path(); o.configFiles = {path}; o.refreshIntervalMs = 0; o.limits.initializeTimeoutMs = 200;
    auto registry = std::make_shared<a::ToolRegistry>(); a::McpConnections connections(registry, o);
    const auto status = QJsonDocument(connections.status()).toJson();
    QVERIFY(!status.contains("very-secret-token")); QVERIFY(!status.contains("Authorization"));
    QCOMPARE(connections.status().first().toObject()["state"].toString(), "failed");
    QCOMPARE(connections.status().first().toObject()["error_phase"], "connect");
    QVERIFY(connections.status().first().toObject().contains("error_elapsed_ms"));
    write(path, {{"mcpServers", QJsonArray{}}});
    QVERIFY_THROWS_EXCEPTION(Error, connections.reload());
    QCOMPARE(connections.status().size(), 1);
}
void DiscoveryTests::configuredStdioReloadAndRecovery() {
    QTemporaryDir root; QDir().mkpath(root.filePath("work"));
    const auto first = root.filePath("base.json"), override = root.filePath("override.json");
    const QJsonObject entry{{"command", "${PYTHON}"}, {"args", QJsonArray{"-B", MCP_TEST_PEER}},
        {"cwd", "work"}, {"env", QJsonObject{{"IILOCAL_MCP_TEST_VALUE", "${MISSING:-literal $(not-executed)}"}}}};
    write(first, {{"mcpServers", QJsonObject{{"app", entry}, {"disabled", QJsonObject{{"disabled", true}}}}}});
    write(override, {{"mcpServers", QJsonObject{}}});
    a::McpConnectionOptions o; o.workingDirectory = root.path(); o.configFiles = {first, override};
    o.environment.insert("PYTHON", MCP_TEST_PYTHON); o.environment.remove("MISSING"); o.refreshIntervalMs = 25; o.retryDelayMs = 25;
    o.clientOptions = [root = root.path()](const QString&) {
        m::ClientOptions client; client.roots = {QJsonObject{{"uri", QUrl::fromLocalFile(root).toString()}, {"name", "workspace"}}}; return client;
    };
    auto registry = std::make_shared<a::ToolRegistry>(); a::McpConnections connections(registry, o);
    auto client = connections.client("app"); auto state = client->request("test/state");
    QCOMPARE(state["configuredValue"], "literal $(not-executed)");
    QCOMPARE(state["cwd"].toString(), QFileInfo(root.filePath("work")).canonicalFilePath());
    QCOMPARE(client->listResources().size(), 1); QCOMPARE(client->listPrompts().size(), 1);
    QCOMPARE(client->request("test/reverse", {{"method", "roots/list"}})["response"].toObject()["result"].toObject()["roots"].toArray().size(), 1);
    const auto old = registry->get("mcp__app__echo");
    const auto oldId = old.definition.metadata["connection_id"];
    connections.reload(); QCOMPARE(connections.client("app"), client);
    QCOMPARE(registry->get("mcp__app__echo").definition.metadata["connection_id"], oldId);
    try { client->request("test/exit"); } catch (const Error&) {}
    QTRY_VERIFY_WITH_TIMEOUT(connections.client("app") != client, 5000);
    QVERIFY(client->isClosed());
    QVERIFY(registry->get("mcp__app__echo").definition.metadata["connection_id"] != oldId);
    QVERIFY_THROWS_EXCEPTION(Error, old.execute({{"value", "stale"}}, {}));
    auto replacement = entry; replacement["alwaysLoad"] = true;
    write(override, {{"mcpServers", QJsonObject{{"app", replacement}}}});
    auto recovered = connections.client("app");
    connections.refresh(); QCOMPARE(connections.client("app"), recovered); // No implicit file reload.
    connections.reload(); QVERIFY(recovered->isClosed()); QVERIFY(!registry->get("mcp__app__echo").definition.deferred);
    auto active = connections.client("app");
    write(override, {{"mcpServers", QJsonObject{{"app", QJsonObject{{"disabled", true}}}}}});
    connections.reload(); QVERIFY(active->isClosed()); QVERIFY(registry->definitions().isEmpty());
    write(override, {{"mcpServers", QJsonObject{{"app", QJsonObject{{"disabled", true}, {"appId", "com.iisacc.renamed"}}}}}});
    connections.reload(); QCOMPARE(connections.status().first().toObject()["app_id"].toString(), "com.iisacc.renamed");
    connections.close(); connections.close();
    QVERIFY_THROWS_EXCEPTION(Error, connections.reload());
}
void DiscoveryTests::validationBeforeEffects() {
    QTemporaryDir root; const auto path = root.filePath("config.json");
    const QJsonObject valid{{"command", MCP_TEST_PYTHON}, {"args", QJsonArray{"-B", MCP_TEST_PEER}}};
    int effects = 0;
    a::McpConnectionOptions o; o.workingDirectory = root.path(); o.configFiles = {path}; o.refreshIntervalMs = 0;
    o.clientOptions = [&](const QString&) { ++effects; return m::ClientOptions{}; };
    const QList<QJsonObject> invalid{
        {{"type", "sse"}, {"url", "http://localhost/mcp"}},
        {{"command", "${UNSET_DISCOVERY_VARIABLE}"}},
        {{"command", "python3"}, {"args", "not-an-array"}},
        {{"type", "http"}, {"url", "https://secret@example.com/mcp"}},
        {{"type", "http"}, {"url", "https://example.com/mcp"}, {"headers", QJsonObject{{"X-App", "value\r\nInjected: header"}}}},
        {{"type", "http"}, {"url", "https://example.com/mcp"}, {"headers", QJsonObject{{"X-App\n", "value"}}}},
        {{"command", "python3"}, {"env", QJsonObject{{"NAME\n", "value"}}}},
        {{"command", "python3"}, {"initializeTimeoutMs", 0}},
        {{"command", "python3"}, {"initializeTimeoutMs", -1}},
        {{"command", "python3"}, {"requestTimeoutMs", 1.5}},
        {{"command", "python3"}, {"requestTimeoutMs", "30000"}},
        {{"command", "python3"}, {"requestTimeoutMs", true}},
        {{"disabled", true}, {"initializeTimeoutMs", QJsonValue::Null}},
        {{"command", "python3"}, {"initializeTimeoutMs", 2147483648.0}}
    };
    o.environment.remove("UNSET_DISCOVERY_VARIABLE");
    for (const auto& bad : invalid) {
        write(path, {{"mcpServers", QJsonObject{{"a-valid", valid}, {"z-invalid", bad}}}});
        QVERIFY_THROWS_EXCEPTION(Error, a::McpConnections(std::make_shared<a::ToolRegistry>(), o));
        QCOMPARE(effects, 0);
    }
    write(root.filePath(".mcp.json"), {{"mcpServers", QJsonObject{{"app", valid}}}});
    o.configFiles.clear();
    a::McpConnections empty(std::make_shared<a::ToolRegistry>(), o);
    QVERIFY(empty.status().isEmpty()); QCOMPARE(effects, 0); // Workspace files require explicit host selection.
}
void DiscoveryTests::failedConnectionAndDiscoveryDiagnostics() {
    QTemporaryDir root; const auto path = root.filePath("mcp.json"), gate = root.filePath("release-list");
    auto registry = std::make_shared<a::ToolRegistry>();
    a::McpConnectionOptions o; o.workingDirectory = root.path(); o.configFiles = {path}; o.refreshIntervalMs = 0;
    o.limits.initializeTimeoutMs = 200; o.limits.requestTimeoutMs = 150;
    QJsonObject peer{{"command", MCP_TEST_PYTHON}, {"args", QJsonArray{"-B", MCP_TEST_PEER, "hang-initialize"}},
        {"env", QJsonObject{{"IILOCAL_MCP_TEST_VALUE", "private-configuration"}}}};
    write(path, {{"mcpServers", QJsonObject{{"peer", peer}}}});
    a::McpConnections failed(registry, o);
    auto status = failed.status().first().toObject();
    QCOMPARE(status["state"], "failed"); QCOMPARE(status["error_code"], "timeout");
    QCOMPARE(status["error_phase"], "connect"); QVERIFY(status["error_elapsed_ms"].toInteger() >= 200);
    auto timeout = status["request_timeout"].toObject();
    QCOMPARE(timeout["method"], "initialize"); QCOMPARE(timeout["timeout_ms"].toInt(), 200);
    QVERIFY(timeout["elapsed_ms"].toInteger() >= 200); QVERIFY(timeout["submitted"].toBool());
    QVERIFY(!QJsonDocument(status).toJson().contains("private-configuration"));
    QVERIFY(registry->definitions().isEmpty()); failed.close();

    peer["args"] = QJsonArray{"-B", MCP_TEST_PEER, "gated-list", gate};
    write(path, {{"mcpServers", QJsonObject{{"peer", peer}}}});
    o.limits.initializeTimeoutMs = 10000;
    a::McpConnections discovery(registry, o);
    status = discovery.status().first().toObject();
    QCOMPARE(status["state"], "failed"); QCOMPARE(status["error_phase"], "discover_tools");
    timeout = status["request_timeout"].toObject();
    QCOMPARE(timeout["method"], "tools/list");
    // Each page receives the remaining aggregate list budget, rounded to ms.
    QVERIFY(timeout["timeout_ms"].toInt() > 0 && timeout["timeout_ms"].toInt() <= 150);
    QVERIFY(timeout["elapsed_ms"].toInteger() >= timeout["timeout_ms"].toInt());
    QVERIFY(status["error_elapsed_ms"].toInteger() >= timeout["elapsed_ms"].toInteger());
    QVERIFY(timeout["submitted"].toBool());
    auto client = discovery.client("peer"); QVERIFY(client && client->isConnected());
    QVERIFY(registry->definitions().isEmpty());
    write(gate, {}); discovery.reload();
    status = discovery.status().first().toObject();
    QCOMPARE(status["state"], "ready"); QCOMPARE(discovery.client("peer"), client);
    QVERIFY(!status.contains("error_code")); QVERIFY(!status.contains("error_phase"));
    QVERIFY(!status.contains("error_elapsed_ms")); QVERIFY(!status.contains("request_timeout"));
    QCOMPARE(registry->definitions().size(), 2);
    peer["command"] = root.filePath("nonexistent-server"); peer["args"] = QJsonArray{};
    write(path, {{"mcpServers", QJsonObject{{"peer", peer}}}}); discovery.reload();
    status = discovery.status().first().toObject();
    QCOMPARE(status["state"], "failed"); QCOMPARE(status["error_code"], "runtime_unavailable");
    QCOMPARE(status["error_phase"], "connect"); QVERIFY(status["error_elapsed_ms"].toInteger() >= 0);
    QVERIFY(!status.contains("request_timeout")); QVERIFY(client->isClosed());
}
void DiscoveryTests::perServerDeadlinesAndReload() {
    QTemporaryDir root; const auto path = root.filePath("mcp.json");
    auto registry = std::make_shared<a::ToolRegistry>();
    a::McpConnectionOptions o; o.workingDirectory = root.path(); o.configFiles = {path}; o.refreshIntervalMs = 0;
    o.limits.initializeTimeoutMs = 100; o.limits.requestTimeoutMs = 2000;
    QJsonObject peer{{"command", MCP_TEST_PYTHON}, {"args", QJsonArray{"-B", MCP_TEST_PEER, "delay-initialize", "0.3"}},
        {"initializeTimeoutMs", 3000}, {"requestTimeoutMs", 75}};
    write(path, {{"mcpServers", QJsonObject{{"slow", peer}}}});
    a::McpConnections connections(registry, o);
    QCOMPARE(connections.status().first().toObject()["state"], "ready");
    auto client = connections.client("slow"); QVERIFY(client);
    try { client->request("test/slow", {{"delay", .3}}); QFAIL("Expected per-server deadline"); }
    catch (const m::RequestTimeoutError& error) { QCOMPARE(error.timeoutMs(), 75); }
    peer["requestTimeoutMs"] = 1000;
    write(path, {{"mcpServers", QJsonObject{{"slow", peer}}}}); connections.reload();
    QVERIFY(client->isClosed()); client = connections.client("slow"); QVERIFY(client);
    QCOMPARE(client->request("test/slow", {{"delay", .15}, {"value", "reloaded"}})["value"], "reloaded");
    // Removing the override restores the host's short initialization budget.
    peer.remove("initializeTimeoutMs");
    write(path, {{"mcpServers", QJsonObject{{"slow", peer}}}}); connections.reload();
    auto status = connections.status().first().toObject();
    QCOMPARE(status["state"], "failed"); QCOMPARE(status["error_phase"], "connect");
    // The same short budget also bounds QProcess startup. A process-start
    // deadline may fail before there is an initialize RPC to describe.
    if (status["error_code"] == "timeout")
        QCOMPARE(status["request_timeout"].toObject()["timeout_ms"].toInt(), 100);
    else {
        QCOMPARE(status["error_code"], "runtime_unavailable");
        QVERIFY(!status.contains("request_timeout"));
    }
    QVERIFY(client->isClosed());
}
void DiscoveryTests::selectionBoundsPolicyAndCompaction() {
    QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>();
    registry->add(tool("first", "First")); registry->add(tool("second", "Second"));
    auto model = std::make_shared<Model>();
    auto options = engineOptions(root.filePath("sessions")); options.toolSearch.maxResults = 1; options.toolSearch.maxActiveTools = 1;
    a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Default,
        QList<a::PermissionRule>{{"second", a::PermissionBehavior::Deny}}), options);
    const auto id = engine.createSession("model://test", root.path()).id;
    model->replies = {{{}, {{"one", "ToolSearch", {{"query", "first"}}}}},
        {{}, {{"two", "ToolSearch", {{"query", "second"}}}}}, {{}, {{"denied", "second", {}}}}, {"done", {}}};
    QCOMPARE(engine.run({id, "Select two tools"}).result.get().status, a::RunStatus::Completed);
    QVERIFY(!visible(model->requests.last(), "first")); QVERIFY(visible(model->requests.last(), "second"));
    QVERIFY(engine.session(id).messages[6].isError);
    {
        a::SessionStore store(options.sessionsDirectory); auto lease = store.acquire(id);
        a::Compaction checkpoint; checkpoint.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        checkpoint.atMessageId = lease->session().messages.last().id;
        checkpoint.throughMessageId = lease->session().messages[6].id; checkpoint.summary = "Tools were selected; execution of second was denied.";
        checkpoint.retainedUserMessageId = lease->session().messages.first().id;
        checkpoint.inputTokensBefore = 100; checkpoint.inputTokensAfter = 10; lease->compact(checkpoint);
    }
    model->replies = {{"continued", {}}}; QCOMPARE(engine.run({id, "Continue after summary"}).result.get().status, a::RunStatus::Completed);
    QVERIFY(visible(model->requests.last(), "second")); QVERIFY(!visible(model->requests.last(), "first"));
    auto eagerOptions = engineOptions(root.filePath("eager")); eagerOptions.toolSearch.enabled = false;
    a::Engine eager(model, registry, std::make_shared<a::RulePolicy>(), eagerOptions);
    model->replies = {{"all tools", {}}};
    QCOMPARE(eager.run({eager.createSession("model://test", root.path()).id, "Start"}).result.get().status, a::RunStatus::Completed);
    QVERIFY(visible(model->requests.last(), "first")); QVERIFY(visible(model->requests.last(), "second")); QVERIFY(!visible(model->requests.last(), "ToolSearch"));
}
void DiscoveryTests::closeDoesNotCancelTheHostsToken() {
    QTemporaryDir root; a::McpConnectionOptions options;
    options.workingDirectory = root.path(); options.refreshIntervalMs = 0;
    a::McpConnections connections(std::make_shared<a::ToolRegistry>(), options);
    CancellationToken token; connections.refresh(token); connections.close();
    QVERIFY(!token.isCancelled());
}
QTEST_GUILESS_MAIN(DiscoveryTests)
#include "discovery_tests.moc"
