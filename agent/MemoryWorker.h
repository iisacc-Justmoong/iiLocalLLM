#pragma once
#include "MemoryContext.h"
#include "ProjectMemory.h"
namespace iiLocalLLM::agent::detail {
struct FrozenMemoryContext:MemoryContext {
    std::function<void(const ToolContext&)> inheritReads,clearReads;
};
struct MemoryWorkerOptions {
    QString activity="memory-extraction";
    bool history=false;
    int maxTurns=5,timeoutMs=60000,maxInputBytes=4*1024*1024,maxOutputBytes=1024*1024,maxToolCallsPerTurn=64;
    QList<Hook> hooks;AgentHookExecutor hookAgent;
    std::function<void(const QJsonObject&)> progress;
    // Nonempty host feedback continues the same bounded worker conversation.
    std::function<QString(const CancellationToken&)> completionCheck;
};
qsizetype memoryContextBytes(const ModelRequest&);
std::shared_ptr<const FrozenMemoryContext> freezeMemoryContext(MemoryContext,const ProjectMemory&,int maxInputBytes);
QJsonObject runMemoryWorker(const FrozenMemoryContext&,std::shared_ptr<ProjectMemory>,std::shared_ptr<Model>,
    std::shared_ptr<const PermissionPolicy>,const MemoryWorkerOptions&,const CancellationToken&,const QString& jobId,
    const std::function<Message(const QString&,const CancellationToken&)>& instruction);
}
