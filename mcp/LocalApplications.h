#pragma once
#include "HttpServer.h"

namespace iiLocalLLM::mcp {
struct LocalApplicationIdentity {
    QString id;
    QString name;
    QString version;
};
struct LocalApplicationEndpoint {
    LocalApplicationIdentity application;
    QString instanceId;
    QString serverName;
    QUrl endpoint;
    // Same-user credential. Never include this field in logs, tool metadata or API diagnostics.
    QByteArray bearerToken;
    qint64 processId = 0;
};
struct LocalApplicationDiscovery {
    QList<LocalApplicationEndpoint> applications;
    ErrorCode error = ErrorCode::None;
    int rejectedRecords = 0;
    int staleRecords = 0;
    bool truncated = false;
};
// IILOCALLLM_APP_ENDPOINTS, otherwise GenericDataLocation/iisacc/AgentEndpoints.
IILOCALLLM_EXPORT QString localApplicationsDirectory();
// POSIX same-user directory, regular private files, numeric loopback HTTP only.
// A missing directory is empty. An unsafe/unreadable directory returns an error
// without using its records. PID liveness is not application/code-signing attestation.
IILOCALLLM_EXPORT LocalApplicationDiscovery discoverLocalApplications(
    const QString& directory, int maxApplications = 32);

struct LocalApplicationServerOptions {
    QString directory;
    // Authentication is owned by LocalApplicationServer; leave authenticate unset.
    HttpServerOptions http;
};
// Publishes an already-running app, never a command to launch. Desktop POSIX only.
// Lifecycle is serial. Close before destroying QCoreApplication or tool owners.
class IILOCALLLM_EXPORT LocalApplicationServer {
public:
    LocalApplicationServer(LocalApplicationIdentity, ServerOptions,
                           LocalApplicationServerOptions = {});
    ~LocalApplicationServer();
    LocalApplicationServer(const LocalApplicationServer&) = delete;
    LocalApplicationServer& operator=(const LocalApplicationServer&) = delete;
    bool listen();
    QString errorString() const;
    QString registrationPath() const;
    QString serverName() const;
    QUrl endpoint() const;
    void close();
private:
    class Impl;
    std::unique_ptr<Impl> d;
};
}
