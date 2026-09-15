#include "third_party/cpp-httplib/httplib.h"
#include "HttpApiServer.h"
#include "Parameters.h"
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
    case ErrorCode::Unauthorized: return 401;
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
{
    if (error.code() == ErrorCode::Unauthorized) response.set_header("WWW-Authenticate", "Bearer realm=\"iiLocalLLM\"");
    respond(response, errorObject(error.code(), QString::fromUtf8(error.what())), statusFor(error.code()));
}
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
struct ParsedCompletion { CompletionRequest request; std::optional<ConversationRequest> conversation; bool stream = false; bool includeUsage = false; };
ParsedCompletion parse(const std::string& body)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(body), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(), QStringLiteral("Expected a JSON object"));
    const auto object = document.object();
    fields(object, {"model", "messages", "stream", "stream_options", "max_tokens", "max_completion_tokens", "temperature", "top_p", "top_k", "seed", "stop", "n", "keep_alive", "min_p", "typical_p", "min_keep", "repetition_penalty", "repetition_context_size", "presence_penalty", "frequency_penalty", "xtc_probability", "xtc_threshold", "logit_bias", "tools", "tool_choice", "parallel_tool_calls"});
    ParsedCompletion parsed;
    require(object.value("model").isString(), QStringLiteral("model must be a model:// URI"));
    parsed.request.model = object.value("model").toString();
    require(!parsed.request.model.trimmed().isEmpty(), QStringLiteral("model must be a model:// URI or registered alias"));
    parsed.request.keepAliveMs = parseKeepAlive(object.value("keep_alive"));
    require(object.value("messages").isArray(), QStringLiteral("messages must be an array"));
    const auto messages = object.value("messages").toArray();
    require(!messages.isEmpty() && messages.size() <= 4096, QStringLiteral("messages must contain 1 to 4096 text messages"));
    require(!object.contains("tools") || object["tools"].isArray(), "tools must be an array");
    const auto toolChoice = object.value("tool_choice").toString("auto");
    require(!object.contains("tool_choice") || (object["tool_choice"].isString()
        && (toolChoice == "auto" || toolChoice == "none" || toolChoice == "required")), "tool_choice supports auto, none, or required");
    const auto parallel = boolean(object, "parallel_tool_calls", true);
    bool structured = !object["tools"].toArray().isEmpty() || toolChoice == "required";
    for (const auto& value : messages) {
        const auto m = value.toObject();
        structured |= m["role"] == "tool" || m.contains("tool_calls") || m.contains("tool_call_id") || m.contains("reasoning_content");
    }
    for (const auto& value : messages) {
        require(value.isObject(), QStringLiteral("Each message must be an object"));
        const auto message = value.toObject();
        if (structured) {
            fields(message, {"role", "content", "tool_calls", "tool_call_id", "reasoning_content", "name"});
            continue; // Structured protocol validation belongs to Service::converse.
        }
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
    QJsonObject generation;
    for (const auto& field : ParameterCatalog::builtin().group("iiLocalLLM.GenerationOptions").parameters)
        if (object.contains(field.name)) generation.insert(field.name, object.value(field.name));
    if (object.contains("max_completion_tokens")) generation.insert("max_tokens", object.value("max_completion_tokens"));
    if (generation.value("stop").isString()) generation.insert("stop", QJsonArray{generation.value("stop")});
    parsed.request.options = generationOptionsFromJson(generation);
    require(parsed.request.options.temperature <= 2, QStringLiteral("HTTP temperature must be in [0, 2]"));
    (void)number(object, "n", 1, 1, 1, true);
    if (structured) {
        ConversationRequest conversation;
        conversation.model = parsed.request.model; conversation.messages = messages; conversation.tools = object["tools"].toArray();
        conversation.toolChoice = toolChoice; conversation.parallelToolCalls = parallel;
        conversation.options = parsed.request.options; conversation.keepAliveMs = parsed.request.keepAliveMs;
        parsed.conversation = std::move(conversation);
    }
    return parsed;
}
QString completionReason(const GenerationResult& result) {
    return result.toolCalls.isEmpty() ? enumName(result.finishReason) : QStringLiteral("tool_calls");
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
struct RpcStreamState {
    std::mutex mutex;
    std::deque<QByteArray> frames;
    qsizetype buffered = 0;
    bool overflow = false, abandoned = false;
    RpcHandle operation; // Only the HTTP handler/provider accesses this field.
};
}
class HttpApiServer::Impl {
public:
    Impl(Service& service, HttpOptions options) : service(service), options(options)
    {
        require(options.workerThreads >= 1 && options.workerThreads <= 64 && options.maxQueuedConnections >= 1
            && options.maxControlRequests >= 1 && options.maxControlRequests <= 16
            && options.maxRequestBytes > 0 && options.maxBufferedOutputBytes > 0
            && options.readTimeoutMs > 0 && options.writeTimeoutMs > 0 && options.requestTimeoutMs > 0,
            QStringLiteral("Invalid HTTP limits"));
        // Admitted work and controls have separate response-lifetime limits.
        // Four more workers can parse/reject excess work and serve /health.
        server.new_task_queue = [options] { return new httplib::ThreadPool(std::min(4,options.workerThreads),
            options.workerThreads+options.maxControlRequests+4, options.maxQueuedConnections); };
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
                admit(response,false);
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
        server.Post("/v1/rpc", [this](const auto& request, auto& response) {
            guarded(response, [&] { rpc(request, response); });
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
    std::shared_ptr<RpcHandler> rpcHandler;
    std::shared_ptr<std::atomic_int> workResponses=std::make_shared<std::atomic_int>(0);
    std::shared_ptr<std::atomic_int> controlResponses=std::make_shared<std::atomic_int>(0);

    void admit(httplib::Response& response,bool control) {
        struct Lease {
            std::shared_ptr<std::atomic_int> count;bool charged=false;
            ~Lease(){if(charged)--*count;}
        };
        auto lease=std::make_shared<Lease>();lease->count=control?controlResponses:workResponses;
        const auto limit=control?options.maxControlRequests:options.workerThreads;
        auto count=lease->count->load();
        do {if(count>=limit)throw Error(ErrorCode::QueueFull,control?"HTTP control capacity reached":"HTTP work capacity reached");}
        while(!lease->count->compare_exchange_weak(count,count+1));
        lease->charged=true;
        // cpp-httplib destroys Response after writing JSON or finishing SSE,
        // including failed writes. Copies share this lease and cannot double-release.
        response.user_data.set("iisacc/admission",std::move(lease));
    }

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
    template<class Future> void await(Future& future, const httplib::Request& request, Clock::time_point deadline)
    {
        while (future.wait_for(50ms) != std::future_status::ready) check(request.is_connection_closed, deadline);
        check(request.is_connection_closed, deadline);
    }
    void rpc(const httplib::Request& request, httplib::Response& response)
    {
        if (!rpcHandler) throw Error(ErrorCode::NotFound, "RPC API is not configured");
        const auto type = QString::fromStdString(request.get_header_value("Content-Type")).section(';', 0, 0).trimmed();
        if (type.compare("application/json", Qt::CaseInsensitive) != 0) {
            respond(response, errorObject(ErrorCode::InvalidArgument, "Content-Type must be application/json"), 415); return;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(request.body), &parseError);
        require(parseError.error == QJsonParseError::NoError && document.isObject(), "Expected a JSON object");
        const auto body = document.object(); fields(body, {"id", "method", "params", "stream"});
        require(body["id"].isString() && !body["id"].toString().trimmed().isEmpty() && body["id"].toString().size() <= 128, "Invalid RPC id");
        require(body["method"].isString() && !body["method"].toString().trimmed().isEmpty() && body["method"].toString().size() <= 128, "Invalid RPC method");
        require(!body.contains("params") || body["params"].isObject(), "RPC params must be an object");
        const auto auth = request.get_header_value("Authorization");
        if (request.get_header_value_count("Authorization") != 1 || !auth.starts_with("Bearer ") || auth.size() > 263)
            throw Error(ErrorCode::Unauthorized, "Expected one Bearer credential");
        const bool streaming = boolean(body, "stream"); const auto id = body["id"].toString();
        admit(response,rpcHandler->isControlMethod(body["method"].toString()));
        const auto deadline = Clock::now() + std::chrono::milliseconds(options.requestTimeoutMs);
        check(request.is_connection_closed, deadline);
        auto state = std::make_shared<RpcStreamState>(); const auto limit = options.maxBufferedOutputBytes;
        RpcEventCallback callback;
        if (streaming) callback = [state, id, limit](const QJsonObject& event) {
            auto data = frame({{"id", id}, {"event", "rpc"}, {"data", event}});
            std::lock_guard lock(state->mutex);
            if (state->abandoned) throw Error(ErrorCode::Cancelled, "RPC connection closed");
            if (state->overflow || data.size() > limit - state->buffered) {
                state->overflow = true; throw Error(ErrorCode::ResourceLimit, "HTTP RPC output limit exceeded");
            }
            state->buffered += data.size(); state->frames.push_back(std::move(data));
        };
        state->operation = rpcHandler->dispatch(body["method"].toString(), body["params"].toObject(), QString::fromStdString(auth.substr(7)), std::move(callback));
        auto guard = std::shared_ptr<void>(nullptr, [this, state](void*) {
            state->operation.cancel();
            { std::lock_guard lock(state->mutex); state->abandoned = true; state->frames.clear(); state->buffered = 0; }
            std::lock_guard lock(activeMutex); std::erase_if(active, [](const auto& token) { return token.isCancelled(); });
        });
        require(state->operation.result.valid() && !state->operation.requestId.isEmpty() && state->operation.requestId.size() <= 128, "Invalid RPC handler response");
        { std::lock_guard lock(activeMutex); active.push_back(state->operation.cancellation); }
        const QJsonObject envelope{{"id", id}, {"request_id", state->operation.requestId}};
        auto finish = [envelope, limit](const std::shared_future<QJsonValue>& future) {
            auto result = envelope; result["result"] = future.get();
            if (json(result).size() > limit) throw Error(ErrorCode::ResourceLimit, "HTTP RPC result limit exceeded");
            return result;
        };
        if (!streaming) {
            try { await(state->operation.result, request, deadline); respond(response, finish(state->operation.result)); }
            catch (const Error& error) { auto result = errorObject(error.code(), QString::fromUtf8(error.what()));
                for (auto it = envelope.begin(); it != envelope.end(); ++it) result[it.key()] = it.value();
                respond(response, result, statusFor(error.code())); }
            return;
        }
        auto accepted = envelope; accepted["event"] = "accepted";
        { std::lock_guard lock(state->mutex); auto data = frame(accepted);
            state->buffered += data.size(); state->frames.push_front(std::move(data));
            state->overflow |= state->buffered > limit; }
        response.set_header("X-Accel-Buffering", "no");
        response.set_chunked_content_provider("text/event-stream; charset=utf-8",
            [this, state, guard, finish, envelope, deadline, disconnected = request.is_connection_closed](size_t, httplib::DataSink& sink) {
                auto failed = [&](ErrorCode code, const QString& message) {
                    state->operation.cancel();
                    if (code == ErrorCode::Cancelled || code == ErrorCode::ShuttingDown) return false;
                    auto result = envelope; result["event"] = "done"; result["error"] = errorObject(code, message)["error"];
                    const auto data = frame(result) + QByteArrayLiteral("data: [DONE]\n\n");
                    if (!sink.write(data.constData(), size_t(data.size()))) return false; sink.done(); return true;
                };
                try {
                    check(disconnected, deadline);
                    // Read readiness before draining: the future fences callbacks.
                    const bool ready = state->operation.result.wait_for(10ms) == std::future_status::ready;
                    std::deque<QByteArray> pending;
                    { std::lock_guard lock(state->mutex);
                        if (state->overflow) throw Error(ErrorCode::ResourceLimit, "HTTP RPC output limit exceeded");
                        pending.swap(state->frames); state->buffered = 0; }
                    for (const auto& data : pending) if (!sink.write(data.constData(), size_t(data.size()))) return false;
                    if (ready) {
                        auto result = finish(state->operation.result); result["event"] = "done";
                        const auto data = frame(result) + QByteArrayLiteral("data: [DONE]\n\n");
                        if (!sink.write(data.constData(), size_t(data.size()))) return false; sink.done();
                    }
                    return true;
                } catch (const Error& error) { return failed(error.code(), QString::fromUtf8(error.what())); }
                catch (const std::exception&) { return failed(ErrorCode::RuntimeFailure, "RPC handler failed"); }
                catch (...) { return failed(ErrorCode::RuntimeFailure, "Unknown RPC handler failure"); }
            }, [guard](bool) {});
    }
    void complete(const httplib::Request& request, httplib::Response& response)
    {
        const auto type = QString::fromStdString(request.get_header_value("Content-Type")).section(';', 0, 0).trimmed();
        if (type.compare(QStringLiteral("application/json"), Qt::CaseInsensitive) != 0) {
            respond(response, errorObject(ErrorCode::InvalidArgument, QStringLiteral("Content-Type must be application/json")), 415);
            return;
        }
        const auto parsed = parse(request.body);
        admit(response,false);
        const auto deadline = Clock::now() + std::chrono::milliseconds(options.requestTimeoutMs);
        const auto created = QDateTime::currentSecsSinceEpoch();
        auto state = std::make_shared<StreamState>();
        const auto limit = options.maxBufferedOutputBytes;
        const auto onEvent = [state, parsed, created, limit](const StreamEvent& event) {
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
                if (!event.result.toolCalls.isEmpty()) state->output += QJsonDocument(event.result.toolCalls).toJson(QJsonDocument::Compact).size();
                state->output += event.result.reasoning.toUtf8().size();
                if (parsed.stream) {
                    if (event.result.errorCode == ErrorCode::None) {
                        if (!event.result.reasoning.isEmpty()) bytes += makeChunk({{"reasoning_content", event.result.reasoning}});
                        if (!event.result.toolCalls.isEmpty()) {
                            QJsonArray calls;
                            for (qsizetype i = 0; i < event.result.toolCalls.size(); ++i) {
                                auto call = event.result.toolCalls[i].toObject(); call["index"] = i; calls.append(call);
                            }
                            bytes += makeChunk({{"tool_calls", calls}});
                        }
                        bytes += makeChunk({}, completionReason(event.result));
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
        };
        state->generation = parsed.conversation ? service.converse(*parsed.conversation, onEvent) : service.complete(parsed.request, onEvent);
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
                QJsonObject message{{"role", "assistant"}, {"content", state->result.text}};
                if (!state->result.toolCalls.isEmpty()) {
                    message["tool_calls"] = state->result.toolCalls;
                    if (state->result.text.isEmpty()) message["content"] = QJsonValue::Null;
                }
                if (!state->result.reasoning.isEmpty()) message["reasoning_content"] = state->result.reasoning;
                value.insert("choices", QJsonArray{QJsonObject{{"index", 0}, {"message", message},
                    {"finish_reason", completionReason(state->result)}}});
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
void HttpApiServer::setRpcHandler(std::shared_ptr<RpcHandler> handler) {
    if (d->thread.joinable()) throw Error(ErrorCode::ModelInUse, "Close HTTP before replacing its RPC handler");
    d->rpcHandler = std::move(handler);
}
void HttpApiServer::close() { d->close(); }
quint16 HttpApiServer::port() const { return d->boundPort; }
QString HttpApiServer::errorString() const { return d->lastError; }
} // namespace iiLocalLLM
