#include "HttpClient.h"
#include "ClientTransport.h"
#include "Protocol.h"
#include <QtCore/QEventLoop>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringDecoder>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslConfiguration>
#include <chrono>
#include <cstring>
#include <limits>
#include <vector>

namespace iiLocalLLM::mcp {
HttpError::HttpError(int status, QByteArray challenge)
    : Error(status == 401 || status == 403 ? ErrorCode::Unauthorized : ErrorCode::ProtocolError,
        "MCP HTTP status " + QString::number(status) + "; request was not replayed"),
      status_(status), challenge_(std::move(challenge)) {}
namespace {
using namespace detail;
using Clock = std::chrono::steady_clock;
// QNAM can retry a POST after an EOF before response headers. A sequential,
// non-buffered body that refuses rewind makes that retry fail before a second
// request reaches the server. This applies to every POST, including notifications.
class OneShotUpload final : public QIODevice {
    QByteArray bytes;
    qint64 total = 0;
    qint64 offset = 0;
public:
    explicit OneShotUpload(QByteArray value) : bytes(std::move(value)), total(bytes.size()) { open(ReadOnly | Unbuffered); }
    bool isSequential() const override { return true; }
    bool reset() override { return offset == 0; }
    qint64 size() const override { return total; }
    qint64 bytesAvailable() const override { return total - offset + QIODevice::bytesAvailable(); }
    bool atEnd() const override { return offset == total; }
    qsizetype retainedBytes() const { return bytes.size(); }
protected:
    qint64 readData(char* target, qint64 limit) override {
        const qint64 count = std::min(limit, total - offset);
        if (count <= 0) return -1;
        std::memcpy(target, bytes.constData() + offset, size_t(count)); offset += count;
        if (offset == total) bytes.clear();
        return count;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
};
struct NetworkSlot {
    QNetworkAccessManager manager;
    bool busy = false;
};
struct Exchange {
    NetworkSlot* slot = nullptr;
    QNetworkReply* reply = nullptr;
    std::unique_ptr<OneShotUpload> upload;
    QString requestKey;
    QString method;
    bool initialize = false, initializedNotification = false, control = false, background = false;
    bool completed = false, stopped = false, headersRead = false, sse = false;
    bool streamBeginning = true, afterCr = false, eventHadId = false;
    QByteArray json, line, data, eventType, eventId, lastEventId;
    quint64 retryMs = 1000;
    int retries = 0;
    Clock::time_point started, resumeAt;
    bool awaitingResume = false;
    qsizetype bufferedBytes() const { return json.size() + line.size() + data.size() + eventType.size() + eventId.size() + lastEventId.size(); }
};
bool headerValue(const QByteArray& value) {
    for (unsigned char ch : value) if (ch < 0x20 || ch == 0x7f) return false;
    return true;
}
void tick(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
class HttpTransport final : public ClientTransport {
    HttpOptions options;
    std::vector<std::unique_ptr<NetworkSlot>> connections;
    std::vector<std::shared_ptr<Exchange>> exchanges;
    QList<QByteArray> deferred;
    qsizetype deferredBytes = 0;
    QByteArray session, version;
    bool ready = false, listeningStarted = false;
    int maxExchanges() const {
        return int(std::min<qint64>(1000000, qint64(options.maxPendingRequests)
            + options.maxServerRequests + options.maxNotificationCount + 4));
    }
    NetworkSlot* acquire() {
        for (auto& slot : connections) if (!slot->busy) {
            slot->busy = true; return slot.get();
        }
        require(connections.size() < size_t(maxExchanges()), "MCP HTTP exchange capacity reached", ErrorCode::QueueFull);
        auto slot = std::make_unique<NetworkSlot>(); slot->busy = true;
        connections.push_back(std::move(slot)); return connections.back().get();
    }
    void release(Exchange& e) {
        if (e.reply) {
            const bool interrupted = !e.reply->isFinished();
            e.reply->abort(); delete e.reply; e.reply = nullptr;
            // An interrupted SSE socket must finish closing before the slot is reused.
            if (interrupted) e.slot->manager.clearConnectionCache();
        }
        if (e.slot) { e.slot->busy = false; e.slot = nullptr; }
        e.upload.reset();
    }
    QNetworkRequest networkRequest(const QByteArray& accept, const QByteArray& cursor = {}) {
        QNetworkRequest request(options.endpoint);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        auto ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyPeer); request.setSslConfiguration(ssl);
        request.setRawHeader("Accept", accept);
        request.setRawHeader("User-Agent", "iiLocalLLM-MCP/0.8.0");
        for (auto it = options.headers.cbegin(); it != options.headers.cend(); ++it)
            request.setRawHeader(it.key(), it.value());
        if (options.bearerToken) {
            QByteArray credential;
            try { credential = options.bearerToken(); }
            catch (...) { throw Error(ErrorCode::ConsumerFailure, "MCP credential provider failed"); }
            require(credential.size() <= 16384 && headerValue(credential), "Invalid MCP bearer credential", ErrorCode::InvalidArgument);
            if (!credential.isEmpty()) request.setRawHeader("Authorization", "Bearer " + credential);
        }
        if (!session.isEmpty()) request.setRawHeader("Mcp-Session-Id", session);
        if (!version.isEmpty()) request.setRawHeader("Mcp-Protocol-Version", version);
        if (!cursor.isEmpty()) request.setRawHeader("Last-Event-ID", cursor);
        return request;
    }
    void issue(const std::shared_ptr<Exchange>& e, const QByteArray& body = {}) {
        const bool get = e->background || e->awaitingResume;
        auto request = networkRequest(get ? "text/event-stream" : "application/json, text/event-stream", get ? e->lastEventId : QByteArray{});
        if (!get) {
            request.setRawHeader("Content-Type", "application/json");
            request.setHeader(QNetworkRequest::ContentLengthHeader, body.size());
            request.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
            e->upload = std::make_unique<OneShotUpload>(body);
        }
        e->slot = acquire();
        e->reply = get ? e->slot->manager.get(request) : e->slot->manager.post(request, e->upload.get());
        e->reply->setReadBufferSize(65536);
        e->started = Clock::now(); e->headersRead = false; e->sse = false;
        e->json.clear(); e->line.clear(); e->data.clear(); e->eventId.clear(); e->eventType.clear();
        e->eventHadId = false; e->streamBeginning = true; e->afterCr = false; e->awaitingResume = false;
    }
    void startListening() {
        if (!ready || listeningStarted || !options.listenForNotifications) return;
        listeningStarted = true;
        auto e = std::make_shared<Exchange>(); e->background = true; e->retryMs = options.reconnectDelayMs;
        exchanges.push_back(e); issue(e);
    }
    void deliver(Exchange& e, const QByteArray& bytes, QList<TransportEvent>& events) {
        QJsonParseError error; const auto document = QJsonDocument::fromJson(bytes, &error);
        require(error.error == QJsonParseError::NoError && (document.isObject() || document.isArray()), "MCP HTTP response is not JSON-RPC");
        const auto messages = document.isArray() ? document.array() : QJsonArray{document.object()};
        for (const auto& value : messages) {
            const auto message = value.toObject();
            if (message.contains("id") && !message.contains("method")) {
                require(!e.requestKey.isEmpty() && key(message["id"]) == e.requestKey && !e.completed,
                    "MCP HTTP response arrived on the wrong request stream");
                e.completed = true;
                if (e.initialize) version = message["result"].toObject()["protocolVersion"].toString().toUtf8();
            }
        }
        events.append({bytes, {}, {}, false}); e.retries = 0;
    }
    void line(Exchange& e, QList<TransportEvent>& events) {
        auto value = std::move(e.line); e.line.clear();
        if (e.streamBeginning) {
            if (value.startsWith("\xef\xbb\xbf")) value.remove(0, 3);
            e.streamBeginning = false;
        }
        if (value.isEmpty()) {
            const bool duplicate = e.eventHadId && !e.eventId.isEmpty() && e.eventId == e.lastEventId;
            if (e.eventHadId) e.lastEventId = e.eventId;
            if (!e.data.isEmpty()) {
                e.data.chop(1);
                if (!e.data.isEmpty() && !duplicate && (e.eventType.isEmpty() || e.eventType == "message"))
                    deliver(e, e.data, events);
            }
            e.data.clear(); e.eventType.clear(); e.eventId.clear(); e.eventHadId = false;
            return;
        }
        if (value.startsWith(':')) return;
        const auto colon = value.indexOf(':');
        const auto field = colon < 0 ? value : value.first(colon);
        auto body = colon < 0 ? QByteArray{} : value.sliced(colon + 1);
        if (body.startsWith(' ')) body.remove(0, 1);
        if (field == "data") e.data += body + '\n';
        else if (field == "event") e.eventType = body;
        else if (field == "id" && !body.contains('\0')) {
            QStringDecoder decoder(QStringDecoder::Utf8); const QString decoded = decoder(body);
            require(!decoder.hasError() && body.size() <= 4096 && headerValue(body), "Invalid MCP SSE event ID");
            e.eventId = body; e.eventHadId = true;
        } else if (field == "retry" && !body.isEmpty()) {
            const bool digits = std::all_of(body.begin(), body.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
            bool ok = false; const auto delay = body.toULongLong(&ok);
            if (digits) e.retryMs = ok ? delay : std::numeric_limits<quint64>::max();
        }
    }
    void sse(Exchange& e, const QByteArray& bytes, QList<TransportEvent>& events) {
        for (char byte : bytes) {
            if (e.afterCr) { e.afterCr = false; if (byte == '\n') continue; }
            if (byte == '\r' || byte == '\n') { line(e, events); e.afterCr = byte == '\r'; }
            else e.line += byte;
            require(e.bufferedBytes() <= options.maxMessageBytes, "MCP SSE event exceeds limit", ErrorCode::ResourceLimit);
        }
    }
    void fail(Exchange& e, QList<TransportEvent>& events, std::exception_ptr error, bool expired = false) {
        events.append({{}, e.requestKey, error, expired}); e.stopped = true;
    }
    void read(const std::shared_ptr<Exchange>& holder, QList<TransportEvent>& events) {
        auto& e = *holder;
        if (!e.reply) return;
        auto* reply = e.reply;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status && !e.headersRead) {
            e.headersRead = true;
            if (status == 404 && !session.isEmpty() && !e.initialize) {
                ready = false;
                fail(e, events, std::make_exception_ptr(HttpError(status)), true); return;
            }
            if (status == 405 && e.background && e.lastEventId.isEmpty()) { e.stopped = true; return; }
            if ((e.control && status != 202) || (!e.control && status != 200)) {
                const auto challenge = reply->rawHeader("WWW-Authenticate");
                require(challenge.size() <= 16384, "MCP authentication challenge exceeds limit", ErrorCode::ResourceLimit);
                fail(e, events, std::make_exception_ptr(HttpError(status, challenge))); return;
            }
            if (e.initialize && reply->hasRawHeader("Mcp-Session-Id")) {
                const auto sid = reply->rawHeader("Mcp-Session-Id");
                require(sid.size() <= 4096 && std::all_of(sid.begin(), sid.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e; }), "Invalid MCP HTTP session ID");
                session = sid;
            }
            if (!e.control) {
                const auto type = reply->rawHeader("Content-Type").split(';').first().trimmed().toLower();
                e.sse = type == "text/event-stream";
                require(e.sse || (!e.background && type == "application/json"), "Unsupported MCP HTTP response content type");
            }
        }
        while (reply->bytesAvailable() > 0) {
            const auto bytes = reply->read(65536);
            if (e.sse) sse(e, bytes, events);
            else e.json += bytes;
            require(e.bufferedBytes() <= options.maxMessageBytes, "MCP HTTP response exceeds limit", ErrorCode::ResourceLimit);
        }
        if (e.completed) { e.stopped = true; return; }
        if (!reply->isFinished()) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - e.started).count();
            if ((e.background && !e.headersRead && elapsed >= options.initializeTimeoutMs) || (e.control && elapsed >= options.requestTimeoutMs))
                fail(e, events, std::make_exception_ptr(Error(ErrorCode::Timeout, "MCP HTTP exchange timed out")));
            return;
        }
        if (e.control && status == 202 && reply->error() == QNetworkReply::NoError) {
            require(e.json.isEmpty(), "MCP HTTP 202 response must have no body");
            if (e.initializedNotification) ready = true;
            e.stopped = true; return;
        }
        if (!e.sse && status == 200 && reply->error() == QNetworkReply::NoError) {
            deliver(e, e.json, events);
            require(e.completed, "MCP HTTP JSON response does not complete the request");
            e.stopped = true; return;
        }
        if (e.sse && (e.background || !e.lastEventId.isEmpty()) && e.retries < options.maxReconnectAttempts) {
            ++e.retries; release(e); e.awaitingResume = true;
            e.resumeAt = e.retryMs > quint64(std::numeric_limits<int>::max()) ? Clock::time_point::max()
                : Clock::now() + std::chrono::milliseconds(e.retryMs);
            return;
        }
        fail(e, events, std::make_exception_ptr(Error(ErrorCode::RuntimeFailure,
            "MCP HTTP connection ended before its response (network error " + QString::number(reply->error())
            + ", method " + e.method.left(128) + "); remote outcome may be unknown")));
    }
public:
    explicit HttpTransport(HttpOptions o) : options(std::move(o)) {
        const auto& url = options.endpoint;
        const bool loopback = url.host().compare("localhost", Qt::CaseInsensitive) == 0 || QHostAddress(url.host()).isLoopback();
        require(url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasFragment()
            && (url.scheme() == "https" || (url.scheme() == "http" && (loopback || options.allowInsecureHttp)))
            && options.reconnectDelayMs >= 0 && options.maxReconnectAttempts >= 0 && options.maxReconnectAttempts <= 100,
            "MCP endpoint requires HTTPS or explicit trusted HTTP; invalid URL or retry limits", ErrorCode::InvalidArgument);
        const QSet<QByteArray> reserved{"accept", "authorization", "host", "content-type", "content-length", "transfer-encoding",
            "connection", "cookie", "proxy-authorization", "mcp-session-id", "mcp-protocol-version", "last-event-id"};
        static const QRegularExpression field(QRegularExpression::anchoredPattern("[!#$%&'*+.^_`|~0-9A-Za-z-]+"));
        qsizetype headerBytes = 0;
        for (auto it = options.headers.cbegin(); it != options.headers.cend(); ++it) {
            headerBytes += it.key().size() + it.value().size();
            require(field.match(QString::fromLatin1(it.key())).hasMatch() && !reserved.contains(it.key().toLower())
                && headerValue(it.value()) && headerBytes <= 16384, "Invalid or reserved MCP HTTP header", ErrorCode::InvalidArgument);
        }
    }
    void start() override {}
    void send(const QByteArray& bytes) override {
        qsizetype retained = deferredBytes;
        for (const auto& e : exchanges) if (e->upload) retained += e->upload->retainedBytes();
        require(retained + bytes.size() <= options.maxQueuedBytes, "MCP HTTP upload queue is full", ErrorCode::QueueFull);
        const auto document = QJsonDocument::fromJson(bytes); const auto message = document.object();
        const auto method = message["method"].toString();
        if (method == "notifications/cancelled") {
            const auto cancelled = key(message["params"].toObject()["requestId"]);
            for (auto& e : exchanges) if (e->requestKey == cancelled) { e->stopped = true; release(*e); }
            for (auto it = deferred.begin(); it != deferred.end();) {
                const auto item = QJsonDocument::fromJson(*it).object();
                if (item.contains("id") && key(item["id"]) == cancelled) { deferredBytes -= it->size(); it = deferred.erase(it); }
                else ++it;
            }
        }
        if (!ready && !method.isEmpty() && method != "initialize" && method != "notifications/initialized") {
            require(deferredBytes + bytes.size() <= options.maxQueuedBytes, "MCP HTTP initialization queue is full", ErrorCode::QueueFull);
            deferred.append(bytes); deferredBytes += bytes.size(); return;
        }
        auto e = std::make_shared<Exchange>();
        e->method = method.isEmpty() ? "response" : method;
        e->initialize = method == "initialize"; e->initializedNotification = method == "notifications/initialized";
        e->control = !message.contains("method") || !message.contains("id");
        if (!e->control) e->requestKey = key(message["id"]);
        e->retryMs = options.reconnectDelayMs;
        require(exchanges.size() < size_t(maxExchanges()), "MCP HTTP exchange capacity reached", ErrorCode::QueueFull);
        exchanges.push_back(e); issue(e, bytes);
        // Before an SSE initialize result arrives, a resumption GET can still
        // identify the requested revision. The negotiated result replaces it.
        if (e->initialize) version = message["params"].toObject()["protocolVersion"].toString().toUtf8();
    }
    QList<TransportEvent> poll() override {
        tick(10); QList<TransportEvent> events;
        for (const auto& e : exchanges) {
            if (e->stopped) continue;
            if (e->awaitingResume && Clock::now() >= e->resumeAt) issue(e);
            read(e, events);
            qsizetype total = 0;
            for (const auto& other : exchanges) total += other->bufferedBytes();
            for (const auto& event : events) total += event.message.size();
            require(total <= options.maxQueuedBytes, "MCP HTTP input exceeds aggregate buffer limit", ErrorCode::ResourceLimit);
        }
        for (auto it = exchanges.begin(); it != exchanges.end();) {
            if ((*it)->stopped) { release(**it); it = exchanges.erase(it); }
            else ++it;
        }
        if (ready) {
            startListening();
            const auto waiting = std::move(deferred); deferred.clear(); deferredBytes = 0;
            for (const auto& bytes : waiting) send(bytes);
        }
        return events;
    }
    void reset() override {
        for (auto& e : exchanges) release(*e);
        exchanges.clear(); deferred.clear(); deferredBytes = 0;
        session.clear(); version.clear(); ready = false; listeningStarted = false;
    }
    void close() override {
        const auto sid = session;
        for (auto& e : exchanges) release(*e);
        exchanges.clear();
        if (!sid.isEmpty() && options.shutdownTimeoutMs > 0) {
            try {
                QNetworkAccessManager manager;
                auto* reply = manager.deleteResource(networkRequest("application/json, text/event-stream"));
                const auto deadline = Clock::now() + std::chrono::milliseconds(options.shutdownTimeoutMs);
                while (!reply->isFinished() && Clock::now() < deadline) tick(10);
                reply->abort(); delete reply;
            } catch (...) { /* Destruction never overrides a request/credential failure. */ }
        }
        reset(); connections.clear();
    }
};
}
HttpClient::HttpClient(HttpOptions options, ClientOptions client)
    : Client(std::make_unique<HttpTransport>(options), options, std::move(client)) {}
}
