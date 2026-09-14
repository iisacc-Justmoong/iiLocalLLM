#pragma once
#include "Client.h"
#include <QtCore/QMap>
#include <QtCore/QUrl>

namespace iiLocalLLM::mcp {
class IILOCALLLM_EXPORT HttpError : public Error {
public:
    HttpError(int statusCode, QByteArray wwwAuthenticate = {});
    int statusCode() const noexcept { return status_; }
    QByteArray wwwAuthenticate() const { return challenge_; }
private:
    int status_;
    QByteArray challenge_;
};
struct HttpOptions : ClientLimits {
    QUrl endpoint;
    QMap<QByteArray, QByteArray> headers;
    // Host-owned credentials, read afresh for each HTTP exchange on the I/O thread.
    // Must return promptly; OAuth discovery/interactive flows belong to the host.
    std::function<QByteArray()> bearerToken;
    bool allowInsecureHttp = false; // HTTP loopback is always allowed.
    bool listenForNotifications = true;
    int reconnectDelayMs = 1000;
    int maxReconnectAttempts = 8;
};
// Streamable HTTP for negotiated 2025 revisions. TLS verifies peers; redirects
// and automatic POST replay are disabled. SSE resumes through GET only.
// A session 404 fails outstanding requests and negotiates a new session.
class IILOCALLLM_EXPORT HttpClient final : public Client {
public:
    explicit HttpClient(HttpOptions, ClientOptions = {});
};
}
