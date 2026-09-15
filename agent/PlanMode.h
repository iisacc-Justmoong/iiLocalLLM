#pragma once
#include "Tools.h"

namespace iiLocalLLM::agent {
// Session-owned planning state. The host supplies a private directory; no model
// argument chooses a state path, owner, restored mode or approval decision.
class IILOCALLLM_EXPORT PlanMode {
public:
    explicit PlanMode(QString directory, int maxPlanBytes = 65536);
    QJsonObject status(const QString& sessionId, const CancellationToken& = {}) const;
    ToolContext scope(ToolContext, const PermissionPolicy&) const;
    // ToolRunner calls this after permission and observers, immediately around
    // execution. Transitions exclude in-flight tools of this owner; reads may
    // still run concurrently. No lock is held while waiting for a review.
    ToolResult execute(const ToolContext&,const ToolDefinition&,const std::function<ToolResult()>&) const;
    QList<Tool> tools(std::shared_ptr<const PermissionPolicy>, bool deferred = true) const;
    // A fork gets its own file and must obtain a new approval. Clear uses a new
    // owner with no copied plan. Session end retains the original plan.
    void fork(const QString& from, const QString& to, const CancellationToken& = {}) const;
private:
    class Impl;
    std::shared_ptr<Impl> d;
};
}
