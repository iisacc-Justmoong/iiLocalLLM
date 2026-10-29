#pragma once
// Windows CRT <memory.h> can find this header through a case-insensitive path.
// This API always has C++ linkage, including when reached from an extern-C block.
extern "C++" {
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
}
