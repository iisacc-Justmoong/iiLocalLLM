#pragma once
#include "Types.h"

namespace iiLocalLLM::agent {
class IILOCALLLM_EXPORT SessionLease {
public:
    ~SessionLease();
    const Session& session() const;
    void append(Message message);
    void compact(Compaction checkpoint);
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
    // External artifacts are not copied. The caller must handle that boundary.
    Session createFromSnapshot(Session snapshot) const;
    // Copies a complete message boundary into a new, atomically published session.
    Session fork(const QString& id, const QString& throughMessageId = {}) const;
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
