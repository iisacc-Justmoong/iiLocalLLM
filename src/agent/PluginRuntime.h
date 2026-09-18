#pragma once
#include "Plugins.h"
#include "CommandHooks.h"
#include "McpConnections.h"

namespace iiLocalLLM::agent {
struct EngineOptions;
// Host composition layer above the execution engines. Construct and attach once
// before creating Engine/Subagents/Teams/McpConnections. Children inherit the
// composed options; they never discover or re-add tools after profile filtering.
class IILOCALLLM_EXPORT PluginRuntime {
public:
    PluginRuntime(PluginSnapshot, CommandHookOptions);
    static void attach(EngineOptions&, AgentProfileOptions&, McpConnectionOptions&,
        std::shared_ptr<PluginRuntime>);
    QJsonObject status() const;
private:
    PluginSnapshot snapshot;
    QString workspace;
    QList<Hook> callbacks;
};
}
