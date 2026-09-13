#include "Server.h"
#include "Protocol.h"
#include "BatchReplies.h"
#include <QtCore/QThreadPool>
#include <QtCore/QUuid>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace iiLocalLLM::mcp {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using namespace detail;
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
void rpcRequire(bool value, QString message, int code = -32602) { if (!value) throw RpcError(code, message); }
QString listField(const QString& method) {
    if (method == "tools/list") return "tools";
    if (method == "resources/list") return "resources";
    if (method == "resources/templates/list") return "resourceTemplates";
    if (method == "prompts/list") return "prompts";
    return {};
}
void validateResult(const QString& method, const QJsonObject& result) {
    if (method == "tools/call") {
        require(result["content"].isArray() && (!result.contains("isError") || result["isError"].isBool())
            && (!result.contains("structuredContent") || result["structuredContent"].isObject())
            && (!result.contains("_meta") || result["_meta"].isObject()), "Invalid MCP tool handler result");
        for (const auto& block : result["content"].toArray()) validateContent(block);
    } else if (method == "resources/read") {
        require(result["contents"].isArray(), "Invalid MCP resource handler result");
        for (const auto& value : result["contents"].toArray()) {
            const auto block = value.toObject();
            require(block["uri"].isString() && (block["text"].isString() != block["blob"].isString()), "Invalid MCP resource contents");
        }
    } else if (method == "prompts/get") {
        require(result["messages"].isArray(), "Invalid MCP prompt handler result");
        for (const auto& value : result["messages"].toArray()) {
            const auto message = value.toObject();
            require(message["role"] == "user" || message["role"] == "assistant", "Invalid MCP prompt role");
            validateContent(message["content"]);
        }
    }
}
QJsonValue legacyContent(QJsonValue value) {
    if (value.toObject()["type"] != "resource_link") return value;
    return QJsonObject{{"type", "text"}, {"text", QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact))}};
}
QJsonObject legacyResult(const QString& method, QJsonObject result) {
    if (method == "tools/call") {
        auto content = result["content"].toArray();
        for (qsizetype i = 0; i < content.size(); ++i) content[i] = legacyContent(content[i]);
        if (result.contains("structuredContent")) {
            const auto serialized = QString::fromUtf8(QJsonDocument(result["structuredContent"].toObject()).toJson(QJsonDocument::Compact));
            bool included = false; for (const auto& value : content) if (value.toObject()["type"] == "text" && value.toObject()["text"] == serialized) included = true;
            if (!included) content.append(QJsonObject{{"type", "text"}, {"text", serialized}});
            result.remove("structuredContent");
        }
        result["content"] = content;
    } else if (method == "tools/list") {
        auto tools = result["tools"].toArray();
        for (qsizetype i = 0; i < tools.size(); ++i) { auto tool = tools[i].toObject(); tool.remove("outputSchema"); tools[i] = tool; }
        result["tools"] = tools;
    } else if (method == "prompts/get") {
        auto messages = result["messages"].toArray();
        for (qsizetype i = 0; i < messages.size(); ++i) { auto m = messages[i].toObject(); m["content"] = legacyContent(m["content"]); messages[i] = m; }
        result["messages"] = messages;
    }
    return result;
}
}
class ServerSession::Impl : public std::enable_shared_from_this<Impl> {
public:
    struct Job {
        QJsonValue id, progressToken;
        QString key, method;
        QJsonObject params;
        CancellationToken cancellation;
        Clock::time_point deadline;
        bool responded = false;
        double lastProgress = -1;
    };
    struct Reverse {
        QJsonValue id;
        bool done = false;
        QJsonObject result;
        std::exception_ptr error;
    };
    struct Page {
        QString method;
        QJsonArray items;
        int offset = 0;
        qsizetype bytes = 0;
        Clock::time_point expires;
    };
    ServerOptions options;
    const QString sessionId = uuid();
    mutable std::mutex mutex;
    std::mutex joining;
    std::mutex receiving;
    std::condition_variable changed;
    QThreadPool workers;
    std::thread timer;
    bool closed = false, notifiedClosed = false;
    int phase = 0; // new, initialize response sent, initialized
    Clock::time_point initializationDeadline;
    QString version;
    QJsonObject info, clientCaps, capabilities;
    std::exception_ptr failure;
    QSet<QString> seenIds, subscriptions;
    std::map<QString, std::shared_ptr<Job>> jobs;
    std::map<QString, std::shared_ptr<Reverse>> reverse;
    std::map<QString, Page> pages;
    QList<QJsonValue> outgoing;
    QList<QJsonObject> notifications;
    BatchReplies batches;
    qsizetype outgoingBytes = 0, notificationBytes = 0, pageBytes = 0;

    explicit Impl(ServerOptions o) : options(std::move(o)), batches(int(std::min<qint64>(1000000,
        qint64(options.maxNotifications) + options.maxConcurrentRequests + options.maxQueuedRequests)), options.maxQueuedBytes, options.maxMessageBytes) {
        require(options.initializeTimeoutMs > 0 && options.requestTimeoutMs > 0
            && options.maxConcurrentRequests > 0 && options.maxQueuedRequests >= 0
            && options.maxConcurrentRequests <= 1024 && options.maxQueuedRequests <= 100000
            && options.maxReverseRequests > 0 && options.maxMessageBytes >= 1024
            && options.maxQueuedBytes >= options.maxMessageBytes && options.maxNotifications > 0
            && options.maxRequestsPerSession > 0 && options.listPageSize > 0
            && options.maxListItems > 0 && options.maxListSnapshots > 0 && options.cursorTimeoutMs > 0,
            "Invalid MCP server limits", ErrorCode::InvalidArgument);
        require(options.implementation["name"].isString() && !options.implementation["name"].toString().isEmpty()
            && options.implementation["version"].isString(), "Invalid MCP server implementation", ErrorCode::InvalidArgument);
        require(!options.protocolVersions.isEmpty(), "No MCP server protocol versions", ErrorCode::InvalidArgument);
        for (const auto& v : options.protocolVersions)
            require(QStringList{"2025-11-25", "2025-06-18", "2025-03-26"}.contains(v), "Unsupported MCP server protocol version", ErrorCode::InvalidArgument);
        for (const auto& [name, handler] : options.handlers)
            require(handler && !name.isEmpty() && name != "initialize" && name != "ping"
                && !name.startsWith("notifications/") && !name.startsWith("tasks/") && listField(name).isEmpty(),
                "Invalid or reserved MCP server handler", ErrorCode::InvalidArgument);
        for (const auto& [name, handler] : options.lists)
            require(handler && !listField(name).isEmpty(), "Invalid MCP list handler", ErrorCode::InvalidArgument);
        if (options.lists.contains("tools/list") || options.handlers.contains("tools/call")) {
            require(options.lists.contains("tools/list") && options.handlers.contains("tools/call"), "MCP tools require list and call handlers", ErrorCode::InvalidArgument);
            capabilities["tools"] = QJsonObject{{"listChanged", true}};
        }
        if (options.lists.contains("resources/list") || options.lists.contains("resources/templates/list") || options.handlers.contains("resources/read")) {
            require(options.handlers.contains("resources/read"), "MCP resources require a read handler", ErrorCode::InvalidArgument);
            capabilities["resources"] = QJsonObject{{"listChanged", true}, {"subscribe", options.handlers.contains("resources/subscribe")}};
        }
        if (options.lists.contains("prompts/list") || options.handlers.contains("prompts/get")) {
            require(options.lists.contains("prompts/list") && options.handlers.contains("prompts/get"), "MCP prompts require list and get handlers", ErrorCode::InvalidArgument);
            capabilities["prompts"] = QJsonObject{{"listChanged", true}};
        }
        require(!options.handlers.contains("resources/subscribe") || capabilities.contains("resources"), "Cannot subscribe without resources", ErrorCode::InvalidArgument);
        workers.setMaxThreadCount(options.maxConcurrentRequests);
        initializationDeadline = Clock::now() + std::chrono::milliseconds(options.initializeTimeoutMs);
    }
    void emitFrameLocked(const QJsonValue& message) {
        const auto size = encodeValue(message, options.maxMessageBytes).size();
        require(outgoingBytes + size <= options.maxQueuedBytes, "MCP server output queue is full", ErrorCode::QueueFull);
        outgoing.append(message); outgoingBytes += size; changed.notify_all();
    }
    void emitLocked(const QJsonObject& message) {
        if (const auto frame = batches.response(message)) emitFrameLocked(*frame);
    }
    void stopLocked(std::exception_ptr error = {}) {
        if (closed) return;
        closed = true; failure = error;
        for (const auto& [id, job] : jobs) job->cancellation.cancel();
        for (const auto& [id, pending] : reverse) {
            pending->done = true;
            pending->error = std::make_exception_ptr(Error(ErrorCode::ShuttingDown, "MCP server connection closed"));
        }
        pages.clear(); pageBytes = 0; batches.clear();
        changed.notify_all();
    }
    void safeEmitLocked(const QJsonObject& message) {
        try { emitLocked(message); } catch (...) { stopLocked(std::current_exception()); }
    }
    void tick() {
        std::unique_lock lock(mutex);
        while (!closed) {
            const auto now = Clock::now();
            if (phase != 2 && now >= initializationDeadline) {
                stopLocked(std::make_exception_ptr(Error(ErrorCode::Timeout, "MCP server initialization timed out"))); break;
            }
            for (const auto& [id, job] : jobs) if (!job->responded && !job->cancellation.isCancelled() && now >= job->deadline) {
                job->responded = true; job->cancellation.cancel();
                safeEmitLocked(rpcError(job->id, -32000, "MCP server request timed out"));
            }
            for (auto it = pages.begin(); it != pages.end();) {
                if (it->second.expires <= now) { pageBytes -= it->second.bytes; it = pages.erase(it); }
                else ++it;
            }
            changed.wait_for(lock, 10ms, [&] { return closed; });
        }
    }
    void progress(const std::shared_ptr<Job>& job, QJsonObject params) {
        std::lock_guard lock(mutex); job->cancellation.throwIfCancelled();
        require(!closed && jobs.contains(job->key), "MCP request already finished", ErrorCode::Cancelled);
        if (job->progressToken.isUndefined()) return;
        const auto progress = params["progress"].toDouble(-1);
        require(params["progress"].isDouble() && std::isfinite(progress) && progress >= 0 && progress > job->lastProgress
            && (!params.contains("total") || (params["total"].isDouble() && std::isfinite(params["total"].toDouble()) && params["total"].toDouble() >= progress))
            && (!params.contains("message") || params["message"].isString()), "Invalid MCP progress update", ErrorCode::InvalidArgument);
        job->lastProgress = progress; params["progressToken"] = job->progressToken;
        try { emitLocked(notification("notifications/progress", params)); }
        catch (...) { stopLocked(std::current_exception()); throw; }
    }
    QJsonObject requestClient(const std::shared_ptr<Job>& parent, QString method, QJsonObject params, int timeout) {
        require(timeout >= 0, "Invalid reverse request timeout", ErrorCode::InvalidArgument);
        auto pending = std::make_shared<Reverse>(); pending->id = "server-" + uuid();
        const auto pendingKey = key(pending->id);
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout ? timeout : options.requestTimeoutMs);
        std::unique_lock lock(mutex);
        parent->cancellation.throwIfCancelled();
        require(!closed && phase == 2 && jobs.contains(parent->key), "Reverse RPC requires a live parent request", ErrorCode::ShuttingDown);
        const auto cap = method == "roots/list" ? "roots" : method == "sampling/createMessage" ? "sampling"
            : method == "elicitation/create" ? "elicitation" : "";
        rpcRequire(method == "ping" || (QString::fromLatin1(cap).size() && clientCaps[cap].isObject()), "Client capability was not negotiated", -32601);
        rpcRequire(!params.contains("task") && !(version == "2025-03-26" && method == "elicitation/create"), "Reverse RPC is unsupported by this protocol version", -32601);
        require(reverse.size() < size_t(options.maxReverseRequests), "Too many reverse MCP requests", ErrorCode::QueueFull);
        emitLocked({{"jsonrpc", "2.0"}, {"id", pending->id}, {"method", method}, {"params", params}});
        reverse.emplace(pendingKey, pending);
        while (!pending->done && !closed && !parent->cancellation.isCancelled() && Clock::now() < deadline)
            changed.wait_for(lock, 10ms);
        reverse.erase(pendingKey);
        if (pending->done) {
            if (pending->error) std::rethrow_exception(pending->error);
            return pending->result;
        }
        if (!closed) safeEmitLocked(notification("notifications/cancelled", {{"requestId", pending->id}, {"reason", "Request cancelled or timed out"}}));
        parent->cancellation.throwIfCancelled();
        throw Error(closed ? ErrorCode::ShuttingDown : ErrorCode::Timeout, "MCP reverse request did not complete");
    }
    QJsonObject list(const std::shared_ptr<Job>& job, const ServerRequestContext& context) {
        const auto field = listField(job->method); Page page;
        const auto cursor = job->params.value("cursor");
        if (!cursor.isUndefined()) {
            rpcRequire(cursor.isString() && !cursor.toString().isEmpty(), "Invalid MCP cursor");
            std::lock_guard lock(mutex);
            const auto found = pages.find(cursor.toString());
            rpcRequire(found != pages.end() && found->second.method == job->method && found->second.expires > Clock::now(), "Unknown or expired MCP cursor");
            page = found->second; pageBytes -= page.bytes; pages.erase(found);
        } else {
            page.method = job->method;
            const auto found = options.lists.find(job->method);
            page.items = found == options.lists.end() ? QJsonArray{} : found->second(context);
            require(page.items.size() <= options.maxListItems, "MCP list exceeds item limit", ErrorCode::ResourceLimit);
            page.bytes = QJsonDocument(page.items).toJson(QJsonDocument::Compact).size();
            require(page.bytes <= options.maxQueuedBytes, "MCP list exceeds byte limit", ErrorCode::ResourceLimit);
            QSet<QString> names;
            for (const auto& value : page.items) {
                const auto item = value.toObject();
                const auto nameKey = field == "resources" ? "uri" : field == "resourceTemplates" ? "uriTemplate" : "name";
                require(item[nameKey].isString() && !item[nameKey].toString().isEmpty() && !names.contains(item[nameKey].toString()), "Invalid or duplicate MCP list entry");
                if (field == "tools") require(item["inputSchema"].isObject() && (!item.contains("outputSchema") || item["outputSchema"].isObject()), "Invalid MCP tool schema");
                names.insert(item[nameKey].toString());
            }
        }
        QJsonArray items; qsizetype bytes = 256;
        while (page.offset < page.items.size() && items.size() < options.listPageSize) {
            const auto size = QJsonDocument(page.items[page.offset].toObject()).toJson(QJsonDocument::Compact).size() + 1;
            if (bytes + size > options.maxMessageBytes - 256) {
                require(!items.isEmpty(), "MCP list item exceeds frame limit", ErrorCode::ResourceLimit); break;
            }
            bytes += size; items.append(page.items[page.offset++]);
        }
        QJsonObject result{{field, items}};
        if (page.offset < page.items.size()) {
            std::lock_guard lock(mutex); job->cancellation.throwIfCancelled();
            require(!closed && pages.size() < size_t(options.maxListSnapshots) && pageBytes + page.bytes <= options.maxQueuedBytes,
                "MCP list snapshot limit reached", ErrorCode::ResourceLimit);
            const auto next = uuid(); page.expires = Clock::now() + std::chrono::milliseconds(options.cursorTimeoutMs);
            pageBytes += page.bytes; pages.emplace(next, std::move(page)); result["nextCursor"] = next;
        }
        return result;
    }
    void execute(const std::shared_ptr<Job>& job) {
        auto self = shared_from_this(); QJsonObject response;
        try {
            job->cancellation.throwIfCancelled();
            ServerRequestContext context;
            { std::lock_guard lock(mutex); context = {sessionId, job->id, version, info, clientCaps, job->cancellation, {}, {}}; }
            context.progress = [self, job](const auto& p) { self->progress(job, p); };
            context.requestClient = [self, job](QString m, QJsonObject p, int t) { return self->requestClient(job, std::move(m), std::move(p), t); };
            QJsonObject result;
            if (!listField(job->method).isEmpty()) result = list(job, context);
            else if (job->method == "resources/unsubscribe" && !options.handlers.contains(job->method)) result = {};
            else result = options.handlers.at(job->method)(job->params, context);
            job->cancellation.throwIfCancelled(); validateResult(job->method, result);
            if (context.protocolVersion == "2025-03-26") result = legacyResult(job->method, std::move(result));
            response = {{"jsonrpc", "2.0"}, {"id", job->id}, {"result", result}};
        } catch (const RpcError& e) { response = rpcError(job->id, e.rpcCode(), QString::fromUtf8(e.what()), e.data()); }
        catch (const Error& e) { response = rpcError(job->id, e.code() == ErrorCode::InvalidArgument ? -32602 : -32000,
            QString::fromUtf8(e.what()), QJsonObject{{"error_code", iiLocalLLM::enumName(e.code())}}); }
        catch (const std::exception& e) { response = rpcError(job->id, -32603, QString::fromUtf8(e.what())); }
        catch (...) { response = rpcError(job->id, -32603, "MCP handler failed"); }
        std::lock_guard lock(mutex);
        if (!closed && !job->responded && !job->cancellation.isCancelled()) {
            if (Clock::now() >= job->deadline) response = rpcError(job->id, -32000, "MCP server request timed out");
            if (response.contains("result")) {
                if (job->method == "resources/subscribe") {
                    const auto uri = job->params["uri"].toString();
                    if (subscriptions.contains(uri) || subscriptions.size() < options.maxNotifications) subscriptions.insert(uri);
                    else response = rpcError(job->id, -32000, "MCP subscription limit reached");
                }
                if (job->method == "resources/unsubscribe") subscriptions.remove(job->params["uri"].toString());
            }
            try { emitLocked(response); }
            catch (const Error& e) {
                if (e.code() == ErrorCode::ResourceLimit) safeEmitLocked(rpcError(job->id, -32000, "MCP response exceeds frame limit"));
                else stopLocked(std::current_exception());
            }
        }
        jobs.erase(job->key); changed.notify_all();
    }
    void receive(const QJsonObject& message) {
        std::unique_lock lock(mutex);
        require(!closed, "MCP server connection is closed", ErrorCode::ShuttingDown);
        QJsonValue id(QJsonValue::Null);
        try {
            encode(message, options.maxMessageBytes);
            rpcRequire(message["jsonrpc"] == "2.0", "Invalid JSON-RPC version", -32600);
            if (!message.contains("method")) {
                rpcRequire(message.contains("id") && (message.contains("result") != message.contains("error")), "Invalid JSON-RPC response", -32600);
                const auto found = reverse.find(key(message["id"]));
                if (found == reverse.end()) return; // Late response to an abandoned reverse RPC.
                auto pending = found->second;
                if (pending->done) return;
                if (message.contains("error")) {
                    const auto e = message["error"].toObject();
                    rpcRequire(e["code"].isDouble() && std::trunc(e["code"].toDouble()) == e["code"].toDouble()
                        && e["code"].toDouble() >= INT_MIN && e["code"].toDouble() <= INT_MAX && e["message"].isString(), "Invalid JSON-RPC error", -32600);
                    pending->error = std::make_exception_ptr(RpcError(e["code"].toInt(), e["message"].toString(), e.value("data")));
                } else {
                    rpcRequire(message["result"].isObject(), "MCP results must be objects", -32600); pending->result = message["result"].toObject();
                }
                pending->done = true; changed.notify_all(); return;
            }
            rpcRequire(message["method"].isString() && !message["method"].toString().isEmpty()
                && !message.contains("result") && !message.contains("error"), "Invalid JSON-RPC request", -32600);
            const auto method = message["method"].toString(); const auto params = message["params"].toObject();
            if (!message.contains("id")) {
                if (message.contains("params") && !message["params"].isObject()) return;
                if (method == "notifications/initialized") {
                    if (phase == 1) { phase = 2; changed.notify_all(); }
                    return;
                }
                if (phase != 2) return;
                if (method == "notifications/cancelled") {
                    QString requestKey; try { requestKey = key(params["requestId"]); } catch (const Error&) { return; }
                    const auto found = jobs.find(requestKey);
                    if (found != jobs.end()) { found->second->cancellation.cancel(); if (const auto frame = batches.cancel(requestKey)) emitFrameLocked(*frame); }
                    changed.notify_all(); return;
                }
                const auto size = QJsonDocument(message).toJson(QJsonDocument::Compact).size();
                require(notifications.size() < options.maxNotifications && notificationBytes + size <= options.maxQueuedBytes,
                    "MCP server notification queue is full", ErrorCode::ResourceLimit);
                notifications.append(message); notificationBytes += size; return;
            }
            QString requestKey;
            try { requestKey = key(message["id"]); } catch (const Error&) { throw RpcError(-32600, "Invalid MCP request ID"); }
            id = message["id"];
            require(requestKey.size() <= 258 && !seenIds.contains(requestKey), "MCP request ID was reused or exceeds its limit", ErrorCode::ProtocolError);
            require(seenIds.size() < options.maxRequestsPerSession, "MCP session request limit reached", ErrorCode::ResourceLimit);
            seenIds.insert(requestKey);
            rpcRequire(!message.contains("params") || message["params"].isObject(), "MCP params must be an object");
            rpcRequire(!params.contains("_meta") || params["_meta"].isObject(), "MCP _meta must be an object");
            if (method == "initialize") {
                rpcRequire(phase == 0, "MCP connection already initialized", -32600);
                rpcRequire(params["protocolVersion"].isString() && params["capabilities"].isObject()
                    && params["clientInfo"].isObject() && params["clientInfo"].toObject()["name"].isString()
                    && params["clientInfo"].toObject()["version"].isString(), "Invalid MCP initialization");
                const auto caps = params["capabilities"].toObject();
                for (auto it = caps.begin(); it != caps.end(); ++it) rpcRequire(it.value().isObject(), "Invalid client capability");
                info = params["clientInfo"].toObject(); clientCaps = caps;
                version = options.protocolVersions.contains(params["protocolVersion"].toString()) ? params["protocolVersion"].toString() : options.protocolVersions.first();
                QJsonObject result{{"protocolVersion", version}, {"serverInfo", options.implementation}, {"capabilities", capabilities}};
                if (!options.instructions.isEmpty()) result["instructions"] = options.instructions;
                emitLocked({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}); phase = 1; return;
            }
            if (method == "ping") { emitLocked({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}}); return; }
            rpcRequire(phase == 2, "MCP connection is not initialized", -32002);
            const auto feature = method.section('/', 0, 0);
            if (QStringList{"tools", "resources", "prompts"}.contains(feature))
                rpcRequire(capabilities.contains(feature), "MCP capability not available", -32601);
            const bool listMethod = !listField(method).isEmpty();
            const bool unsubscribe = method == "resources/unsubscribe" && capabilities["resources"].toObject()["subscribe"] == true;
            rpcRequire(listMethod || unsubscribe || options.handlers.contains(method), "Unknown MCP method", -32601);
            rpcRequire(!params.contains("task"), "MCP task-augmented requests are not supported");
            if (method == "tools/call" || method == "prompts/get") {
                rpcRequire(params["name"].isString() && !params["name"].toString().isEmpty(), "Missing MCP name");
                rpcRequire(!params.contains("arguments") || params["arguments"].isObject(), "MCP arguments must be an object");
            }
            if (method == "prompts/get") {
                const auto args = params["arguments"].toObject();
                for (auto it = args.begin(); it != args.end(); ++it) rpcRequire(it.value().isString(), "MCP prompt arguments must be strings");
            }
            if (method == "resources/read" || method == "resources/subscribe" || method == "resources/unsubscribe")
                rpcRequire(params["uri"].isString() && !params["uri"].toString().isEmpty(), "Missing MCP resource URI");
            if (method == "resources/subscribe")
                rpcRequire(subscriptions.contains(params["uri"].toString()) || subscriptions.size() < options.maxNotifications, "MCP subscription limit reached", -32000);
            rpcRequire(jobs.size() < size_t(options.maxConcurrentRequests + options.maxQueuedRequests), "MCP server request queue is full", -32000);
            auto job = std::make_shared<Job>(); job->id = id; job->key = requestKey; job->method = method; job->params = params;
            job->progressToken = params.value("_meta").toObject().value("progressToken");
            if (!job->progressToken.isUndefined()) { try { key(job->progressToken); } catch (const Error&) { throw RpcError(-32602, "Invalid MCP progress token"); } }
            job->deadline = Clock::now() + std::chrono::milliseconds(options.requestTimeoutMs);
            jobs.emplace(requestKey, job); auto self = shared_from_this();
            workers.start([self, job] { self->execute(job); });
        } catch (const RpcError& e) { safeEmitLocked(rpcError(id, e.rpcCode(), QString::fromUtf8(e.what()), e.data())); }
        catch (...) { stopLocked(std::current_exception()); throw; }
    }
    void receiveBatch(const QJsonArray& messages) {
        BatchReplies::Prepared prepared;
        {
            std::lock_guard lock(mutex);
            require(!closed, "MCP connection is closed", ErrorCode::ShuttingDown);
            const auto first = messages.isEmpty() ? QJsonObject{} : messages[0].toObject();
            const bool legacyInit = phase == 0 && first["method"] == "initialize"
                && first["params"].toObject()["protocolVersion"] == "2025-03-26" && options.protocolVersions.contains("2025-03-26");
            if ((!legacyInit && version != "2025-03-26") || messages.isEmpty()) {
                safeEmitLocked(rpcError(QJsonValue::Null, -32600, "JSON-RPC batches require protocol 2025-03-26")); return;
            }
            try {
                require(messages.size() <= options.maxNotifications, "MCP batch item limit reached", ErrorCode::ResourceLimit);
                encodeValue(messages, options.maxMessageBytes);
                prepared = batches.prepare(messages);
                if (prepared.immediate) emitFrameLocked(*prepared.immediate);
            } catch (...) { stopLocked(std::current_exception()); throw; }
        }
        for (const auto& message : prepared.messages) receive(message);
    }
    void stop() {
        std::lock_guard join(joining);
        { std::lock_guard lock(mutex); stopLocked(); }
        if (timer.joinable()) timer.join();
        workers.waitForDone();
        if (!notifiedClosed) {
            notifiedClosed = true;
            if (options.onClosed) { try { options.onClosed(sessionId); } catch (...) {} }
        }
    }
};
ServerSession::ServerSession(ServerOptions options) : d(std::make_shared<Impl>(std::move(options))) {
    d->timer = std::thread([p = d] { p->tick(); });
}
ServerSession::~ServerSession() { d->stop(); }
QString ServerSession::id() const { return d->sessionId; }
bool ServerSession::isInitialized() const { std::lock_guard lock(d->mutex); return !d->closed && d->phase == 2; }
bool ServerSession::isClosed() const { std::lock_guard lock(d->mutex); return d->closed; }
QJsonObject ServerSession::clientInfo() const { std::lock_guard lock(d->mutex); return d->info; }
QJsonObject ServerSession::clientCapabilities() const { std::lock_guard lock(d->mutex); return d->clientCaps; }
void ServerSession::receive(const QJsonObject& message) { std::lock_guard lock(d->receiving); d->receive(message); }
void ServerSession::receiveBatch(const QJsonArray& messages) { std::lock_guard lock(d->receiving); d->receiveBatch(messages); }
QList<QJsonValue> ServerSession::takeMessages(int waitMs) {
    require(waitMs >= 0, "Invalid MCP message wait", ErrorCode::InvalidArgument);
    std::unique_lock lock(d->mutex);
    if (waitMs) d->changed.wait_for(lock, std::chrono::milliseconds(waitMs), [&] { return d->closed || !d->outgoing.isEmpty(); });
    if (d->failure) std::rethrow_exception(d->failure);
    QList<QJsonValue> messages; messages.swap(d->outgoing); d->outgoingBytes = 0; return messages;
}
QList<QJsonObject> ServerSession::takeNotifications() {
    std::lock_guard lock(d->mutex); QList<QJsonObject> result; result.swap(d->notifications); d->notificationBytes = 0; return result;
}
void ServerSession::notify(QString method, QJsonObject params) {
    std::lock_guard lock(d->mutex);
    require(!d->closed && d->phase == 2, "MCP connection is not initialized", ErrorCode::ShuttingDown);
    const auto feature = method.section('/', 1, 1);
    require(QStringList{"notifications/tools/list_changed", "notifications/resources/list_changed", "notifications/prompts/list_changed", "notifications/resources/updated"}.contains(method)
        && d->capabilities.contains(feature), "MCP notification capability was not negotiated", ErrorCode::InvalidArgument);
    if (method == "notifications/resources/updated") {
        require(params["uri"].isString(), "Resource update needs a URI", ErrorCode::InvalidArgument);
        if (!d->subscriptions.contains(params["uri"].toString())) return;
    }
    try { d->emitLocked(notification(method, params)); } catch (...) { d->stopLocked(std::current_exception()); throw; }
}
void ServerSession::close() { d->stop(); }
}
