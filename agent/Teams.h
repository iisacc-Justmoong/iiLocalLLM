#pragma once
#include "Engine.h"
#include "AgentProfiles.h"

namespace iiLocalLLM::agent {
struct TeamsOptions {
    QString workingDirectory;
    QList<SubagentDefinition> definitions;
    AgentProfileOptions profiles{false};
    QStringList allowedModels;
    QMap<QString, QString> modelAliases;
    GenerationOptions generation;
    int maxTeams = 32;
    int maxMembers = 32; // Retained members across all teams in this coordinator.
    int maxTurns = 32;
    int maxRuntimeMs = 300000;
    int maxRunsPerMember = 64;
    int maxMailboxMessages = 256;
};

// Orchestration above Engine. State lives in parentOptions.sessionsDirectory/teams;
// members use independent conversations, shared task namespaces and the host model.
// Keep this owner alive until the parent Engine is closed. close() cancels and joins
// accepted workers; never call close/destroy from a member model or tool callback.
class IILOCALLLM_EXPORT Teams {
public:
    Teams(std::shared_ptr<Model>, std::shared_ptr<ToolRegistry>,
        std::shared_ptr<const PermissionPolicy>, EngineOptions parentOptions, TeamsOptions);
    ~Teams();
    Teams(const Teams&) = delete;
    Teams& operator=(const Teams&) = delete;

    ToolResult create(const ToolContext&, const QJsonObject&);
    ToolResult spawn(const ToolContext&, const QJsonObject&);
    ToolResult send(const ToolContext&, const QJsonObject&);
    ToolResult remove(const ToolContext&);
    QJsonObject status(const QString& sessionId) const;
    QJsonObject inbox(const QString& sessionId, int offset = 0, int limit = 100) const;
    QJsonObject stop(const QString& leaderSessionId, const QString& name);
    QJsonObject wait(const QString& sessionId, int timeoutMs = 30000,
        const CancellationToken& = {}) const;

    QString taskList(const QString& sessionId) const;
    std::shared_ptr<TaskStore> taskStore() const;
    static void attach(EngineOptions&, std::shared_ptr<Teams>);
    void close();
private:
    class Impl;
    std::unique_ptr<Impl> d;
    static QList<Tool> tools(std::weak_ptr<Teams>, bool leaderTools = true);
};
}
