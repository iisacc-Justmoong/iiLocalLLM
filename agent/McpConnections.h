#pragma once
#include "McpTools.h"

namespace iiLocalLLM::agent {
struct McpConnectionOptions {
    QString workingDirectory;
    // Host-authorized files, in increasing priority. Later entries replace whole
    // server definitions. No automatic filesystem discovery or file watching.
    QStringList configFiles;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    mcp::ClientLimits limits;
    int refreshIntervalMs = 250; // 0: host drives refresh explicitly.
    int retryDelayMs = 1000;
    int maxServers = 32;
    bool deferTools = true;
    bool trustAnnotations = false;
    bool allowInsecureHttp = false;
    // Host callbacks, never populated by configuration JSON. Do not re-enter
    // reload/refresh/close from these callbacks; cooperate with cancellation.
    std::function<mcp::ClientOptions(const QString& server)> clientOptions;
    std::function<QByteArray(const QString& server)> bearerToken;
};
// Blocking construction/reload/close: use outside the application UI thread.
// Owns its registered names exclusively. Registry snapshots retain handlers, but
// replaced/removed connections are closed so stale calls cannot use revoked peers.
class IILOCALLLM_EXPORT McpConnections {
public:
    McpConnections(std::shared_ptr<ToolRegistry>, McpConnectionOptions);
    ~McpConnections();
    McpConnections(const McpConnections&) = delete;
    McpConnections& operator=(const McpConnections&) = delete;
    void reload(CancellationToken = {});
    void refresh(CancellationToken = {});
    // Diagnostics omit configuration values, remote instructions and error text.
    QJsonArray status() const;
    // Remote notification payloads are untrusted data and are not diagnostics.
    QList<QJsonObject> takeNotifications();
    std::shared_ptr<mcp::Client> client(const QString& server) const;
    void close();
private:
    class Impl;
    std::unique_ptr<Impl> d;
};
}
