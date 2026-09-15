#pragma once
#include "Tools.h"

namespace iiLocalLLM::agent {
struct PermissionRequestsOptions {
    int timeoutMs = 120000;
    int maxPending = 64;
    int maxHistory = 512;
    int maxRequestBytes = 1024 * 1024;
    int maxPendingBytes = 8 * 1024 * 1024;
};
// One trusted approval channel. API clients and MCP connections receive separate
// instances. It never executes tools or applies policy updates itself.
class IILOCALLLM_EXPORT PermissionRequests final {
    struct Entry;
    struct Impl;
public:
    struct Ticket {
        QJsonObject request;
    private:
        friend class PermissionRequests;
        std::shared_ptr<Entry> entry;
    };
    explicit PermissionRequests(PermissionRequestsOptions = {});
    ~PermissionRequests();
    PermissionRequests(const PermissionRequests&) = delete;
    PermissionRequests& operator=(const PermissionRequests&) = delete;
    Ticket begin(const ToolCall&, const PermissionDecision&, const ToolContext&, const QJsonObject& preview = {});
    PermissionResponse wait(const Ticket&, const CancellationToken& = {}, QJsonObject* resolution = nullptr);
    QJsonObject pending(qint64 after = 0, int limit = 32, int maxBytes = 4 * 1024 * 1024);
    // Acknowledges a decision, not tool completion. Identical retries are
    // idempotent while retained; conflicting retries never replace the winner.
    QJsonObject respond(const QString& requestId, const QJsonObject& decision);
    void dismiss(const Ticket&, const QString& reason = "Permission request cancelled");
    void close();
private:
    friend class ToolRunner;
    bool settle(const Ticket&, const PermissionResponse&, const QString& source);
    std::shared_ptr<Impl> d;
};
}
