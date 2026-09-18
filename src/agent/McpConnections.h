#pragma once
#include "McpTools.h"
#include <QtCore/QSet>

namespace iiLocalLLM::agent {
struct McpConnectionOptions {
    QString workingDirectory;
    // Host-authorized files, in increasing priority. Later entries replace whole
    // server definitions. They are reread only by explicit reload().
    QStringList configFiles;
    // Host-resolved definitions (for example an installed plugin snapshot).
    // Files have higher priority. Values are never exposed by status().
    QJsonObject inlineServers;
    // Additional interpolation values scoped to one host-configured server.
    // Expansion is one pass, so a replacement path cannot introduce a variable.
    QMap<QString, QMap<QString, QString>> serverVariables;
    QSet<QString> normalizedNameServers; // Explicit plugin naming convention; other servers retain SDK names.
    // Optional private same-user registry of running app HTTP endpoints. Refreshed
    // automatically; it cannot supply commands, arguments, environment or remote URLs.
    QString localApplicationsDirectory;
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
    // Failure diagnostics include the last attempted phase and its elapsed time;
    // local RPC deadlines also include RequestTimeoutError fields. Cleared on
    // successful recovery. Omit configuration values, instructions and error text.
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
