#pragma once
#include "../Types.h"
#include <QtCore/QProcessEnvironment>
#include <map>

namespace iiLocalLLM::mcp {
class IILOCALLLM_EXPORT RpcError : public Error {
public:
    RpcError(int code, const QString& message, QJsonValue data = {});
    int rpcCode() const noexcept { return code_; }
    QJsonValue data() const { return data_; }
private:
    int code_;
    QJsonValue data_;
};
// A locally enforced RPC deadline. Submission means transport acceptance, not
// peer receipt or execution. Times exclude process startup and shutdown.
class IILOCALLLM_EXPORT RequestTimeoutError : public Error {
public:
    RequestTimeoutError(QString method, int timeoutMs, qint64 elapsedMs, bool submitted);
    QString method() const { return method_; }
    int timeoutMs() const noexcept { return timeoutMs_; }
    qint64 elapsedMs() const noexcept { return elapsedMs_; }
    bool submitted() const noexcept { return submitted_; }
private:
    QString method_;
    int timeoutMs_;
    qint64 elapsedMs_;
    bool submitted_;
};
using ProgressCallback = std::function<void(const QJsonObject&)>;
using RequestHandler = std::function<QJsonObject(const QJsonObject&, const CancellationToken&)>;
struct ClientLimits {
    int initializeTimeoutMs = 10000;
    int requestTimeoutMs = 60000;
    int shutdownTimeoutMs = 500;
    int maxPendingRequests = 128;
    int maxServerRequests = 4;
    int maxMessageBytes = 8 * 1024 * 1024;
    int maxQueuedBytes = 16 * 1024 * 1024;
    int maxNotificationCount = 128;
    int maxListItems = 10000;
};
struct StdioOptions : ClientLimits {
    QString program;
    QStringList arguments;
    QString workingDirectory;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    int maxStderrBytes = 65536;
};
namespace detail { class ClientTransport; }
struct ClientOptions {
    QJsonObject implementation{{"name", "iiLocalLLM"}, {"version", "0.13.2"}};
    QStringList protocolVersions{"2025-11-25", "2025-06-18", "2025-03-26"};
    QJsonArray roots;
    // Optional host-owned handlers. Capability objects must match the handlers.
    QJsonObject capabilities;
    std::map<QString, RequestHandler> requestHandlers;
};
// Blocking C++ API with one dedicated transport I/O thread. Calls may run concurrently.
// Construction negotiates initialize -> notifications/initialized. Never construct,
// request or destroy on an application UI thread. Requests are never auto-replayed.
class IILOCALLLM_EXPORT Client {
public:
    virtual ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    QJsonObject serverInfo() const;
    QJsonObject serverCapabilities() const;
    QString protocolVersion() const;
    QString instructions() const;
    bool isConnected() const;
    // Terminal shutdown, unlike temporary HTTP session reinitialization.
    bool isClosed() const;
    QByteArray stderrTail() const;
    quint64 connectionGeneration() const;
    // Notifications are data for the host; they never execute agent instructions.
    QList<QJsonObject> takeNotifications();
    void setRoots(QJsonArray);
    void close();
    QJsonObject request(QString method, QJsonObject params = {},
        CancellationToken = {}, ProgressCallback = {}, int timeoutMs = 0, quint64 expectedGeneration = 0);
    void notify(QString method, QJsonObject params = {});
    QJsonArray listTools(CancellationToken = {});
    QJsonObject callTool(const QString& name, QJsonObject arguments,
        CancellationToken = {}, ProgressCallback = {}, quint64 expectedGeneration = 0);
    QJsonArray listResources(CancellationToken = {});
    QJsonArray listResourceTemplates(CancellationToken = {});
    QJsonObject readResource(const QString& uri, CancellationToken = {});
    void subscribeResource(const QString& uri, CancellationToken = {});
    void unsubscribeResource(const QString& uri, CancellationToken = {});
    QJsonArray listPrompts(CancellationToken = {});
    QJsonObject getPrompt(const QString& name, QJsonObject arguments = {}, CancellationToken = {});
protected:
    Client(std::unique_ptr<detail::ClientTransport>, ClientLimits, ClientOptions);
private:
    class Impl;
    std::shared_ptr<Impl> d;
    QJsonArray list(const QString& capability, const QString& method, const QString& field, CancellationToken);
    void requireCapability(const QString&) const;
};
class IILOCALLLM_EXPORT StdioClient final : public Client {
public:
    explicit StdioClient(StdioOptions, ClientOptions = {});
};
}
