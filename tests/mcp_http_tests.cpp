#include <mcp/HttpClient.h>
#include <agent/McpTools.h>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QSslConfiguration>
#include <QtTest/QTest>
#include <chrono>
#include <thread>

using namespace iiLocalLLM;
using namespace std::chrono_literals;
namespace m = iiLocalLLM::mcp;
namespace {
struct Peer {
    QProcess process;
    QUrl url;
    explicit Peer(QString mode = "normal", QStringList extra = {}) {
        process.start(MCP_TEST_PYTHON, QStringList{"-B", MCP_HTTP_PEER, mode} + extra);
        if (!process.waitForStarted(3000) || !process.waitForReadyRead(3000))
            throw std::runtime_error("HTTP peer did not start");
        url = QUrl(QString::fromUtf8(process.readLine()).trimmed());
        if (!url.isValid()) throw std::runtime_error("Invalid HTTP peer URL");
    }
    ~Peer() { process.kill(); process.waitForFinished(3000); }
    m::HttpOptions options() const {
        m::HttpOptions o; o.endpoint = url; o.requestTimeoutMs = 3000;
        o.initializeTimeoutMs = 3000; o.shutdownTimeoutMs = 100;
        o.bearerToken = [] { return QByteArray("test-credential"); }; return o;
    }
};
template<class F> void checkError(F f, ErrorCode code) {
    try { f(); QFAIL("Expected error"); }
    catch (const Error& e) { QCOMPARE(e.code(), code); }
}
}
class McpHttpTests : public QObject {
    Q_OBJECT
private slots:
    void jsonLifecycleAndToolAdapter() {
        Peer peer; auto client = std::make_shared<m::HttpClient>(peer.options());
        QVERIFY(client->isConnected()); QCOMPARE(client->protocolVersion(), "2025-11-25");
        QCOMPARE(client->listTools().size(), 1);
        const auto result = client->callTool("echo", {{"value", "로컬 앱"}});
        QCOMPARE(result["structuredContent"].toObject()["value"].toString(), "로컬 앱");
        auto tools = agent::mcpTools(client, {"app", "com.iisacc.fixture"});
        QCOMPARE(tools.size(), 1);
        const auto imported = tools[0].execute({{"value", "adapter"}}, {});
        QCOMPARE(imported.data["value"].toString(), "adapter");
        QVERIFY(imported.text.contains("\"value\":\"adapter\""));
        auto state = client->request("test/state");
        QCOMPARE(state["initializations"].toInt(), 1); QVERIFY(state["headersValid"].toBool());
        client->close(); QVERIFY(!client->isConnected()); QVERIFY(client->isClosed());
    }
    void sseReverseRequestsProgressAndConcurrency() {
        Peer peer;
        m::HttpClient* connectedClient = nullptr;
        m::ClientOptions c; c.capabilities = {{"sampling", QJsonObject{}}};
        c.requestHandlers["sampling/createMessage"] = [&](const auto&, const auto&) {
            connectedClient->request("ping");
            return QJsonObject{{"model", "host"}, {"role", "assistant"},
                {"content", QJsonObject{{"type", "text"}, {"text", "host-result"}}}};
        };
        c.roots = {QJsonObject{{"uri", "file:///workspace"}}};
        auto http = peer.options(); http.maxServerRequests = 16;
        m::HttpClient client(http, c); connectedClient = &client;
        for (int round = 0; round < 5; ++round) {
        std::vector<std::future<QJsonObject>> calls;
        for (int n = 0; n < 8; ++n) calls.push_back(std::async(std::launch::async, [&, n] {
            return client.request("test/reverse", {{"value", n}});
        }));
        try { for (int n = 0; n < 8; ++n) {
            const auto r = calls[n].get(); QCOMPARE(r["value"].toInt(), n);
            QCOMPARE(r["sample"].toString(), "host-result");
            QCOMPARE(r["roots"].toArray().size(), 1);
        } } catch (...) {
            peer.process.waitForReadyRead(1);
            qWarning().noquote() << peer.process.readAllStandardError(); throw;
        }
        }
        QList<double> progress;
        QCOMPARE(client.request("test/progress", {}, {}, [&](const auto& p) {
            progress.append(p["progress"].toDouble());
        })["text"].toString(), "분할된 UTF-8");
        QCOMPARE(progress, (QList<double>{1, 2}));
        client.request("test/notify");
        QList<QJsonObject> received;
        QTRY_VERIFY(([&] { received.append(client.takeNotifications()); return !received.isEmpty(); })());
        QCOMPARE(received.first()["method"].toString(), "notifications/tools/list_changed");
    }
    void resumeWithoutReposting() {
        Peer peer; m::HttpClient client(peer.options());
        const auto start = std::chrono::steady_clock::now();
        QCOMPARE(client.request("test/resume")["resumed"].toBool(), true);
        QVERIFY(std::chrono::steady_clock::now() - start >= 70ms);
        auto state = client.request("test/state");
        QCOMPARE(state["resumePosts"].toInt(), 1); QCOMPARE(state["resumeGets"].toInt(), 1);
    }
    void initializationCanResumeThroughSse() {
        Peer peer("resume-initialize"); m::HttpClient client(peer.options());
        QVERIFY(client.isConnected()); QCOMPARE(client.listTools().size(), 1);
        const auto state = client.request("test/state");
        QCOMPARE(state["initializations"].toInt(), 1); QVERIFY(state["headersValid"].toBool());
    }
    void largeRetryDoesNotReconnectEarly() {
        Peer peer; auto o = peer.options(); o.reconnectDelayMs = 0;
        m::HttpClient client(o);
        checkError([&] { client.request("test/resume-long", {}, {}, {}, 150); }, ErrorCode::Timeout);
        QCOMPARE(client.request("test/state")["resumeGets"].toInt(), 0);
    }
    void postHasItsOwnDeadline() {
        Peer peer; auto o = peer.options(); o.initializeTimeoutMs = 150;
        m::HttpClient client(o);
        QVERIFY(client.request("test/slow-headers", {}, {}, {}, 1000)["ok"].toBool());
    }
    void cancellationTimeoutAndNoReplay() {
        Peer peer; m::HttpClient client(peer.options()); CancellationToken token;
        std::promise<void> started; auto ready = started.get_future();
        auto work = std::async(std::launch::async, [&] {
            checkError([&] { client.request("test/wait", {}, token, [&](const auto&) { started.set_value(); }); }, ErrorCode::Cancelled);
        });
        QVERIFY(ready.wait_for(2s) == std::future_status::ready); token.cancel(); work.get();
        try { client.request("test/wait", {}, {}, {}, 100); QFAIL("Expected HTTP request timeout"); }
        catch (const Error& error) {
            const auto* timeout = dynamic_cast<const m::RequestTimeoutError*>(&error);
            QVERIFY(timeout); QCOMPARE(timeout->code(), ErrorCode::Timeout);
            QCOMPARE(timeout->method(), "test/wait"); QCOMPARE(timeout->timeoutMs(), 100);
            QVERIFY(timeout->elapsedMs() >= 100); QVERIFY(timeout->submitted());
        }
        QTRY_COMPARE(client.request("test/state")["cancelled"].toInt(), 2);
        checkError([&] { client.request("test/disconnect"); }, ErrorCode::RuntimeFailure);
        QCOMPARE(client.request("test/state")["disconnectPosts"].toInt(), 1);
        checkError([&] { client.request("test/drop"); }, ErrorCode::RuntimeFailure);
        QCOMPARE(client.request("test/state")["droppedPosts"].toInt(), 1);
    }
    void expiredSessionReinitializesWithoutReplayingTools() {
        Peer peer; auto client = std::make_shared<m::HttpClient>(peer.options());
        auto tools = agent::mcpTools(client, {"app", {}});
        std::promise<void> accepted; auto waiting = accepted.get_future();
        auto oldRequest = std::async(std::launch::async, [&] {
            try { client->request("test/wait", {}, {}, [&](const auto&) { accepted.set_value(); }); return 0; }
            catch (const m::HttpError& e) { return e.statusCode(); }
        });
        QVERIFY(waiting.wait_for(2s) == std::future_status::ready);
        try { client->request("test/expire"); QFAIL("Expected expired session"); }
        catch (const m::HttpError& e) { QCOMPARE(e.statusCode(), 404); }
        QCOMPARE(oldRequest.get(), 404);
        QVERIFY(!client->isClosed());
        QTRY_VERIFY(client->isConnected() && client->connectionGeneration() == 2);
        auto state = client->request("test/state");
        QCOMPARE(state["initializations"].toInt(), 2); QCOMPARE(state["expirePosts"].toInt(), 1);
        checkError([&] { tools[0].execute({{"value", "stale-schema"}}, {}); }, ErrorCode::RuntimeFailure);
        QCOMPARE(client->callTool("echo", {{"value", "new-session"}})["structuredContent"].toObject()["value"].toString(), "new-session");
    }
    void authenticationAndRedirectIsolation() {
        Peer peer; auto o = peer.options(); o.bearerToken = [] { return QByteArray("wrong-secret"); };
        try { m::HttpClient bad(o); QFAIL("Expected HTTP 401"); }
        catch (const m::HttpError& e) {
            QCOMPARE(e.statusCode(), 401); QVERIFY(e.wwwAuthenticate().contains("Bearer"));
            QVERIFY(!QByteArray(e.what()).contains("wrong-secret"));
        }
        m::HttpClient client(peer.options());
        try { client.request("test/redirect"); QFAIL("Redirect must not be followed"); }
        catch (const m::HttpError& e) { QCOMPARE(e.statusCode(), 307); }
        QCOMPARE(client.request("test/state")["redirectTargetHits"].toInt(), 0);
    }
    void separateClientsAndNoEventChannel() {
        Peer peer; auto options = peer.options(); options.listenForNotifications = false;
        m::HttpClient first(options), second(options);
        const auto one = first.request("test/session")["session"].toString();
        const auto two = second.request("test/session")["session"].toString();
        QVERIFY(!one.isEmpty() && !two.isEmpty() && one != two);
        first.close();
        QCOMPARE(second.request("test/session")["session"].toString(), two);
    }
    void tlsRejectsAnUntrustedCertificate() {
        const auto openssl = QStandardPaths::findExecutable("openssl");
        if (openssl.isEmpty()) QSKIP("TLS certificate fixture requires the OpenSSL command line tool");
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto cert = dir.filePath("test-cert.pem"), key = dir.filePath("test-key.pem");
        QProcess generator;
        generator.start(openssl, {"req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1", "-keyout", key, "-out", cert});
        QVERIFY(generator.waitForFinished(10000)); QCOMPARE(generator.exitCode(), 0);
        Peer peer("tls", {cert, key});
        struct Restore {
            QSslConfiguration saved = QSslConfiguration::defaultConfiguration();
            ~Restore() { QSslConfiguration::setDefaultConfiguration(saved); }
        } restore;
        auto insecure = restore.saved; insecure.setPeerVerifyMode(QSslSocket::VerifyNone);
        QSslConfiguration::setDefaultConfiguration(insecure);
        checkError([&] { m::HttpClient client(peer.options()); }, ErrorCode::RuntimeFailure);
    }
    void invalidInputAndBounds() {
        Peer peer; auto o = peer.options(); o.endpoint = QUrl("http://example.com/mcp");
        checkError([&] { m::HttpClient c(o); }, ErrorCode::InvalidArgument);
        o = peer.options(); o.headers.insert("Mcp-Session-Id", "injected");
        checkError([&] { m::HttpClient c(o); }, ErrorCode::InvalidArgument);
        o = peer.options(); o.headers.insert("X-Host", "bad\r\nheader");
        checkError([&] { m::HttpClient c(o); }, ErrorCode::InvalidArgument);
        o = peer.options(); o.headers.insert("X-Host\n", "value");
        checkError([&] { m::HttpClient c(o); }, ErrorCode::InvalidArgument);
        o = peer.options(); o.maxMessageBytes = 1024; o.maxQueuedBytes = 2048;
        m::HttpClient limited(o);
        checkError([&] { limited.request("test/oversize"); }, ErrorCode::ResourceLimit);
        m::HttpClient invalidEvent(peer.options());
        checkError([&] { invalidEvent.request("test/invalid-event-id"); }, ErrorCode::ProtocolError);
    }
};
QTEST_GUILESS_MAIN(McpHttpTests)
#include "mcp_http_tests.moc"
