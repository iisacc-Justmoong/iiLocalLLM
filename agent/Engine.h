#pragma once
#include "Skills.h"
#include "Tools.h"
#include "SessionStore.h"
#include "ProjectContext.h"
#include "Compaction.h"
#include "ToolSearch.h"
#include "TaskStore.h"
#include "InputQueue.h"
#include "AsyncHooks.h"
#include "PlanMode.h"
#include "UserQuestions.h"
#include "ProjectMemory.h"
#include "MemoryRecall.h"
#include "MemoryExtraction.h"
#include "SessionHistory.h"
#include "MemoryDream.h"
#include "WebFetch.h"
#include "Lsp.h"
#include "Worktrees.h"
#include "FileCheckpoints.h"
#include "../Service.h"

namespace iiLocalLLM::agent {
struct EngineOptions {
    QString sessionsDirectory;
    int maxConcurrentRuns = 4;
    int maxQueuedRuns = 32;
    int maxConcurrentTools = 10;
    int maxToolCallsPerTurn = 64;
    int maxInputCharacters = 1024 * 1024;
    QList<Hook> hooks;
    PermissionCallback permission;
    ProjectContextOptions projectContext;
    CompactionOptions compaction;
    ToolSearchOptions toolSearch;
    bool taskToolsEnabled = false; // Opt in for embedded hosts; daemon/MCP CLI enable it by default.
    bool taskToolsDeferred = true;
    InputQueueOptions inputQueue;
    SkillOptions skills;
    QList<Tool> additionalTools; // Host-owned orchestration tools; merged into each live registry snapshot.
    std::function<bool(const ToolDefinition&)> toolFilter; // Applied before and after deferred discovery.
    // Invoked outside engine locks, before each model turn and native dispatch.
    // May add live host definitions; duplicates with any other source are errors.
    std::function<QList<Tool>()> additionalToolsProvider;
    SkillForkExecutor forkedSkill; // Synchronous child execution, supplied by Subagents::attach().
    bool sessionStartHooks = true; // Main SessionStart/End; delegated engines use SubagentStart/Stop instead.
    int sessionEndTimeoutMs = 1500; // Shared cooperative budget for this session's end hooks.
    PermissionResponseCallback permissionResponse;
    PermissionUpdateCallback permissionUpdates;
    std::shared_ptr<PermissionRequests> permissionRequests;
    int maxAsyncHookRecords = 128;
    int maxAsyncHookWakeRuns = 8; // Per explicit run; 0 disables automatic wake, retaining queued context.
    bool planToolsEnabled = false; // Embedded hosts opt in; daemon and agent-enabled MCP default to enabled.
    bool planToolsDeferred = true;
    bool userQuestionsEnabled = false; // Embedded hosts opt in; daemon/agent MCP enable by default.
    UserQuestionOptions userQuestions;
    bool projectMemoryEnabled = false; // Embedded hosts opt in; daemon and agent MCP enable by default.
    ProjectMemoryOptions projectMemory; // Empty directory uses sessionsDirectory/memory.
    MemoryRecallOptions memoryRecall; // Automatic prefetch and explicit host recall; no external provider is introduced.
    MemoryExtractionOptions memoryExtraction; // Isolated automatic maintenance after a main-agent response.
    bool sessionHistoryEnabled = false; // Embedded opt-in; daemon and agent MCP enable it by default.
    SessionHistoryOptions sessionHistory; // Uses this Engine's sessionsDirectory; no external path from a caller.
    bool webFetchEnabled = false; // Embedded opt-in; agent-enabled daemon/MCP expose it by default.
    WebFetchOptions webFetch;
    WorktreeOptions worktrees; // Embedded hosts opt in; daemon/agent MCP enable by default.
    LspOptions lsp; // Explicit trusted server configuration; empty disables LSP.
    MemoryDreamOptions memoryDream; // Available with memory + history; automatic scheduling is opt-in.
    bool fileCheckpointsEnabled = false; // Embedded opt-in; daemon and agent MCP enable by default.
};
class IILOCALLLM_EXPORT Engine {
public:
    Engine(std::shared_ptr<Model>, std::shared_ptr<ToolRegistry>,
        std::shared_ptr<const PermissionPolicy>, EngineOptions);
    // Calls close(). Do not destroy, close or end a session from its hook/event callback.
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Session createSession(QString model, QString workspace, QString systemPrompt = {});
    Session session(const QString& id) const;
    Session sessionMetadata(const QString& id) const;
    QString transcriptPath(const QString& sessionId) const;
    std::shared_ptr<Model> hookModel() const; // C++ host binding for direct MCP/ToolRunner hooks; not a wire capability.
    AgentHookExecutor hookAgent() const; // Owns its captured host configuration; does not re-enter this Engine's session lock.
    std::shared_ptr<AsyncHookScope> hookScope(const QString& sessionId) const; // Host binding for direct MCP hooks.
    QJsonObject hookStatus(const QString& sessionId, int offset = 0, int limit = 32) const;
    // With no hookId, also suppress queued wakes and cancel an active automatic
    // wake run. A subsequent explicit run re-enables wakes with a fresh budget.
    QJsonObject cancelHooks(const QString& sessionId, const QString& hookId = {}) const;
    SkillCatalog skills(const QString& sessionId, const CancellationToken& = {}) const;
    QJsonObject permissions(const QString& sessionId, const CancellationToken& = {}) const;
    bool projectMemoryEnabled() const;
    QJsonObject memory(const QString& sessionId,const QString& query = {},const CancellationToken& = {}) const;
    bool memoryRecallEnabled() const;
    // Explicit recall returns notes without appending them to the conversation.
    QJsonObject recallMemory(const QString& sessionId,const QString& query,const CancellationToken& = {}) const;
    bool memoryExtractionEnabled() const;
    QJsonObject extractMemory(const QString& sessionId,const CancellationToken& = {}) const;
    QJsonObject memoryExtractionStatus(const QString& sessionId,int offset = 0,int limit = 32) const;
    QJsonObject cancelMemoryExtraction(const QString& sessionId) const;
    bool drainMemoryExtractions(int timeoutMs,const QString& sessionId = {},const CancellationToken& = {}) const;
    bool memoryDreamAvailable() const;
    bool automaticMemoryDream() const;
    QJsonObject consolidateMemory(const QString& sessionId,const CancellationToken& = {}) const;
    QJsonObject memoryDreamStatus(const QString& sessionId,int offset=0,int limit=8) const;
    QJsonObject cancelMemoryDream(const QString& sessionId) const;
    bool drainMemoryDreams(int timeoutMs,const QString& sessionId={},const CancellationToken& = {}) const;
    // Host registry composition; preserves the original workspace tool schemas.
    void bindProjectMemoryTools(ToolRegistry&,bool deferredForget = true) const;
    ToolResult runMemoryTool(const QString& sessionId,const QString& name,const QJsonObject& arguments = {},
        const CancellationToken& = {},const EventCallback& = {},std::shared_ptr<PermissionRequests> = {}) const;
    std::optional<Tool> webFetchTool(bool deferred = false) const;
    ToolResult runWebFetch(const QString& sessionId,const QJsonObject&,const CancellationToken& = {},
        const EventCallback& = {},std::shared_ptr<PermissionRequests> = {}) const;
    std::optional<Tool> lspTool(bool deferred=false) const;
    ToolResult runLsp(const QString& sessionId,const QJsonObject&,const CancellationToken& = {},
        const EventCallback& = {},std::shared_ptr<PermissionRequests> = {}) const;
    QJsonObject lspStatus(const QString& sessionId,const CancellationToken& = {}) const;
    bool notebookToolsEnabled() const;
    bool fileCheckpointsEnabled() const;
    QJsonObject fileCheckpoints(const QString& sessionId,const CancellationToken& = {}) const;
    QJsonObject checkpointFiles(const QString& sessionId,const CancellationToken& = {}) const;
    ToolResult rewindFiles(const QString& sessionId,const QString& messageId,bool dryRun=false,
        const CancellationToken& = {},const EventCallback& = {},std::shared_ptr<PermissionRequests> = {}) const;
    // Read accepts notebook_path plus optional offset/limit; NotebookEdit uses
    // its native arguments. A session lease binds observations to compaction.
    ToolResult runNotebookTool(const QString& sessionId,const QString& name,const QJsonObject&,
        const CancellationToken& = {},const EventCallback& = {},std::shared_ptr<PermissionRequests> = {}) const;
    bool worktreesEnabled() const;
    std::optional<Tool> worktreeTool(const QString& name,bool deferred=false) const;
    QJsonObject worktreeStatus(const QString& sessionId,const CancellationToken& = {}) const;
    ToolResult runWorktreeTool(const QString& sessionId,const QString& name,const QJsonObject&,
        const CancellationToken& = {},const EventCallback& = {},std::shared_ptr<PermissionRequests> = {}) const;
    // Trusted direct-tool/MCP scope. With worktrees enabled or leaseSession set,
    // retains admission and a transcript lease. Rejects a busy run and binds the
    // context revision so file observations expire after compaction.
    std::shared_ptr<void> bindWorkspaceContext(ToolContext&) const;
    std::shared_ptr<void> bindWorkspaceContext(ToolContext&,bool leaseSession) const;
    QStringList sessions() const;
    std::optional<Tool> sessionSearchTool(bool deferred = false) const;
    ToolResult runSessionSearch(const QString& ownerSessionId,const QJsonObject& arguments,
        const CancellationToken& = {},const EventCallback& = {},std::shared_ptr<PermissionRequests> = {}) const;
    Session forkSession(const QString& id, const QString& throughMessageId = {});
    // Cancel/join this session's accepted work, stop its native background jobs,
    // then run non-vetoing SessionEnd hooks. Retains history; a later run resumes.
    // Cancellation is checked before admission; admitted cleanup owns its token.
    QJsonObject endSession(const QString& id, QString reason = "other", const CancellationToken& = {});
    // Ends this activation with reason clear, preserves background jobs, creates
    // an empty session and runs SessionStart(clear) immediately. Old history and
    // pending user input remain addressable under the previous ID. Check complete
    // and diagnostics: independent owners/files are not one atomic transaction.
    QJsonObject clearSession(const QString& id,const CancellationToken& = {});
    // Stops admission and closes all sessions touched by this Engine. Idempotent.
    QJsonArray close(QString reason = "other");
    ProjectContext context(const QString& sessionId, const QStringList& targetPaths = {}, const CancellationToken& = {}) const;
    RunHandle run(RunRequest, EventCallback = {});
    RunHandle compact(CompactRequest, EventCallback = {});
    QJsonObject enqueueInput(const QString& sessionId, const QJsonObject&, const CancellationToken& = {});
    QJsonObject queuedInputs(const QString& sessionId, int offset = 0, int limit = 100, const CancellationToken& = {}) const;
    QJsonObject removeInput(const QString& sessionId, const QString& inputId, const CancellationToken& = {}) const;
    // Start an idle session from queued input. prompt must be empty; generation,
    // maxTurns and contextPaths use the same contract as run().
    RunHandle runQueued(RunRequest, EventCallback = {});
    bool taskToolsEnabled() const;
    bool backgroundTasksEnabled() const;
    bool subagentsEnabled() const;
    QList<ToolDefinition> subagentToolDefinitions() const;
    bool permissionRequestsEnabled() const;
    std::optional<Tool> userQuestionTool(bool deferred = false) const;
    ToolResult runQuestionTool(const QString& sessionId,const QJsonObject& arguments,
        const CancellationToken& = {},const EventCallback& = {},std::shared_ptr<PermissionRequests> = {}) const;
    std::shared_ptr<PlanMode> planning() const; // Trusted host/MCP binding; null when disabled.
    QJsonObject planStatus(const QString& sessionId,const CancellationToken& = {}) const;
    ToolResult runPlanTool(const QString& sessionId,const QString& name,const QJsonObject& arguments = {},
        const CancellationToken& = {},const EventCallback& = {},std::shared_ptr<PermissionRequests> = {}) const;
    void stopSubagents(const QString& sessionId) const; // Host lifecycle cleanup, independent of model permissions.
    ToolResult runSubagentTool(const QString& sessionId, const QString& name, const QJsonObject& arguments = {},
        const CancellationToken& = {}, const EventCallback& = {}, std::shared_ptr<PermissionRequests> = {}) const;
    ToolResult runShellTool(const QString& sessionId, const QString& name, const QJsonObject& arguments = {},
        const CancellationToken& = {}, const EventCallback& = {}, std::shared_ptr<PermissionRequests> = {}) const;
    // Uses the same policy and hooks as model calls. Available during an active
    // run; the task transaction uses a separate lock from the transcript lease.
    ToolResult runTaskTool(const QString& sessionId, const QString& name, const QJsonObject& arguments = {},
        const CancellationToken& = {}, const EventCallback& = {}, std::shared_ptr<PermissionRequests> = {}) const;
private:
    QJsonObject endSessionImpl(const QString&,QString,const CancellationToken&,bool clear);
    RunHandle submit(RunRequest, EventCallback, bool compactOnly, QString instructions = {}, bool queuedOnly = false);
    class Impl;
    std::shared_ptr<Impl> d;
};
// Native structured conversation adapter. Unsupported runtimes fail explicitly.
// Uses the existing Service scheduler, model residency policy, and bounded KV cache.
class IILOCALLLM_EXPORT ServiceModel final : public Model {
public:
    explicit ServiceModel(Service& service);
    ModelReply generate(const ModelRequest&, const CancellationToken&, const TextCallback&) override;
    std::optional<ContextBudget> measure(const ModelRequest&, const CancellationToken&) override;
private:
    Service& service_;
};
}
