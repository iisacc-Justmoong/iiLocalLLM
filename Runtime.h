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
class IILOCALLLM_EXPORT RuntimeContext {
public:
    virtual ~RuntimeContext() = default;
    virtual RuntimeResult generate(const TokenList& prompt, const GenerationOptions& options,
        const CancellationToken& cancellation, const TextCallback& onText) = 0;
};
class IILOCALLLM_EXPORT RuntimeModel {
public:
    virtual ~RuntimeModel() = default;
    virtual TokenList tokenize(const QList<ChatMessage>& messages, const CancellationToken& cancellation) = 0;
    virtual std::unique_ptr<RuntimeContext> createContext(const CancellationToken& cancellation) = 0;
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
IILOCALLLM_EXPORT std::shared_ptr<Runtime> createLlamaRuntime();
struct MlxRuntimeOptions {
    QString pythonExecutable = QStringLiteral("python3");
    QString workerScript; // Explicit installed/source mlx_worker.py path; no working-directory lookup.
    int timeoutMs = 120000;
};
IILOCALLLM_EXPORT std::shared_ptr<Runtime> createMlxRuntime(MlxRuntimeOptions options);

} // namespace iiLocalLLM
