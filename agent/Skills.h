#pragma once
#include "Types.h"

namespace iiLocalLLM::agent {
struct SkillOptions {
    bool enabled = true;
    // Explicit skills directories, highest priority first. Never inferred from HOME.
    // Each directory contains <name>/SKILL.md; the workspace's .claude/skills is last.
    QStringList directories;
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
    QJsonObject toJson() const;
};
struct IILOCALLLM_EXPORT SkillCatalog {
    QList<SkillInfo> skills;
    QJsonArray shadowed;
    QJsonObject toJson() const;
    Message message() const; // Metadata only, eligible model-invocable skills only.
};
enum class SkillInvocationSource { User, Model };
// Fresh, bounded discovery. Bodies are read for validation but never included in the catalog.
// No ancestor/home scan, shell execution, network access or permission grants.
IILOCALLLM_EXPORT SkillCatalog discoverSkills(const QString& workingDirectory,
    const SkillOptions& = {}, const CancellationToken& = {});
// Returns a durable user message containing one expanded, immutable skill snapshot.
IILOCALLLM_EXPORT Message loadSkill(const QString& workingDirectory, const QString& name,
    const QString& arguments, const QString& sessionId, SkillInvocationSource,
    const SkillOptions& = {}, const CancellationToken& = {});
}
