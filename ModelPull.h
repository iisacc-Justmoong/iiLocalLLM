#pragma once
#include "ModelManifest.h"
namespace iiLocalLLM {
struct PullProgress {
    QString model;
    QString file;
    qint64 receivedBytes = 0;
    qint64 totalBytes = 0;
};
using PullCallback = std::function<void(const PullProgress&)>;
struct ModelPullHandle {
    QString requestId;
    CancellationToken cancellation;
    std::shared_future<ModelRecord> result;
    void cancel() const noexcept { cancellation.cancel(); }
};
} // namespace iiLocalLLM
