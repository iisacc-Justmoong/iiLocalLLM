#include "mcp/LocalApplications.h"
#include "mcp/HttpClient.h"
#include "agent/McpConnections.h"
#include "agent/McpServer.h"
#include "agent/ObjectTools.h"
#include <QtTest/QtTest>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <future>
#include <thread>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
namespace m = iiLocalLLM::mcp;
using namespace std::chrono_literals;
namespace {
QJsonObject schema() { return {{"type", "object"}, {"additionalProperties", false}}; }
a::ToolDefinition definition() { return {"status", "Read actual application state", schema(), {}, true, true}; }
m::ServerOptions echoServer() {
    m::ServerOptions server;
    server.lists["tools/list"] = [](const auto&) { return QJsonArray{a::toJson(definition())}; };
    server.handlers["tools/call"] = [](const auto&, const auto&) {
        return QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "app response"}}}}};
    };
    return server;
}
void write(const QString& path, const QJsonObject& value) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) throw std::runtime_error("Fixture write failed");
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(value).toJson());
}
QJsonObject read(const QString& path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Fixture read failed");
    return QJsonDocument::fromJson(file.readAll()).object();
}
m::HttpOptions clientOptions(const m::LocalApplicationEndpoint& endpoint) {
    m::HttpOptions options; options.endpoint = endpoint.endpoint;
    options.bearerToken = [token = endpoint.bearerToken] { return token; };
    options.initializeTimeoutMs = 2000; options.requestTimeoutMs = 2000; return options;
}
}
class LocalApplicationTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
#if !defined(Q_OS_UNIX) || defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
        QSKIP("Private local application registration requires desktop POSIX");
#endif
    }
    void authenticatedRegistrationAndRemoval();
    void privateDirectoryAndRecords();
    void rejectRemoteCommandsMalformedAndStaleRecords();
    void automaticRefreshAndCredentialRedaction();
    void appRefreshDoesNotRereadCommandConfiguration();
    void objectThreadAndOwnerLifetime();
    void timedOutAndCancelledQueuedActionsAreDiscarded();
    void timeoutAfterDispatchReportsPossibleEffects();
    void shutdownDoesNotWaitForTheUiThread();
};
void LocalApplicationTests::authenticatedRegistrationAndRemoval() {
    QTemporaryDir root; const auto directory = root.filePath("apps");
    QVERIFY(m::discoverLocalApplications(directory).applications.isEmpty());
    m::LocalApplicationServer server({"com.iisacc.test", "Actual fixture app", "1"}, echoServer(), {directory});
    QVERIFY2(server.listen(), qPrintable(server.errorString()));
    const auto found = m::discoverLocalApplications(directory);
    QCOMPARE(found.error, ErrorCode::None); QCOMPARE(found.applications.size(), 1);
    const auto endpoint = found.applications.first();
    QCOMPARE(endpoint.application.id, QString("com.iisacc.test"));
    QCOMPARE(endpoint.serverName, server.serverName()); QCOMPARE(endpoint.endpoint, server.endpoint());
    QVERIFY(endpoint.bearerToken.size() >= 32);
    m::HttpClient client(clientOptions(endpoint));
    QCOMPARE(client.serverInfo()["name"].toString(), QString("Actual fixture app"));
    QCOMPARE(client.request("tools/call", {{"name", "status"}, {"arguments", QJsonObject{}}})["content"].toArray().size(), 1);
    auto wrong = clientOptions(endpoint); wrong.bearerToken = [] { return QByteArray(43, 'x'); };
    QVERIFY_THROWS_EXCEPTION(Error, m::HttpClient denied(wrong));
    const auto oldInstance = endpoint.instanceId; const auto oldPath = server.registrationPath();
    server.close(); QVERIFY(!QFileInfo::exists(oldPath));
    QVERIFY(m::discoverLocalApplications(directory).applications.isEmpty());
    QVERIFY(server.listen()); QVERIFY(m::discoverLocalApplications(directory).applications.first().instanceId != oldInstance);
}
void LocalApplicationTests::privateDirectoryAndRecords() {
#ifdef Q_OS_UNIX
    QTemporaryDir root; const auto directory = root.filePath("apps");
    m::LocalApplicationServer server({"com.iisacc.test", "Fixture", "1"}, echoServer(), {directory});
    QVERIFY(server.listen());
    struct stat info{}; QVERIFY(!stat(QFile::encodeName(directory).constData(), &info)); QCOMPARE(info.st_mode & 0777, mode_t(0700));
    QVERIFY(!stat(QFile::encodeName(server.registrationPath()).constData(), &info)); QCOMPARE(info.st_mode & 0777, mode_t(0600));
    const auto alias = root.filePath("alias"); QVERIFY(QFile::link(directory, alias));
    QVERIFY(m::discoverLocalApplications(alias).applications.isEmpty());
    QVERIFY(QFile::setPermissions(server.registrationPath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadOther));
    QCOMPARE(m::discoverLocalApplications(directory).rejectedRecords, 1);
    QVERIFY(QFile::setPermissions(server.registrationPath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    const auto recordAlias = root.filePath("hardlink.json");
    QVERIFY(!link(QFile::encodeName(server.registrationPath()).constData(), QFile::encodeName(recordAlias).constData()));
    QCOMPARE(m::discoverLocalApplications(directory).rejectedRecords, 1);
    QVERIFY(QFile::remove(recordAlias));
    QVERIFY(!chmod(QFile::encodeName(directory).constData(), 0777));
    QCOMPARE(m::discoverLocalApplications(directory).error, ErrorCode::Unauthorized);
    m::LocalApplicationServer refused({"com.iisacc.test", "Fixture", "1"}, echoServer(), {directory});
    QVERIFY(!refused.listen());
    QVERIFY(!chmod(QFile::encodeName(directory).constData(), 0700));
#endif
}
void LocalApplicationTests::rejectRemoteCommandsMalformedAndStaleRecords() {
    QTemporaryDir root; const auto directory = root.filePath("apps");
    m::LocalApplicationServer server({"com.iisacc.test", "Fixture", "1"}, echoServer(), {directory}); QVERIFY(server.listen());
    const auto good = read(server.registrationPath());
    auto bad = good; bad["endpoint"] = "https://example.com/mcp"; write(server.registrationPath(), bad);
    QCOMPARE(m::discoverLocalApplications(directory).rejectedRecords, 1);
    bad = good; bad["endpoint"] = "http://127.0.0.1:123/mcp?token=private"; write(server.registrationPath(), bad);
    QCOMPARE(m::discoverLocalApplications(directory).rejectedRecords, 1);
    bad = good; bad["command"] = "/never/execute/this"; write(server.registrationPath(), bad);
    QCOMPARE(m::discoverLocalApplications(directory).rejectedRecords, 1);
    bad = good; bad["pid"] = 1.5; write(server.registrationPath(), bad);
    QCOMPARE(m::discoverLocalApplications(directory).rejectedRecords, 1);
#ifdef Q_OS_UNIX
    const auto child = fork(); QVERIFY(child >= 0);
    if (!child) _exit(0);
    int status = 0; QCOMPARE(waitpid(child, &status, 0), child);
    bad = good; bad["pid"] = qint64(child); write(server.registrationPath(), bad);
    QCOMPARE(m::discoverLocalApplications(directory).staleRecords, 1);
    QVERIFY(QFile::remove(server.registrationPath()));
    QVERIFY(!mkfifo(QFile::encodeName(server.registrationPath()).constData(), 0600));
    QCOMPARE(m::discoverLocalApplications(directory).rejectedRecords, 1);
    QVERIFY(QFile::remove(server.registrationPath()));
#endif
    write(server.registrationPath(), good);
    m::LocalApplicationServer other({"com.iisacc.test", "Second instance", "1"}, echoServer(), {directory}); QVERIFY(other.listen());
    QCOMPARE(m::discoverLocalApplications(directory).applications.size(), 2);
    const auto limited = m::discoverLocalApplications(directory, 1);
    QCOMPARE(limited.applications.size(), 1); QVERIFY(limited.truncated);
}
void LocalApplicationTests::automaticRefreshAndCredentialRedaction() {
    QTemporaryDir root; const auto directory = root.filePath("apps");
    auto registry = std::make_shared<a::ToolRegistry>();
    a::McpConnectionOptions options; options.workingDirectory = root.path(); options.localApplicationsDirectory = directory;
    options.refreshIntervalMs = 20; options.retryDelayMs = 20;
    a::McpConnections connections(registry, options);
    m::LocalApplicationServer server({"com.iisacc.test", "Fixture", "1"}, echoServer(), {directory}); QVERIFY(server.listen());
    QTRY_COMPARE_WITH_TIMEOUT(registry->definitions().size(), 1, 5000);
    const auto first = registry->definitions().first(); QVERIFY(first.deferred);
    QCOMPARE(first.metadata["app_id"].toString(), QString("com.iisacc.test"));
    QCOMPARE(first.metadata["discovery_source"].toString(), QString("local_application"));
    const auto endpoint = m::discoverLocalApplications(directory).applications.first();
    QVERIFY(!QJsonDocument(connections.status()).toJson().contains(endpoint.bearerToken));
    QVERIFY(!QJsonDocument(a::toJson(first)).toJson().contains(endpoint.bearerToken));
    const auto old = registry->get(first.name);
    QCOMPARE(old.execute({}, {}).text, QString("app response"));
    auto oldClient = connections.client(server.serverName()); QVERIFY(oldClient);
    server.close();
    QTRY_VERIFY_WITH_TIMEOUT(registry->definitions().isEmpty(), 5000);
    // Automatic refresh publishes the registry before closing old transports
    // outside its locks. Observe both completion conditions independently.
    QTRY_VERIFY_WITH_TIMEOUT(oldClient->isClosed(), 5000);
    QVERIFY_THROWS_EXCEPTION(Error, old.execute({}, {}));
    QVERIFY(server.listen()); QTRY_COMPARE_WITH_TIMEOUT(registry->definitions().size(), 1, 5000);
    QVERIFY(registry->definitions().first().metadata["connection_id"] != first.metadata["connection_id"]);
}
void LocalApplicationTests::appRefreshDoesNotRereadCommandConfiguration() {
    QTemporaryDir root; const auto directory = root.filePath("apps");
    m::LocalApplicationServer server({"com.iisacc.test", "Fixture", "1"}, echoServer(), {directory}); QVERIFY(server.listen());
    const auto endpoint = m::discoverLocalApplications(directory).applications.first();
    const auto config = root.filePath("mcp.json");
    write(config, {{"mcpServers", QJsonObject{{"fixed", QJsonObject{{"type", "http"}, {"url", endpoint.endpoint.toString()},
        {"headers", QJsonObject{{"Authorization", "Bearer " + QString::fromLatin1(endpoint.bearerToken)}}}}}}}});
    a::McpConnectionOptions options; options.workingDirectory = root.path(); options.configFiles = {config};
    options.localApplicationsDirectory = directory; options.refreshIntervalMs = 0;
    auto registry = std::make_shared<a::ToolRegistry>(); a::McpConnections connections(registry, options);
    QCOMPARE(connections.status().size(), 2); QVERIFY(QFile::remove(config));
    connections.refresh(); QCOMPARE(connections.status().size(), 2);
    QVERIFY(connections.client("fixed")->isConnected());
    QVERIFY_THROWS_EXCEPTION(Error, connections.reload());
    QVERIFY(connections.client("fixed")->isConnected());
}
void LocalApplicationTests::objectThreadAndOwnerLifetime() {
    auto owner = std::make_unique<QObject>(); owner->setProperty("answer", 42);
    auto tool = a::objectTool(owner.get(), definition(), [](QObject& object, const auto&, const auto&) {
        if (QThread::currentThread() != object.thread()) throw std::runtime_error("Wrong thread");
        return a::ToolResult{"owner state", {{"answer", QJsonValue::fromVariant(object.property("answer"))}}};
    });
    auto result = std::async(std::launch::async, [&] { return tool.execute({}, {}); });
    QTRY_VERIFY_WITH_TIMEOUT(result.wait_for(0ms) == std::future_status::ready, 3000);
    QCOMPARE(result.get().data["answer"].toInt(), 42);
    owner.reset();
    auto destroyed = std::async(std::launch::async, [&] { return tool.execute({}, {}); });
    QTRY_VERIFY_WITH_TIMEOUT(destroyed.wait_for(0ms) == std::future_status::ready, 3000);
    QVERIFY_THROWS_EXCEPTION(Error, (void)destroyed.get());
}
void LocalApplicationTests::timedOutAndCancelledQueuedActionsAreDiscarded() {
    QObject owner; int effects = 0;
    auto tool = a::objectTool(&owner, definition(), [&](auto&, const auto&, const auto&) { ++effects; return a::ToolResult{"changed"}; }, 30);
    auto timeout = std::async(std::launch::async, [&] { return tool.execute({}, {}); });
    QVERIFY(timeout.wait_for(2000ms) == std::future_status::ready); // Deliberately do not pump Qt.
    const auto result = timeout.get(); QVERIFY(result.isError); QCOMPARE(result.data["dispatch_started"].toBool(), false);
    QCoreApplication::processEvents(); QCOMPARE(effects, 0);
    a::ToolContext context;
    tool = a::objectTool(&owner, definition(), [&](auto&, const auto&, const auto&) { ++effects; return a::ToolResult{"changed"}; }, 1000);
    auto cancelled = std::async(std::launch::async, [&] { return tool.execute({}, context); });
    std::this_thread::sleep_for(30ms); context.cancellation.cancel();
    QVERIFY(cancelled.wait_for(2000ms) == std::future_status::ready);
    QVERIFY_THROWS_EXCEPTION(Error, (void)cancelled.get());
    QCoreApplication::processEvents(); QCOMPARE(effects, 0);
}
void LocalApplicationTests::timeoutAfterDispatchReportsPossibleEffects() {
    QObject owner; int effects = 0;
    auto tool = a::objectTool(&owner, definition(), [&](auto&, const auto&, const auto&) {
        std::this_thread::sleep_for(200ms); ++effects; return a::ToolResult{"changed"};
    }, 40);
    auto call = std::async(std::launch::async, [&] { return tool.execute({}, {}); });
    QTRY_VERIFY_WITH_TIMEOUT(call.wait_for(0ms) == std::future_status::ready, 3000);
    const auto result = call.get(); QVERIFY(result.isError); QVERIFY(result.data["dispatch_started"].toBool()); QCOMPARE(effects, 1);
}
void LocalApplicationTests::shutdownDoesNotWaitForTheUiThread() {
    QTemporaryDir root; QObject owner; int effects = 0;
    auto registry = std::make_shared<a::ToolRegistry>();
    registry->add(a::objectTool(&owner, definition(), [&](auto&, const auto&, const auto&) { ++effects; return a::ToolResult{"changed"}; }, 10000));
    a::McpServerOptions bridge; bridge.workingDirectory = root.path();
    m::LocalApplicationServer server({"com.iisacc.test", "Fixture", "1"},
        a::mcpServerOptions(registry, std::make_shared<a::RulePolicy>(), bridge), {root.filePath("apps")});
    QVERIFY(server.listen());
    m::HttpClient client(clientOptions(m::discoverLocalApplications(root.filePath("apps")).applications.first()));
    auto call = std::async(std::launch::async, [&] { return client.request("tools/call", {{"name", "status"}, {"arguments", QJsonObject{}}}); });
    std::this_thread::sleep_for(100ms);
    QElapsedTimer elapsed; elapsed.start(); server.close(); QVERIFY(elapsed.elapsed() < 2000);
    QVERIFY(call.wait_for(2000ms) == std::future_status::ready);
    try { (void)call.get(); } catch (const Error&) {}
    QCoreApplication::processEvents(); QCOMPARE(effects, 0);
}
QTEST_GUILESS_MAIN(LocalApplicationTests)
#include "local_applications_tests.moc"
