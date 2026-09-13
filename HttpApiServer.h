#pragma once
#include "Service.h"
#include "Rpc.h"

namespace iiLocalLLM {
struct HttpOptions {
    int workerThreads = 8;
    int maxQueuedConnections = 16;
    int maxRequestBytes = 1024 * 1024;
    int maxBufferedOutputBytes = 4 * 1024 * 1024;
    int readTimeoutMs = 5000;
    int writeTimeoutMs = 5000;
    int requestTimeoutMs = 300000;
};
// HTTP/1.1 on 127.0.0.1 only. Owns transport threads; Service must outlive this server.
// Call listen/close/port/errorString serially, outside stream callbacks.
class IILOCALLLM_EXPORT HttpApiServer {
public:
    explicit HttpApiServer(Service& service, HttpOptions options = {});
    ~HttpApiServer();
    HttpApiServer(const HttpApiServer&) = delete;
    HttpApiServer& operator=(const HttpApiServer&) = delete;
    bool listen(quint16 port = 0); // 0 asks the OS for an available port.
    void setRpcHandler(std::shared_ptr<RpcHandler>); // Only while closed; enables POST /v1/rpc.
    void close();
    quint16 port() const;
    QString errorString() const;
private:
    class Impl;
    std::unique_ptr<Impl> d;
};
} // namespace iiLocalLLM
