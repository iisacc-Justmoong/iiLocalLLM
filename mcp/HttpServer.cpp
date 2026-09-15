#include "../third_party/cpp-httplib/httplib.h"
#include "HttpServer.h"
#include "Protocol.h"
#include <QtCore/QStringDecoder>
#include <QtCore/QUuid>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <limits>
#include <thread>

namespace iiLocalLLM::mcp {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using detail::require;
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
struct Rejection { int status; QString message; };
void check(bool value, int status, const QString& message) { if (!value) throw Rejection{status, message}; }
void reject(httplib::Response& response, int status, const QString& message) {
    response.status = status;
    if (status == 401) response.set_header("WWW-Authenticate", "Bearer realm=\"iiLocalLLM MCP\"");
    response.set_content(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"error", QJsonObject{
        {"code", -32000}, {"message", message}}}}).toJson(QJsonDocument::Compact).toStdString(), "application/json");
}
template<class F> void guarded(httplib::Response& response, F function) {
    try { function(); }
    catch (const Rejection& error) { reject(response, error.status, error.message); }
    catch (const Error& error) {
        const auto code = error.code();
        reject(response, code == ErrorCode::QueueFull || code == ErrorCode::ResourceLimit ? 429
            : code == ErrorCode::ShuttingDown ? 404 : 400, "MCP request could not be accepted");
    }
    catch (...) { reject(response, 503, "MCP server could not process the request"); }
}
QByteArray header(const httplib::Request& request, const char* name) {
    check(request.get_header_value_count(name) <= 1, 400, "Duplicate MCP HTTP header");
    return QByteArray::fromStdString(request.get_header_value(name));
}
bool accepts(const QByteArray& value, const QByteArray& type) {
    for (auto item : value.toLower().split(',')) {
        const auto fields = item.split(';');
        if (fields.first().trimmed() != type) continue;
        bool enabled = true;
        for (qsizetype n = 1; n < fields.size(); ++n) {
            const auto field = fields[n].trimmed();
            if (field.startsWith("q=")) {
                bool ok = false; const auto quality = field.sliced(2).toDouble(&ok);
                enabled = ok && quality > 0 && quality <= 1;
            }
        }
        if (enabled) return true;
    }
    return false;
}
bool validControl(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const auto message = value.toObject();
    if (message["jsonrpc"] != "2.0") return false;
    if (message.contains("method")) return !message.contains("id") && message["method"].isString()
        && !message["method"].toString().isEmpty() && !message.contains("result") && !message.contains("error")
        && (!message.contains("params") || message["params"].isObject());
    try { detail::key(message["id"]); } catch (...) { return false; }
    if (message.contains("result") == message.contains("error")) return false;
    if (message.contains("result")) return message["result"].isObject();
    const auto error = message["error"].toObject();
    const auto code = error["code"].toDouble(std::numeric_limits<double>::quiet_NaN());
    return error["code"].isDouble() && std::isfinite(code) && std::trunc(code) == code
        && code >= INT_MIN && code <= INT_MAX && error["message"].isString();
}
struct Event { quint64 sequence, order; QByteArray frame; };
struct Stream {
    const QString id = uuid();
    std::deque<Event> events;
    QSet<QString> requestIds;
    quint64 nextSequence = 1, delivered = 0, lease = 0;
    int writers = 0;
    bool background = false, completed = false, control = false;
    Clock::time_point completedAt;
};
struct Session {
    QString principal;
    std::shared_ptr<ServerSession> protocol;
    std::mutex mutex;
    std::condition_variable changed;
    std::map<QString, std::shared_ptr<Stream>> streams;
    std::shared_ptr<Stream> background;
    Clock::time_point touched = Clock::now();
    qsizetype historyBytes = 0, historyEvents = 0;
    quint64 order = 0, nextLease = 0;
    bool closed = false;
    std::shared_future<void> cleanup;
};
}
class HttpServer::Impl {
public:
    HttpServerFactory factory;
    HttpServerOptions options;
    httplib::Server server;
    std::thread listener, maintenance;
    std::atomic_bool stopping = true;
    std::atomic_int writers = 0, controlWriters = 0;
    mutable std::mutex mutex;
    std::map<QString, std::shared_ptr<Session>> sessions;
    int initializing = 0;
    QList<HttpServerNotification> notifications;
    qsizetype notificationBytes = 0;
    quint16 boundPort = 0;
    QString lastError;

    Impl(HttpServerFactory f, HttpServerOptions o) : factory(std::move(f)), options(std::move(o)) {
        require(factory && options.authenticate && options.maxSessions > 0 && options.maxSessions <= 256
            && options.maxControlStreams >= 1 && options.maxControlStreams <= 64
            && options.maxStreams > 0 && options.maxStreams <= 256 && options.maxStreamsPerSession >= 2
            && options.maxStreamsPerSession <= 4096 && options.maxQueuedConnections > 0
            && options.maxRequestBytes >= 1024 && options.maxHistoryBytes >= 1024 && options.maxHistoryEvents >= 2
            && options.maxNotifications > 0 && options.sessionIdleTimeoutMs > 0 && options.streamRetentionMs > 0
            && options.readTimeoutMs > 0 && options.writeTimeoutMs > 0 && options.heartbeatMs > 0 && options.retryMs >= 0,
            "Invalid MCP HTTP server configuration", ErrorCode::InvalidArgument);
        for (const auto& origin : options.allowedOrigins) {
            const QUrl url(origin);
            require(url.isValid() && (url.scheme() == "http" || url.scheme() == "https") && !url.host().isEmpty()
                && url.userInfo().isEmpty() && url.path().isEmpty() && !url.hasQuery() && !url.hasFragment(),
                "MCP allowed origins must be exact HTTP origins", ErrorCode::InvalidArgument);
        }
        // Normal and host-control SSE have separate limits. Four more workers
        // accept cancellation, reverse responses, initialization and DELETE.
        server.new_task_queue = [o = options] { return new httplib::ThreadPool(4, o.maxStreams + o.maxControlStreams + 4, o.maxQueuedConnections); };
        server.set_socket_options([](auto socket) {
            // httplib defaults to SO_REUSEPORT where available. A stateful
            // local endpoint must not distribute sessions among unrelated
            // listeners sharing the same address and port.
#ifdef _WIN32
            httplib::set_socket_opt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
            httplib::set_socket_opt(socket, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
        });
        server.set_payload_max_length(options.maxRequestBytes);
        server.set_read_timeout(std::chrono::milliseconds(options.readTimeoutMs));
        server.set_write_timeout(std::chrono::milliseconds(options.writeTimeoutMs));
        server.set_keep_alive_max_count(1);
        server.set_default_headers({{"Cache-Control", "no-store"}, {"X-Content-Type-Options", "nosniff"}});
        server.set_pre_routing_handler([this](const auto& request, auto& response) {
            bool accepted = false;
            guarded(response, [&] { origin(request, response); check(!stopping, 503, "MCP server is stopping"); accepted = true; });
            return accepted ? httplib::Server::HandlerResponse::Unhandled : httplib::Server::HandlerResponse::Handled;
        });
        server.Post("/mcp", [this](const auto& request, auto& response) {
            guarded(response, [&] { post(request, response, authenticate(request)); });
        });
        server.Get("/mcp", [this](const auto& request, auto& response) {
            guarded(response, [&] {
                check(request.method == "GET", 405, "Unsupported MCP HTTP method");
                get(request, response, authenticate(request));
            });
        });
        server.Delete("/mcp", [this](const auto& request, auto& response) {
            guarded(response, [&] { auto session = lookup(request, authenticate(request)); retire(session); response.status = 200; });
        });
        server.Options("/mcp", [this](const auto& request, auto& response) {
            guarded(response, [&] {
                check(request.has_header("Origin"), 400, "MCP preflight requires an origin");
                const auto method = header(request, "Access-Control-Request-Method");
                check(method == "POST" || method == "GET" || method == "DELETE", 405, "Unsupported MCP preflight method");
                const QByteArray allowed = "authorization, content-type, accept, mcp-session-id, mcp-protocol-version, last-event-id";
                QSet<QByteArray> fields;
                for (const auto& field : allowed.split(',')) fields.insert(field.trimmed());
                for (const auto& field : header(request, "Access-Control-Request-Headers").toLower().split(','))
                    check(field.trimmed().isEmpty() || fields.contains(field.trimmed()), 400, "Unsupported MCP preflight header");
                response.set_header("Access-Control-Allow-Methods", "POST, GET, DELETE");
                response.set_header("Access-Control-Allow-Headers", allowed.toStdString()); response.status = 204;
            });
        });
        server.set_error_handler([](const auto&, auto& response) {
            if (response.body.empty()) reject(response, response.status, "MCP HTTP request rejected");
        });
    }
    void origin(const httplib::Request& request, httplib::Response& response) {
        const auto suffix = ":" + QByteArray::number(request.local_port);
        const auto host = header(request, "Host");
        check(host == "127.0.0.1" + suffix || host == "localhost" + suffix, 403, "MCP requires a direct localhost Host");
        if (!request.has_header("Origin")) return;
        const auto value = header(request, "Origin");
        check(value == "http://127.0.0.1" + suffix || value == "http://localhost" + suffix
            || options.allowedOrigins.contains(QString::fromUtf8(value)), 403, "MCP origin is not allowed");
        response.set_header("Access-Control-Allow-Origin", value.toStdString());
        response.set_header("Access-Control-Expose-Headers", "Mcp-Session-Id, Www-Authenticate");
        response.set_header("Vary", "Origin");
    }
    QString authenticate(const httplib::Request& request) {
        const auto authorization = header(request, "Authorization");
        check(authorization.size() <= 16384 && authorization.left(7).toLower() == "bearer ", 401, "MCP authentication required");
        QString principal;
        try { principal = options.authenticate(authorization.sliced(7)); }
        catch (...) { throw Rejection{503, "MCP authentication provider failed"}; }
        check(!principal.isEmpty(), 401, "MCP credential is invalid");
        check(principal.size() <= 256 && !principal.contains(QChar::Null), 503, "MCP authentication provider returned an invalid principal");
        return principal;
    }
    std::vector<std::shared_ptr<Session>> snapshot() const {
        std::lock_guard lock(mutex); std::vector<std::shared_ptr<Session>> values;
        for (const auto& [id, session] : sessions) values.push_back(session); return values;
    }
    std::shared_ptr<Session> lookup(const httplib::Request& request, const QString& principal) {
        const auto id = QString::fromLatin1(header(request, "Mcp-Session-Id"));
        check(!id.isEmpty(), 400, "MCP session ID is required");
        std::shared_ptr<Session> session;
        { std::lock_guard lock(mutex); const auto found = sessions.find(id);
          check(found != sessions.end() && found->second->principal == principal, 404, "MCP session not found"); session = found->second; }
        { std::lock_guard lock(session->mutex); check(!session->closed, 404, "MCP session expired"); session->touched = Clock::now(); }
        const auto revision = header(request, "Mcp-Protocol-Version");
        check(revision.isEmpty() || revision == session->protocol->protocolVersion().toUtf8(), 400, "MCP protocol version does not match the session");
        return session;
    }
    void retire(const std::shared_ptr<Session>& session) {
        std::lock_guard lock(session->mutex);
        if (session->closed) return;
        session->closed = true; session->protocol->requestClose(); session->changed.notify_all();
        try { session->cleanup = std::async(std::launch::async, [protocol = session->protocol] { protocol->close(); }).share(); }
        catch (...) { /* Keep this closed session charged until close() can join it without another thread. */ }
    }
    void eraseStream(Session& session, const QString& id) {
        const auto found = session.streams.find(id); if (found == session.streams.end()) return;
        for (const auto& event : found->second->events) { session.historyBytes -= event.frame.size(); --session.historyEvents; }
        session.streams.erase(found);
    }
    void expireStreams(Session& session, bool capacity = false, bool control = false) {
        size_t count=0;for(const auto& [id,stream]:session.streams)count+=stream->control==control;
        const auto limit=size_t(control?options.maxControlStreams:options.maxStreamsPerSession);
        for (auto it = session.streams.begin(); it != session.streams.end();) {
            const auto stream = it->second; ++it;
            if (stream->completed && !stream->writers
                && (Clock::now() - stream->completedAt >= std::chrono::milliseconds(options.streamRetentionMs)
                    || (capacity && stream->control==control && count>=limit))) {
                count-=stream->control==control;eraseStream(session,stream->id);
            }
        }
    }
    void append(Session& session, Stream& stream, const QByteArray& data) {
        const auto sequence = stream.nextSequence++;
        const auto frame = "id: " + stream.id.toLatin1() + ":" + QByteArray::number(sequence)
            + "\nretry: " + QByteArray::number(options.retryMs) + "\ndata: " + data + "\n\n";
        require(frame.size() <= options.maxHistoryBytes, "MCP HTTP event exceeds replay limit", ErrorCode::ResourceLimit);
        stream.events.push_back({sequence, ++session.order, frame});
        session.historyBytes += frame.size(); ++session.historyEvents;
        while (session.historyBytes > options.maxHistoryBytes || session.historyEvents > options.maxHistoryEvents) {
            std::shared_ptr<Stream> oldest;
            for (const auto& [id, candidate] : session.streams) if (!candidate->events.empty()
                && (!oldest || candidate->events.front().order < oldest->events.front().order)) oldest = candidate;
            require(bool(oldest), "Invalid MCP HTTP replay accounting", ErrorCode::RuntimeFailure);
            session.historyBytes -= oldest->events.front().frame.size(); --session.historyEvents; oldest->events.pop_front();
        }
        session.changed.notify_all();
    }
    std::shared_ptr<Stream> newStream(Session& session, bool background = false, bool control = false) {
        expireStreams(session, true, control);
        size_t count=0;for(const auto& [id,stream]:session.streams)count+=stream->control==control;
        check(count<size_t(control?options.maxControlStreams:options.maxStreamsPerSession),429,"MCP retained stream capacity reached");
        auto stream = std::make_shared<Stream>(); stream->background = background;stream->control=control;
        session.streams.emplace(stream->id, stream); append(session, *stream, {}); return stream;
    }
    struct Writer {
        Impl* owner;
        std::shared_ptr<Session> session;
        std::shared_ptr<Stream> stream;
        quint64 sequence = 0, lease = 0;
        bool charged = false, attached = false;
        Clock::time_point lastWrite = Clock::now();
        std::function<bool()> disconnected;
        ~Writer() {
            if (attached) { std::lock_guard lock(session->mutex); --stream->writers;
                if (stream->lease == lease) stream->lease = 0; session->changed.notify_all(); }
            if (charged) --(stream->control?owner->controlWriters:owner->writers);
        }
        bool send(httplib::DataSink& sink) {
            if (disconnected()) return false;
            QByteArray frame; quint64 next = sequence; bool done = false;
            {
                std::unique_lock lock(session->mutex);
                auto available = [&] { return session->closed || stream->lease != lease || stream->completed
                    || stream->nextSequence > sequence + 1; };
                if (!available()) session->changed.wait_for(lock, 20ms, available);
                if (session->closed || stream->lease != lease) return false;
                const auto first = stream->events.empty() ? stream->nextSequence : stream->events.front().sequence;
                if (sequence + 1 < first) return false; // Retention overrun; never skip missing events.
                for (const auto& event : stream->events) if (event.sequence > sequence) { frame = event.frame; next = event.sequence; break; }
                done = frame.isEmpty() && stream->completed;
            }
            if (done) { sink.done(); return true; }
            if (frame.isEmpty() && Clock::now() - lastWrite >= std::chrono::milliseconds(owner->options.heartbeatMs)) frame = ": heartbeat\n\n";
            if (frame.isEmpty()) return !disconnected();
            if (!sink.write(frame.constData(), size_t(frame.size()))) return false;
            lastWrite = Clock::now(); sequence = next;
            { std::lock_guard lock(session->mutex); stream->delivered = std::max(stream->delivered, sequence); }
            return true;
        }
    };
    // The caller holds the session mutex while it attaches a response writer.
    std::shared_ptr<Writer> attach(const httplib::Request& request, const std::shared_ptr<Session>& session,
        const std::shared_ptr<Stream>& stream, quint64 cursor, bool replace = false) {
        auto writer = std::make_shared<Writer>(); writer->owner = this; writer->session = session; writer->stream = stream;
        auto& charged=stream->control?controlWriters:writers;int count=charged.load();
        do { check(count<(stream->control?options.maxControlStreams:options.maxStreams),429,"MCP active stream capacity reached"); }
        while (!charged.compare_exchange_weak(count, count + 1));
        writer->charged = true;
        check(!session->closed, 404, "MCP session expired");
        check(!stream->lease || replace, 409, "MCP stream already has a reader");
        const auto first = stream->events.empty() ? stream->nextSequence : stream->events.front().sequence;
        check(cursor < stream->nextSequence && cursor + 1 >= first, 410, "MCP replay cursor is no longer available");
        writer->sequence = cursor; writer->lease = ++session->nextLease; writer->disconnected = request.is_connection_closed;
        stream->lease = writer->lease; ++stream->writers; writer->attached = true; session->changed.notify_all(); return writer;
    }
    void streamResponse(httplib::Response& response, const std::shared_ptr<Writer>& writer) {
        response.status = 200;
        response.set_chunked_content_provider("text/event-stream", [writer](size_t, httplib::DataSink& sink) {
            return writer->send(sink);
        });
    }
    void initialize(const httplib::Request& request, httplib::Response& response, const QString& principal, const QJsonValue& value) {
        const auto message = value.isArray() ? value.toArray().first().toObject() : value.toObject();
        check(message["method"] == "initialize" && message.contains("id") && (!value.isArray()
            || (value.toArray().size() == 1 && message["params"].toObject()["protocolVersion"] == "2025-03-26")), 400, "MCP initialization is required");
        { std::lock_guard lock(mutex); check(!stopping && sessions.size() + size_t(initializing) < size_t(options.maxSessions), 429, "MCP session capacity reached"); ++initializing; }
        struct Reservation { Impl* owner; ~Reservation() { std::lock_guard lock(owner->mutex); --owner->initializing; } } reservation{this};
        auto session = std::make_shared<Session>(); session->principal = principal;
        session->protocol = std::make_shared<ServerSession>(factory(principal));
        if (value.isArray()) session->protocol->receiveBatch(value.toArray(), "initialize");
        else session->protocol->receive(message, "initialize");
        const auto frames = session->protocol->takeFrames();
        check(frames.size() == 1, 400, "MCP initialization did not complete");
        const auto result = frames[0].message;
        const auto object = result.isArray() ? result.toArray().first().toObject() : result.toObject();
        if (!object.contains("result")) {
            response.status = 400; response.set_content(detail::encodeValue(result, options.maxRequestBytes).toStdString(), "application/json"); return;
        }
        const auto revision = header(request, "Mcp-Protocol-Version");
        check(revision.isEmpty() || revision == session->protocol->protocolVersion().toUtf8(), 400, "Unsupported MCP protocol version");
        session->background = newStream(*session, true);
        const auto encoded = detail::encodeValue(result, options.maxRequestBytes).toStdString();
        { std::lock_guard lock(mutex); check(!stopping, 503, "MCP server is stopping"); sessions.emplace(session->protocol->id(), session); }
        response.set_header("Mcp-Session-Id", session->protocol->id().toStdString()); response.status = 200;
        response.set_content(encoded, "application/json");
    }
    void post(const httplib::Request& request, httplib::Response& response, const QString& principal) {
        const auto accept = header(request, "Accept");
        check(accepts(accept, "application/json") && accepts(accept, "text/event-stream"), 406, "MCP POST requires JSON and SSE Accept types");
        check(header(request, "Content-Type").split(';').first().trimmed().toLower() == "application/json", 415, "MCP POST requires application/json");
        check(!request.has_header("Last-Event-ID"), 400, "MCP stream resumption requires GET");
        const auto bytes = QByteArray::fromStdString(request.body); QStringDecoder decoder(QStringDecoder::Utf8);
        const QString decoded = decoder(bytes); check(!decoder.hasError(), 400, "MCP JSON must be UTF-8");
        QJsonParseError error; const auto document = QJsonDocument::fromJson(bytes, &error);
        check(error.error == QJsonParseError::NoError && (document.isObject() || (document.isArray() && !document.array().isEmpty())), 400, "Invalid MCP JSON envelope");
        const QJsonValue value = document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object());
        if (!request.has_header("Mcp-Session-Id")) { initialize(request, response, principal, value); return; }
        auto session = lookup(request, principal);
        check(!value.isArray() || session->protocol->protocolVersion() == "2025-03-26", 400, "MCP batches require protocol 2025-03-26");
        const auto messages = value.isArray() ? value.toArray() : QJsonArray{value};
        bool responds = false, control = true; QSet<QString> ids;
        for (const auto& item : messages) {
            const auto message = item.toObject(); responds |= !validControl(item);
            // A mixed legacy batch never promotes ordinary requests into the
            // reserved class. Notifications/reverse replies need no SSE slot.
            if(!validControl(item))control&=session->protocol->isControlMethod(message["method"].toString());
            check(message["method"] != "initialize", 400, "MCP session is already initialized");
            if (message.contains("method") && message.contains("id")) {
                try { ids.insert(detail::key(message["id"])); } catch (...) { check(value.isArray(), 400, "Invalid MCP request ID"); }
            }
        }
        if (!value.isArray() && responds) {
            const auto message = value.toObject();
            check(message["jsonrpc"] == "2.0" && message["method"].isString() && !message["method"].toString().isEmpty()
                && message.contains("id") && !message.contains("result") && !message.contains("error"), 400, "Invalid MCP request envelope");
        }
        std::shared_ptr<Stream> stream; std::shared_ptr<Writer> writer;
        if (responds) {
            std::lock_guard lock(session->mutex);
            check((control?controlWriters:writers)<(control?options.maxControlStreams:options.maxStreams),429,"MCP active stream capacity reached");
            stream = newStream(*session,false,control); stream->requestIds = ids;
            try { writer = attach(request, session, stream, 0); }
            catch (...) { eraseStream(*session, stream->id); throw; }
        }
        const auto channel = stream ? stream->id : QString{};
        if (value.isArray()) session->protocol->receiveBatch(value.toArray(), channel);
        else session->protocol->receive(value.toObject(), channel);
        // The protocol queues cancellation completion after the handler and
        // any reverse cancellation/batch reply. A socket close never cancels.
        if (writer) streamResponse(response, writer); else response.status = 202;
    }
    void get(const httplib::Request& request, httplib::Response& response, const QString& principal) {
        check(accepts(header(request, "Accept"), "text/event-stream"), 406, "MCP GET requires SSE Accept");
        auto session = lookup(request, principal); const auto cursor = header(request, "Last-Event-ID");
        std::shared_ptr<Writer> writer;
        { std::lock_guard lock(session->mutex);
          if (cursor.isEmpty()) writer = attach(request, session, session->background, session->background->delivered);
          else {
              check(cursor.size() <= 128, 400, "Invalid MCP replay cursor");
              const auto colon = cursor.lastIndexOf(':'); bool ok = false;
              const auto sequence = cursor.sliced(colon + 1).toULongLong(&ok);
              check(colon > 0 && ok && sequence > 0 && QByteArray::number(sequence) == cursor.sliced(colon + 1), 400, "Invalid MCP replay cursor");
              const auto found = session->streams.find(QString::fromLatin1(cursor.first(colon)));
              check(found != session->streams.end(), 410, "MCP replay stream is no longer available");
              writer = attach(request, session, found->second, sequence, true);
          }
        }
        streamResponse(response, writer);
    }
    void pump() {
        while (!stopping) {
            for (const auto& session : snapshot()) {
                try {
                    if (session->protocol->isClosed()) { retire(session); }
                    else {
                        const auto frames = session->protocol->takeFrames();
                        const auto received = session->protocol->takeNotifications();
                        bool expired = false;
                        {
                            std::lock_guard lock(session->mutex);
                            if (!session->closed) {
                                for (const auto& frame : frames) {
                                    const auto found = frame.channel.isEmpty() ? session->background : session->streams.contains(frame.channel) ? session->streams.at(frame.channel) : nullptr;
                                    if (!found || found->completed) continue;
                                    if (frame.message.isUndefined()) {
                                        if (found->requestIds.remove(detail::key(frame.cancelledRequestId)) && found->requestIds.isEmpty()) {
                                            found->completed = true; found->completedAt = Clock::now();
                                        }
                                        continue;
                                    }
                                    const auto message = frame.message;
                                    append(*session, *found, detail::encodeValue(message, options.maxHistoryBytes).trimmed());
                                    if (!found->background && (message.isArray() || (message.isObject() && !message.toObject().contains("method")))) {
                                        found->completed = true; found->completedAt = Clock::now();
                                    }
                                }
                                expireStreams(*session);
                                bool active = false;
                                for (const auto& [id, stream] : session->streams) active |= stream->writers || (!stream->background && !stream->completed);
                                expired = !active && Clock::now() - session->touched >= std::chrono::milliseconds(options.sessionIdleTimeoutMs);
                                session->changed.notify_all();
                            }
                        }
                        if (expired) retire(session);
                        if (!received.isEmpty()) {
                            std::lock_guard lock(mutex);
                            for (const auto& message : received) {
                                const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact).size();
                                require(notifications.size() < options.maxNotifications && notificationBytes + bytes <= options.maxHistoryBytes,
                                    "MCP HTTP host notification queue is full", ErrorCode::ResourceLimit);
                                notifications.append({session->protocol->id(), session->principal, message}); notificationBytes += bytes;
                            }
                        }
                    }
                    bool cleaned = false;
                    { std::lock_guard lock(session->mutex); cleaned = session->closed && session->cleanup.valid()
                        && session->cleanup.wait_for(0ms) == std::future_status::ready; }
                    if (cleaned) { std::lock_guard lock(mutex); sessions.erase(session->protocol->id()); }
                } catch (...) { retire(session); }
            }
            std::this_thread::sleep_for(10ms);
        }
    }
    bool listen(quint16 requested) {
        if (!stopping || listener.joinable()) { lastError = "MCP HTTP server is already listening"; return false; }
        const int port = requested ? (server.bind_to_port("127.0.0.1", requested) ? int(requested) : -1) : server.bind_to_any_port("127.0.0.1");
        if (port < 1) { server.stop(); lastError = "Cannot bind MCP HTTP server"; return false; }
        boundPort = quint16(port); stopping = false; lastError.clear();
        try {
            maintenance = std::thread([this] { pump(); });
            listener = std::thread([this] {
                try { server.listen_after_bind(); }
                catch (...) { server.stop(); server.decommission(); }
                stopping = true;
            });
        }
        catch (...) { close(); throw; }
        server.wait_until_ready();
        if (!server.is_running()) { close(); lastError = "Cannot start MCP HTTP listener"; return false; }
        return true;
    }
    void close() {
        stopping = true; server.stop();
        const auto remaining = snapshot(); for (const auto& session : remaining) retire(session);
        if (maintenance.joinable()) maintenance.join();
        if (listener.joinable()) listener.join();
        // A bind/accept race can mark httplib decommissioned after the first
        // stop. Reset it only after the accept thread has finished.
        server.stop();
        const auto finalSessions = snapshot();
        for (const auto& session : finalSessions) { retire(session); session->protocol->close(); }
        { std::lock_guard lock(mutex); sessions.clear(); notifications.clear(); notificationBytes = 0; }
        boundPort = 0;
    }
};
HttpServer::HttpServer(HttpServerFactory factory, HttpServerOptions options) : d(std::make_unique<Impl>(std::move(factory), std::move(options))) {}
HttpServer::~HttpServer() { close(); }
bool HttpServer::listen(quint16 port) { return d->listen(port); }
quint16 HttpServer::port() const { return d->boundPort; }
QUrl HttpServer::endpoint() const { return QUrl("http://127.0.0.1:" + QString::number(port()) + "/mcp"); }
QString HttpServer::errorString() const { return d->lastError; }
void HttpServer::close() { d->close(); }
QStringList HttpServer::sessionIds(const QString& principal) const {
    QStringList result;
    for (const auto& session : d->snapshot()) if ((principal.isEmpty() || principal == session->principal) && session->protocol->isInitialized()) result.append(session->protocol->id());
    return result;
}
void HttpServer::notify(const QString& sessionId, QString method, QJsonObject params) {
    std::shared_ptr<Session> session;
    { std::lock_guard lock(d->mutex); const auto found = d->sessions.find(sessionId);
      require(found != d->sessions.end(), "MCP HTTP session not found", ErrorCode::NotFound); session = found->second; }
    session->protocol->notify(std::move(method), std::move(params));
}
QList<HttpServerNotification> HttpServer::takeNotifications() {
    std::lock_guard lock(d->mutex); auto result = std::move(d->notifications); d->notifications.clear(); d->notificationBytes = 0; return result;
}
}
