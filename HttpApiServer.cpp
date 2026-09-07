#include "third_party/cpp-httplib/httplib.h"
#include "HttpApiServer.h"
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSet>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <optional>

namespace iiLocalLLM {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
int statusFor(ErrorCode code)
{
    switch (code) {
    case ErrorCode::InvalidArgument: case ErrorCode::InvalidManifest: case ErrorCode::ContextOverflow: return 400;
    case ErrorCode::NotFound: return 404;
    case ErrorCode::AlreadyExists: case ErrorCode::ModelInUse: return 409;
    case ErrorCode::QueueFull: case ErrorCode::ResourceLimit: return 429;
    case ErrorCode::Timeout: return 504;
    case ErrorCode::RuntimeUnavailable: case ErrorCode::ShuttingDown: return 503;
    case ErrorCode::Cancelled: return 499;
    default: return 500;
    }
}
QJsonObject errorObject(ErrorCode code, const QString& message)
{
    return {{"error", QJsonObject{{"message", message}, {"type", statusFor(code) < 500 ? "invalid_request_error" : "server_error"},
        {"param", QJsonValue::Null}, {"code", enumName(code)}}}};
}
QByteArray json(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
void respond(httplib::Response& response, const QJsonObject& value, int status = 200)
{
    response.status = status;
    response.set_content(json(value).toStdString(), "application/json; charset=utf-8");
}
void fail(httplib::Response& response, const Error& error)
{ respond(response, errorObject(error.code(), QString::fromUtf8(error.what())), statusFor(error.code())); }
void require(bool condition, const QString& message)
{ if (!condition) throw Error(ErrorCode::InvalidArgument, message); }
void fields(const QJsonObject& object, const QSet<QString>& allowed)
{
    for (auto it = object.begin(); it != object.end(); ++it)
        require(allowed.contains(it.key()), QStringLiteral("Unsupported field: ") + it.key());
}
double number(const QJsonObject& object, const QString& name, double fallback, double minimum, double maximum, bool integer = false)
{
    const auto value = object.value(name);
    if (value.isUndefined()) return fallback;
    const double result = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    require(value.isDouble() && std::isfinite(result) && result >= minimum && result <= maximum
        && (!integer || std::floor(result) == result), QStringLiteral("Invalid numeric field: ") + name);
    return result;
}
bool boolean(const QJsonObject& object, const QString& name, bool fallback = false)
{
    const auto value = object.value(name);
    require(value.isUndefined() || value.isBool(), name + QStringLiteral(" must be a boolean"));
    return value.isUndefined() ? fallback : value.toBool();
}
struct ParsedCompletion { CompletionRequest request; bool stream = false; bool includeUsage = false; };
ParsedCompletion parse(const std::string& body)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(body), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(), QStringLiteral("Expected a JSON object"));
    const auto object = document.object();
    fields(object, {"model", "messages", "stream", "stream_options", "max_tokens", "max_completion_tokens", "temperature", "top_p", "top_k", "seed", "stop", "n", "keep_alive"});
    ParsedCompletion parsed;
    require(object.value("model").isString(), QStringLiteral("model must be a model:// URI"));
    parsed.request.model = object.value("model").toString();
    require(!parsed.request.model.trimmed().isEmpty(), QStringLiteral("model must be a model:// URI or registered alias"));
    parsed.request.keepAliveMs = parseKeepAlive(object.value("keep_alive"));
    require(object.value("messages").isArray(), QStringLiteral("messages must be an array"));
    const auto messages = object.value("messages").toArray();
    require(!messages.isEmpty() && messages.size() <= 4096, QStringLiteral("messages must contain 1 to 4096 text messages"));
    for (const auto& value : messages) {
        require(value.isObject(), QStringLiteral("Each message must be an object"));
        const auto message = value.toObject();
        fields(message, {"role", "content"});
        require(message.value("role").isString() && message.value("content").isString(), QStringLiteral("Only string role/content messages are supported"));
        const auto role = message.value("role").toString();
        require(role == "system" || role == "user" || role == "assistant", QStringLiteral("Unsupported message role"));
        parsed.request.messages.append({role == "system" ? Role::System : role == "user" ? Role::User : Role::Assistant, message.value("content").toString()});
    }
    parsed.stream = boolean(object, "stream");
    if (object.contains("stream_options")) {
        require(parsed.stream && object.value("stream_options").isObject(), QStringLiteral("stream_options requires stream=true and an object"));
        const auto options = object.value("stream_options").toObject();
        fields(options, {"include_usage"});
        parsed.includeUsage = boolean(options, "include_usage");
    }
    require(!object.contains("max_tokens") || !object.contains("max_completion_tokens"), QStringLiteral("Specify one token limit field"));
    auto& options = parsed.request.options;
    options.maxTokens = int(number(object, object.contains("max_completion_tokens") ? "max_completion_tokens" : "max_tokens", 256, 1, 1048576, true));
    options.temperature = number(object, "temperature", 0.7, 0, 2);
    options.topP = number(object, "top_p", 0.9, 0.000001, 1);
    options.topK = int(number(object, "top_k", 40, 0, 1000000, true));
    options.seed = quint32(number(object, "seed", 0, 0, std::numeric_limits<quint32>::max(), true));
    (void)number(object, "n", 1, 1, 1, true);
    if (object.contains("stop")) {
        const auto stops = object.value("stop");
        require(stops.isString() || stops.isArray(), QStringLiteral("stop must be a string or string array"));
        const auto array = stops.isString() ? QJsonArray{stops} : stops.toArray();
        require(array.size() <= 16, QStringLiteral("At most 16 stop strings are supported"));
        for (const auto& stop : array) {
            require(stop.isString() && !stop.toString().isEmpty() && stop.toString().size() <= 1024, QStringLiteral("Invalid stop string"));
            options.stop.append(stop.toString());
        }
    }
    return parsed;
}
QJsonObject usage(const Usage& value)
{
    return {{"prompt_tokens", value.promptTokens}, {"completion_tokens", value.generatedTokens},
        {"total_tokens", value.promptTokens + value.generatedTokens},
        {"prompt_tokens_details", QJsonObject{{"cached_tokens", value.cachedTokens}}}};
}
QJsonObject envelope(const QString& id, const QString& model, qint64 created, bool streaming)
{ return {{"id", id}, {"object", streaming ? "chat.completion.chunk" : "chat.completion"}, {"created", created}, {"model", model}}; }
QByteArray frame(const QJsonObject& object) { return QByteArrayLiteral("data: ") + json(object) + QByteArrayLiteral("\n\n"); }
QJsonObject chunk(const QString& id, const QString& model, qint64 created, QJsonObject delta, QJsonValue reason = QJsonValue::Null)
{
    auto value = envelope(id, model, created, true);
    value.insert("choices", QJsonArray{QJsonObject{{"index", 0}, {"delta", delta}, {"finish_reason", reason}}});
    return value;
}
struct StreamState {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<QByteArray> frames;
    qsizetype buffered = 0, output = 0;
    bool started = false, finished = false, overflow = false, abandoned = false;
    GenerationResult result;
    GenerationHandle generation; // Assigned/read only by the HTTP handler, never by the service callback.
};
}
class HttpApiServer::Impl {
public:
    Impl(Service& service, HttpOptions options) : service(service), options(options)
    {
        require(options.workerThreads >= 1 && options.workerThreads <= 64 && options.maxQueuedConnections >= 1
            && options.maxRequestBytes > 0 && options.maxBufferedOutputBytes > 0
            && options.readTimeoutMs > 0 && options.writeTimeoutMs > 0 && options.requestTimeoutMs > 0,
            QStringLiteral("Invalid HTTP limits"));
        server.new_task_queue = [options] { return new httplib::ThreadPool(options.workerThreads, options.workerThreads, options.maxQueuedConnections); };
        server.set_payload_max_length(options.maxRequestBytes);
        server.set_read_timeout(std::chrono::milliseconds(options.readTimeoutMs));
        server.set_write_timeout(std::chrono::milliseconds(options.writeTimeoutMs));
        server.set_keep_alive_max_count(1);
        server.set_default_headers({{"Cache-Control", "no-store"}, {"X-Content-Type-Options", "nosniff"}});
        server.set_pre_routing_handler([this](const httplib::Request& request, httplib::Response& response) {
            const auto port = std::to_string(request.local_port);
            const auto host = request.get_header_value("Host");
            const bool localHost = host == "127.0.0.1:" + port || host == "localhost:" + port;
            const auto origin = request.get_header_value("Origin");
            if (!localHost || (request.has_header("Origin") && origin != "http://127.0.0.1:" + port && origin != "http://localhost:" + port)) {
                respond(response, errorObject(ErrorCode::InvalidArgument, QStringLiteral("Only direct localhost clients or same-origin requests are accepted")), 403);
                return httplib::Server::HandlerResponse::Handled;
            }
            if (stopping.load()) {
                fail(response, Error(ErrorCode::ShuttingDown, QStringLiteral("HTTP server is stopping")));
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
            if (!response.body.empty()) return;
            const auto code = response.status == 413 ? ErrorCode::ResourceLimit : response.status == 404 ? ErrorCode::NotFound : ErrorCode::InvalidArgument;
            respond(response, errorObject(code, QStringLiteral("HTTP request rejected (%1)").arg(response.status)), response.status);
        });
        server.Get("/health", [](const auto&, auto& response) { respond(response, {{"status", "ok"}}); });
        server.Get("/v1/models", [this](const auto& request, auto& response) {
            guarded(response, [&] {
                auto future = this->service.models();
                await(future, request, Clock::now() + std::chrono::milliseconds(this->options.requestTimeoutMs));
                QJsonArray models;
                for (const auto& model : future.get())
                    models.append(QJsonObject{{"id", model.model.uri}, {"object", "model"}, {"created", 0}, {"owned_by", "local"}});
                respond(response, {{"object", "list"}, {"data", models}});
            });
        });
        server.Post("/v1/chat/completions", [this](const auto& request, auto& response) {
            guarded(response, [&] { complete(request, response); });
        });
    }
    Service& service;
    HttpOptions options;
    httplib::Server server;
    std::thread thread;
    std::atomic_bool stopping = true;
    quint16 boundPort = 0;
    QString lastError;
    std::mutex activeMutex;
    std::vector<CancellationToken> active;

    template<class F> void guarded(httplib::Response& response, F function)
    {
        try { function(); }
        catch (const Error& error) { fail(response, error); }
        catch (const std::exception& error) { fail(response, Error(ErrorCode::RuntimeFailure, QString::fromUtf8(error.what()))); }
        catch (...) { fail(response, Error(ErrorCode::RuntimeFailure, QStringLiteral("Unhandled HTTP failure"))); }
    }
    void check(const std::function<bool()>& disconnected, Clock::time_point deadline)
    {
        if (stopping.load()) throw Error(ErrorCode::ShuttingDown, QStringLiteral("HTTP server is stopping"));
        if (disconnected()) throw Error(ErrorCode::Cancelled, QStringLiteral("HTTP client disconnected"));
        if (Clock::now() >= deadline) throw Error(ErrorCode::Timeout, QStringLiteral("HTTP request deadline exceeded"));
    }
    template<class T> void await(std::future<T>& future, const httplib::Request& request, Clock::time_point deadline)
    {
        while (future.wait_for(50ms) != std::future_status::ready) check(request.is_connection_closed, deadline);
        check(request.is_connection_closed, deadline);
    }
    void complete(const httplib::Request& request, httplib::Response& response)
    {
        const auto type = QString::fromStdString(request.get_header_value("Content-Type")).section(';', 0, 0).trimmed();
        if (type.compare(QStringLiteral("application/json"), Qt::CaseInsensitive) != 0) {
            respond(response, errorObject(ErrorCode::InvalidArgument, QStringLiteral("Content-Type must be application/json")), 415);
            return;
        }
        const auto parsed = parse(request.body);
        const auto deadline = Clock::now() + std::chrono::milliseconds(options.requestTimeoutMs);
        const auto created = QDateTime::currentSecsSinceEpoch();
        auto state = std::make_shared<StreamState>();
        const auto limit = options.maxBufferedOutputBytes;
        state->generation = service.complete(parsed.request, [state, parsed, created, limit](const StreamEvent& event) {
            std::lock_guard lock(state->mutex);
            if (state->abandoned) return;
            const auto id = QStringLiteral("chatcmpl-") + event.requestId;
            const auto makeChunk = [&](QJsonObject delta, QJsonValue reason = QJsonValue::Null) {
                auto value = chunk(id, parsed.request.model, created, std::move(delta), std::move(reason));
                if (parsed.includeUsage) value.insert("usage", QJsonValue::Null);
                return frame(value);
            };
            QByteArray bytes;
            if (event.kind == StreamEventKind::Started) {
                state->started = true;
                if (parsed.stream) bytes = makeChunk({{"role", "assistant"}, {"content", ""}});
            } else if (event.kind == StreamEventKind::Delta) {
                state->output += event.text.toUtf8().size();
                if (parsed.stream) bytes = makeChunk({{"content", event.text}});
            } else {
                state->finished = true;
                state->result = event.result;
                if (parsed.stream) {
                    if (event.result.errorCode == ErrorCode::None) {
                        bytes = makeChunk({}, enumName(event.result.finishReason));
                        if (parsed.includeUsage) {
                            auto value = envelope(id, parsed.request.model, created, true);
                            value.insert("choices", QJsonArray{}); value.insert("usage", usage(event.result.usage));
                            bytes += frame(value);
                        }
                    } else bytes = frame(errorObject(event.result.errorCode, event.result.errorMessage));
                    bytes += QByteArrayLiteral("data: [DONE]\n\n");
                }
            }
            if (state->output > limit || state->buffered + bytes.size() > limit) {
                state->overflow = true;
                state->changed.notify_all();
                if (event.kind != StreamEventKind::Finished) throw Error(ErrorCode::ResourceLimit, QStringLiteral("HTTP output limit exceeded"));
                return;
            }
            if (!bytes.isEmpty()) { state->buffered += bytes.size(); state->frames.push_back(std::move(bytes)); }
            state->changed.notify_all();
        });
        {
            std::lock_guard lock(activeMutex);
            active.push_back(state->generation.cancellation);
        }
        // The guard cancels abandoned responses, including exceptions before a content provider is installed.
        auto guard = std::shared_ptr<void>(nullptr, [this, state](void*) {
            state->generation.cancel();
            { std::lock_guard lock(state->mutex); state->abandoned = true; }
            // Remove expired work without comparing CancellationToken internals.
            std::lock_guard lock(activeMutex);
            std::erase_if(active, [](const auto& token) { return token.isCancelled(); });
        });
        {
            std::unique_lock lock(state->mutex);
            while (!(parsed.stream && state->started) && !state->finished && !state->overflow) {
                state->changed.wait_for(lock, 50ms);
                check(request.is_connection_closed, deadline);
            }
            if (state->overflow) throw Error(ErrorCode::ResourceLimit, QStringLiteral("HTTP output limit exceeded"));
            if (state->finished && (!parsed.stream || !state->started)) {
                if (state->result.errorCode != ErrorCode::None) throw Error(state->result.errorCode, state->result.errorMessage);
                auto value = envelope(QStringLiteral("chatcmpl-") + state->result.requestId, parsed.request.model, created, false);
                value.insert("choices", QJsonArray{QJsonObject{{"index", 0}, {"message", QJsonObject{{"role", "assistant"}, {"content", state->result.text}}},
                    {"finish_reason", enumName(state->result.finishReason)}}});
                value.insert("usage", usage(state->result.usage));
                respond(response, value);
                return;
            }
        }
        response.set_header("X-Accel-Buffering", "no");
        response.set_chunked_content_provider("text/event-stream; charset=utf-8",
            [this, state, guard, disconnected = request.is_connection_closed, deadline](size_t, httplib::DataSink& sink) {
                try {
                    std::unique_lock lock(state->mutex);
                    while (state->frames.empty() && !state->finished && !state->overflow) {
                        state->changed.wait_for(lock, 50ms);
                        check(disconnected, deadline);
                    }
                    check(disconnected, deadline);
                    if (state->overflow) throw Error(ErrorCode::ResourceLimit, QStringLiteral("HTTP output limit exceeded"));
                    std::deque<QByteArray> frames;
                    frames.swap(state->frames); state->buffered = 0;
                    const bool finished = state->finished;
                    lock.unlock();
                    for (const auto& bytes : frames) if (!sink.write(bytes.constData(), size_t(bytes.size()))) { state->generation.cancel(); return false; }
                    if (finished) sink.done();
                    return true;
                } catch (const Error& error) {
                    state->generation.cancel();
                    if (error.code() == ErrorCode::Cancelled || error.code() == ErrorCode::ShuttingDown) return false;
                    const auto bytes = frame(errorObject(error.code(), QString::fromUtf8(error.what()))) + QByteArrayLiteral("data: [DONE]\n\n");
                    if (!sink.write(bytes.constData(), size_t(bytes.size()))) return false;
                    sink.done(); return true;
                }
            }, [guard](bool) {});
    }
    bool listen(quint16 requested)
    {
        if (thread.joinable()) { lastError = QStringLiteral("HTTP server is already listening"); return false; }
        lastError.clear();
        const int bound = requested ? (server.bind_to_port("127.0.0.1", requested) ? int(requested) : -1) : server.bind_to_any_port("127.0.0.1");
        if (bound < 0) { lastError = QStringLiteral("Cannot bind localhost HTTP port"); return false; }
        boundPort = quint16(bound);
        stopping = false;
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
        if (!server.is_running()) { close(); lastError = QStringLiteral("HTTP listener failed to start"); return false; }
        return true;
    }
    void close()
    {
        stopping = true;
        server.stop();
        { std::lock_guard lock(activeMutex); for (const auto& token : active) token.cancel(); }
        if (thread.joinable()) thread.join();
        boundPort = 0;
    }
};
HttpApiServer::HttpApiServer(Service& service, HttpOptions options) : d(std::make_unique<Impl>(service, options)) {}
HttpApiServer::~HttpApiServer() { d->close(); }
bool HttpApiServer::listen(quint16 port) { return d->listen(port); }
void HttpApiServer::close() { d->close(); }
quint16 HttpApiServer::port() const { return d->boundPort; }
QString HttpApiServer::errorString() const { return d->lastError; }
} // namespace iiLocalLLM
