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
    QJsonObject snapshot(const QString& sessionId, int offset = 0, int limit = 100, const CancellationToken& = {}) const;
    QJsonObject remove(const QString& sessionId, const QString& inputId, const CancellationToken& = {}) const;
    // Calls persist under the queue lock, then acknowledges each committed item.
    // persist must be idempotent by input.id and must not re-enter this queue or
    // call external observers. A crash between persist and acknowledgement can
    // replay this callback; the transcript must deduplicate the stable ID.
    int deliver(const QString& sessionId, bool includeLater, int limit,
        const std::function<void(const QJsonObject&)>& persist, const CancellationToken& = {}) const;
private:
    QString directory_;
    InputQueueOptions options_;
};
}
