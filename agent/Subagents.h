#pragma once
#include "Engine.h"

namespace iiLocalLLM::agent {
struct SubagentDefinition {
    QString name = "general-purpose";
    QString description = "Execute a delegated task in a separate conversation.";
    QString systemPrompt;
    QString model; // Empty inherits the parent's model.
    QStringList tools = {"*"};
    QStringList disallowedTools;
    bool readOnly = false;
    int maxTurns = 32;
};
struct SubagentOptions {
    QString workingDirectory;
    QString stateDirectory; // Private, disjoint from workingDirectory.
    QList<SubagentDefinition> definitions; // Empty supplies general-purpose.
    QStringList allowedModels; // Additional model overrides authorized by the host.
    GenerationOptions generation;
    int maxConcurrent = 4;
    int maxRecords = 1024;
    int maxTurns = 32;
    int maxRuntimeMs = 300000;
    bool completionNotifications = true;
};
// Orchestration layer above Engine. Parent Engine receives tools() through
// EngineOptions::additionalTools; child engines do not receive delegation tools.
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
    QJsonObject output(const QString& parentSessionId, const QString& agentId,
        bool block = false, int timeoutMs = 30000, const CancellationToken& = {}) const;
    QJsonObject stop(const QString& parentSessionId, const QString& agentId, const CancellationToken& = {});
    QJsonArray list(const QString& parentSessionId) const;
    // Captures this object through shared ownership. The registry passed to the
    // constructor must not contain these tools (avoids recursive ownership).
    static QList<Tool> tools(std::shared_ptr<Subagents>);
    void close();
private:
    class Impl;
    std::unique_ptr<Impl> d;
};
}
