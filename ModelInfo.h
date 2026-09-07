#pragma once
#include "ModelManifest.h"
#include "Hardware.h"
#include "Memory.h"

namespace iiLocalLLM {
struct ModelInfo {
    ModelRecord model;
    int contextTokens = 0;
    QJsonObject options;
    ExecutionSelection execution;
    MemoryEstimate memory;
    qint64 keepAliveMs = 0;
    qint64 expiresInMs = 0; // -1 while a generation holds the model.
    int activeRequests = 0;
};
} // namespace iiLocalLLM
