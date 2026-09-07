#pragma once
#include "Types.h"
#include <optional>

namespace iiLocalLLM {
// Admission reservations, not measured per-model RSS. Includes all permitted cached contexts.
struct IILOCALLLM_EXPORT MemoryEstimate {
    quint64 weightsBytes = 0;
    quint64 contextBytes = 0;
    quint64 overheadBytes = 0;
    QString basis;
    quint64 totalBytes() const;
};
IILOCALLLM_EXPORT std::optional<quint64> availableRamBytes();
// JSON number = seconds; strings accept ms/s/m/h, e.g. "5m". Missing = service policy.
IILOCALLLM_EXPORT qint64 parseKeepAlive(const QJsonValue& value);
IILOCALLLM_EXPORT QJsonObject memoryEstimateObject(const MemoryEstimate& estimate);
} // namespace iiLocalLLM
