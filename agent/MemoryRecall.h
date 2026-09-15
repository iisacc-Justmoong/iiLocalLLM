#pragma once
#include "ProjectMemory.h"

namespace iiLocalLLM::agent {
struct MemoryRecallOptions {
    bool enabled = true; // Active only when the host enables project memory.
    QString model; // Empty selects the conversation's local model.
    int timeoutMs = 30000;
    int maxSelectedFiles = 5;
    int maxInputBytes = 64 * 1024;
    int maxOutputBytes = 4096;
    int maxTokens = 256;
    int maxFileBytes = 4096;
    int maxFileLines = 200;
    int maxContextBytes = 60 * 1024;
};
// Model-ranked topic recall. Selection reads only host-scoped note metadata;
// attachment revalidates exact bytes and records complete/partial file reads.
class IILOCALLLM_EXPORT MemoryRecall {
public:
    MemoryRecall(std::shared_ptr<ProjectMemory>,std::shared_ptr<Model>,MemoryRecallOptions = {});
    QJsonObject select(const QString& workspace,const QString& model,const QString& query,
        const QList<Message>& visible = {},const CancellationToken& = {}) const;
    QList<Message> attach(QJsonObject& selection,const ToolContext&,const QList<Message>& visible = {}) const;
    bool enabled() const;
    static QStringList observedPaths(const QList<Message>&);
    static QStringList recentSuccessfulTools(const QList<Message>&);
private:
    std::shared_ptr<ProjectMemory> memory_;
    std::shared_ptr<Model> model_;
    MemoryRecallOptions options_;
};
}
