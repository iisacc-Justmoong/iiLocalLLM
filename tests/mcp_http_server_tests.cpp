#include <mcp/HttpServer.h>
#include <mcp/HttpClient.h>
#include <QtTest/QTest>
#include <chrono>
#include <thread>
using namespace iiLocalLLM;
using namespace std::chrono_literals;
namespace m = iiLocalLLM::mcp;
namespace {
m::HttpServerOptions transportOptions() {
    m::HttpServerOptions options;
    options.authenticate = [](const QByteArray& token) {
        return token == "alpha-fixture-credential" ? QString("alpha")
            : token == "beta-fixture-credential" ? QString("beta") : QString{};
    };
    return options;
}
m::ServerOptions protocolOptions(const QString& principal) {
    m::ServerOptions options; options.requestTimeoutMs = 3000;
    options.lists["tools/list"] = [](const auto&) {
        return QJsonArray{QJsonObject{{"name", "echo"}, {"inputSchema", QJsonObject{{"type", "object"}}}}};
    };
    options.handlers["tools/call"] = [](const auto& p, const auto&) {
        return QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", p["arguments"].toObject()["value"]}}}}};
    };
    options.handlers["test/identity"] = [principal](const auto&, const auto& context) {
        return QJsonObject{{"principal", principal}, {"session", context.sessionId}};
    };
    options.handlers["test/reverse"] = [](const auto& p, const auto& context) {
        context.progress({{"progress", 1}});
        const auto result = context.requestClient("sampling/createMessage", p, 2000);
        context.progress({{"progress", 2}});
        return result;
    };
    return options;
}
m::HttpOptions clientOptions(const m::HttpServer& server, QByteArray credential = "alpha-fixture-credential") {
    m::HttpOptions options; options.endpoint = server.endpoint(); options.requestTimeoutMs = 3000;
    options.bearerToken = [credential] { return credential; }; return options;
}
}
class McpHttpServerTests : public QObject {
    Q_OBJECT
private slots:
    void registeredControlsHaveCapacityWhenNormalStreamsAndRetentionAreFull() {
        std::atomic_bool entered=false,released=false;
        auto http=transportOptions();http.maxStreams=2;http.maxStreamsPerSession=2;
        m::HttpServer server([&](const QString& principal) {
            auto options=protocolOptions(principal);options.maxConcurrentRequests=1;options.maxQueuedRequests=0;
            options.handlers["test/wait"]=[&](const auto&,const auto& context) {
                entered=true;while(!released&&!context.cancellation.isCancelled())std::this_thread::sleep_for(1ms);
                context.cancellation.throwIfCancelled();return QJsonObject{{"released",true}};
            };
            options.controlHandlers["test/status"]=[&](const auto&,const auto&){return QJsonObject{{"entered",entered.load()}};};
            options.controlHandlers["test/release"]=[&](const auto&,const auto&){released=true;return QJsonObject{};};
            return options;
        },http);QVERIFY(server.listen());m::HttpClient client(clientOptions(server));
        QTRY_VERIFY(!server.sessionIds("alpha").isEmpty());
        const auto sessionId=server.sessionIds("alpha").value(0);QVERIFY(!sessionId.isEmpty());
        server.notify(sessionId,"notifications/tools/list_changed");
        bool notified=false;QTRY_VERIFY((notified|=!client.takeNotifications().isEmpty()));
        auto waiting=std::async(std::launch::async,[&]{return client.request("test/wait");});
        struct Release {std::atomic_bool& value;~Release(){value=true;}} cleanup{released};QTRY_VERIFY(entered.load());
        try {QVERIFY(client.request("test/status")["entered"].toBool());client.request("test/release");}
        catch(const Error& error){QFAIL(qPrintable(QString("Permission control failed at stream capacity: ")+error.what()));}
        QVERIFY(waiting.get()["released"].toBool());
    }
    void authenticatedSessionsAndNotifications() {
        m::HttpServer server(protocolOptions, transportOptions()); QVERIFY(server.listen());
        m::HttpClient first(clientOptions(server)), second(clientOptions(server, "beta-fixture-credential"));
        QCOMPARE(first.request("test/identity")["principal"].toString(), "alpha");
        QCOMPARE(second.request("test/identity")["principal"].toString(), "beta");
        QVERIFY(first.request("test/identity")["session"] != second.request("test/identity")["session"]);
        QCOMPARE(first.listTools().size(), 1);
        QCOMPARE(first.callTool("echo", {{"value", "HTTP 앱"}})["content"].toArray()[0].toObject()["text"].toString(), "HTTP 앱");
        const auto ids = server.sessionIds("alpha"); QCOMPARE(ids.size(), 1);
        server.notify(ids.first(), "notifications/tools/list_changed");
        QList<QJsonObject> received;
        QTRY_VERIFY(([&] { received += first.takeNotifications(); return !received.isEmpty(); })());
        QCOMPARE(received.first()["method"].toString(), "notifications/tools/list_changed");
        QVERIFY(second.takeNotifications().isEmpty());
        first.setRoots({QJsonObject{{"uri", "file:///fixture"}}});
        QList<m::HttpServerNotification> notifications;
        QTRY_VERIFY(([&] { notifications += server.takeNotifications(); return !notifications.isEmpty(); })());
        QCOMPARE(notifications.first().principal, "alpha");
        QCOMPARE(notifications.first().message["method"].toString(), "notifications/roots/list_changed");
        first.close(); QTRY_VERIFY(server.sessionIds("alpha").isEmpty());
        QCOMPARE(second.request("test/identity")["principal"].toString(), "beta");
    }
    void concurrentReverseRequestsStayOnTheirOriginalStreams() {
        // Eight reverse handlers issue ping concurrently. This routing test
        // needs eight control slots; overload behavior has separate coverage.
        auto transport=transportOptions();transport.maxControlStreams=8;
        m::HttpServer server(protocolOptions, transport); QVERIFY(server.listen());
        m::HttpClient* connected = nullptr;
        m::ClientOptions host; host.capabilities = {{"sampling", QJsonObject{}}};
        host.requestHandlers["sampling/createMessage"] = [&](const QJsonObject& p, const auto&) {
            connected->request("ping"); return QJsonObject{{"value", p["value"]}};
        };
        auto options = clientOptions(server); options.maxServerRequests = 8;
        m::HttpClient client(options, host); connected = &client;
        std::vector<std::future<QJsonObject>> results;
        for (int n = 0; n < 8; ++n) results.push_back(std::async(std::launch::async, [&, n] {
            int progress = 0;
            auto result = client.request("test/reverse", {{"value", n}}, {}, [&](const auto&) { ++progress; });
            if (progress != 2) throw std::runtime_error("Progress escaped its original request stream");
            return result;
        }));
        for (int n = 0; n < 8; ++n) QCOMPARE(results[n].get()["value"].toInt(), n);
    }
    void streamCapacityKeepsCancellationAvailable() {
        std::atomic_bool stopped = false;
        auto http = transportOptions(); http.maxStreams = 2;
        m::HttpServer server([&](const QString& principal) {
            auto options = protocolOptions(principal);
            options.handlers["test/wait"] = [&](const auto&, const auto& context) {
                context.progress({{"progress", 1}});
                while (!context.cancellation.isCancelled()) std::this_thread::sleep_for(1ms);
                stopped = true; context.cancellation.throwIfCancelled(); return QJsonObject{};
            }; return options;
        }, http); QVERIFY(server.listen());
        m::HttpClient client(clientOptions(server)); CancellationToken token;
        QTRY_VERIFY(!server.sessionIds("alpha").isEmpty());
        server.notify(server.sessionIds("alpha").first(), "notifications/tools/list_changed");
        bool notified = false;
        QTRY_VERIFY((notified |= !client.takeNotifications().isEmpty())); // The background GET occupies the first slot.
        std::promise<void> started; auto ready = started.get_future();
        auto pending = std::async(std::launch::async, [&] {
            try { client.request("test/wait", {}, token, [&](const auto&) { started.set_value(); }); return ErrorCode::ProtocolError; }
            catch (const Error& error) { return error.code(); }
        });
        QVERIFY(ready.wait_for(2s) == std::future_status::ready);
        try { client.request("test/identity"); QFAIL("Expected a full stream capacity"); }
        catch (const m::HttpError& error) { QCOMPARE(error.statusCode(), 429); }
        token.cancel(); QCOMPARE(pending.get(), ErrorCode::Cancelled); QTRY_VERIFY(stopped.load());
        QCOMPARE(client.request("test/identity")["principal"].toString(), "alpha");
    }
    void shutdownAndRelistenJoinCallbacksWithoutHoldingTheSessionMap() {
        std::atomic_int closed = 0;
        m::HttpServer* exposed = nullptr;
        m::HttpServer server([&](const QString& principal) {
            auto options = protocolOptions(principal);
            options.onClosed = [&](const auto&) { exposed->sessionIds(); ++closed; };
            return options;
        }, transportOptions());
        exposed = &server;
        m::HttpServer occupied(protocolOptions, transportOptions()); QVERIFY(occupied.listen());
        QVERIFY(!server.listen(occupied.port()));
        for (int n = 0; n < 8; ++n) { QVERIFY(server.listen()); server.close(); }
        for (int n = 0; n < 3; ++n) {
            QVERIFY(server.listen());
            auto options = clientOptions(server); options.listenForNotifications = false;
            m::HttpClient client(options);
            QCOMPARE(client.request("test/identity")["principal"].toString(), "alpha");
            server.close(); client.close();
            QCOMPARE(closed.load(), n + 1); QVERIFY(server.sessionIds().isEmpty()); QCOMPARE(server.port(), 0);
        }
    }
};
QTEST_GUILESS_MAIN(McpHttpServerTests)
#include "mcp_http_server_tests.moc"
