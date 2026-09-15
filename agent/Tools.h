#pragma once
#include "Types.h"
#include <optional>

namespace iiLocalLLM::agent {

struct PreparedTool {
    ToolDefinition definition; // Same identity/schemas; exact permission preview for this invocation.
    std::function<ToolResult()> execute; // Executes the snapshot whose preview was checked.
};
struct Tool {
    ToolDefinition definition;
    std::function<ToolResult(const QJsonObject&, const ToolContext&)> execute;
    std::function<void(const QJsonObject&, const ToolContext&)> validate;
    std::function<bool(const QJsonObject&)> canRunConcurrently;
    // Runs after input-changing hooks, before permission. Repeated if an approval
    // changes input or permissions. Must have no execution side effects.
    std::function<PreparedTool(const QJsonObject&, const ToolContext&)> prepare;
    // Optional trusted host lifecycle operation; never exported in tool schemas
    // or dispatchable by a model/MCP caller. Install on one control per owner.
    std::function<QJsonArray(const QString&,const QString&,const CancellationToken&)> transferSession;
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
    QJsonArray suggestions; // Host-supplied PermissionUpdate candidates, never grants by themselves.
};
struct PermissionRule {
    QString toolPattern;
    PermissionBehavior behavior = PermissionBehavior::Ask;
    QString source, rootDirectory, homeDirectory;
    bool settingsSyntax = false; // Source-rooted gitignore file patterns; native rules retain their existing syntax.
};
class IILOCALLLM_EXPORT PermissionPolicy {
public:
    virtual ~PermissionPolicy() = default;
    virtual PermissionDecision decide(const ToolDefinition&, const QJsonObject&, const ToolContext&) const = 0;
    virtual QJsonObject describe(const ToolContext&) const { return {{"provider","custom"},{"inspection_supported",false}}; }
    virtual QStringList workingDirectories(const ToolContext&) const;
    // Trusted host operations; no model/wire authority is implied. Immutable
    // policies reject updates. Inheritance must not call back into the Engine.
    virtual void applyUpdates(const QJsonArray&,const ToolContext&) const;
    virtual void inheritSession(const ToolContext&,const ToolContext&) const {}
    virtual void forgetSession(const ToolContext&) const {}
};
class IILOCALLLM_EXPORT RulePolicy final : public PermissionPolicy {
public:
    explicit RulePolicy(PermissionMode mode = PermissionMode::Default, QList<PermissionRule> rules = {});
    PermissionDecision decide(const ToolDefinition&, const QJsonObject&, const ToolContext&) const override;
    QJsonObject describe(const ToolContext&) const override;
private:
    PermissionMode mode_;
    QList<PermissionRule> rules_;
};
enum class HookKind { BeforeModel, AfterModel, BeforeTool, AfterTool, Stop, BeforeCompact, AfterCompact,
    TaskCreated, TaskCompleted, SubagentStart, SubagentStop, UserPromptSubmit, SessionStart, SessionEnd,
    PermissionRequest }; // Task lifecycle callbacks veto before the transaction commits.
struct PermissionResponse {
    PermissionBehavior behavior = PermissionBehavior::Deny; // Only Allow or Deny; Ask is invalid here.
    QString message;
    std::optional<QJsonObject> updatedArguments; // Allow only; revalidated and re-prepared before execution.
    QJsonArray updatedPermissions; // Allow only; requires a trusted host update handler.
    bool interrupt = false; // Deny only; cancels the owning run as well as rejecting this tool.
};
struct HookInput {
    HookKind kind;
    QString sessionId;
    QString runId;
    ToolCall call;
    ToolResult result;
    QString text;
    QJsonObject context; // Lifecycle identity, plus stop_hook_active on stop callbacks.
};
struct HookResult {
    bool block = false;
    QString feedback;
    std::optional<QJsonObject> updatedArguments;
    std::optional<PermissionDecision> permission; // Invocation-only; existing deny/ask rules and scope still apply.
    bool stop = false; // Stop the current run, distinct from requesting another Stop-hook turn.
    QString stopReason;
    QJsonArray diagnostics;
    std::optional<QString> initialUserMessage; // SessionStart schedules this through the normal prompt input queue.
    std::optional<PermissionResponse> permissionResponse; // PermissionRequest only; decision and payload stay together.
};
using Hook = std::function<HookResult(const HookInput&, const CancellationToken&)>;
using PermissionCallback = std::function<bool(const ToolCall&, const PermissionDecision&, const ToolContext&)>;
using PermissionResponseCallback = std::function<PermissionResponse(const ToolCall&,const PermissionDecision&,const ToolContext&)>;
using PermissionUpdateCallback = std::function<void(const QJsonArray&,const ToolContext&)>;
struct ToolRunnerOptions {
    QList<Hook> hooks;
    PermissionCallback permission;
    int maxResultCharacters = 24000;
    PermissionResponseCallback permissionResponse; // Used after request hooks, before the legacy bool callback.
    PermissionUpdateCallback permissionUpdates; // Trusted host operation; must apply all updates or throw.
    std::shared_ptr<PermissionRequests> permissionRequests; // Optional remote channel, racing local request handlers.
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
// Read, Write, Edit, Glob, Grep and Bash. File paths are restricted to the original
// workspace and the current policy's trusted working-directory snapshot.
// Bash is a permission-controlled process, not an OS sandbox.
IILOCALLLM_EXPORT void registerWorkspaceTools(ToolRegistry&, const QString& workspaceRoot);
class ShellTasks;
// Opt in to host-owned background Bash, TaskOutput, TaskStop and ShellTaskList.
IILOCALLLM_EXPORT void registerWorkspaceTools(ToolRegistry&, const QString& workspaceRoot, std::shared_ptr<ShellTasks>);
// Host-private files/directories remain inaccessible from workspace file/search tools,
// including through additional roots. Only this session's own artifacts/shell output can be read.
IILOCALLLM_EXPORT void registerWorkspaceTools(ToolRegistry&, const QString& workspaceRoot,
    std::shared_ptr<ShellTasks>, const QStringList& privatePaths);
}
