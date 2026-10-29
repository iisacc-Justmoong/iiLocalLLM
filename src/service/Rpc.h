#pragma once
#include "Types.h"

namespace iiLocalLLM {
using RpcEventCallback = std::function<void(const QJsonObject&)>;
struct RpcHandle {
    QString requestId;
    CancellationToken cancellation;
    std::shared_future<QJsonValue> result;
    void cancel() const noexcept { cancellation.cancel(); }
};
// Transport-independent native RPC extension. dispatch must return promptly.
// Events may arrive on worker threads, but must finish before result becomes ready.
// Credentials are host input, never log data. Every handle must have a valid future
// and a unique, nonempty requestId. Implementations must bound accepted work.
// The caller owns cancellation when its connection closes or deadline expires.
class IILOCALLLM_EXPORT RpcHandler {
public:
    virtual ~RpcHandler() = default;
    // Host-owned classification for bounded control operations. Transports may
    // reserve capacity for these methods; dispatch still authenticates/validates.
    virtual bool isControlMethod(const QString&) const { return false; }
    virtual RpcHandle dispatch(QString method, QJsonObject parameters, QString credential,
        RpcEventCallback = {}) = 0;
};
}
