#pragma once
#include "Runtime.h"
#include <map>

namespace iiLocalLLM::detail {

struct LoadedModel { ModelSpec spec; ExecutionSelection execution; std::shared_ptr<RuntimeModel> runtime; };
struct RuntimeModelInfo { ModelSpec spec; ExecutionSelection execution; };
class RuntimeManager {
public:
    RuntimeManager(int limit, HardwareInfo hardware) : limit_(limit), hardware_(std::move(hardware)) {}
    void addRuntime(std::shared_ptr<Runtime> runtime);
    MemoryEstimate estimateMemory(const ModelSpec& model, int contextSlots) const;
    RuntimeModelInfo load(ModelSpec model, const CancellationToken& token);
    LoadedModel& get(const QString& id);
    void unload(const QString& id);
    QList<RuntimeModelInfo> list() const;
private:
    int limit_;
    HardwareInfo hardware_;
    std::map<QString, std::shared_ptr<Runtime>> runtimes_;
    std::map<QString, LoadedModel> models_;
};
class SessionManager {
public:
    explicit SessionManager(int limit) : limit_(limit) {}
    QString create(QString modelId, QString systemPrompt);
    SessionSnapshot& get(const QString& id);
    void reset(const QString& id);
    void close(const QString& id);
    bool usesModel(const QString& id) const;
    int size() const { return static_cast<int>(sessions_.size()); }
private:
    int limit_;
    std::map<QString, SessionSnapshot> sessions_;
};
class ContextCacheManager {
public:
    ContextCacheManager(int count, int tokens) : maxCount_(count), maxTokens_(tokens) {}
    RuntimeContext& acquire(const QString& sessionId, LoadedModel& model, const CancellationToken& token);
    void erase(const QString& id);
    void eraseModel(const QString& modelId);
    void describe(ServiceStats& stats) const;
private:
    struct Entry { QString modelId; int reservedTokens; quint64 access; std::unique_ptr<RuntimeContext> context; };
    int maxCount_, maxTokens_, reserved_ = 0;
    quint64 clock_ = 0, evictions_ = 0;
    std::map<QString, Entry> entries_;
};
struct PreparedChat { QList<ChatMessage> messages; TokenList tokens; int droppedMessages = 0; };
class PromptEngine {
public:
    static PreparedChat prepare(const SessionSnapshot& session, const ChatRequest& request,
                                LoadedModel& model, const CancellationToken& token, int inputLimit);
};
// Holds only a possible stop-string suffix, including matches split across chunks.
class StopFilter {
public:
    explicit StopFilter(QStringList stops) : stops_(std::move(stops)) {}
    QString push(const QString& text);
    QString finish();
    bool stopped() const { return stopped_; }
private:
    QStringList stops_;
    QString pending_;
    bool stopped_ = false;
};

} // namespace iiLocalLLM::detail
