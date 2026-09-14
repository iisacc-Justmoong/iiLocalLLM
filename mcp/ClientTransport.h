#pragma once
#include "Client.h"

namespace iiLocalLLM::mcp::detail {
struct TransportEvent {
    QByteArray message;
    QString requestKey;
    std::exception_ptr error;
    bool sessionExpired = false;
};
// Owned by the protocol worker. All methods and QObject lifetimes stay on it.
class ClientTransport {
public:
    virtual ~ClientTransport() = default;
    virtual void start() = 0;
    virtual void send(const QByteArray&) = 0;
    virtual QList<TransportEvent> poll() = 0;
    virtual void close() = 0;
    virtual void reset() { throw Error(ErrorCode::RuntimeUnavailable, "Transport cannot reconnect"); }
    virtual QByteArray diagnostics() const { return {}; }
};
std::unique_ptr<ClientTransport> stdioTransport(StdioOptions);
}
