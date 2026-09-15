#pragma once
#include "Engine.h"
#include "Subagents.h"
#include "PermissionRequests.h"
#include "../Rpc.h"
#include <QtCore/QMap>

namespace iiLocalLLM::agent {
class McpConnections;
struct ApiOptions {
    QString workingDirectory;
    QString stateDirectory; // Private directory disjoint from the tool workspace.
    QMap<QString, QString> clientTokens; // Stable app/client ID -> secret (at least 32 bytes).
    EngineOptions engine; // sessionsDirectory is assigned per authenticated client.
    std::shared_ptr<McpConnections> mcp; // Optional host-owned, shared MCP catalog.
    int maxConcurrentRequests = 8;
    int maxQueuedRequests = 32;
    int maxSessionsPerClient = 1024;
    int maxTurns = 32;
    int maxResultBytes = 4 * 1024 * 1024;
    int requestTimeoutMs = 300000;
    int maxConcurrentInputControls = 2;
    int maxQueuedInputControls = 16;
    bool subagentsEnabled = false; // Embedded hosts opt in; daemon enables by default.
    SubagentOptions subagents; // workspace/state are assigned per authenticated client.
    std::optional<PermissionRequestsOptions> permissionRequests; // Opt-in; one channel per authenticated client.
};
// One authenticated service shared by HTTP and native IPC. App identities own
// separate persistent Engine stores. Model, registry and policy are host-owned.
// Never close/destroy from an event callback; handlers must cooperate with cancel.
class IILOCALLLM_EXPORT Api final : public RpcHandler {
public:
    Api(std::shared_ptr<Model>, std::shared_ptr<ToolRegistry>,
        std::shared_ptr<const PermissionPolicy>, ApiOptions);
    ~Api() override;
    Api(const Api&) = delete;
    Api& operator=(const Api&) = delete;
    RpcHandle dispatch(QString method, QJsonObject parameters, QString credential,
        RpcEventCallback = {}) override;
    void close();
private:
    class Impl;
    std::shared_ptr<Impl> d;
};
}
