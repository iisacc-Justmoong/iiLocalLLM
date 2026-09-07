#pragma once
#include "Service.h"
#include <QtCore/QObject>

namespace iiLocalLLM {

struct IpcOptions {
    int maxConnections = 16;
    int maxInFlightPerConnection = 64;
    int maxFrameBytes = 1024 * 1024;
    int maxBufferedOutputBytes = 4 * 1024 * 1024;
};
// Newline-delimited JSON over QLocalSocket. Use only on its QObject thread.
// Service must outlive this server. Uses current-user socket permissions.
class IILOCALLLM_EXPORT LocalIpcServer : public QObject {
public:
    explicit LocalIpcServer(Service& service, IpcOptions options = {}, QObject* parent = nullptr);
    ~LocalIpcServer() override;
    bool listen(const QString& name);
    void close();
    QString serverName() const;
    QString errorString() const;
private:
    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace iiLocalLLM
