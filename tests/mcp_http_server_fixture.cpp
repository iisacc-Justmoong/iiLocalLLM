#include <mcp/HttpServer.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
using namespace iiLocalLLM;
namespace m = iiLocalLLM::mcp;
using namespace std::chrono_literals;
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        const auto config = argc > 1 ? QJsonDocument::fromJson(argv[1]).object() : QJsonObject{};
        m::HttpServerOptions http;
        auto option = [&](const char* name, int& target) { if (config.contains(name)) target = config[name].toInt(); };
        option("maxSessions", http.maxSessions); option("maxStreams", http.maxStreams);
        option("maxControlStreams", http.maxControlStreams);
        option("maxHistoryEvents", http.maxHistoryEvents); option("maxHistoryBytes", http.maxHistoryBytes);
        option("maxStreamsPerSession", http.maxStreamsPerSession); option("streamRetentionMs", http.streamRetentionMs);
        option("sessionIdleTimeoutMs", http.sessionIdleTimeoutMs); option("maxRequestBytes", http.maxRequestBytes);
        http.allowedOrigins = {"https://fixture.example"}; http.heartbeatMs = 100;
        http.authenticate = [](const QByteArray& token) {
            if (token == "provider-error") throw std::runtime_error("sensitive-fixture-provider-error");
            return token == "alpha-fixture" ? QString("alpha") : token == "beta-fixture" ? QString("beta") : QString{};
        };
        std::mutex mutex; QJsonObject calls; m::HttpServer* exposed = nullptr;
        m::HttpServer server([&](const QString& principal) {
            m::ServerOptions o; o.requestTimeoutMs = 3000;
            o.handlers["test/echo"] = [principal](const auto& p, const auto& c) {
                return QJsonObject{{"value", p}, {"principal", principal}, {"session", c.sessionId}};
            };
            o.handlers["test/stats"] = [&](const auto&, const auto&) { std::lock_guard lock(mutex); return calls; };
            o.handlers["test/delay"] = [&](const auto& p, const auto& c) {
                { std::lock_guard lock(mutex); const auto label = p["label"].toString(); calls[label] = calls[label].toInt() + 1; }
                c.progress({{"progress", 1}});
                const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(p["milliseconds"].toInt(150));
                while (std::chrono::steady_clock::now() < end) { c.cancellation.throwIfCancelled(); std::this_thread::sleep_for(1ms); }
                c.progress({{"progress", 2}}); return QJsonObject{{"label", p["label"]}};
            };
            o.handlers["test/wait"] = [](const auto&, const auto& c) {
                c.progress({{"progress", 1}});
                while (!c.cancellation.isCancelled()) std::this_thread::sleep_for(1ms);
                c.cancellation.throwIfCancelled(); return QJsonObject{};
            };
            o.controlHandlers["test/controlWait"] = o.handlers["test/wait"];
            o.controlHandlers["test/controlDelay"] = o.handlers["test/delay"];
            o.handlers["test/reverse"] = [](const auto&, const auto& c) { return c.requestClient("ping", {}, 2000); };
            o.handlers["test/notify"] = [&](const auto&, const auto& c) {
                exposed->notify(c.sessionId, "notifications/tools/list_changed"); return QJsonObject{};
            };
            o.lists["tools/list"] = [](const auto&) { return QJsonArray{}; };
            o.handlers["tools/call"] = [](const auto&, const auto&) { return QJsonObject{{"content", QJsonArray{}}}; };
            return o;
        }, http);
        exposed = &server;
        if (!server.listen()) throw std::runtime_error(server.errorString().toStdString());
        std::cout << QJsonDocument(QJsonObject{{"endpoint", server.endpoint().toString()}}).toJson(QJsonDocument::Compact).constData() << std::endl;
        std::string line; std::getline(std::cin, line); server.close(); return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
