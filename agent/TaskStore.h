#pragma once
#include "Tools.h"

namespace iiLocalLLM::agent {
struct TaskStoreOptions {
    int maxTasks = 1000;
    int maxTodos = 1000;
    qint64 maxBytes = 4 * 1024 * 1024;
    int lockTimeoutMs = 5000;
};
struct TaskChange {
    QString listId;
    QString operation;
    QJsonObject before;
    QJsonObject after;
};
// Called with the list locked, before publication. Throwing rejects the whole
// transaction. Cooperate with cancellation and do not reenter the same store.
using TaskCommitCallback = std::function<void(const TaskChange&, const CancellationToken&)>;
class IILOCALLLM_EXPORT TaskStore {
public:
    explicit TaskStore(QString directory, TaskStoreOptions = {});
    // Lists are host-selected namespaces, never filesystem paths. Independent
    // instances/processes coordinate through one lock and atomic file per list.
    QJsonObject snapshot(const QString& listId, const CancellationToken& = {}) const;
    // Permanently retires a host-owned namespace under the same board lock.
    // Removes task contents and prevents stale tools/processes from recreating it.
    void retire(const QString& listId,const CancellationToken& = {}) const;
    ToolResult execute(const QString& listId, const QString& operation, const QJsonObject& arguments = {},
        const CancellationToken& = {}, const TaskCommitCallback& = {}) const;
private:
    QString directory_;
    TaskStoreOptions options_;
};
IILOCALLLM_EXPORT QList<ToolDefinition> taskToolDefinitions(bool deferred = true);
// Empty listId binds each call to ToolContext.sessionId. A supplied listId is
// fixed by the host and cannot be overridden in model/tool arguments.
IILOCALLLM_EXPORT QList<Tool> taskTools(std::shared_ptr<TaskStore>, QString listId = {},
    bool deferred = true, TaskCommitCallback = {});
}
