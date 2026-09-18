#pragma once
#include "Tools.h"

namespace iiLocalLLM::agent {
struct WorktreeOptions {
    bool enabled=false, deferred=true, fetchMissingBase=true;
    QString directory; // Trusted host location; empty uses a sibling of the repository.
    QString gitProgram="git", baseRef;
    int commandTimeoutMs=30000, maxOutputBytes=4*1024*1024;
    QStringList sparsePaths;
    // Trusted host adapters for non-Git VCS; never supplied by a model or API caller.
    std::function<QString(const QString&,const ToolContext&)> create;
    std::function<void(const QString&,const ToolContext&)> remove;
};
struct WorktreeView {
    QString originalDirectory, directory;
    quint64 revision=0;
    QJsonObject state;
};
// Durable ownership and Git lifecycle. No process-wide cwd mutation occurs.
// Engine coordinates execution admission around transitions; tools are serial barriers.
class IILOCALLLM_EXPORT Worktrees {
public:
    Worktrees(QString stateDirectory,WorktreeOptions={});
    ~Worktrees();
    Worktrees(const Worktrees&)=delete;
    Worktrees& operator=(const Worktrees&)=delete;
    WorktreeView view(const ToolContext&) const;
    QJsonObject status(const ToolContext&) const;
    Tool enterTool(bool deferred=true) const;
    Tool exitTool(bool deferred=true) const;
    ToolResult enter(const QJsonObject&,const ToolContext&) const;
    ToolResult exit(const QJsonObject&,const ToolContext&) const;
private:
    class Impl;
    std::shared_ptr<Impl> d;
};
IILOCALLLM_EXPORT WorktreeOptions worktreeOptionsFromJson(const QJsonObject&);
}
