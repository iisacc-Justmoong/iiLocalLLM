#pragma once
#include "../Types.h"
#include <QtCore/QJsonArray>
#include <optional>

namespace iiLocalLLM::agent {
struct Session;
class PermissionRequests;
enum class PermissionMode { Default, AcceptEdits, DontAsk, Bypass, Plan };

enum class MessageRole { User, Assistant, Tool };
enum class RunStatus { Completed, Cancelled, TurnLimit, Failed };
enum class EventKind { Started, ModelDelta, Message, ToolStarted, ToolProgress, ToolFinished,
    PermissionRequested, Hook, Finished, InstructionsLoaded, CompactionStarted, CompactionProgress, Compacted,
    InputDelivered, Interrupted, PermissionResolved, MemoryRecall, MemoryExtraction, MemoryDream };
struct ToolCall {
    QString id;
    QString name;
    QJsonObject arguments;
    bool operator==(const ToolCall&) const = default;
};
struct Message {
    QString id;
    MessageRole role = MessageRole::User;
    QString text;
    QList<ToolCall> toolCalls;
    QString toolCallId;
    bool isError = false;
    QJsonObject data;
    QJsonArray content;
    QJsonObject metadata;
    bool operator==(const Message&) const = default;
};
struct ToolDefinition {
    QString name;
    QString description;
    QJsonObject inputSchema;
    QJsonObject outputSchema;
    bool readOnly = false;
    bool concurrencySafe = false;
    bool editsFiles = false;
    bool deferred = false;
    QJsonObject metadata;
};
struct ToolResult {
    QString text;
    QJsonObject data;
    bool isError = false;
    QJsonArray content;
    QJsonObject metadata;
};
struct ToolContext {
    QString sessionId;
    QString runId;
    QString workingDirectory;
    QString artifactsDirectory;
    CancellationToken cancellation;
    std::function<void(const QJsonObject&)> progress;
    quint64 contextRevision = 0; // Read-before-edit observations expire after compaction.
    std::shared_ptr<const Session> sessionSnapshot; // Immutable parent context at the tool batch boundary.
    QStringList allowedTools; // Trusted invocation grants, never restored from transcript metadata.
    QStringList workingDirectories; // Host policy snapshot, replaced by ToolRunner before preparation; never accepted from wire input.
    QString transcriptPath; // Host-owned transcript location for hooks; empty for standalone tool calls.
    std::shared_ptr<PermissionRequests> permissionRequests; // Trusted channel override; never accepted from wire input.
    std::optional<PermissionMode> permissionMode; // Trusted invocation override, used by isolated verification agents.
    bool verificationAgent = false; // Host-only scope for the verifier's reserved result tool.
    std::shared_ptr<class AsyncHookScope> asyncHooks; // Host lifetime/delivery; never a model or wire field.
    std::optional<CancellationToken> hookCancellation; // Hard run cancellation; foreground input interrupts stay separate.
    bool forceSynchronousHooks = false;
    QString planFilePath, plansDirectory; // Host-owned paths; only this exact plan file is accessible.
    bool planModeActive = false;
    int maxPlanBytes = 65536;
    QString planningSessionId; // MCP owner binding; never accepted from model or wire input.
    QJsonObject approvedToolPreview; // Original prepared metadata, populated only after a trusted Allow response.
    QString planningState; // Admission snapshot used by the planning execution barrier.
    QStringList protectedPaths; // Host-only file/search exclusions; never accepted from model or wire metadata.
    int maxReadBytes = 1024 * 1024; // Host excerpt budget; partial reads never authorize an edit.
    QString expectedReadSha256; // Optional host snapshot check, applied before recording a read.
    bool readOnlyShell = false; // Host-only native classifier + fixed environment; never accepted from model/wire input.
    // Trusted Engine binding, never decoded from arguments, transcripts or MCP.
    QString originalWorkingDirectory;
    quint64 workspaceRevision = 0;
    QString fileCheckpointId; // Accepted user-message ID; empty direct edits get a fresh checkpoint.
    std::function<void(const QString&,const std::optional<QByteArray>&,const ToolContext&)> beforeFileWrite;
};
struct ModelRequest {
    QString model;
    QString systemPrompt;
    QList<Message> messages;
    QList<ToolDefinition> tools;
    GenerationOptions generation;
    QString contextId;
    bool summarizing = false; // Tools are disabled; the supplied system prompt defines the summary task.
    QJsonObject responseSchema;
    std::optional<bool> enableThinking;
    bool systemPromptOnly = false; // Use exactly the host's task instruction without the normal agent preamble.
    QString toolChoice = "auto";
    bool verificationAgent = false; // Host provenance; never accepted from wire/model data.
};
struct ModelReply {
    QString text;
    QList<ToolCall> toolCalls;
    Usage usage;
};
class IILOCALLLM_EXPORT Model {
public:
    virtual ~Model() = default;
    // May be invoked concurrently by distinct runs. Must cooperate with cancellation.
    virtual ModelReply generate(const ModelRequest&, const CancellationToken&,
        const std::function<bool(const QString&)>& onDelta) = 0;
    // Return native template/tokenizer counts, or nullopt if this adapter cannot measure.
    virtual std::optional<ContextBudget> measure(const ModelRequest&, const CancellationToken&) { return std::nullopt; }
};
struct Compaction {
    QString id;
    QString previousId;
    QString atMessageId; // Raw transcript tail when the checkpoint was committed.
    QString throughMessageId; // Covered raw prefix; empty for a micro-only checkpoint.
    QString summary;
    QString retainedUserMessageId; // Exact latest input, when covered by the summary.
    QStringList clearedToolMessageIds; // Cumulative replacements in the retained suffix.
    qint64 inputTokensBefore = 0;
    qint64 inputTokensAfter = 0;
};
struct Session {
    QString id;
    QString model;
    QString systemPrompt;
    QString workingDirectory;
    QList<Message> messages;
    QList<Compaction> compactions;
    QString parentSessionId; // Optional lineage; not an authorization grant.
};
struct RunRequest {
    QString sessionId;
    QString prompt;
    GenerationOptions generation;
    int maxTurns = 32;
    QStringList contextPaths; // Explicit workspace paths whose instructions apply before the first model call.
    QString skill; // Optional direct user invocation; prompt may be empty when set.
    QString skillArguments;
    QJsonObject promptMetadata; // Trusted C++ host provenance, never accepted from API/IPC/MCP input.
    QStringList allowedTools; // Trusted host/child invocation grants; expires at the end of this run.
    bool userPrompt = true; // C++ host provenance. Internal delegated instructions are not user submissions.
    std::shared_ptr<PermissionRequests> permissionRequests; // Trusted per-run channel; not transcript/model authority.
};
struct RunUsage {
    qint64 promptTokens = 0;
    qint64 generatedTokens = 0;
    qint64 cachedTokens = 0;
    qint64 droppedMessages = 0;
    qint64 summaryPromptTokens = 0;
    qint64 summaryGeneratedTokens = 0;
    int compactions = 0;
    qint64 memoryRecallPromptTokens = 0;
    qint64 memoryRecallGeneratedTokens = 0;
    qint64 memoryRecallCachedTokens = 0;
};
struct CompactRequest {
    QString sessionId;
    GenerationOptions generation;
    QString instructions;
};
struct RunResult {
    QString runId;
    QString sessionId;
    QString text;
    RunStatus status = RunStatus::Failed;
    int turns = 0;
    RunUsage usage;
    ErrorCode errorCode = ErrorCode::None;
    QString errorMessage;
};
struct Event {
    EventKind kind = EventKind::Started;
    QString runId;
    QString sessionId;
    QString toolCallId;
    QString text;
    QJsonObject data;
};
using EventCallback = std::function<void(const Event&)>;
struct RunHandle {
    QString runId;
    CancellationToken cancellation;
    std::shared_future<RunResult> result;
    void cancel() const noexcept { cancellation.cancel(); }
};
IILOCALLLM_EXPORT QString enumName(MessageRole);
IILOCALLLM_EXPORT QString enumName(RunStatus);
IILOCALLLM_EXPORT QString enumName(EventKind);
IILOCALLLM_EXPORT QJsonObject toJson(const ToolCall&);
IILOCALLLM_EXPORT QJsonObject toJson(const ToolDefinition&);
IILOCALLLM_EXPORT QJsonObject toJson(const Message&);
IILOCALLLM_EXPORT QJsonObject toJson(const RunResult&);
IILOCALLLM_EXPORT QJsonObject toJson(const Event&);
IILOCALLLM_EXPORT QJsonObject toJson(const Compaction&);
IILOCALLLM_EXPORT Compaction compactionFromJson(const QJsonObject&);
// Reconstructs only the model-visible view. Session.messages always contains the original records.
IILOCALLLM_EXPORT QList<Message> modelMessages(const Session&);
IILOCALLLM_EXPORT Message messageFromJson(const QJsonObject&);
IILOCALLLM_EXPORT ModelReply replyFromJson(const QJsonObject&);
// Rejects duplicate IDs and unmatched/interleaved results. Pending calls are allowed only at the tail.
IILOCALLLM_EXPORT QList<ToolCall> pendingToolCalls(const QList<Message>&);
}
