#pragma once
#include "ProjectMemory.h"

namespace iiLocalLLM::agent {
struct MemoryExtractionOptions {
    bool enabled = false; // Explicit embedded-host opt-in; requires project memory.
    int everyTurns = 1;
    int maxTurns = 5;
    int timeoutMs = 60000;
    int drainTimeoutMs = 60000; // Soft shutdown wait, followed by cooperative cancel/join.
    int maxInputBytes = 4 * 1024 * 1024;
    int maxOutputBytes = 1024 * 1024;
    int maxToolCallsPerTurn = 64;
    int maxRecords = 128;
    int maxSessions = 16; // Idle retained contexts are evicted before admitting another session.
    bool manageIndex = true;
    // Runs on the worker, outside locks. Dispatch UI work to the host thread;
    // do not close/destroy/drain this owner from its callback. No transcript text.
    std::function<void(const QJsonObject&)> completed;
};
struct MemoryExtractionSnapshot {
    QString sessionId, workspace;
    ModelRequest request; // Exact parent request plus the completed assistant response.
    QList<Message> messages; // Model-visible conversation, excluding the host prompt prefix.
    std::shared_ptr<ToolRegistry> registry; // Stable native/MCP identity; definitions alone cannot grant execution.
    ToolContext context; // Only host paths/revision are retained; callbacks and cancellation are not inherited.
};
// One bounded worker with a latest pending snapshot per session. Extraction
// uses the parent's model/prompt/tools/generation and an isolated tool context.
// It never appends its transcript or tool results to the main conversation.
class IILOCALLLM_EXPORT MemoryExtraction {
public:
    MemoryExtraction(std::shared_ptr<ProjectMemory>,std::shared_ptr<Model>,
        std::shared_ptr<const PermissionPolicy>,MemoryExtractionOptions = {},QList<Hook> = {},AgentHookExecutor = {});
    ~MemoryExtraction();
    MemoryExtraction(const MemoryExtraction&) = delete;
    MemoryExtraction& operator=(const MemoryExtraction&) = delete;
    bool enabled() const;
    QJsonObject offer(MemoryExtractionSnapshot);
    QJsonObject request(const QString& sessionId); // Retry the latest retained parent context; no wire-supplied conversation.
    QJsonObject status(const QString& sessionId,int offset = 0,int limit = 32) const;
    QJsonObject cancel(const QString& sessionId);
    bool drain(int timeoutMs,const QString& sessionId = {},const CancellationToken& = {}) const;
    void forget(const QString& sessionId); // Cancel/join this session, then release its context and cursor.
    void close(); // Stop admission, soft-drain, cancel/join. Idempotent.
private:
    class Impl;
    std::unique_ptr<Impl> d;
};
}
