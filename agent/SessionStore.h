#pragma once
#include "Types.h"

namespace iiLocalLLM::agent {
class IILOCALLLM_EXPORT SessionLease {
public:
    ~SessionLease();
    const Session& session() const;
    void append(Message message);
    void compact(Compaction checkpoint);
    // Host execution view only. The immutable on-disk authorization header stays unchanged.
    void setExecutionDirectory(QString);
    QString artifactsDirectory() const;
private:
    friend class SessionStore;
    class Impl;
    explicit SessionLease(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> d;
};
class IILOCALLLM_EXPORT SessionStore {
public:
    explicit SessionStore(QString directory, qint64 maxTranscriptBytes = 64 * 1024 * 1024);
    Session create(QString model, QString systemPrompt, QString workingDirectory) const;
    // Atomically clones an immutable, already-paired snapshot with a fresh ID.
    // Optionally copies artifacts referenced by this snapshot from a trusted
    // source directory. Live jobs, grants and file checkpoints are not copied.
    // initialize receives the fresh ID before publication. It may append seed
    // messages; exceptions leave no session on disk. No store lock is held.
    Session createFromSnapshot(Session snapshot,
        const std::function<void(const QString&, QList<Message>&)>& initialize = {},
        const QString& sourceArtifactsDirectory = {}) const;
    // Copies a complete message boundary and its owned artifacts. beforePublish
    // runs with the source lease held, after cloning, before transcript publication.
    // Its caller must roll back any external state if publication throws.
    Session fork(const QString& id, const QString& throughMessageId = {},
        const std::function<void(const Session&)>& beforePublish = {}) const;
    // Lease excludes concurrent writers, including other processes, for the whole run.
    std::unique_ptr<SessionLease> acquire(const QString& id) const;
    Session load(const QString& id) const;
    // Reads only the immutable identity/workspace header, without a writer lease.
    // messages and compactions are empty; safe while an accepted run is active.
    Session metadata(const QString& id) const;
    QStringList list() const;
private:
    QString directory_;
    qint64 maxTranscriptBytes_;
};
}
