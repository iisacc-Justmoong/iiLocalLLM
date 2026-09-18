#pragma once
#include "Engine.h"
#include "AgentProfiles.h"

namespace iiLocalLLM::agent {
struct SubagentOptions {
    QString workingDirectory;
    QString stateDirectory; // Private, disjoint from workingDirectory.
    QList<SubagentDefinition> definitions; // Empty supplies general-purpose when profiles are disabled.
    QStringList allowedModels; // Additional model overrides authorized by the host.
    GenerationOptions generation;
    int maxConcurrent = 4;
    int maxRecords = 1024;
    int maxTurns = 32;
    int maxRuntimeMs = 300000;
    bool completionNotifications = true;
    AgentProfileOptions profiles{false}; // Embedded hosts opt in to file discovery.
    QMap<QString, QString> modelAliases; // Host-authorized alias -> actual local model URI.
};
// Orchestration layer above Engine. Parent Engine receives tools() through
// EngineOptions through attach(); child engines do not receive delegation tools.
// Last-owner destruction cancels and joins accepted workers. Never close or
// destroy this object from a child model/tool/progress callback.
class IILOCALLLM_EXPORT Subagents {
public:
    Subagents(std::shared_ptr<Model>, std::shared_ptr<ToolRegistry>,
        std::shared_ptr<const PermissionPolicy>, EngineOptions parentOptions, SubagentOptions);
    ~Subagents();
    Subagents(const Subagents&) = delete;
    Subagents& operator=(const Subagents&) = delete;
    ToolResult run(const ToolContext&, const QJsonObject&);
    SkillForkResult runSkill(const SkillForkRequest&, const ToolContext&);
    QJsonObject output(const QString& parentSessionId, const QString& agentId,
        bool block = false, int timeoutMs = 30000, const CancellationToken& = {}) const;
    QJsonObject stop(const QString& parentSessionId, const QString& agentId, const CancellationToken& = {});
    QJsonArray list(const QString& parentSessionId) const;
    // Trusted lifecycle operation. Running/completed background children retain
    // their IDs, execution and transcripts. Pending completion input follows.
    // Ownership commits before notification migration; failures can be retried.
    QJsonArray transferSession(const QString& from,const QString& to,const CancellationToken& = {});
    AgentProfileCatalog profiles(const CancellationToken& = {}) const;
    // Captures this object through shared ownership. The registry passed to the
    // constructor must not contain these tools (avoids recursive ownership).
    static QList<Tool> tools(std::shared_ptr<Subagents>);
    // Installs static state controls and a live Agent definition provider.
    // Call after constructing the owner, before constructing its parent Engine.
    static void attach(EngineOptions&, std::shared_ptr<Subagents>);
    void close();
private:
    class Impl;
    std::unique_ptr<Impl> d;
    static QList<Tool> makeTools(std::shared_ptr<Subagents>, bool includeAgent);
    ToolResult runImpl(const ToolContext&, const QJsonObject&, const SkillForkRequest*, RunResult*);
};
}
