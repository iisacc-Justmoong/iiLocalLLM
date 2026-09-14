#include "Client.h"
#include "ClientTransport.h"
#include "Protocol.h"
#include "BatchReplies.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QSet>
#include <QtCore/QStringDecoder>
#include <QtCore/QThreadPool>
#include <QtCore/QUuid>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <climits>
#include <thread>

namespace iiLocalLLM::mcp {
namespace {
using Clock = std::chrono::steady_clock;
using namespace detail;

}
RpcError::RpcError(int code, const QString& message, QJsonValue data)
    : Error(ErrorCode::ProtocolError, message), code_(code), data_(std::move(data)) {}

class Client::Impl : public std::enable_shared_from_this<Impl> {
public:
    struct Pending {
        QString id;
        QString method;
        QByteArray frame;
        CancellationToken cancellation;
        CancellationToken abandoned;
        Clock::time_point deadline;
        bool progressEnabled = false;
        double lastProgress = -1;
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<std::pair<QJsonObject, qsizetype>> progress;
        std::optional<QJsonObject> result;
        std::exception_ptr error;
        bool done = false;
        bool internal = false;
    };
    struct Outgoing { QByteArray frame; std::shared_ptr<Pending> request; };
    struct Incoming { QJsonValue id; CancellationToken cancellation; };
    struct Completion { QString key; QJsonObject message; quint64 epoch; };
    ClientLimits options;
    std::unique_ptr<detail::ClientTransport> transport;
    quint64 generation = 0, handlerEpoch = 0;
    ClientOptions client;
    mutable std::mutex mutex;
    std::mutex joining;
    std::thread worker;
    std::atomic_bool stopping = false;
    std::atomic<qsizetype> callbackBytes = 0;
    bool connected = false, initialized = false;
    size_t pendingCount = 0;
    qsizetype queuedBytes = 0;
    QJsonObject info;
    QByteArray stderrBytes;
    std::deque<Outgoing> outgoing;
    QList<QJsonObject> notifications;
    qsizetype notificationBytes = 0;
    std::deque<Completion> completions;
    std::map<QString, std::shared_ptr<Pending>> active;
    std::map<QString, Incoming> incoming;
    detail::BatchReplies batches;

    Impl(std::unique_ptr<detail::ClientTransport> t, ClientLimits o, ClientOptions c) : options(std::move(o)), transport(std::move(t)), client(std::move(c)),
        batches(int(std::min<qint64>(1000000, qint64(options.maxNotificationCount) + options.maxServerRequests)), options.maxQueuedBytes, options.maxMessageBytes) {
        require(options.initializeTimeoutMs > 0 && options.requestTimeoutMs > 0
            && options.shutdownTimeoutMs >= 0 && options.shutdownTimeoutMs <= 10000
            && options.maxPendingRequests > 0 && options.maxServerRequests > 0
            && options.maxMessageBytes >= 1024 && options.maxQueuedBytes >= options.maxMessageBytes
            && options.maxNotificationCount > 0 && options.maxListItems > 0,
            "Invalid MCP process or resource limits", ErrorCode::InvalidArgument);
        require(client.implementation["name"].isString() && !client.implementation["name"].toString().isEmpty()
            && client.implementation["version"].isString(), "Invalid MCP client implementation", ErrorCode::InvalidArgument);
        const QStringList supported{"2025-11-25", "2025-06-18", "2025-03-26"};
        require(!client.protocolVersions.isEmpty(), "No MCP protocol versions configured", ErrorCode::InvalidArgument);
        for (const auto& version : client.protocolVersions)
            require(supported.contains(version), "Unsupported MCP client protocol version", ErrorCode::InvalidArgument);
        validateRoots(client.roots);
        require(QJsonDocument(client.roots).toJson(QJsonDocument::Compact).size() + 256 <= options.maxMessageBytes,
            "MCP roots exceed message limit", ErrorCode::ResourceLimit);
        for (const auto& [name, handler] : client.requestHandlers)
            require(!name.isEmpty() && handler && name != "ping" && name != "roots/list",
                "Invalid or reserved MCP request handler", ErrorCode::InvalidArgument);
        for (const auto& [capability, method] : {std::pair{"sampling", "sampling/createMessage"}, {"elicitation", "elicitation/create"}})
            require(!client.capabilities.contains(capability) || (client.capabilities[capability].isObject()
                && client.requestHandlers.contains(method)), "MCP capability has no host handler", ErrorCode::InvalidArgument);
        require(!client.capabilities.contains("tasks"), "MCP task lifecycle is not implemented", ErrorCode::InvalidArgument);
        client.capabilities["roots"] = QJsonObject{{"listChanged", true}};
    }
    void enqueueLocked(QByteArray frame, std::shared_ptr<Pending> request = {}) {
        require(!stopping && connected, "MCP connection is closed", ErrorCode::ShuttingDown);
        require(queuedBytes + frame.size() <= options.maxQueuedBytes, "MCP outbound queue is full", ErrorCode::QueueFull);
        if (request) {
            require(pendingCount < size_t(options.maxPendingRequests), "MCP request queue is full", ErrorCode::QueueFull);
            ++pendingCount;
        }
        queuedBytes += frame.size(); outgoing.push_back({std::move(frame), std::move(request)});
    }
    void finish(const std::shared_ptr<Pending>& p, QJsonObject value = {}, std::exception_ptr error = {}) {
        { std::lock_guard lock(mutex); --pendingCount; }
        { std::lock_guard lock(p->mutex); p->result = std::move(value); p->error = error; p->done = true; }
        p->changed.notify_all();
    }
    QJsonObject perform(QString method, QJsonObject params, CancellationToken token, ProgressCallback progress, int timeout, bool initialize = false, quint64 expectedGeneration = 0) {
        token.throwIfCancelled();
        require(!method.isEmpty() && (initialize || (method != "initialize" && !method.startsWith("notifications/"))),
            "Invalid MCP request method", ErrorCode::InvalidArgument);
        require(timeout >= 0, "MCP timeout cannot be negative", ErrorCode::InvalidArgument);
        auto p = std::make_shared<Pending>(); p->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        p->method = method; p->cancellation = token; p->progressEnabled = bool(progress);
        p->deadline = Clock::now() + std::chrono::milliseconds(timeout ? timeout : options.requestTimeoutMs);
        require(!params.contains("_meta") || params["_meta"].isObject(), "MCP _meta must be an object", ErrorCode::InvalidArgument);
        auto meta = params.value("_meta").toObject();
        require(!meta.contains("progressToken"), "Pass a progress callback instead of a manual progressToken", ErrorCode::InvalidArgument);
        if (progress) { meta["progressToken"] = p->id; params["_meta"] = meta; }
        p->frame = encode({{"jsonrpc", "2.0"}, {"id", p->id}, {"method", method}, {"params", params}}, options.maxMessageBytes);
        {
            std::lock_guard lock(mutex);
            require(initialize || initialized, "MCP connection is not initialized", ErrorCode::ShuttingDown);
            require(!expectedGeneration || generation == expectedGeneration, "MCP session changed; refresh tool definitions", ErrorCode::RuntimeFailure);
            enqueueLocked(p->frame, p);
        }
        try {
            for (;;) {
                std::deque<std::pair<QJsonObject, qsizetype>> updates;
                bool done; QJsonObject result; std::exception_ptr error;
                {
                    std::unique_lock lock(p->mutex);
                    p->changed.wait(lock, [&] { return p->done || !p->progress.empty(); });
                    updates.swap(p->progress); done = p->done; error = p->error;
                    if (done) result = *p->result;
                }
                for (const auto& update : updates) callbackBytes -= update.second;
                for (const auto& update : updates) if (progress) {
                    try { progress(update.first); }
                    catch (...) { throw Error(ErrorCode::ConsumerFailure, "MCP progress consumer threw an exception"); }
                }
                if (done) { if (error) std::rethrow_exception(error); return result; }
            }
        } catch (...) {
            p->abandoned.cancel();
            std::lock_guard lock(p->mutex);
            for (const auto& update : p->progress) callbackBytes -= update.second;
            p->progress.clear();
            throw;
        }
    }
    void write(const QByteArray& frame) { transport->send(frame); }
    void reply(const QJsonObject& message) {
        if (const auto frame = batches.response(message)) write(detail::encodeValue(*frame, options.maxMessageBytes));
    }
    void handle(QThreadPool& handlers, const QByteArray& bytes) {
        QStringDecoder decoder(QStringDecoder::Utf8); const QString decoded = decoder(bytes);
        require(!decoder.hasError(), "MCP frame is not valid UTF-8");
        QJsonParseError parse; const auto document = QJsonDocument::fromJson(bytes, &parse);
        require(parse.error == QJsonParseError::NoError, "MCP requires JSON frames");
        if (document.isArray()) {
            QString negotiated;
            { std::lock_guard lock(mutex); negotiated = info["protocolVersion"].toString(); }
            const auto array = document.array();
            if (negotiated.isEmpty() && client.protocolVersions.contains("2025-03-26")) {
                for (const auto& value : array) {
                    const auto item = value.toObject();
                    if (!item.contains("id") || item.contains("method")) continue;
                    const auto pending = active.find(key(item["id"]));
                    if (pending != active.end() && pending->second->method == "initialize"
                        && item["result"].toObject()["protocolVersion"] == "2025-03-26") negotiated = "2025-03-26";
                }
            }
            require(negotiated == "2025-03-26" && !array.isEmpty(), "MCP batches require protocol 2025-03-26");
            require(array.size() <= options.maxNotificationCount, "MCP batch item limit reached", ErrorCode::ResourceLimit);
            const auto prepared = batches.prepare(array);
            if (prepared.immediate) write(detail::encodeValue(*prepared.immediate, options.maxMessageBytes));
            for (const auto& item : prepared.messages) handle(handlers, QJsonDocument(item).toJson(QJsonDocument::Compact));
            return;
        }
        require(document.isObject(), "MCP requires a JSON-RPC object or negotiated legacy batch");
        const auto message = document.object();
        require(message["jsonrpc"] == "2.0", "Invalid JSON-RPC version");
        if (message.contains("method")) {
            require(message["method"].isString() && !message["method"].toString().isEmpty()
                && !message.contains("result") && !message.contains("error"), "Invalid MCP request or notification");
            const auto method = message["method"].toString();
            if (message.contains("params") && !message["params"].isObject()) {
                if (message.contains("id")) { key(message["id"]); reply(rpcError(message["id"], -32602, "Expected object params")); return; }
                throw Error(ErrorCode::ProtocolError, "MCP notification params must be an object");
            }
            const auto params = message["params"].toObject();
            if (message.contains("id")) {
                const auto requestKey = key(message["id"]);
                require(!incoming.contains(requestKey), "MCP server reused an active request ID");
                if (method == "ping") { reply({{"jsonrpc", "2.0"}, {"id", message["id"]}, {"result", QJsonObject{}}}); return; }
                bool ready; QJsonArray roots;
                { std::lock_guard lock(mutex); ready = initialized; roots = client.roots; }
                if (!ready) { reply(rpcError(message["id"], -32002, "Client not initialized")); return; }
                if (method == "roots/list") { reply({{"jsonrpc", "2.0"}, {"id", message["id"]}, {"result", QJsonObject{{"roots", roots}}}}); return; }
                const auto found = client.requestHandlers.find(method);
                const auto capability = method == "sampling/createMessage" ? "sampling" : method == "elicitation/create" ? "elicitation" : "experimental";
                if (found == client.requestHandlers.end() || !client.capabilities.contains(capability)) {
                    reply(rpcError(message["id"], -32601, "Client method not supported")); return;
                }
                if (incoming.size() >= size_t(options.maxServerRequests)) {
                    reply(rpcError(message["id"], -32000, "Client request capacity reached")); return;
                }
                Incoming request{message["id"], {}}; incoming.emplace(requestKey, request);
                auto self = shared_from_this(); auto handler = found->second;
                handlers.start([self, requestKey, request, handler, params, epoch = handlerEpoch] {
                    QJsonObject response;
                    try {
                        request.cancellation.throwIfCancelled();
                        response = {{"jsonrpc", "2.0"}, {"id", request.id}, {"result", handler(params, request.cancellation)}};
                        encode(response, self->options.maxMessageBytes);
                    } catch (const RpcError& e) { response = rpcError(request.id, e.rpcCode(), QString::fromUtf8(e.what()), e.data()); }
                    catch (const std::exception& e) { response = rpcError(request.id, -32603, QString::fromUtf8(e.what()).left(1024)); }
                    catch (...) { response = rpcError(request.id, -32603, "Host request handler failed"); }
                    std::lock_guard lock(self->mutex); self->completions.push_back({requestKey, std::move(response), epoch});
                });
                return;
            }
            if (method == "notifications/cancelled") {
                const auto requestKey = key(params["requestId"]);
                const auto found = incoming.find(requestKey);
                if (found != incoming.end()) {
                    found->second.cancellation.cancel();
                    if (const auto frame = batches.cancel(requestKey)) write(detail::encodeValue(*frame, options.maxMessageBytes));
                }
            } else if (method == "notifications/progress") {
                const auto token = key(params["progressToken"]); const auto found = active.find(token);
                if (found == active.end() || !found->second->progressEnabled) return;
                auto p = found->second;
                if (p->abandoned.isCancelled()) return;
                require(params["progress"].isDouble() && std::isfinite(params["progress"].toDouble())
                    && params["progress"].toDouble() > p->lastProgress
                    && (!params.contains("total") || (params["total"].isDouble() && std::isfinite(params["total"].toDouble())))
                    && (!params.contains("message") || params["message"].isString()), "Invalid MCP progress notification");
                const auto size = bytes.size();
                require(callbackBytes + size <= options.maxQueuedBytes, "MCP callback queue exceeds its limit", ErrorCode::ResourceLimit);
                { std::lock_guard lock(p->mutex); if (p->abandoned.isCancelled()) return;
                    callbackBytes += size; p->progress.push_back({params, size}); p->lastProgress = params["progress"].toDouble(); }
                p->changed.notify_all();
            } else {
                std::lock_guard lock(mutex);
                require(notifications.size() < options.maxNotificationCount && notificationBytes + bytes.size() <= options.maxQueuedBytes,
                    "MCP notification queue exceeds its limit", ErrorCode::ResourceLimit);
                notifications.append(message); notificationBytes += bytes.size();
            }
            return;
        }
        require(message.contains("id") && (message.contains("result") != message.contains("error")), "Invalid JSON-RPC response");
        const auto requestKey = key(message["id"]);
        std::exception_ptr error;
        if (message.contains("error")) {
            const auto e = message["error"].toObject(); const auto code = e["code"].toDouble();
            require(e["code"].isDouble() && std::trunc(code) == code && code >= INT_MIN && code <= INT_MAX
                && e["message"].isString(), "Invalid JSON-RPC error");
            error = std::make_exception_ptr(RpcError(int(code), e["message"].toString(), e["data"]));
        } else require(message["result"].isObject(), "MCP result must be an object");
        const auto found = active.find(requestKey);
        if (found == active.end()) return; // A cancelled or timed-out request may still produce a reply.
        auto p = found->second; active.erase(found);
        if (p->cancellation.isCancelled() || p->abandoned.isCancelled())
            error = std::make_exception_ptr(Error(ErrorCode::Cancelled, "MCP request cancelled"));
        else if (Clock::now() >= p->deadline)
            error = std::make_exception_ptr(Error(ErrorCode::Timeout,
                "MCP " + p->method.left(128) + " response arrived after the request deadline"));
        if (p->internal) {
            if (error) { finish(p, {}, error); std::rethrow_exception(error); }
            try { acceptInitialize(message["result"].toObject()); }
            catch (...) { finish(p, {}, std::current_exception()); throw; }
        }
        finish(p, message["result"].toObject(), error);
    }
    void acceptInitialize(QJsonObject infoValue) {
        require(infoValue["protocolVersion"].isString() && client.protocolVersions.contains(infoValue["protocolVersion"].toString()),
            "MCP server selected an unsupported protocol version");
        const auto implementation = infoValue["serverInfo"].toObject();
        require(infoValue["capabilities"].isObject() && implementation["name"].isString()
            && implementation["version"].isString() && (!infoValue.contains("instructions") || infoValue["instructions"].isString()), "Invalid MCP initialize result");
        std::lock_guard lock(mutex); info = std::move(infoValue);
        enqueueLocked(encode(notification("notifications/initialized"), options.maxMessageBytes));
        initialized = true; ++generation;
    }
    void restart(std::exception_ptr error) {
        std::deque<Outgoing> queue;
        { std::lock_guard lock(mutex); initialized = false; queue.swap(outgoing); queuedBytes = 0;
          notifications.clear(); notificationBytes = 0; completions.clear(); }
        for (auto& [id, request] : active) finish(request, {}, error);
        active.clear();
        for (auto& packet : queue) if (packet.request) finish(packet.request, {}, error);
        for (auto& [id, request] : incoming) request.cancellation.cancel();
        incoming.clear(); batches.clear(); ++handlerEpoch;
        transport->reset();
        auto p = std::make_shared<Pending>(); p->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        p->method = "initialize"; p->internal = true;
        p->deadline = Clock::now() + std::chrono::milliseconds(options.initializeTimeoutMs);
        { std::lock_guard lock(mutex); ++pendingCount; }
        active.emplace("s:" + p->id, p);
        write(encode({{"jsonrpc", "2.0"}, {"id", p->id}, {"method", "initialize"}, {"params", QJsonObject{
            {"protocolVersion", client.protocolVersions.first()}, {"capabilities", client.capabilities}, {"clientInfo", client.implementation}}}}, options.maxMessageBytes));
    }
    void run(std::shared_ptr<std::promise<void>> startup) {
        QThreadPool handlers; handlers.setMaxThreadCount(options.maxServerRequests);
        bool announced = false; std::exception_ptr failure;
        try {
            transport->start();
            { std::lock_guard lock(mutex); connected = true; }
            startup->set_value(); announced = true;
            while (!stopping) {
                std::deque<Outgoing> queue; std::deque<Completion> completed;
                { std::lock_guard lock(mutex); queue.swap(outgoing); queuedBytes = 0; completed.swap(completions); }
                // Register the entire drained batch before writing: an I/O failure
                // must complete every accepted waiter, including unsent requests.
                for (auto& packet : queue) if (packet.request) {
                    auto p = packet.request;
                    if (p->cancellation.isCancelled() || p->abandoned.isCancelled() || Clock::now() >= p->deadline) {
                        const bool expired = Clock::now() >= p->deadline;
                        finish(p, {}, std::make_exception_ptr(Error(expired ? ErrorCode::Timeout : ErrorCode::Cancelled,
                            "MCP " + p->method.left(128) + " request stopped before transmission")));
                        packet.frame.clear();
                    } else active.emplace("s:" + p->id, p);
                    p->frame.clear();
                }
                for (const auto& packet : queue) if (!packet.frame.isEmpty()) write(packet.frame);
                for (const auto& completion : completed) {
                    if (completion.epoch != handlerEpoch) continue;
                    const auto it = incoming.find(completion.key);
                    if (it == incoming.end()) continue;
                    if (!it->second.cancellation.isCancelled()) reply(completion.message);
                    incoming.erase(it);
                }
                for (auto it = active.begin(); it != active.end();) {
                    auto p = it->second; const bool expired = Clock::now() >= p->deadline;
                    if (!expired && !p->cancellation.isCancelled() && !p->abandoned.isCancelled()) { ++it; continue; }
                    if (p->method != "initialize") write(encode(notification("notifications/cancelled",
                        {{"requestId", p->id}, {"reason", expired ? "Request timed out" : "Request cancelled"}}), options.maxMessageBytes));
                    it = active.erase(it);
                    finish(p, {}, std::make_exception_ptr(Error(expired ? ErrorCode::Timeout : ErrorCode::Cancelled,
                        "MCP " + p->method.left(128) + (expired ? " request timed out; remote outcome may be unknown" : " request cancelled; remote outcome may be unknown"))));
                    if (p->internal) throw Error(ErrorCode::Timeout, "MCP session reinitialization timed out");
                }
                const auto events = transport->poll();
                { std::lock_guard lock(mutex); stderrBytes = transport->diagnostics(); }
                auto expired = std::find_if(events.begin(), events.end(), [](const auto& e) { return e.sessionExpired; });
                if (expired != events.end()) { restart(expired->error); continue; }
                for (const auto& event : events) {
                    if (event.error) {
                        if (event.requestKey.isEmpty()) std::rethrow_exception(event.error);
                        const auto found = active.find(event.requestKey);
                        if (found == active.end()) continue;
                        auto p = found->second; active.erase(found); finish(p, {}, event.error);
                        if (p->internal) std::rethrow_exception(event.error);
                    } else handle(handlers, event.message);
                }
            }
            throw Error(ErrorCode::ShuttingDown, "MCP connection was closed");
        } catch (...) { failure = std::current_exception(); }
        std::deque<Outgoing> queue;
        { std::lock_guard lock(mutex); connected = false; initialized = false; stopping = true; queue.swap(outgoing); queuedBytes = 0; }
        if (!announced) startup->set_exception(failure);
        for (auto& [id, request] : active) finish(request, {}, failure);
        active.clear();
        for (auto& packet : queue) if (packet.request) finish(packet.request, {}, failure);
        for (auto& [id, request] : incoming) request.cancellation.cancel();
        transport->close();
        // Host callbacks must cooperate with their cancellation token, like agent tools.
        handlers.waitForDone();
        incoming.clear(); batches.clear();
        { std::lock_guard lock(mutex); completions.clear(); }
    }
    void stop() {
        std::lock_guard lock(joining); stopping = true;
        if (worker.joinable()) worker.join();
    }
};

Client::Client(std::unique_ptr<detail::ClientTransport> transport, ClientLimits options, ClientOptions client)
    : d(std::make_shared<Impl>(std::move(transport), std::move(options), std::move(client))) {
    auto startup = std::make_shared<std::promise<void>>(); auto started = startup->get_future();
    d->worker = std::thread([impl = d, startup] { impl->run(startup); });
    try {
        started.get();
        auto info = d->perform("initialize", {{"protocolVersion", d->client.protocolVersions.first()},
            {"capabilities", d->client.capabilities}, {"clientInfo", d->client.implementation}}, {}, {}, d->options.initializeTimeoutMs, true);
        d->acceptInitialize(std::move(info));
    } catch (...) { d->stop(); throw; }
}
Client::~Client() { close(); }
quint64 Client::connectionGeneration() const { std::lock_guard lock(d->mutex); return d->generation; }
void Client::close() { d->stop(); }
bool Client::isConnected() const { std::lock_guard lock(d->mutex); return d->connected && d->initialized && !d->stopping; }
bool Client::isClosed() const { return d->stopping.load(); }
QByteArray Client::stderrTail() const { std::lock_guard lock(d->mutex); return d->stderrBytes; }
QJsonObject Client::serverInfo() const { std::lock_guard lock(d->mutex); return d->info["serverInfo"].toObject(); }
QJsonObject Client::serverCapabilities() const { std::lock_guard lock(d->mutex); return d->info["capabilities"].toObject(); }
QString Client::protocolVersion() const { std::lock_guard lock(d->mutex); return d->info["protocolVersion"].toString(); }
QString Client::instructions() const { std::lock_guard lock(d->mutex); return d->info["instructions"].toString(); }
QList<QJsonObject> Client::takeNotifications() { std::lock_guard lock(d->mutex); auto result = std::move(d->notifications); d->notifications.clear(); d->notificationBytes = 0; return result; }
void Client::setRoots(QJsonArray roots) {
    validateRoots(roots); const auto frame = encode(notification("notifications/roots/list_changed"), d->options.maxMessageBytes);
    require(QJsonDocument(roots).toJson(QJsonDocument::Compact).size() + 256 <= d->options.maxMessageBytes, "MCP roots exceed message limit", ErrorCode::ResourceLimit);
    std::lock_guard lock(d->mutex);
    if (roots == d->client.roots) return;
    d->enqueueLocked(frame); d->client.roots = std::move(roots);
}
QJsonObject Client::request(QString method, QJsonObject params, CancellationToken token, ProgressCallback progress, int timeoutMs, quint64 expectedGeneration) {
    auto impl = d; return impl->perform(std::move(method), std::move(params), std::move(token), std::move(progress), timeoutMs, false, expectedGeneration);
}
void Client::notify(QString method, QJsonObject params) {
    require(method.startsWith("notifications/") && method != "notifications/initialized" && method != "notifications/cancelled"
        && method != "notifications/roots/list_changed", "Reserved or invalid MCP notification", ErrorCode::InvalidArgument);
    auto frame = encode(notification(method, params), d->options.maxMessageBytes);
    std::lock_guard lock(d->mutex); d->enqueueLocked(std::move(frame));
}
void Client::requireCapability(const QString& capability) const {
    require(serverCapabilities()[capability].isObject(), "MCP server did not negotiate capability: " + capability);
}
QJsonArray Client::list(const QString& capability, const QString& method, const QString& field, CancellationToken token) {
    const auto generation = connectionGeneration();
    requireCapability(capability); QJsonArray items; QJsonObject params; QSet<QString> cursors, names;
    qsizetype totalBytes = 0;
    const auto deadline = Clock::now() + std::chrono::milliseconds(d->options.requestTimeoutMs);
    for (int page = 0;; ++page) {
        require(page < d->options.maxListItems, "MCP list has too many pages", ErrorCode::ResourceLimit);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        require(remaining > 0, "MCP list exceeded its deadline", ErrorCode::Timeout);
        const auto response = request(method, params, token, {}, int(remaining));
        require(connectionGeneration() == generation, "MCP session changed during pagination", ErrorCode::RuntimeFailure);
        totalBytes += QJsonDocument(response).toJson(QJsonDocument::Compact).size();
        require(totalBytes <= d->options.maxQueuedBytes, "MCP paginated list exceeds its byte limit", ErrorCode::ResourceLimit);
        require(response[field].isArray(), "MCP list result is missing " + field);
        for (const auto& value : response[field].toArray()) {
            const auto item = value.toObject();
            const auto id = field == "resources" ? "uri" : field == "resourceTemplates" ? "uriTemplate" : "name";
            require(value.isObject() && item[id].isString() && !item[id].toString().isEmpty() && !names.contains(item[id].toString()), "Invalid or duplicate MCP list entry");
            if (field == "tools") require(item["inputSchema"].isObject()
                && (!item.contains("outputSchema") || item["outputSchema"].isObject())
                && (!item.contains("annotations") || item["annotations"].isObject()), "Invalid MCP tool schema or annotations");
            names.insert(item[id].toString()); items.append(value);
            require(items.size() <= d->options.maxListItems, "MCP list exceeds item limit", ErrorCode::ResourceLimit);
        }
        if (!response.contains("nextCursor")) return items;
        require(response["nextCursor"].isString() && !response["nextCursor"].toString().isEmpty()
            && !cursors.contains(response["nextCursor"].toString()), "Invalid or repeated MCP pagination cursor");
        cursors.insert(response["nextCursor"].toString()); params["cursor"] = response["nextCursor"];
    }
}
QJsonArray Client::listTools(CancellationToken token) { return list("tools", "tools/list", "tools", token); }
QJsonArray Client::listResources(CancellationToken token) { return list("resources", "resources/list", "resources", token); }
QJsonArray Client::listResourceTemplates(CancellationToken token) { return list("resources", "resources/templates/list", "resourceTemplates", token); }
QJsonArray Client::listPrompts(CancellationToken token) { return list("prompts", "prompts/list", "prompts", token); }
QJsonObject Client::callTool(const QString& name, QJsonObject arguments, CancellationToken token, ProgressCallback progress, quint64 expectedGeneration) {
    requireCapability("tools"); require(!name.isEmpty(), "MCP tool name is required", ErrorCode::InvalidArgument);
    auto result = request("tools/call", {{"name", name}, {"arguments", arguments}}, token, std::move(progress), 0, expectedGeneration);
    require(result["content"].isArray() && (!result.contains("isError") || result["isError"].isBool())
        && (!result.contains("structuredContent") || result["structuredContent"].isObject())
        && (!result.contains("_meta") || result["_meta"].isObject()), "Invalid MCP tool result");
    for (const auto& block : result["content"].toArray()) validateContent(block);
    return result;
}
QJsonObject Client::readResource(const QString& uri, CancellationToken token) {
    requireCapability("resources"); auto result = request("resources/read", {{"uri", uri}}, token);
    require(result["contents"].isArray(), "Invalid MCP resource result");
    for (const auto& value : result["contents"].toArray()) {
        const auto content = value.toObject();
        require(content["uri"].isString() && (content["text"].isString() != content["blob"].isString()), "Invalid MCP resource contents");
    }
    return result;
}
void Client::subscribeResource(const QString& uri, CancellationToken token) {
    require(serverCapabilities()["resources"].toObject()["subscribe"] == true, "MCP resource subscription was not negotiated");
    request("resources/subscribe", {{"uri", uri}}, token);
}
void Client::unsubscribeResource(const QString& uri, CancellationToken token) {
    require(serverCapabilities()["resources"].toObject()["subscribe"] == true, "MCP resource subscription was not negotiated");
    request("resources/unsubscribe", {{"uri", uri}}, token);
}
QJsonObject Client::getPrompt(const QString& name, QJsonObject arguments, CancellationToken token) {
    requireCapability("prompts");
    for (auto it = arguments.begin(); it != arguments.end(); ++it) require(it.value().isString(), "MCP prompt arguments must be strings", ErrorCode::InvalidArgument);
    auto result = request("prompts/get", {{"name", name}, {"arguments", arguments}}, token);
    require(result["messages"].isArray(), "Invalid MCP prompt result");
    for (const auto& value : result["messages"].toArray()) {
        const auto message = value.toObject();
        require(message["role"] == "user" || message["role"] == "assistant", "Invalid MCP prompt message role");
        validateContent(message["content"]);
    }
    return result;
}
StdioClient::StdioClient(StdioOptions options, ClientOptions client)
    : Client(detail::stdioTransport(options), options, std::move(client)) {}

}
