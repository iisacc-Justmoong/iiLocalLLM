#include "IpcClient.h"
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonDocument>
#include <stdexcept>

namespace iiLocalLLMClient {
namespace {
[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toStdString()); }
void checkError(const QJsonObject& response)
{
    if (response.contains("error")) {
        const auto error = response.value("error").toObject();
        fail(error.value("code").toString() + QStringLiteral(": ") + error.value("message").toString());
    }
}
}
IpcClient::IpcClient(QString endpoint, const volatile std::sig_atomic_t* interrupted)
    : endpoint_(std::move(endpoint)), interrupted_(interrupted) { socket_.setReadBufferSize(4 * 1024 * 1024 + 1); }
QJsonValue IpcClient::call(const QString& method, const QJsonObject& params,
                         std::function<void(const QJsonObject&)> event, int timeoutMs)
{
    if (interrupted_ && *interrupted_) fail(QStringLiteral("Interrupted"));
    if (socket_.state() != QLocalSocket::ConnectedState) {
        socket_.connectToServer(endpoint_);
        if (!socket_.waitForConnected(3000)) fail(QStringLiteral("Cannot connect to iiLocalLLMD at ") + endpoint_
            + QStringLiteral(". Start iiLocalLLMD with the same --socket (or IILLM_SOCKET). ") + socket_.errorString());
    }
    const auto id = QString::number(++nextId_);
    const auto bytes = QJsonDocument(QJsonObject{{"id", id}, {"method", method}, {"params", params}}).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > 1024 * 1024) fail(QStringLiteral("Request exceeds the service frame limit"));
    if (socket_.write(bytes) != bytes.size()) fail(socket_.errorString());
    socket_.flush();
    QElapsedTimer elapsed; elapsed.start();
    for (;;) {
        if ((interrupted_ && *interrupted_) || elapsed.elapsed() > timeoutMs) {
            socket_.abort(); // The server cancels this connection's queued/active generation or pull.
            fail(interrupted_ && *interrupted_ ? QStringLiteral("Interrupted") : QStringLiteral("Service request timed out"));
        }
        input_ += socket_.readAll();
        for (;;) {
            const auto newline = input_.indexOf('\n');
            if (newline < 0) break;
            if (newline > 4 * 1024 * 1024) { socket_.abort(); fail(QStringLiteral("Service response exceeds frame limit")); }
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(input_.first(newline), &parse);
            input_.remove(0, newline + 1);
            if (parse.error != QJsonParseError::NoError || !document.isObject()) fail(QStringLiteral("Invalid service JSON response"));
            const auto response = document.object();
            if (response.value("id").toString() != id) fail(QStringLiteral("Unexpected service request id"));
            checkError(response);
            const auto kind = response.value("event").toString();
            if (!kind.isEmpty() && event) event(response);
            if (kind == "done" || (kind.isEmpty() && response.contains("result"))) {
                const auto result = response.value("result");
                if (result.isObject()) checkError(result.toObject());
                return result;
            }
        }
        if (input_.size() > 4 * 1024 * 1024) { socket_.abort(); fail(QStringLiteral("Service response exceeds frame limit")); }
        if (socket_.state() != QLocalSocket::ConnectedState) fail(QStringLiteral("iiLocalLLMD disconnected before completing the request"));
        socket_.waitForReadyRead(100);
    }
}
} // namespace iiLocalLLMClient
