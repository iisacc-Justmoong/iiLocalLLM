#pragma once
#include "Tools.h"

namespace iiLocalLLM::agent {
struct ShellTaskOptions {
    int maxConcurrent = 8;
    int maxRecords = 1000;
    qint64 maxOutputBytes = 8 * 1024 * 1024;
    int maxRuntimeMs = 24 * 60 * 60 * 1000;
};
// Host-owned execution store. Last-owner destruction stops and joins all its
// workers. A successful start transfers lifetime away from the caller's token.
class IILOCALLLM_EXPORT ShellTasks {
public:
    ShellTasks(QString workspace, QString stateDirectory, ShellTaskOptions = {});
    ~ShellTasks();
    ShellTasks(const ShellTasks&) = delete;
    ShellTasks& operator=(const ShellTasks&) = delete;
    QString workspace() const;
    QJsonObject start(const ToolContext&, QString command, QString description = {}, int timeoutMs = 3600000);
    QJsonObject output(const QString& sessionId, const QString& taskId, bool block = true, int timeoutMs = 30000,
        qint64 offset = 0, int limitBytes = 24576, const CancellationToken& = {}) const;
    QJsonObject stop(const QString& sessionId, const QString& taskId, const CancellationToken& = {});
    QJsonArray list(const QString& sessionId, int offset = 0, int limit = 100) const;
    bool ownsOutput(const QString& sessionId, const QString& path) const;
    bool containsStatePath(const QString& path) const;
    void close();
private:
    class Impl;
    std::unique_ptr<Impl> d;
};
IILOCALLLM_EXPORT void registerShellTaskControls(ToolRegistry&, std::shared_ptr<ShellTasks>, bool deferred = true);
}
