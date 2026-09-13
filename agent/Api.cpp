#include "Api.h"
#include "../Parameters.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QThreadPool>
#include <QtCore/QUuid>
#include <chrono>
#include <map>
#include <mutex>

namespace iiLocalLLM::agent {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
void require(bool ok, QString message, ErrorCode code = ErrorCode::InvalidArgument) { if (!ok) throw Error(code, message); }
void fields(const QJsonObject& p, const QSet<QString>& allowed) {
    for (auto it = p.begin(); it != p.end(); ++it) require(allowed.contains(it.key()), "Unknown agent API parameter: " + it.key());
}
QString text(const QJsonObject& p, const QString& key, bool required = true) {
    const auto value = p.value(key);
    require((!required && value.isUndefined()) || (value.isString() && (!required || !value.toString().trimmed().isEmpty())), "Invalid agent API " + key);
    return value.toString();
}
int integer(const QJsonObject& p, const QString& key, int fallback, int minimum, int maximum) {
    const auto value = p.value(key); if (value.isUndefined()) return fallback;
    require(value.isDouble() && value.toDouble() >= minimum && value.toDouble() <= maximum
        && value.toDouble() == value.toInt(), "Invalid agent API " + key); return value.toInt();
}
QJsonObject object(const QJsonObject& p, const QString& key) {
    const auto value = p.value(key); require(value.isUndefined() || value.isObject(), "Invalid agent API " + key); return value.toObject();
}
QStringList methods() { return {"agent.info", "agent.sessions.create", "agent.sessions.list", "agent.sessions.get",
    "agent.sessions.fork", "agent.run", "agent.cancel", "agent.status"}; }
QJsonObject sessionObject(const Session& s, int offset = 0, int limit = 0) {
    require(offset <= s.messages.size(), "Message offset exceeds the session length");
    QJsonArray messages;
    const auto end = std::min<qsizetype>(s.messages.size(), qsizetype(offset) + limit);
    for (auto n = offset; n < end; ++n) messages.append(toJson(s.messages[n]));
    QJsonObject value{{"session_id", s.id}, {"model", s.model}, {"system", s.systemPrompt},
        {"working_directory", s.workingDirectory}, {"message_count", s.messages.size()}, {"messages", messages}};
    if (end < s.messages.size()) value["next_offset"] = end;
    return value;
}
bool nested(const QString& path, const QString& root) { return path == root || path.startsWith(root.endsWith('/') ? root : root + '/'); }
}
class Api::Impl : public std::enable_shared_from_this<Impl> {
public:
    struct Client { QString id; QByteArray digest; std::shared_ptr<Engine> engine; std::mutex creation; };
    struct Job {
        QString id, method, clientId; QJsonObject params; CancellationToken token;
        Clock::time_point deadline; std::atomic_bool running = false;
        std::shared_ptr<std::promise<QJsonValue>> promise;
    };
    ApiOptions options;
    std::mutex mutex, joining;
    bool stopping = false;
    std::map<QString, std::shared_ptr<Client>> clients;
    std::map<QString, std::shared_ptr<Job>> active;
    QThreadPool workers;
    std::unique_ptr<QLockFile> stateLock;

    Impl(std::shared_ptr<Model> model, std::shared_ptr<ToolRegistry> registry,
        std::shared_ptr<const PermissionPolicy> policy, ApiOptions o) : options(std::move(o)) {
        require(model && registry && policy && !options.clientTokens.isEmpty() && options.clientTokens.size() <= 64
            && options.engine.sessionsDirectory.isEmpty() && options.maxConcurrentRequests >= 1 && options.maxConcurrentRequests <= 64
            && options.maxQueuedRequests >= 0 && options.maxQueuedRequests <= 10000
            && options.maxSessionsPerClient >= 1 && options.maxTurns >= 1 && options.maxTurns <= 10000
            && options.maxResultBytes >= 1024 && options.requestTimeoutMs > 0, "Invalid agent API configuration");
        options.workingDirectory = QFileInfo(options.workingDirectory).canonicalFilePath();
        require(!options.workingDirectory.isEmpty() && QFileInfo(options.workingDirectory).isDir(), "Agent API workspace must exist");
        require(!options.stateDirectory.trimmed().isEmpty() && QDir().mkpath(options.stateDirectory), "Cannot create agent API state directory");
        options.stateDirectory = QFileInfo(options.stateDirectory).canonicalFilePath();
        require(!nested(options.stateDirectory, options.workingDirectory) && !nested(options.workingDirectory, options.stateDirectory),
            "Agent API state and tool workspace must be disjoint");
        require(QFile::setPermissions(options.stateDirectory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner),
            "Cannot make agent API state private", ErrorCode::StorageFailure);
        stateLock = std::make_unique<QLockFile>(QDir(options.stateDirectory).filePath("api.lock")); stateLock->setStaleLockTime(0);
        require(stateLock->tryLock(0), "Agent API state is already owned or inaccessible", ErrorCode::AlreadyExists);
        const QRegularExpression idPattern("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"), tokenPattern("^[A-Za-z0-9_-]{32,256}$");
        QSet<QByteArray> digests;
        for (auto it = options.clientTokens.begin(); it != options.clientTokens.end(); ++it) {
            require(idPattern.match(it.key()).hasMatch() && tokenPattern.match(it.value()).hasMatch(), "Invalid agent API client ID or token format");
            auto client = std::make_shared<Client>(); client->id = it.key();
            client->digest = QCryptographicHash::hash(it.value().toUtf8(), QCryptographicHash::Sha256);
            require(!digests.contains(client->digest), "Agent API tokens must be distinct"); digests.insert(client->digest);
            auto engineOptions = options.engine;
            const auto directory = QString::fromLatin1(QCryptographicHash::hash(client->id.toUtf8(), QCryptographicHash::Sha256).toHex());
            engineOptions.sessionsDirectory = QDir(options.stateDirectory).filePath(directory + "/sessions");
            client->engine = std::make_shared<Engine>(model, registry, policy, engineOptions);
            clients.emplace(client->id, std::move(client));
        }
        options.clientTokens.clear(); workers.setMaxThreadCount(options.maxConcurrentRequests);
    }
    std::shared_ptr<Client> authenticateLocked(const QString& credential) {
        require(!stopping, "Agent API is shutting down", ErrorCode::ShuttingDown);
        const auto digest = QCryptographicHash::hash(credential.toUtf8(), QCryptographicHash::Sha256);
        std::shared_ptr<Client> found;
        for (const auto& [id, client] : clients) {
            unsigned difference = 0;
            for (qsizetype n = 0; n < digest.size(); ++n) difference |= unsigned(uchar(digest[n]) ^ uchar(client->digest[n]));
            if (!difference) found = client;
        }
        require(bool(found), "Agent API credential is missing or invalid", ErrorCode::Unauthorized); return found;
    }
    Session session(const std::shared_ptr<Client>& client, const QJsonObject& p) {
        auto result = client->engine->session(text(p, "session_id"));
        require(result.workingDirectory == options.workingDirectory, "Session belongs to a different workspace", ErrorCode::NotFound); return result;
    }
    QJsonValue perform(const std::shared_ptr<Client>& client, const std::shared_ptr<Job>& job, RpcEventCallback callback) {
        const auto& p = job->params; const auto& method = job->method;
        if (method == "agent.info") {
            fields(p, {}); QJsonArray names; for (const auto& name : methods()) names.append(name);
            return QJsonObject{{"protocol", "iisacc.agent/1"}, {"client_id", client->id}, {"methods", names}, {"max_turns", options.maxTurns},
                {"working_directory", options.workingDirectory}};
        }
        if (method == "agent.sessions.create") {
            fields(p, {"model", "system"}); const auto model = text(p, "model"), prompt = text(p, "system", false);
            require(model.size() <= 512 && prompt.size() <= options.engine.maxInputCharacters, "Agent session input exceeds limit");
            std::lock_guard lock(client->creation);
            require(client->engine->sessions().size() < options.maxSessionsPerClient, "Agent session limit reached", ErrorCode::ResourceLimit);
            return sessionObject(client->engine->createSession(model, options.workingDirectory, prompt));
        }
        if (method == "agent.sessions.list") {
            fields(p, {"cursor", "limit"}); const auto cursor = text(p, "cursor", false); require(cursor.size() <= 128, "Invalid session cursor");
            const int limit = integer(p, "limit", 32, 1, 100); const auto ids = client->engine->sessions();
            QJsonArray items; QString last; bool more = false;
            for (const auto& id : ids) if (id > cursor) {
                if (items.size() == limit) { more = true; break; }
                items.append(QJsonObject{{"session_id", id}}); last = id;
            }
            QJsonObject result{{"sessions", items}}; if (more) result["next_cursor"] = last; return result;
        }
        if (method == "agent.sessions.get") {
            fields(p, {"session_id", "offset", "limit"});
            return sessionObject(session(client, p), integer(p, "offset", 0, 0, 1000000), integer(p, "limit", 32, 0, 100));
        }
        if (method == "agent.sessions.fork") {
            fields(p, {"session_id", "through_message_id"}); const auto original = session(client, p);
            std::lock_guard lock(client->creation);
            require(client->engine->sessions().size() < options.maxSessionsPerClient, "Agent session limit reached", ErrorCode::ResourceLimit);
            auto result = sessionObject(client->engine->forkSession(original.id, text(p, "through_message_id", false)));
            result["parent_session_id"] = original.id; return result;
        }
        fields(p, {"session_id", "prompt", "options", "max_turns"}); const auto original = session(client, p);
        RunRequest request{original.id, text(p, "prompt"), generationOptionsFromJson(object(p, "options")), integer(p, "max_turns", options.maxTurns, 1, options.maxTurns)};
        job->token.throwIfCancelled();
        require(Clock::now() < job->deadline, "Agent API request deadline exceeded", ErrorCode::Timeout);
        quint64 sequence = 0;
        auto handle = client->engine->run(std::move(request), [&](const Event& event) {
            auto value = toJson(event); value["sequence"] = double(++sequence);
            require(sequence <= 200000 && QJsonDocument(value).toJson(QJsonDocument::Compact).size() <= options.maxResultBytes,
                "Agent API event exceeds limit", ErrorCode::ResourceLimit);
            if (callback) callback(value);
        });
        bool timedOut = false;
        while (handle.result.wait_for(10ms) != std::future_status::ready) {
            timedOut |= Clock::now() >= job->deadline;
            if (job->token.isCancelled() || timedOut) handle.cancel();
        }
        auto result = handle.result.get();
        require(!timedOut && Clock::now() < job->deadline, "Agent API request deadline exceeded", ErrorCode::Timeout);
        return toJson(result);
    }
    void execute(std::shared_ptr<Client> client, std::shared_ptr<Job> job, RpcEventCallback callback) {
        QJsonValue result; std::exception_ptr failure;
        try {
            job->token.throwIfCancelled(); require(Clock::now() < job->deadline, "Agent API request expired in queue", ErrorCode::Timeout);
            job->running = true; result = perform(client, job, std::move(callback));
            require(QJsonDocument(result.toObject()).toJson(QJsonDocument::Compact).size() <= options.maxResultBytes,
                "Agent API result exceeds limit", ErrorCode::ResourceLimit);
        } catch (...) { failure = std::current_exception(); }
        { std::lock_guard lock(mutex); active.erase(job->id); }
        if (failure) job->promise->set_exception(failure); else job->promise->set_value(std::move(result));
    }
    RpcHandle dispatch(QString method, QJsonObject params, QString credential, RpcEventCallback callback) {
        std::lock_guard lock(mutex); auto client = authenticateLocked(credential);
        require(methods().contains(method), "Unknown agent API method", ErrorCode::NotFound);
        require(QJsonDocument(params).toJson(QJsonDocument::Compact).size() <= options.maxResultBytes, "Agent API parameters exceed limit", ErrorCode::ResourceLimit);
        auto promise = std::make_shared<std::promise<QJsonValue>>(); RpcHandle handle{uuid(), {}, promise->get_future().share()};
        if (method == "agent.cancel" || method == "agent.status") {
            fields(params, {"request_id"}); const auto found = active.find(text(params, "request_id"));
            require(found != active.end() && found->second->clientId == client->id, "Agent request was not found", ErrorCode::NotFound);
            const auto job = found->second;
            if (method == "agent.cancel") { job->token.cancel(); promise->set_value(QJsonObject{{"cancel_requested", true}}); }
            else promise->set_value(QJsonObject{{"request_id", job->id}, {"method", job->method}, {"session_id", job->params.value("session_id")},
                {"state", job->running ? "running" : "queued"}, {"cancel_requested", job->token.isCancelled()}});
            return handle;
        }
        require(active.size() < size_t(options.maxConcurrentRequests + options.maxQueuedRequests), "Agent API queue is full", ErrorCode::QueueFull);
        auto job = std::make_shared<Job>(); job->id = handle.requestId; job->method = std::move(method); job->params = std::move(params);
        job->clientId = client->id; job->token = handle.cancellation; job->promise = std::move(promise);
        job->deadline = Clock::now() + std::chrono::milliseconds(options.requestTimeoutMs);
        active.emplace(job->id, job); auto self = shared_from_this();
        workers.start([self, client, job, callback = std::move(callback)] { self->execute(client, job, callback); });
        return handle;
    }
    void stop() {
        std::lock_guard join(joining);
        { std::lock_guard lock(mutex); stopping = true; for (const auto& [id, job] : active) job->token.cancel(); }
        workers.waitForDone();
        { std::lock_guard lock(mutex); clients.clear(); stateLock.reset(); }
    }
};
Api::Api(std::shared_ptr<Model> model, std::shared_ptr<ToolRegistry> registry,
    std::shared_ptr<const PermissionPolicy> policy, ApiOptions options)
    : d(std::make_shared<Impl>(std::move(model), std::move(registry), std::move(policy), std::move(options))) {}
Api::~Api() { d->stop(); }
RpcHandle Api::dispatch(QString method, QJsonObject params, QString credential, RpcEventCallback callback) {
    return d->dispatch(std::move(method), std::move(params), std::move(credential), std::move(callback));
}
void Api::close() { d->stop(); }
}
