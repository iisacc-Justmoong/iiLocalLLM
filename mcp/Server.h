#pragma once
#include "Client.h"

namespace iiLocalLLM::mcp {
struct ServerRequestContext {
    QString sessionId;
    QJsonValue requestId;
    QString protocolVersion;
    QJsonObject clientInfo;
    QJsonObject clientCapabilities;
    CancellationToken cancellation;
    ProgressCallback progress;
    // Blocking, capability-checked reverse RPC tied to this request's lifetime.
    std::function<QJsonObject(QString, QJsonObject, int)> requestClient;
};
using ServerRequestHandler = std::function<QJsonObject(const QJsonObject&, const ServerRequestContext&)>;
using ServerListHandler = std::function<QJsonArray(const ServerRequestContext&)>;
struct ServerOptions {
    QJsonObject implementation{{"name", "iiLocalLLM"}, {"version", "0.8.0"}};
    QString instructions;
    QStringList protocolVersions{"2025-11-25", "2025-06-18", "2025-03-26"};
    std::map<QString, ServerRequestHandler> handlers;
    // tools/list, resources/list, resources/templates/list, prompts/list.
    // The session validates, snapshots and paginates each complete result.
    std::map<QString, ServerListHandler> lists;
    std::function<void(const QString& sessionId)> onClosed;
    int initializeTimeoutMs = 10000;
    int requestTimeoutMs = 60000;
    int maxConcurrentRequests = 8;
    int maxQueuedRequests = 32;
    int maxReverseRequests = 8;
    int maxMessageBytes = 8 * 1024 * 1024;
    int maxQueuedBytes = 16 * 1024 * 1024;
    int maxNotifications = 128;
    int maxRequestsPerSession = 100000;
    int listPageSize = 64;
    int maxListItems = 10000;
    int maxListSnapshots = 8;
    int cursorTimeoutMs = 60000;
};
struct ServerFrame {
    QJsonValue message;
    // Transport-owned correlation, never included in the JSON-RPC message.
    // Empty for unsolicited notifications. Reverse RPC inherits its parent's channel.
    QString channel;
    // A cancelled handler has stopped. message is Undefined for this sideband
    // frame, which must never be serialized as a JSON-RPC message.
    QJsonValue cancelledRequestId = QJsonValue::Undefined;
};
// One isolated connection. receive()/takeMessages()/notify() are thread-safe.
// Handlers run on a bounded pool and must cooperate with cancellation. Never
// close/destroy the session from its handlers or onClosed callback.
class IILOCALLLM_EXPORT ServerSession {
public:
    explicit ServerSession(ServerOptions);
    ~ServerSession();
    ServerSession(const ServerSession&) = delete;
    ServerSession& operator=(const ServerSession&) = delete;
    QString id() const;
    bool isInitialized() const;
    bool isClosed() const;
    QString protocolVersion() const;
    QJsonObject clientInfo() const;
    QJsonObject clientCapabilities() const;
    void receive(const QJsonObject&);
    void receive(const QJsonObject&, const QString& channel);
    void receiveBatch(const QJsonArray&);
    void receiveBatch(const QJsonArray&, const QString& channel);
    // Arrays are returned only for negotiated 2025-03-26 batch requests.
    QList<QJsonValue> takeMessages(int waitMs = 0);
    // Alternative consumer of the same output queue for multiplexed transports.
    QList<ServerFrame> takeFrames(int waitMs = 0);
    QList<QJsonObject> takeNotifications();
    // Capability and resource-subscription checks apply to outgoing notifications.
    void notify(QString method, QJsonObject params = {});
    void requestClose(); // Cancels without joining host callbacks; close() joins.
    void close();
private:
    class Impl;
    std::shared_ptr<Impl> d;
};
// Blocking stdio transport. Currently uses POSIX pipe polling; other transports
// embed ServerSession. stdout is reserved exclusively for JSON-RPC frames.
IILOCALLLM_EXPORT void serveStdio(ServerOptions, CancellationToken = {}, int inputFd = 0, int outputFd = 1);
}
