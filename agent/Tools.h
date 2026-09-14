#pragma once
#include "Types.h"
#include <optional>

namespace iiLocalLLM::agent {

struct Tool {
    ToolDefinition definition;
    std::function<ToolResult(const QJsonObject&, const ToolContext&)> execute;
    std::function<void(const QJsonObject&, const ToolContext&)> validate;
    std::function<bool(const QJsonObject&)> canRunConcurrently;
};
class IILOCALLLM_EXPORT ToolRegistry {
public:
    ToolRegistry();
    ~ToolRegistry();
    ToolRegistry(const ToolRegistry&) = delete;
    ToolRegistry& operator=(const ToolRegistry&) = delete;
    void add(Tool tool);
    void remove(const QString& name);
    // Validate first, then atomically replace an owned set. Collisions with
    // retained tools leave the registry unchanged. Existing snapshots survive.
    void replace(const QStringList& removeNames, QList<Tool> additions);
    QList<ToolDefinition> definitions(bool includeDeferred = true) const;
    Tool get(const QString& name) const;
    // Freeze definitions, validators and handlers together for one model/tool turn.
    std::shared_ptr<ToolRegistry> snapshot() const;
    void validateInput(const QString& name, const QJsonObject& arguments) const;
    void validateOutput(const QString& name, const QJsonObject& output) const;
private:
    friend class ToolRunner;
    struct Entry;
    std::shared_ptr<const Entry> resolve(const QString& name) const;
    class Impl;
    std::unique_ptr<Impl> d;
};

enum class PermissionMode { Default, AcceptEdits, DontAsk, Bypass, Plan };
enum class PermissionBehavior { Allow, Deny, Ask };
struct PermissionDecision {
    PermissionBehavior behavior = PermissionBehavior::Ask;
    QString reason;
};
struct PermissionRule {
    QString toolPattern;
    PermissionBehavior behavior = PermissionBehavior::Ask;
};
class IILOCALLLM_EXPORT PermissionPolicy {
public:
    virtual ~PermissionPolicy() = default;
    virtual PermissionDecision decide(const ToolDefinition&, const QJsonObject&, const ToolContext&) const = 0;
};
class IILOCALLLM_EXPORT RulePolicy final : public PermissionPolicy {
public:
    explicit RulePolicy(PermissionMode mode = PermissionMode::Default, QList<PermissionRule> rules = {});
    PermissionDecision decide(const ToolDefinition&, const QJsonObject&, const ToolContext&) const override;
private:
    PermissionMode mode_;
    QList<PermissionRule> rules_;
};
enum class HookKind { BeforeModel, AfterModel, BeforeTool, AfterTool, Stop, BeforeCompact, AfterCompact };
struct HookInput {
    HookKind kind;
    QString sessionId;
    QString runId;
    ToolCall call;
    ToolResult result;
    QString text;
};
struct HookResult {
    bool block = false;
    QString feedback;
    std::optional<QJsonObject> updatedArguments;
};
using Hook = std::function<HookResult(const HookInput&, const CancellationToken&)>;
using PermissionCallback = std::function<bool(const ToolCall&, const PermissionDecision&, const ToolContext&)>;
struct ToolRunnerOptions {
    QList<Hook> hooks;
    PermissionCallback permission;
    int maxResultCharacters = 24000;
};
class IILOCALLLM_EXPORT ToolRunner {
public:
    ToolRunner(std::shared_ptr<ToolRegistry>, std::shared_ptr<const PermissionPolicy>, ToolRunnerOptions = {});
    ToolResult run(ToolCall call, const ToolContext&, const EventCallback& = {}) const;
    bool concurrencySafe(const ToolCall&) const;
private:
    std::shared_ptr<ToolRegistry> registry_;
    std::shared_ptr<const PermissionPolicy> policy_;
    ToolRunnerOptions options_;
};
// Read, Write, Edit, Glob, Grep and Bash. Paths are restricted to the canonical workspace.
// Bash is a permission-controlled process, not an OS sandbox.
IILOCALLLM_EXPORT void registerWorkspaceTools(ToolRegistry&, const QString& workspaceRoot);
}
