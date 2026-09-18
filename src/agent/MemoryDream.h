#pragma once
#include "MemoryContext.h"
#include "ProjectMemory.h"
#include "SessionHistory.h"

namespace iiLocalLLM::agent {
struct MemoryDreamOptions {
    bool automatic = false; // Manual request remains available when automatic scheduling is off.
    qint64 minIntervalMs = 24LL*60*60*1000;
    int minSessions = 5;
    int scanIntervalMs = 10*60*1000;
    int maxTurns = 30;
    int timeoutMs = 300000;
    int drainTimeoutMs = 60000;
    int maxInputBytes = 4*1024*1024;
    int maxOutputBytes = 1024*1024;
    int maxToolCallsPerTurn = 64;
    int maxRecords = 128;
    int maxSessions = 16;
    // Worker callbacks, outside locks. Dispatch UI work to the host thread.
    // Do not close/destroy/drain this owner from its callbacks.
    std::function<void(const QJsonObject&)> progress,completed;
};
// A bounded background consolidation owner. Shares the native memory worker
// with extraction, but uses project-wide time/session gates and a process lock.
// It never appends its conversation or completion summary to parent model input.
class IILOCALLLM_EXPORT MemoryDream {
public:
    MemoryDream(std::shared_ptr<ProjectMemory>,std::shared_ptr<SessionHistory>,std::shared_ptr<Model>,
        std::shared_ptr<const PermissionPolicy>,MemoryDreamOptions = {},QList<Hook> = {},AgentHookExecutor = {});
    ~MemoryDream();
    MemoryDream(const MemoryDream&) = delete;
    MemoryDream& operator=(const MemoryDream&) = delete;
    bool automatic() const;
    QJsonObject offer(MemoryContext);
    QJsonObject request(const QString& sessionId); // Latest retained parent; bypasses gates, never the process lock.
    QJsonObject status(const QString& sessionId,int offset=0,int limit=8) const;
    QJsonObject cancel(const QString& sessionId);
    bool drain(int timeoutMs,const QString& sessionId={},const CancellationToken& = {}) const;
    void forget(const QString& sessionId);
    void close();
private:
    class Impl;
    std::unique_ptr<Impl> d;
};
}
