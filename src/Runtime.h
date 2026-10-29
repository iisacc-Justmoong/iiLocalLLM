#pragma once
#include "Hardware.h"
#include "Memory.h"

namespace iiLocalLLM {

// Called serially on the service worker, including construction and destruction.
// Returning false from TextCallback requests early termination (e.g. a stop string).
using TextCallback = std::function<bool(const QString&)>;
struct RuntimeResult {
    FinishReason finishReason = FinishReason::Stop;
    int generatedTokens = 0;
    int cachedTokens = 0;
};
struct IILOCALLLM_EXPORT RuntimePromptState { virtual ~RuntimePromptState() = default; };
struct RuntimeConversationPrompt {
    TokenList tokens;
    QStringList stop;
    std::shared_ptr<const RuntimePromptState> state;
    qint64 inputTokens = 0; // Includes native media embeddings; zero uses text tokens.
    qint64 tokenCount() const noexcept { return inputTokens ? inputTokens : tokens.size(); }
};
struct RuntimeConversationReply {
    QString text;
    QString reasoning;
    QJsonArray toolCalls;
};
class IILOCALLLM_EXPORT RuntimeContext {
public:
    virtual ~RuntimeContext() = default;
    virtual RuntimeResult generate(const TokenList& prompt, const GenerationOptions& options,
        const CancellationToken& cancellation, const TextCallback& onText) = 0;
    virtual RuntimeResult generateConversation(const RuntimeConversationPrompt&, const GenerationOptions&,
        const CancellationToken&, const TextCallback&) {
        throw Error(ErrorCode::RuntimeUnavailable, "Runtime does not support structured conversations");
    }
};
class IILOCALLLM_EXPORT RuntimeModel {
public:
    virtual ~RuntimeModel() = default;
    virtual TokenList tokenize(const QList<ChatMessage>& messages, const CancellationToken& cancellation) = 0;
    virtual std::unique_ptr<RuntimeContext> createContext(const CancellationToken& cancellation) = 0;
    virtual RuntimeConversationPrompt prepareConversation(const ConversationRequest&, const CancellationToken&) {
        throw Error(ErrorCode::RuntimeUnavailable, "Runtime does not support structured conversations");
    }
    virtual RuntimeConversationReply parseConversation(const RuntimeConversationPrompt&, const QString&) {
        throw Error(ErrorCode::RuntimeUnavailable, "Runtime does not support structured conversations");
    }
};
// Optional interface alongside RuntimeModel, so older runtime implementations
// need not change their virtual table. Calls are serialized by Service.
class IILOCALLLM_EXPORT EmbeddingRuntimeModel {
public:
    virtual ~EmbeddingRuntimeModel() = default;
    virtual QList<float> embedText(const QString&, const CancellationToken&) = 0;
};
class IILOCALLLM_EXPORT Runtime {
public:
    virtual ~Runtime() = default;
    virtual QString id() const = 0;
    virtual bool supportsModel(const ModelSpec& spec) const = 0;
    virtual QList<RuntimeDevice> devices(const HardwareInfo& hardware) const = 0;
    virtual MemoryEstimate estimateMemory(const ModelSpec& spec, int contextSlots) const;
    virtual std::shared_ptr<RuntimeModel> load(const ModelSpec& spec, const RuntimeDevice& device,
        const CancellationToken& cancellation) = 0;
};

IILOCALLLM_EXPORT bool llamaRuntimeAvailable() noexcept;
// Host-owned limits. Model manifests and generation requests cannot override them.
struct LlamaRuntimeOptions {
    bool memoryMapWeights = true; // Allow upstream automatic mapping; false copies weights into owned memory.
    int maxGpuLayers = -1;        // -1 retains the upstream default; otherwise caps native layer offload.
    bool validateTensorData = false; // Ask the native loader to reject invalid tensor data.
};
IILOCALLLM_EXPORT std::shared_ptr<Runtime> createLlamaRuntime();
IILOCALLLM_EXPORT std::shared_ptr<Runtime> createLlamaRuntime(LlamaRuntimeOptions options);
struct MlxRuntimeOptions {
    QString pythonExecutable = QStringLiteral("python3");
    QString workerScript; // Explicit installed/source mlx_worker.py path; no working-directory lookup.
    int timeoutMs = 120000;
};
IILOCALLLM_EXPORT std::shared_ptr<Runtime> createMlxRuntime(MlxRuntimeOptions options);

} // namespace iiLocalLLM
