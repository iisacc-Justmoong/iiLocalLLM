#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QList>
#include <QtCore/QStringList>
#include <QtCore/qglobal.h>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>

#if defined(IILOCALLLM_BUILDING_LIBRARY)
#  define IILOCALLLM_EXPORT Q_DECL_EXPORT
#else
#  define IILOCALLLM_EXPORT Q_DECL_IMPORT
#endif

namespace iiLocalLLM {

enum class ErrorCode { None, InvalidArgument, NotFound, AlreadyExists, ModelInUse,
    QueueFull, Cancelled, ContextOverflow, ResourceLimit, RuntimeUnavailable,
    RuntimeFailure, Timeout, ConsumerFailure, ShuttingDown, ProtocolError,
    InvalidManifest, IntegrityFailure, StorageFailure, Unauthorized };
enum class Role { System, User, Assistant };
enum class FinishReason { Stop, Length, Cancelled, Error };
enum class StreamEventKind { Started, Delta, Finished };

IILOCALLLM_EXPORT QString enumName(ErrorCode code);
IILOCALLLM_EXPORT QString enumName(Role role);
IILOCALLLM_EXPORT QString enumName(FinishReason reason);

class IILOCALLLM_EXPORT Error : public std::runtime_error {
public:
    Error(ErrorCode code, const QString& message);
    ErrorCode code() const noexcept { return code_; }
private:
    ErrorCode code_;
};

class IILOCALLLM_EXPORT CancellationToken {
public:
    CancellationToken();
    // A child observes parent cancellation; cancelling the child leaves the parent usable.
    static CancellationToken linkedTo(const CancellationToken& parent);
    void cancel() const noexcept;
    bool isCancelled() const noexcept;
    void throwIfCancelled() const;
private:
    std::shared_ptr<std::atomic_bool> flag_;
    std::shared_ptr<const CancellationToken> parent_;
};

struct ChatMessage {
    Role role = Role::User;
    QString content;
    bool operator==(const ChatMessage&) const = default;
};
using TokenList = QList<qint32>;
// Exact prepared prompt size and the currently loaded model's context capacity.
struct ContextBudget {
    qint64 inputTokens = 0;
    int contextTokens = 0;
};
struct ModelSpec {
    QString id;
    QString path;
    int contextTokens = 2048;
    QJsonObject options;
    QString format; // Resolved by model management; consumed by runtime adapters.
};
struct ModelLoadRequest {
    QString model; // Canonical model://id; applications never supply a filesystem path.
    int contextTokens = 0; // 0 uses the service default, bounded by the manifest and cache budget.
    QJsonObject options;
    qint64 keepAliveMs = -1; // -1 adopts the service/current model policy; 0 expires when idle.
};
struct GenerationOptions {
    int maxTokens = 256;
    double temperature = 0.7;
    double topP = 0.9;
    int topK = 40;
    quint32 seed = 0;
    QStringList stop;
    double minP = 0;
    double typicalP = 1; // llama.cpp only when different from 1.
    int minKeep = 1;
    double repetitionPenalty = 1;
    int repetitionContextSize = 64; // -1 applies to the full prompt and generated history.
    double presencePenalty = 0;
    double frequencyPenalty = 0;
    double xtcProbability = 0;
    double xtcThreshold = 0.1;
    QJsonObject logitBias; // Decimal token IDs -> finite additive logit biases.
};
struct ChatRequest {
    QString sessionId;
    QString prompt;
    GenerationOptions options;
    qint64 keepAliveMs = -1;
};
// Stateless conversation input; the service owns the temporary session and its cleanup.
struct CompletionRequest {
    QString model;
    QList<ChatMessage> messages;
    GenerationOptions options;
    qint64 keepAliveMs = -1;
};
// Structured text/tool conversation in OpenAI function-call message format.
// The caller owns history; contextId optionally reuses a bounded, model-scoped KV cache.
struct ConversationRequest {
    QString model;
    QJsonArray messages;
    QJsonArray tools;
    QString toolChoice = QStringLiteral("auto"); // auto, required, none
    QString contextId;
    GenerationOptions options;
    qint64 keepAliveMs = -1;
    bool parallelToolCalls = true;
};
struct Usage {
    int promptTokens = 0;
    int generatedTokens = 0;
    int cachedTokens = 0;
    int droppedMessages = 0;
};
struct GenerationResult {
    QString requestId;
    QString sessionId;
    QString text;
    FinishReason finishReason = FinishReason::Error;
    Usage usage;
    ErrorCode errorCode = ErrorCode::None;
    QString errorMessage;
    QJsonArray toolCalls; // OpenAI function-call objects; empty for text-only APIs.
    QString reasoning;
};
struct StreamEvent {
    StreamEventKind kind = StreamEventKind::Started;
    QString requestId;
    QString sessionId;
    QString text;
    GenerationResult result;
};
using StreamCallback = std::function<void(const StreamEvent&)>;
struct GenerationHandle {
    QString requestId;
    CancellationToken cancellation;
    std::shared_future<GenerationResult> result;
    void cancel() const noexcept { cancellation.cancel(); }
};
struct SessionSnapshot {
    QString id;
    QString modelId;
    QList<ChatMessage> messages;
};
struct ServiceOptions {
    int maxQueuedRequests = 64;
    int maxModels = 4;
    int maxSessions = 128;
    int maxCachedContexts = 4;
    int maxCachedContextTokens = 16384;
    int maxInputCharacters = 1024 * 1024;
    QString modelsDirectory = QStringLiteral("Models"); // Service deployment setting.
    int defaultContextTokens = 2048;
    quint64 memoryBudgetBytes = 0; // 0 reserves max(2 GiB, 25% of physical RAM) for the system.
    quint64 memoryReserveBytes = 256 * 1024 * 1024; // Minimum live free/reclaimable RAM at admission.
    qint64 keepAliveMs = -1; // Auto: 5 minutes, or 0 on machines with <= 8 GiB RAM.
    QString registryFile; // Deployment-owned download catalog. Empty uses the bundled registry.
};
struct ServiceStats {
    int loadedModels = 0;
    int sessions = 0;
    int cachedContexts = 0;
    int reservedContextTokens = 0;
    quint64 cacheEvictions = 0;
    quint64 residentBytes = 0; // Estimated reservation, not measured RSS.
    quint64 memoryBudgetBytes = 0;
    quint64 availableRamBytes = 0;
    bool availableRamKnown = false;
    qint64 defaultKeepAliveMs = 0;
    quint64 modelLoads = 0;
    quint64 modelEvictions = 0;
};

} // namespace iiLocalLLM
