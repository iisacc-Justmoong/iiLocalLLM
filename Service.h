#pragma once
#include "Runtime.h"
#include "ModelInfo.h"
#include "ModelPull.h"

namespace iiLocalLLM {

// Thread-safe asynchronous API. Futures must not be waited on inside stream callbacks.
// Callbacks run on the worker (or the submitting thread for queue rejection).
// Destroy the service from outside its callbacks; destruction cancels and joins the worker.
class IILOCALLLM_EXPORT Service {
public:
    explicit Service(ServiceOptions options = {}, MlxRuntimeOptions mlx = {});
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    std::future<void> registerRuntime(std::shared_ptr<Runtime> runtime);
    std::future<ModelRecord> installModel(QString packageDirectory);
    ModelPullHandle pullModel(QString reference, PullCallback progress = {});
    std::future<void> removeModel(QString model);
    std::future<ModelListing> installedModels();
    std::future<ModelRecord> resolveModel(QString model);
    std::future<ModelVerification> verifyModel(QString model);
    std::future<ModelInfo> loadModel(ModelLoadRequest request);
    std::future<void> unloadModel(QString model);
    std::future<QList<ModelInfo>> models();
    HardwareInfo hardware() const;
    std::future<QString> createSession(QString model, QString systemPrompt = {});
    std::future<SessionSnapshot> session(QString sessionId);
    std::future<void> resetSession(QString sessionId);
    std::future<void> closeSession(QString sessionId);
    std::future<ServiceStats> stats();
    GenerationHandle chat(ChatRequest request, StreamCallback onEvent = {});
    GenerationHandle complete(CompletionRequest request, StreamCallback onEvent = {});
    // Native tool template/grammar/parser lane. Never coerces tool results into user text.
    GenerationHandle converse(ConversationRequest request, StreamCallback onEvent = {});
    // Uses the same native template/tokenizer as converse, without allocating a KV context or generating.
    // Reports over-budget prompts too; request syntax and host input limits still apply.
    std::future<ContextBudget> measureConversation(ConversationRequest request, CancellationToken cancellation = {});
private:
    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace iiLocalLLM
