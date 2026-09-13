#pragma once
#include "Tools.h"
#include "SessionStore.h"
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
    QStringList sessions() const;
    Session forkSession(const QString& id, const QString& throughMessageId = {});
    RunHandle run(RunRequest, EventCallback = {});
private:
    class Impl;
    std::unique_ptr<Impl> d;
};
// Native structured conversation adapter. Unsupported runtimes fail explicitly.
// Uses the existing Service scheduler, model residency policy, and bounded KV cache.
class IILOCALLLM_EXPORT ServiceModel final : public Model {
public:
    explicit ServiceModel(Service& service);
    ModelReply generate(const ModelRequest&, const CancellationToken&, const TextCallback&) override;
private:
    Service& service_;
};
}
