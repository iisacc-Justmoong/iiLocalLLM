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
using ProgressCallback = std::function<void(const QJsonObject&)>;
using RequestHandler = std::function<QJsonObject(const QJsonObject&, const CancellationToken&)>;
struct StdioOptions {
    QString program;
    QStringList arguments;
    QString workingDirectory;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    int initializeTimeoutMs = 10000;
    int requestTimeoutMs = 60000;
    int shutdownTimeoutMs = 500;
    int maxPendingRequests = 128;
    int maxServerRequests = 4;
    int maxMessageBytes = 8 * 1024 * 1024;
    int maxQueuedBytes = 16 * 1024 * 1024;
    int maxNotificationCount = 128;
    int maxStderrBytes = 65536;
    int maxListItems = 10000;
};
struct ClientOptions {
    QJsonObject implementation{{"name", "iiLocalLLM"}, {"version", "0.4.0"}};
    QStringList protocolVersions{"2025-11-25", "2025-06-18", "2025-03-26"};
    QJsonArray roots;
    // Optional host-owned handlers. Capability objects must match the handlers.
    QJsonObject capabilities;
    std::map<QString, RequestHandler> requestHandlers;
};
// Blocking C++ API with one dedicated QProcess I/O thread. Calls may run concurrently.
// Construction negotiates initialize -> notifications/initialized. Never construct,
// request or destroy on an application UI thread. Requests are never auto-replayed.
class IILOCALLLM_EXPORT StdioClient {
public:
    explicit StdioClient(StdioOptions, ClientOptions = {});
    ~StdioClient();
    StdioClient(const StdioClient&) = delete;
    StdioClient& operator=(const StdioClient&) = delete;
    QJsonObject serverInfo() const;
    QJsonObject serverCapabilities() const;
    QString protocolVersion() const;
    QString instructions() const;
    bool isConnected() const;
    QByteArray stderrTail() const;
    // Notifications are data for the host; they never execute agent instructions.
    QList<QJsonObject> takeNotifications();
    void setRoots(QJsonArray);
    void close();
    QJsonObject request(QString method, QJsonObject params = {},
        CancellationToken = {}, ProgressCallback = {}, int timeoutMs = 0);
    void notify(QString method, QJsonObject params = {});
    QJsonArray listTools(CancellationToken = {});
    QJsonObject callTool(const QString& name, QJsonObject arguments,
        CancellationToken = {}, ProgressCallback = {});
    QJsonArray listResources(CancellationToken = {});
    QJsonArray listResourceTemplates(CancellationToken = {});
    QJsonObject readResource(const QString& uri, CancellationToken = {});
    void subscribeResource(const QString& uri, CancellationToken = {});
    void unsubscribeResource(const QString& uri, CancellationToken = {});
    QJsonArray listPrompts(CancellationToken = {});
    QJsonObject getPrompt(const QString& name, QJsonObject arguments = {}, CancellationToken = {});
private:
    class Impl;
    std::shared_ptr<Impl> d;
    QJsonArray list(const QString& capability, const QString& method, const QString& field, CancellationToken);
    void requireCapability(const QString&) const;
};
}
