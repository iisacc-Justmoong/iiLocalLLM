#pragma once
#include "Types.h"

namespace iiLocalLLM::agent {
// Host-resolved document, including a legacy command Markdown file. A digest
// binds the selected cache revision; plugin variables expand only in the body.
struct SkillSource {
    QString name, root, path, sha256, pluginRoot, pluginData;
    QMap<QString, QString> agentAliases;
};
struct SkillOptions {
    bool enabled = true;
    bool includeProject = true;
    // Explicit skills directories, highest priority first. Never inferred from HOME.
    // Each directory contains <name>/SKILL.md; the workspace's .claude/skills is last.
    QStringList directories;
    QList<SkillSource> sources;
    int maxFileBytes = 128 * 1024;
    int maxTotalBytes = 512 * 1024;
    int maxSkills = 128;
    int maxScannedEntries = 4096;
};
struct IILOCALLLM_EXPORT SkillInfo {
    QString name, displayName, description, argumentHint, whenToUse, version;
    QString path, directory, sha256;
    QStringList argumentNames, unsupportedFeatures;
    bool disableModelInvocation = false, userInvocable = true;
    QString executionContext = "inline";
    QString agent, model; // Fork target; model aliases must be authorized by the host.
    QStringList allowedTools; // Requested invocation grants; host deny/ask and child scope remain authoritative.
    QJsonObject toJson() const;
};
struct IILOCALLLM_EXPORT SkillCatalog {
    QList<SkillInfo> skills;
    QJsonArray shadowed;
    QJsonObject toJson() const;
    Message message() const; // Metadata only, eligible model-invocable skills only.
};
enum class SkillInvocationSource { User, Model };
struct SkillForkRequest {
    Message prompt; // Already expanded in the calling session; never reread by the executor.
    GenerationOptions generation;
    int maxTurns = 32; // Further bounded by the child profile and host.
    QStringList contextPaths;
};
struct SkillForkResult {
    RunResult result;
    QJsonObject execution; // Child identity and final public execution record.
};
using SkillForkExecutor = std::function<SkillForkResult(const SkillForkRequest&, const ToolContext&)>;
// Fresh, bounded discovery. Bodies are read for validation but never included in the catalog.
// No ancestor/home scan, shell execution, network access or permission grants.
IILOCALLLM_EXPORT SkillCatalog discoverSkills(const QString& workingDirectory,
    const SkillOptions& = {}, const CancellationToken& = {});
// Returns a durable user message containing one expanded, immutable skill snapshot.
IILOCALLLM_EXPORT Message loadSkill(const QString& workingDirectory, const QString& name,
    const QString& arguments, const QString& sessionId, SkillInvocationSource,
    const SkillOptions& = {}, const CancellationToken& = {});
}
