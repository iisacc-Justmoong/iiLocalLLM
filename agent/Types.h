#pragma once
#include "../Types.h"
#include <QtCore/QJsonArray>

namespace iiLocalLLM::agent {

enum class MessageRole { User, Assistant, Tool };
enum class RunStatus { Completed, Cancelled, TurnLimit, Failed };
enum class EventKind { Started, ModelDelta, Message, ToolStarted, ToolProgress, ToolFinished,
    PermissionRequested, Hook, Finished };
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
};
struct ModelRequest {
    QString model;
    QString systemPrompt;
    QList<Message> messages;
    QList<ToolDefinition> tools;
    GenerationOptions generation;
    QString contextId;
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
};
struct Session {
    QString id;
    QString model;
    QString systemPrompt;
    QString workingDirectory;
    QList<Message> messages;
};
struct RunRequest {
    QString sessionId;
    QString prompt;
    GenerationOptions generation;
    int maxTurns = 32;
};
struct RunUsage {
    qint64 promptTokens = 0;
    qint64 generatedTokens = 0;
    qint64 cachedTokens = 0;
    qint64 droppedMessages = 0;
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
IILOCALLLM_EXPORT Message messageFromJson(const QJsonObject&);
IILOCALLLM_EXPORT ModelReply replyFromJson(const QJsonObject&);
// Rejects duplicate IDs and unmatched/interleaved results. Pending calls are allowed only at the tail.
IILOCALLLM_EXPORT QList<ToolCall> pendingToolCalls(const QList<Message>&);
}
