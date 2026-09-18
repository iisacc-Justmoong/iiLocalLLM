#pragma once
#include "Types.h"

namespace iiLocalLLM::agent {
struct InputQueueOptions {
    int maxPending = 256;
    int maxTextCharacters = 65536;
    qint64 maxBytes = 8 * 1024 * 1024;
    int lockTimeoutMs = 5000;
};
// Persistent per-session input, separate from the transcript writer lease.
class IILOCALLLM_EXPORT InputQueue {
public:
    explicit InputQueue(QString directory, InputQueueOptions = {});
    QJsonObject enqueue(const QString& sessionId, const QJsonObject& input, const CancellationToken& = {}) const;
    // Trusted producer identity, not a wire input field. Replays of an identical
    // pending item return that item; conflicting payloads are rejected. Transcript
    // delivery continues to deduplicate this identity after acknowledgement.
    QJsonObject enqueueIdentified(const QString& sessionId, const QString& inputId,
        const QJsonObject& input, const CancellationToken& = {}) const;
    QJsonObject snapshot(const QString& sessionId, int offset = 0, int limit = 100, const CancellationToken& = {}) const;
    QJsonObject remove(const QString& sessionId, const QString& inputId, const CancellationToken& = {}) const;
    // Trusted lifecycle transfer of selected pending notifications. Preserves
    // IDs/payload, assigns destination ordering, writes destination before source
    // acknowledgement. Retrying after a partial write is safe; delivery must
    // deduplicate IDs by payload, excluding the queue-local sequence number.
    // Never call from a delivery callback. Both delivery locks are acquired.
    // pendingAtDestination receives the selected IDs still queued at destination,
    // including an earlier interrupted transfer. from==to only queries those IDs.
    int transferNotifications(const QString& from,const QString& to,const QStringList& inputIds,const CancellationToken& = {},QStringList* pendingAtDestination = nullptr) const;
    // Calls persist under the queue lock, then acknowledges each committed item.
    // persist must be idempotent by input.id and must not re-enter this queue or
    // call external observers. A crash between persist and acknowledgement can
    // replay this callback; the transcript must deduplicate the stable ID.
    int deliver(const QString& sessionId, bool includeLater, int limit,
        const std::function<void(const QJsonObject&)>& persist, const CancellationToken& = {}) const;
    // Preparation runs outside queue.lock, so it may inspect/enqueue/remove input.
    // A separate delivery lock serializes consumers; prepare must not re-enter deliver.
    // Persist returns false to end the batch after acknowledging this item.
    // Withdrawn inputs are skipped before persist. Prepared external effects can
    // repeat after a crash; persist must still deduplicate the stable input ID.
    int deliver(const QString& sessionId, bool includeLater, int limit,
        const std::function<QJsonObject(const QJsonObject&)>& prepare,
        const std::function<bool(const QJsonObject&,const QJsonObject&)>& persist,
        const CancellationToken& = {}) const;
private:
    QString directory_;
    InputQueueOptions options_;
};
}
