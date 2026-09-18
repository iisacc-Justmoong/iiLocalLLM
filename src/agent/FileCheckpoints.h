#pragma once
#include "Types.h"

namespace iiLocalLLM::agent {
// Bounded, durable native-file history. Callers hold the session lease while
// binding a user message or mutating files. A separate lock protects the journal.
class IILOCALLLM_EXPORT FileCheckpoints {
public:
    explicit FileCheckpoints(QString directory);
    QJsonObject checkpoint(const QString& messageId,const ToolContext&) const;
    void track(const QString& path,const std::optional<QByteArray>& before,const ToolContext&) const;
    QJsonObject list(const ToolContext&) const;
    // Caller holds the source session lease. Copies verified blobs to a fresh
    // owner; an explicit message set keeps only matching snapshots and files.
    // The target must not exist. Failure cleans the newly created target.
    void fork(const ToolContext& source,const QString& targetSessionId,
        const std::optional<QStringList>& retainedMessageIds = {}) const;
    // Preflights every target and blob before mutation; each file is atomic,
    // the set is not. Partial failures report exactly which files were restored.
    QJsonObject rewind(const QString& messageId,bool dryRun,const ToolContext&,
        const QString& expectedFingerprint = {}) const;
private:
    QString directory_;
};
}
