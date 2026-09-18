#pragma once
#include "../Types.h"
#include <QtCore/QJsonArray>
#include <functional>

namespace iiLocalLLM::agent {
class CommandHooks;
// Host-owned command-hook lifetime and bounded completion records. Completion
// runs on the process worker, outside this scope's lock. It must not close this
// scope or destroy its CommandHooks owner. Model/wire data cannot supply it.
class IILOCALLLM_EXPORT AsyncHookScope {
public:
    explicit AsyncHookScope(std::function<void(const QJsonObject&)> completion = {}, int maxRecords = 128);
    ~AsyncHookScope();
    AsyncHookScope(const AsyncHookScope&) = delete;
    AsyncHookScope& operator=(const AsyncHookScope&) = delete;
    QJsonArray status(const QString& sessionId = {}) const;
    QJsonArray takeCompleted(const QString& sessionId = {});
    int cancel(const QString& hookId = {}); // Empty cancels every currently running command in this scope.
    void close(); // Reject new work, cancel processes, wait for cleanup and completion callbacks.
private:
    friend class CommandHooks;
    void attach(const QString&,const QJsonObject&,const CancellationToken&);
    void background(const QString&,int asyncTimeoutMs);
    void finish(const QString&,QJsonObject);
    class Impl;
    std::unique_ptr<Impl> d;
};
}
