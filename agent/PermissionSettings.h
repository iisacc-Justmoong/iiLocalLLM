#pragma once
#include "Tools.h"

namespace iiLocalLLM::agent {
struct PermissionSettingsOptions {
    QString workingDirectory;
    QString userDirectory, managedDirectory, homeDirectory; // Explicit host paths; never infer another app's configuration.
    QStringList enabledSources = {"user", "project", "local"}; // Flag and managed settings are always included.
    QStringList flagFiles;
    QJsonObject inlineSettings;
    PermissionMode fallbackMode = PermissionMode::Default;
    std::optional<PermissionMode> modeOverride;
    int maxFileBytes = 128 * 1024;
    int maxTotalBytes = 1024 * 1024;
    int maxFiles = 128;
};
struct IILOCALLLM_EXPORT PermissionSettingsSnapshot {
    PermissionMode mode = PermissionMode::Default;
    bool managedRulesOnly = false, bypassDisabled = false;
    QList<PermissionRule> rules;
    QJsonArray sources;
    QStringList unsupportedFeatures;
    QJsonObject toJson() const; // Permission metadata only; unrelated settings values are never disclosed.
};
class IILOCALLLM_EXPORT SettingsPermissionPolicy final : public PermissionPolicy {
public:
    explicit SettingsPermissionPolicy(PermissionSettingsOptions, QList<PermissionRule> cliRules = {},
        QList<PermissionRule> hostRules = {});
    PermissionSettingsSnapshot snapshot(const CancellationToken& = {}) const;
    PermissionDecision decide(const ToolDefinition&, const QJsonObject&, const ToolContext&) const override;
    QJsonObject describe(const ToolContext&) const override;
private:
    PermissionSettingsOptions options_;
    QList<PermissionRule> cliRules_, hostRules_;
};
}
