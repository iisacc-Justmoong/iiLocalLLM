#pragma once
#include "Types.h"

namespace iiLocalLLM::agent {
struct IILOCALLLM_EXPORT SubagentDefinition {
    QString name = "general-purpose";
    QString description = "Execute a delegated task in a separate conversation.";
    QString systemPrompt;
    QString model;
    QStringList tools = {"*"};
    QStringList disallowedTools;
    bool readOnly = false;
    int maxTurns = 32;
    QString initialPrompt;
    QStringList skills;
    bool background = false;
    QString permissionMode; // Inherit unless supplied; never widens the parent policy.
    QString source, path, directory, sha256;
    QStringList unsupportedFeatures;
    QJsonObject metadata;
    QJsonObject toJson(bool includePrompt = false) const;
};
struct AgentProfileSource {
    QString name, root, path, sha256, pluginRoot, pluginData;
    QMap<QString, QString> skillAliases;
};
struct AgentProfileOptions {
    bool enabled = true;
    bool includeBuiltins = true;
    bool includeProject = true;
    QString projectBoundary; // Empty: nearest .git, home, or filesystem root.
    QString userDirectory; // Explicit host path; the library never infers HOME.
    QString managedDirectory;
    QStringList pluginDirectories;
    QList<AgentProfileSource> pluginSources;
    QStringList directories; // Flag-scope directories, highest priority first.
    QJsonObject overrides; // Name -> JSON definition; same scope as CLI --agents.
    int maxFileBytes = 128 * 1024;
    int maxTotalBytes = 1024 * 1024;
    int maxProfiles = 256;
    int maxScannedEntries = 4096;
};
struct IILOCALLLM_EXPORT AgentProfileCatalog {
    QList<SubagentDefinition> profiles;
    QJsonArray shadowed;
    QJsonArray failedFiles;
    const SubagentDefinition& find(const QString& name) const;
    QJsonObject toJson() const;
};
// Metadata is fresh for each discovery. File bodies and their hashes are copied
// into the selected execution record, so in-flight execution stays immutable.
IILOCALLLM_EXPORT AgentProfileCatalog discoverAgentProfiles(const QString& workingDirectory,
    const AgentProfileOptions& = {}, const QList<SubagentDefinition>& hostDefinitions = {},
    const CancellationToken& = {});
}
