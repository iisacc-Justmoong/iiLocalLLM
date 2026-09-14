#pragma once
#include "Skills.h"
#include "Tools.h"
#include "SessionStore.h"
#include "ProjectContext.h"
#include "Compaction.h"
#include "ToolSearch.h"
#include "TaskStore.h"
#include "InputQueue.h"
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
};
class IILOCALLLM_EXPORT Engine {
public:
    Engine(std::shared_ptr<Model>, std::shared_ptr<ToolRegistry>,
        std::shared_ptr<const PermissionPolicy>, EngineOptions);
    // Cancels and joins accepted runs. Do not destroy/wait from an event callback.
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Session createSession(QString model, QString workspace, QString systemPrompt = {});
    Session session(const QString& id) const;
    Session sessionMetadata(const QString& id) const;
    SkillCatalog skills(const QString& sessionId, const CancellationToken& = {}) const;
    QStringList sessions() const;
    Session forkSession(const QString& id, const QString& throughMessageId = {});
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
    ToolResult runShellTool(const QString& sessionId, const QString& name, const QJsonObject& arguments = {},
        const CancellationToken& = {}, const EventCallback& = {}) const;
    // Uses the same policy and hooks as model calls. Available during an active
    // run; the task transaction uses a separate lock from the transcript lease.
    ToolResult runTaskTool(const QString& sessionId, const QString& name, const QJsonObject& arguments = {},
        const CancellationToken& = {}, const EventCallback& = {}) const;
private:
    RunHandle submit(RunRequest, EventCallback, bool compactOnly, QString instructions = {}, bool queuedOnly = false);
    class Impl;
    std::unique_ptr<Impl> d;
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
