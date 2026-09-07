#pragma once
#include <QtCore/QJsonObject>
#include <QtNetwork/QLocalSocket>
#include <csignal>
#include <functional>

namespace iiLocalLLMClient {
// Deliberately independent of the SDK/runtime library. This client only speaks native IPC.
class IpcClient {
public:
    IpcClient(QString endpoint, const volatile std::sig_atomic_t* interrupted = nullptr);
    QJsonValue call(const QString& method, const QJsonObject& params = {},
                   std::function<void(const QJsonObject&)> event = {}, int timeoutMs = 300000);
private:
    QString endpoint_;
    const volatile std::sig_atomic_t* interrupted_;
    QLocalSocket socket_;
    QByteArray input_;
    quint64 nextId_ = 0;
};
} // namespace iiLocalLLMClient
