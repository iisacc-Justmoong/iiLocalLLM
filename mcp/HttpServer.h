#pragma once
#include "Server.h"
#include <QtCore/QUrl>

namespace iiLocalLLM::mcp {
struct HttpServerOptions {
    // Required host-owned authentication. Return a stable principal, empty to
    // reject, or throw on provider failure. Called for every HTTP exchange.
    std::function<QString(const QByteArray& bearerToken)> authenticate;
    QStringList allowedOrigins;
    int maxSessions = 32;
    int maxStreams = 64;
    int maxStreamsPerSession = 64;
    int maxQueuedConnections = 64;
    int maxRequestBytes = 8 * 1024 * 1024;
    int maxHistoryBytes = 16 * 1024 * 1024;
    int maxHistoryEvents = 1024;
    int maxNotifications = 128;
    int sessionIdleTimeoutMs = 600000;
    int streamRetentionMs = 60000;
    int readTimeoutMs = 5000;
    int writeTimeoutMs = 5000;
    int heartbeatMs = 15000;
    int retryMs = 1000;
    int maxControlStreams = 4; // Separate global active and per-session retained control-stream limits.
};
struct HttpServerNotification {
    QString sessionId;
    QString principal;
    QJsonObject message;
};
using HttpServerFactory = std::function<ServerOptions(const QString& principal)>;
// Authenticated 2025 Streamable HTTP on 127.0.0.1/mcp. The factory scopes
// tools/policy/engines to a trusted principal; sessions are separately isolated.
// listen/close/port/endpoint/errorString are serial lifecycle operations.
// Never close/destroy from authentication, factory, tool or onClosed callbacks.
class IILOCALLLM_EXPORT HttpServer {
public:
    HttpServer(HttpServerFactory, HttpServerOptions);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    bool listen(quint16 port = 0);
    quint16 port() const;
    QUrl endpoint() const;
    QString errorString() const;
    void close();
    QStringList sessionIds(const QString& principal = {}) const;
    void notify(const QString& sessionId, QString method, QJsonObject params = {});
    QList<HttpServerNotification> takeNotifications();
private:
    class Impl;
    std::unique_ptr<Impl> d;
};
}
