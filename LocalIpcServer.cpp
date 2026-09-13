#include "LocalIpcServer.h"
#include "Parameters.h"
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QLockFile>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>
#include <cmath>
#include <deque>
#include <mutex>
#include <vector>

namespace iiLocalLLM {
namespace {
QJsonObject errorObject(ErrorCode code, const QString& message)
{ return {{QStringLiteral("code"), enumName(code)}, {QStringLiteral("message"), message}}; }
QJsonObject resultObject(const GenerationResult& r)
{
    QJsonObject out{{QStringLiteral("request_id"), r.requestId}, {QStringLiteral("session_id"), r.sessionId},
        {QStringLiteral("text"), r.text}, {QStringLiteral("finish_reason"), enumName(r.finishReason)},
        {QStringLiteral("usage"), QJsonObject{{QStringLiteral("prompt_tokens"), r.usage.promptTokens},
            {QStringLiteral("generated_tokens"), r.usage.generatedTokens}, {QStringLiteral("cached_tokens"), r.usage.cachedTokens},
            {QStringLiteral("dropped_messages"), r.usage.droppedMessages}}}};
    if (r.errorCode != ErrorCode::None) out.insert(QStringLiteral("error"), errorObject(r.errorCode, r.errorMessage));
    return out;
}
QJsonObject modelObject(const ModelInfo& m)
{
    auto object = modelRecordObject(m.model);
    object.insert(QStringLiteral("context_tokens"), m.contextTokens);
    object.insert(QStringLiteral("options"), m.options);
    object.insert(QStringLiteral("execution"), executionObject(m.execution));
    object.insert(QStringLiteral("memory"), memoryEstimateObject(m.memory));
    object.insert(QStringLiteral("keep_alive_ms"), m.keepAliveMs);
    object.insert(QStringLiteral("expires_in_ms"), m.expiresInMs);
    object.insert(QStringLiteral("active_requests"), m.activeRequests);
    return object;
}
QString string(const QJsonObject& o, const QString& key, bool required = true)
{
    const auto value = o.value(key);
    if ((!required && value.isUndefined())) return {};
    if (!value.isString() || (required && value.toString().trimmed().isEmpty()))
        throw Error(ErrorCode::InvalidArgument, key + QStringLiteral(" must be a nonempty string"));
    return value.toString();
}
double number(const QJsonObject& o, const QString& key, double fallback, double min, double max, bool integral = false)
{
    const auto v = o.value(key);
    if (v.isUndefined()) return fallback;
    const double n = v.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!v.isDouble() || !std::isfinite(n) || n < min || n > max || (integral && std::floor(n) != n))
        throw Error(ErrorCode::InvalidArgument, key + QStringLiteral(" has an invalid numeric value"));
    return n;
}
QJsonObject object(const QJsonObject& o, const QString& key)
{
    const auto v = o.value(key);
    if (!v.isUndefined() && !v.isObject()) throw Error(ErrorCode::InvalidArgument, key + QStringLiteral(" must be an object"));
    return v.toObject();
}
QByteArray line(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n'; }
struct Inbox {
    std::mutex mutex;
    std::deque<QJsonObject> events;
    qsizetype bytes = 0;
    bool closed = false;
    bool overflow = false;
};
}
class LocalIpcServer::Impl {
public:
    struct Client {
        QLocalSocket* socket;
        QByteArray input;
        QSet<QString> ids;
        QHash<QString, CancellationToken> generations; // Owned generation and pull cancellation tokens.
        std::vector<std::function<bool()>> pending;
        std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
    };
    Impl(Service& service, IpcOptions options, QObject* owner) : service(service), options(options), owner(owner)
    {
        if (options.maxConnections < 1 || options.maxInFlightPerConnection < 1 || options.maxFrameBytes < 64
            || options.maxBufferedOutputBytes < 256)
            throw Error(ErrorCode::InvalidArgument, QStringLiteral("Invalid IPC limits"));
        server.setSocketOptions(QLocalServer::UserAccessOption);
        QObject::connect(&server, &QLocalServer::newConnection, owner, [this] { accept(); });
        timer.setInterval(10);
        QObject::connect(&timer, &QTimer::timeout, owner, [this] { poll(); });
        timer.start();
    }
    ~Impl() { close(); }
    Service& service;
    IpcOptions options;
    QObject* owner;
    QLocalServer server;
    std::unique_ptr<QLockFile> endpointLock;
    QString listenError;
    QTimer timer;
    std::vector<std::shared_ptr<Client>> clients;

    void close()
    {
        server.close();
        endpointLock.reset();
        for (auto& c : clients) disconnect(c);
        clients.clear();
    }
    bool listen(const QString& name)
    {
        listenError.clear();
        if (server.isListening() || name.trimmed().isEmpty()) {
            listenError = QStringLiteral("Server is already listening or socket name is empty");
            return false;
        }
#ifdef Q_OS_WIN
        const auto lockPath = QDir::temp().filePath(QStringLiteral("iilocal-llm-")
            + QString::fromLatin1(QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256).toHex()) + QStringLiteral(".lock"));
#else
        const auto path = QDir::cleanPath(name.startsWith('/') ? name : QDir::temp().filePath(name));
        const auto lockPath = path + QStringLiteral(".lock");
#endif
        auto lock = std::make_unique<QLockFile>(lockPath);
        lock->setStaleLockTime(0);
        if (!lock->tryLock(0)) { listenError = QStringLiteral("Local endpoint is locked or inaccessible"); return false; }
#ifndef Q_OS_WIN
        // Qt 6.8 UserAccessOption uses rename(), which can otherwise replace a live socket/file.
        const QFileInfo existing(path);
        if (existing.exists() || existing.isSymbolicLink()) {
            listenError = QStringLiteral("Local endpoint already exists; refusing to replace it");
            return false;
        }
#endif
        if (!server.listen(name)) return false;
        endpointLock = std::move(lock);
        return true;
    }
    void disconnect(const std::shared_ptr<Client>& c)
    {
        {
            std::lock_guard lock(c->inbox->mutex);
            c->inbox->closed = true;
            c->inbox->events.clear();
        }
        for (const auto& h : c->generations) h.cancel();
        c->generations.clear();
        QObject::disconnect(c->socket, nullptr, owner, nullptr);
        c->socket->abort();
        c->socket->deleteLater();
    }
    void send(const std::shared_ptr<Client>& c, const QJsonObject& value)
    {
        if (c->socket->state() != QLocalSocket::ConnectedState) return;
        const auto bytes = line(value);
        if (c->socket->bytesToWrite() + bytes.size() > options.maxBufferedOutputBytes) {
            c->socket->abort();
            for (const auto& h : c->generations) h.cancel();
            return;
        }
        c->socket->write(bytes);
    }
    void reply(const std::shared_ptr<Client>& c, const QString& id, QJsonValue result)
    {
        send(c, {{QStringLiteral("id"), id}, {QStringLiteral("result"), std::move(result)}});
        c->ids.remove(id);
    }
    void error(const std::shared_ptr<Client>& c, const QString& id, const Error& e, bool release = true)
    {
        send(c, {{QStringLiteral("id"), id}, {QStringLiteral("error"), errorObject(e.code(), QString::fromUtf8(e.what()))}});
        if (release) c->ids.remove(id);
    }
    template<class Future, class Convert> void watch(const std::shared_ptr<Client>& c, const QString& id,
                                               Future future, Convert convert, QString operationId = {})
    {
        auto f = std::make_shared<Future>(std::move(future));
        c->pending.push_back([this, weak = std::weak_ptr<Client>(c), id, f, convert, operationId]() {
            auto client = weak.lock();
            if (!client) return true;
            if (f->wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
            if (!operationId.isEmpty()) client->generations.remove(operationId);
            try {
                if constexpr (std::is_void_v<decltype(f->get())>) { f->get(); reply(client, id, QJsonObject{}); }
                else reply(client, id, convert(f->get()));
            } catch (const Error& e) { error(client, id, e); }
            catch (const std::exception& e) { error(client, id, Error(ErrorCode::RuntimeFailure, QString::fromUtf8(e.what()))); }
            return true;
        });
    }
    void accept()
    {
        while (auto* socket = server.nextPendingConnection()) {
            if (clients.size() >= static_cast<size_t>(options.maxConnections)) { socket->abort(); socket->deleteLater(); continue; }
            auto c = std::make_shared<Client>();
            c->socket = socket;
            socket->setReadBufferSize(options.maxFrameBytes + 1);
            clients.push_back(c);
            QObject::connect(socket, &QLocalSocket::readyRead, owner, [this, weak = std::weak_ptr<Client>(c)] {
                if (auto client = weak.lock()) read(client);
            });
            QObject::connect(socket, &QLocalSocket::disconnected, owner, [weak = std::weak_ptr<Client>(c)] {
                if (auto client = weak.lock()) {
                    for (const auto& h : client->generations) h.cancel();
                    std::lock_guard lock(client->inbox->mutex);
                    client->inbox->closed = true;
                }
            });
        }
    }
    void poll()
    {
        for (auto it = clients.begin(); it != clients.end();) {
            auto c = *it;
            bool overflow;
            std::deque<QJsonObject> events;
            {
                std::lock_guard lock(c->inbox->mutex);
                overflow = c->inbox->overflow;
                events.swap(c->inbox->events);
                c->inbox->bytes = 0;
            }
            if (overflow || c->socket->state() != QLocalSocket::ConnectedState) {
                disconnect(c);
                it = clients.erase(it);
                continue;
            }
            for (const auto& e : events) {
                send(c, e);
                if (e.value(QStringLiteral("event")) == QStringLiteral("done")) {
                    c->ids.remove(e.value(QStringLiteral("id")).toString());
                    c->generations.remove(e.value(QStringLiteral("request_id")).toString());
                }
            }
            std::erase_if(c->pending, [](auto& ready) { return ready(); });
            ++it;
        }
    }
    void read(const std::shared_ptr<Client>& c)
    {
        c->input += c->socket->readAll();
        for (;;) {
            const auto newline = c->input.indexOf('\n');
            if (newline < 0) break;
            if (newline > options.maxFrameBytes) { c->socket->abort(); return; }
            const auto frame = c->input.first(newline);
            c->input.remove(0, newline + 1);
            QJsonParseError parse;
            const auto doc = QJsonDocument::fromJson(frame, &parse);
            if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
                error(c, {}, Error(ErrorCode::ProtocolError, QStringLiteral("Expected one JSON object per line")), false);
                continue;
            }
            const auto request = doc.object();
            QString id;
            bool reserved = false;
            try {
                id = string(request, QStringLiteral("id"));
                if (id.size() > 128) throw Error(ErrorCode::InvalidArgument, QStringLiteral("Request id too long"));
                if (c->ids.contains(id)) throw Error(ErrorCode::AlreadyExists, QStringLiteral("Request id already in flight"));
                const auto method = string(request, QStringLiteral("method"));
                if (c->ids.size() >= options.maxInFlightPerConnection && method != QStringLiteral("cancel"))
                    throw Error(ErrorCode::QueueFull, QStringLiteral("Connection request limit reached"));
                c->ids.insert(id);
                reserved = true;
                dispatch(c, id, method, object(request, QStringLiteral("params")));
            } catch (const Error& e) { error(c, id, e, reserved); }
        }
        if (c->input.size() > options.maxFrameBytes) c->socket->abort();
    }
    void dispatch(const std::shared_ptr<Client>& c, const QString& id, const QString& method, const QJsonObject& p)
    {
        const auto noValue = [] { return QJsonObject{}; };
        if (method == QStringLiteral("hardware.get")) {
            reply(c, id, hardwareObject(service.hardware()));
        } else if (method == "parameters.list") {
            QJsonArray groups;
            const auto catalog = ParameterCatalog::builtin();
            for (const auto& name : catalog.groups()) {
                const auto group = catalog.group(name);
                groups.append(QJsonObject{{"id",name},{"description",group.description},
                    {"phase",enumName(group.phase)},{"fields",int(group.parameters.size())}});
            }
            reply(c, id, groups);
        } else if (method == "parameters.get") {
            const auto name = string(p, "group");
            const auto catalog = ParameterCatalog::builtin();
            (void)catalog.group(name);
            for (const auto& group : catalog.toJson().value("groups").toArray())
                if (group.toObject().value("id") == name) { reply(c,id,group); break; }
        } else if (method == "parameters.validate") {
            for (const auto* flag : {"defaults", "redact"})
                if (p.contains(flag) && !p.value(flag).isBool()) throw Error(ErrorCode::InvalidArgument, "Parameter export flags must be boolean");
            auto parameters = ParameterObject::fromNativeJson(string(p, "group"), object(p, "values"));
            const auto issues = parameters.validate(true);
            if (!issues.isEmpty()) throw Error(ErrorCode::InvalidArgument, issues.first().path + ": " + issues.first().message);
            reply(c,id,parameters.toNativeJson(p.value("defaults").toBool(),p.value("redact").toBool()));
        } else if (method == QStringLiteral("models.list")) {
            watch(c, id, service.installedModels(), modelListingObject);
        } else if (method == QStringLiteral("models.install")) {
            watch(c, id, service.installModel(string(p, QStringLiteral("package_directory"))), modelRecordObject);
        } else if (method == QStringLiteral("models.pull")) {
            if (p.size() != 1) throw Error(ErrorCode::InvalidArgument, QStringLiteral("models.pull accepts only a model reference"));
            auto handle = service.pullModel(string(p, QStringLiteral("model")),
                [weak = std::weak_ptr<Inbox>(c->inbox), id, limit = options.maxBufferedOutputBytes](const PullProgress& progress) {
                    const auto inbox = weak.lock();
                    if (!inbox) throw Error(ErrorCode::Cancelled, QStringLiteral("Pull client disconnected"));
                    QJsonObject event{{"id", id}, {"event", "progress"}, {"model", progress.model}, {"file", progress.file},
                        {"received_bytes", double(progress.receivedBytes)}, {"total_bytes", double(progress.totalBytes)}};
                    const auto size = line(event).size();
                    std::lock_guard lock(inbox->mutex);
                    if (inbox->closed || inbox->overflow || inbox->bytes + size > limit) {
                        inbox->overflow = true;
                        throw Error(ErrorCode::Cancelled, QStringLiteral("Pull client output limit or disconnect"));
                    }
                    inbox->bytes += size; inbox->events.push_back(std::move(event));
                });
            c->generations.insert(handle.requestId, handle.cancellation);
            send(c, {{"id", id}, {"event", "accepted"}, {"request_id", handle.requestId}});
            watch(c, id, handle.result, modelRecordObject, handle.requestId);
        } else if (method == QStringLiteral("models.remove")) {
            watch(c, id, service.removeModel(string(p, QStringLiteral("model"))), noValue);
        } else if (method == QStringLiteral("models.resolve")) {
            watch(c, id, service.resolveModel(string(p, QStringLiteral("model"))), modelRecordObject);
        } else if (method == QStringLiteral("models.verify")) {
            watch(c, id, service.verifyModel(string(p, QStringLiteral("model"))), verificationObject);
        } else if (method == QStringLiteral("models.loaded")) {
            watch(c, id, service.models(), [](const QList<ModelInfo>& models) {
                QJsonArray array; for (const auto& model : models) array.append(modelObject(model)); return array;
            });
        } else if (method == QStringLiteral("models.load")) {
            for (auto it = p.begin(); it != p.end(); ++it) {
                if (it.key() != QStringLiteral("model")
                    && it.key() != QStringLiteral("context_tokens") && it.key() != QStringLiteral("options") && it.key() != QStringLiteral("keep_alive"))
                    throw Error(ErrorCode::InvalidArgument, QStringLiteral("Unknown models.load field; execution is selected by the service: ") + it.key());
            }
            ModelLoadRequest m{string(p, QStringLiteral("model")),
                int(number(p, QStringLiteral("context_tokens"), 0, 0, 1024 * 1024, true)), object(p, QStringLiteral("options")),
                parseKeepAlive(p.value(QStringLiteral("keep_alive")))};
            watch(c, id, service.loadModel(m), modelObject);
        } else if (method == QStringLiteral("models.unload")) {
            watch(c, id, service.unloadModel(string(p, QStringLiteral("model"))), noValue);
        } else if (method == QStringLiteral("sessions.create")) {
            watch(c, id, service.createSession(string(p, QStringLiteral("model")), string(p, QStringLiteral("system"), false)),
                  [](const QString& session) { return QJsonObject{{QStringLiteral("session_id"), session}}; });
        } else if (method == QStringLiteral("sessions.get")) {
            watch(c, id, service.session(string(p, QStringLiteral("session_id"))), [](const SessionSnapshot& session) {
                QJsonArray messages;
                for (const auto& message : session.messages)
                    messages.append(QJsonObject{{QStringLiteral("role"), enumName(message.role)}, {QStringLiteral("content"), message.content}});
                return QJsonObject{{QStringLiteral("session_id"), session.id}, {QStringLiteral("model"), modelUri(session.modelId)},
                                   {QStringLiteral("messages"), messages}};
            });
        } else if (method == QStringLiteral("sessions.reset")) {
            watch(c, id, service.resetSession(string(p, QStringLiteral("session_id"))), noValue);
        } else if (method == QStringLiteral("sessions.close")) {
            watch(c, id, service.closeSession(string(p, QStringLiteral("session_id"))), noValue);
        } else if (method == QStringLiteral("stats")) {
            watch(c, id, service.stats(), [](const ServiceStats& s) {
                return QJsonObject{{QStringLiteral("loaded_models"), s.loadedModels}, {QStringLiteral("sessions"), s.sessions},
                    {QStringLiteral("cached_contexts"), s.cachedContexts}, {QStringLiteral("reserved_context_tokens"), s.reservedContextTokens},
                    {QStringLiteral("cache_evictions"), double(s.cacheEvictions)}, {"resident_estimated_bytes", double(s.residentBytes)},
                    {"memory_budget_bytes", double(s.memoryBudgetBytes)}, {"available_ram_bytes", s.availableRamKnown ? QJsonValue(double(s.availableRamBytes)) : QJsonValue(QJsonValue::Null)},
                    {"default_keep_alive_ms", s.defaultKeepAliveMs}, {"model_loads", double(s.modelLoads)}, {"model_evictions", double(s.modelEvictions)}};
            });
        } else if (method == QStringLiteral("chat")) {
            ChatRequest r;
            r.sessionId = string(p, QStringLiteral("session_id"));
            r.prompt = string(p, QStringLiteral("prompt"));
            r.keepAliveMs = parseKeepAlive(p.value(QStringLiteral("keep_alive")));
            const auto o = object(p, QStringLiteral("options"));
            r.options = generationOptionsFromJson(o);
            auto handle = service.chat(r, [weak = std::weak_ptr<Inbox>(c->inbox), id, limit = options.maxBufferedOutputBytes](const StreamEvent& e) {
                const auto inbox = weak.lock();
                if (!inbox) throw Error(ErrorCode::ConsumerFailure, QStringLiteral("IPC client disconnected"));
                QJsonObject event{{QStringLiteral("id"), id}, {QStringLiteral("request_id"), e.requestId}};
                if (e.kind == StreamEventKind::Started) event.insert(QStringLiteral("event"), QStringLiteral("started"));
                else if (e.kind == StreamEventKind::Delta) {
                    event.insert(QStringLiteral("event"), QStringLiteral("delta"));
                    event.insert(QStringLiteral("text"), e.text);
                } else {
                    event.insert(QStringLiteral("event"), QStringLiteral("done"));
                    event.insert(QStringLiteral("result"), resultObject(e.result));
                }
                const auto size = line(event).size();
                std::lock_guard lock(inbox->mutex);
                if (inbox->closed || inbox->overflow || inbox->bytes + size > limit) {
                    inbox->overflow = true;
                    throw Error(ErrorCode::ConsumerFailure, QStringLiteral("IPC output buffer limit reached"));
                }
                inbox->bytes += size;
                inbox->events.push_back(std::move(event));
            });
            c->generations.insert(handle.requestId, handle.cancellation);
            send(c, {{QStringLiteral("id"), id}, {QStringLiteral("event"), QStringLiteral("accepted")},
                     {QStringLiteral("request_id"), handle.requestId}});
        } else if (method == QStringLiteral("cancel")) {
            const auto requestId = string(p, QStringLiteral("request_id"));
            const auto found = c->generations.constFind(requestId);
            if (found == c->generations.cend()) throw Error(ErrorCode::NotFound, QStringLiteral("Request not owned by this connection"));
            found->cancel();
            reply(c, id, QJsonObject{{QStringLiteral("cancel_requested"), true}});
        } else throw Error(ErrorCode::NotFound, QStringLiteral("Unknown method"));
    }
};
LocalIpcServer::LocalIpcServer(Service& service, IpcOptions options, QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>(service, options, this)) {}
LocalIpcServer::~LocalIpcServer() = default;
bool LocalIpcServer::listen(const QString& name) { return d->listen(name); }
void LocalIpcServer::close() { d->close(); }
QString LocalIpcServer::serverName() const { return d->server.fullServerName(); }
QString LocalIpcServer::errorString() const { return d->listenError.isEmpty() ? d->server.errorString() : d->listenError; }
} // namespace iiLocalLLM
