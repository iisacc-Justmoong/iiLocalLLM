#include "McpConnections.h"
#include "../mcp/HttpClient.h"
#include "../mcp/LocalApplications.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QUuid>
#include <chrono>
#include <condition_variable>
#include <climits>
#include <mutex>
#include <optional>
#include <thread>

namespace iiLocalLLM::agent {
namespace {
using Clock = std::chrono::steady_clock;
void require(bool condition, const char* message) {
    if (!condition) throw Error(ErrorCode::InvalidArgument, QString::fromLatin1(message));
}
QString string(const QJsonValue& value) {
    require(value.isString(), "MCP configuration requires a string");
    const auto text = value.toString();
    require(text.size() <= 65536 && !text.contains(QChar(0)), "Invalid MCP configuration string");
    return text;
}
bool flag(const QJsonObject& object, const char* key) {
    require(!object.contains(key) || object[key].isBool(), "MCP configuration requires a boolean");
    return object[key].toBool();
}
std::optional<int> timeout(const QJsonObject& object, const char* key) {
    if (!object.contains(key)) return {};
    const auto value = object.value(key);
    const auto integer = value.toInteger();
    require(value.isDouble() && integer > 0 && integer <= INT_MAX
        && value.toDouble() == double(integer), "MCP timeout must be a positive 32-bit integer in milliseconds");
    return int(integer);
}
QString expand(const QJsonValue& value, const QProcessEnvironment& environment) {
    const auto input = string(value);
    static const QRegularExpression expression(R"(\$\{([A-Za-z_][A-Za-z0-9_]*)(?::-([^}]*))?\})");
    auto matches = expression.globalMatch(input); QString output; qsizetype offset = 0;
    while (matches.hasNext()) {
        const auto match = matches.next(); output += input.mid(offset, match.capturedStart() - offset);
        require(environment.contains(match.captured(1)) || match.hasCaptured(2), "MCP configuration environment variable is missing");
        output += environment.contains(match.captured(1)) ? environment.value(match.captured(1)) : match.captured(2);
        require(output.size() <= 65536, "Expanded MCP configuration string is too large");
        offset = match.capturedEnd();
    }
    output += input.mid(offset);
    require(output.size() <= 65536 && !output.contains(QChar(0)), "Invalid expanded MCP configuration string");
    return output;
}
struct Config {
    QString name, type, command, cwd, appId;
    QString applicationInstance;
    QStringList args;
    QProcessEnvironment environment;
    QUrl endpoint;
    QMap<QByteArray, QByteArray> headers;
    bool disabled = false, alwaysLoad = false;
    std::optional<int> initializeTimeoutMs, requestTimeoutMs;
    QByteArray fingerprint;
};
std::map<QString, Config> readConfigs(const McpConnectionOptions& options) {
    QJsonObject merged;
    require(options.configFiles.size() <= 64, "Too many MCP configuration files");
    for (const auto& name : options.configFiles) {
        QFile file(QDir(options.workingDirectory).absoluteFilePath(name));
        require(file.open(QIODevice::ReadOnly), "Cannot read MCP configuration file");
        const auto bytes = file.read(1024 * 1024 + 1);
        require(file.error() == QFileDevice::NoError && bytes.size() <= 1024 * 1024, "MCP configuration exceeds 1 MiB or could not be read");
        QJsonParseError error; const auto document = QJsonDocument::fromJson(bytes, &error);
        require(error.error == QJsonParseError::NoError && document.isObject()
            && document.object()["mcpServers"].isObject(), "MCP configuration requires a mcpServers object");
        const auto servers = document.object()["mcpServers"].toObject();
        for (auto it = servers.begin(); it != servers.end(); ++it) merged[it.key()] = it.value();
        require(merged.size() <= options.maxServers, "Too many configured MCP servers");
    }
    std::map<QString, Config> configs;
    for (auto it = merged.begin(); it != merged.end(); ++it) {
        require(!it.key().trimmed().isEmpty() && it.key().size() <= 128
            && !it.key().contains(QRegularExpression("[\\x{0}-\\x{1f}\\x{7f}]")), "Invalid MCP server name");
        require(it.value().isObject(), "MCP server definition must be an object");
        const auto object = it.value().toObject(); Config c; c.name = it.key();
        c.disabled = flag(object, "disabled"); c.alwaysLoad = flag(object, "alwaysLoad");
        c.initializeTimeoutMs = timeout(object, "initializeTimeoutMs");
        c.requestTimeoutMs = timeout(object, "requestTimeoutMs");
        if (object.contains("appId")) { c.appId = string(object["appId"]); require(c.appId.size() <= 256, "MCP app ID is too long"); }
        if (c.disabled) { configs.emplace(c.name, std::move(c)); continue; }
        c.type = object.contains("type") ? string(object["type"]) : "stdio";
        if (c.type == "streamable-http") c.type = "http";
        require(c.type == "stdio" || c.type == "http", "Unsupported MCP transport; use stdio or http");
        c.environment = options.environment;
        c.cwd = object.contains("cwd") ? QDir(options.workingDirectory).absoluteFilePath(expand(object["cwd"], options.environment)) : options.workingDirectory;
        require(QFileInfo(c.cwd).isDir(), "MCP working directory does not exist");
        if (c.type == "stdio") {
            require(!object.contains("url") && !object.contains("headers"), "Stdio MCP configuration cannot contain HTTP fields");
            c.command = expand(object["command"], options.environment);
            require(!c.command.trimmed().isEmpty(), "MCP command is empty");
            require(!object.contains("args") || object["args"].isArray(), "MCP arguments must be an array");
            const auto args = object["args"].toArray(); require(args.size() <= 256, "Too many MCP arguments");
            for (const auto& arg : args) c.args.append(expand(arg, options.environment));
            require(!object.contains("env") || object["env"].isObject(), "MCP environment must be an object");
            const auto env = object["env"].toObject(); require(env.size() <= 256, "Too many MCP environment entries");
            for (auto v = env.begin(); v != env.end(); ++v) {
                require(QRegularExpression(QRegularExpression::anchoredPattern("[A-Za-z_][A-Za-z0-9_]*")).match(v.key()).hasMatch(), "Invalid MCP environment variable name");
                c.environment.insert(v.key(), expand(v.value(), options.environment));
            }
        } else {
            require(!object.contains("command") && !object.contains("args") && !object.contains("env"), "HTTP MCP configuration cannot contain stdio fields");
            c.endpoint = QUrl(expand(object["url"], options.environment), QUrl::StrictMode);
            require(c.endpoint.isValid() && (c.endpoint.scheme() == "https" || c.endpoint.scheme() == "http")
                && !c.endpoint.host().isEmpty() && c.endpoint.userInfo().isEmpty() && !c.endpoint.hasFragment(), "Invalid MCP HTTP endpoint");
            require(!object.contains("headers") || object["headers"].isObject(), "MCP headers must be an object");
            const auto headers = object["headers"].toObject(); require(headers.size() <= 128, "Too many MCP headers");
            for (auto h = headers.begin(); h != headers.end(); ++h) {
                const auto key = h.key().toLatin1().toLower(); const auto value = expand(h.value(), options.environment);
                require(QRegularExpression(QRegularExpression::anchoredPattern("[!#$%&'*+.^_`|~0-9A-Za-z-]+")).match(h.key()).hasMatch()
                    && !value.contains(QRegularExpression("[\\x{0}-\\x{1f}\\x{7f}]"))
                    && QString::fromLatin1(value.toLatin1()) == value && !c.headers.contains(key), "Invalid MCP HTTP header");
                c.headers[key] = value.toLatin1();
            }
        }
        // Hash expanded effective values; no configuration/credential is published.
        QJsonObject effective{{"source", object}, {"command", c.command}, {"args", QJsonArray::fromStringList(c.args)},
            {"cwd", c.cwd}, {"environment", QJsonArray::fromStringList(c.environment.toStringList())}, {"url", c.endpoint.toString()}};
        QJsonObject headers; for (auto h = c.headers.begin(); h != c.headers.end(); ++h) headers[QString::fromLatin1(h.key())] = QString::fromLatin1(h.value());
        effective["headers"] = headers;
        c.fingerprint = QCryptographicHash::hash(QJsonDocument(effective).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
        configs.emplace(c.name, std::move(c));
    }
    return configs;
}
}
class McpConnections::Impl {
public:
    struct Entry {
        Config config;
        std::shared_ptr<mcp::Client> client;
        QString connectionId, state = "pending";
        QList<Tool> tools;
        quint64 generation = 0;
        quint64 toolsRevision = 0;
        bool dirty = true;
        ErrorCode error = ErrorCode::None;
        int httpStatus = 0;
        QString errorPhase;
        qint64 errorElapsedMs = 0;
        QJsonObject requestTimeout;
        Clock::time_point retryAt{};
    };
    std::shared_ptr<ToolRegistry> registry;
    McpConnectionOptions options;
    std::mutex operation, stoppingMutex;
    mutable std::mutex stateMutex;
    std::condition_variable wake;
    std::thread worker;
    std::atomic_bool stopping = false;
    std::map<QString, Entry> entries;
    std::map<QString, Config> configured;
    ErrorCode applicationDiscoveryError = ErrorCode::None;
    QStringList ownedNames;
    QJsonArray statuses;
    std::map<QString, std::shared_ptr<mcp::Client>> clients;
    QList<QJsonObject> notifications;
    qsizetype notificationBytes = 0;

    Impl(std::shared_ptr<ToolRegistry> r, McpConnectionOptions o) : registry(std::move(r)), options(std::move(o)) {
        require(bool(registry) && options.maxServers >= 1 && options.maxServers <= 256
            && options.refreshIntervalMs >= 0 && options.refreshIntervalMs <= 60000
            && options.retryDelayMs >= 1 && options.retryDelayMs <= 3600000
            && options.limits.maxNotificationCount >= 1 && options.limits.maxQueuedBytes >= 1, "Invalid MCP connection manager options");
        options.workingDirectory = QFileInfo(options.workingDirectory).canonicalFilePath();
        require(QFileInfo(options.workingDirectory).isDir(), "MCP workspace must be an existing directory");
        require(options.localApplicationsDirectory.isEmpty()
            || (QDir::isAbsolutePath(options.localApplicationsDirectory) && !options.localApplicationsDirectory.contains(QChar(0))),
            "Local application registry must be an absolute directory");
    }
    void enqueue(QJsonObject notification) {
        const auto size = QJsonDocument(notification).toJson(QJsonDocument::Compact).size();
        std::lock_guard lock(stateMutex);
        if (size > options.limits.maxQueuedBytes) return;
        while (!notifications.isEmpty() && (notifications.size() >= options.limits.maxNotificationCount
            || notificationBytes + size > options.limits.maxQueuedBytes))
            notificationBytes -= QJsonDocument(notifications.takeFirst()).toJson(QJsonDocument::Compact).size();
        notifications.append(std::move(notification)); notificationBytes += size;
    }
    std::shared_ptr<mcp::Client> connect(const Config& c) {
        auto clientOptions = options.clientOptions ? options.clientOptions(c.name) : mcp::ClientOptions{};
        auto limits = options.limits;
        if (c.initializeTimeoutMs) limits.initializeTimeoutMs = *c.initializeTimeoutMs;
        if (c.requestTimeoutMs) limits.requestTimeoutMs = *c.requestTimeoutMs;
        if (c.type == "stdio") {
            mcp::StdioOptions o; static_cast<mcp::ClientLimits&>(o) = limits;
            o.program = c.command; o.arguments = c.args; o.environment = c.environment; o.workingDirectory = c.cwd;
            return std::make_shared<mcp::StdioClient>(std::move(o), std::move(clientOptions));
        }
        mcp::HttpOptions o; static_cast<mcp::ClientLimits&>(o) = limits;
        o.endpoint = c.endpoint; o.headers = c.headers; o.allowInsecureHttp = options.allowInsecureHttp;
        const auto authorization = o.headers.take("authorization");
        if (!authorization.isEmpty()) {
            require(authorization.left(7).toLower() == "bearer " && !authorization.mid(7).trimmed().isEmpty(),
                "Configured MCP Authorization must use Bearer authentication");
            o.bearerToken = [credential = authorization.mid(7)] { return credential; };
        }
        if (options.bearerToken && c.applicationInstance.isEmpty())
            o.bearerToken = [callback = options.bearerToken, name = c.name] { return callback(name); };
        return std::make_shared<mcp::HttpClient>(std::move(o), std::move(clientOptions));
    }
    void update(Entry& entry, const CancellationToken& cancellation, bool force) {
        if (entry.config.disabled) { entry.state = "disabled"; entry.tools.clear(); return; }
        if (entry.client) {
            for (const auto& notification : entry.client->takeNotifications()) {
                if (notification["method"] == "notifications/tools/list_changed") {
                    entry.dirty = true;
                    // Keep invalidation if this refresh is cancelled before publish.
                    const auto old = entries.find(entry.config.name);
                    if (old != entries.end() && old->second.client == entry.client) old->second.dirty = true;
                }
                enqueue({{"server", entry.config.name}, {"notification", notification}});
            }
            if (!entry.client->isConnected()) {
                entry.tools.clear(); entry.dirty = true;
                if (!entry.client->isClosed()) { entry.state = "reconnecting"; return; }
            }
        }
        if (!force && Clock::now() < entry.retryAt) return;
        QString phase = "connect";
        auto phaseStarted = Clock::now();
        QJsonObject requestTimeout;
        try {
            cancellation.throwIfCancelled();
            if (!entry.client || entry.client->isClosed()) {
                entry.client = connect(entry.config); entry.connectionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
                entry.dirty = true;
            }
            if (force || entry.dirty || entry.generation != entry.client->connectionGeneration()) {
                phase = "discover_tools"; phaseStarted = Clock::now();
                QList<Tool> tools;
                const auto generation = entry.client->connectionGeneration();
                if (entry.client->serverCapabilities().contains("tools"))
                    tools = mcpTools(entry.client, {entry.config.name, entry.config.appId, options.trustAnnotations}, cancellation);
                if (generation != entry.client->connectionGeneration() || !entry.client->isConnected())
                    throw Error(ErrorCode::RuntimeFailure, "MCP session changed during discovery");
                for (auto& tool : tools) {
                    tool.definition.deferred = options.deferTools && !entry.config.alwaysLoad;
                    tool.definition.metadata["connection_id"] = entry.connectionId;
                    tool.definition.metadata["connection_generation"] = QString::number(generation);
                    if (!entry.config.applicationInstance.isEmpty()) {
                        tool.definition.metadata["application_instance"] = entry.config.applicationInstance;
                        tool.definition.metadata["discovery_source"] = "local_application";
                    }
                }
                entry.tools = std::move(tools); entry.generation = generation; entry.dirty = false; ++entry.toolsRevision;
            }
            entry.state = "ready"; entry.error = ErrorCode::None; entry.httpStatus = 0; entry.retryAt = {};
            entry.errorPhase.clear(); entry.errorElapsedMs = 0; entry.requestTimeout = {};
        } catch (const Error& error) {
            if (error.code() == ErrorCode::Cancelled) throw;
            entry.error = error.code(); entry.httpStatus = 0;
            if (const auto* http = dynamic_cast<const mcp::HttpError*>(&error)) entry.httpStatus = http->statusCode();
            if (const auto* timeout = dynamic_cast<const mcp::RequestTimeoutError*>(&error))
                requestTimeout = {{"method", timeout->method()}, {"timeout_ms", timeout->timeoutMs()},
                    {"elapsed_ms", timeout->elapsedMs()}, {"submitted", timeout->submitted()}};
        } catch (...) { entry.error = ErrorCode::RuntimeFailure; entry.httpStatus = 0; }
        if (entry.error != ErrorCode::None) {
            entry.errorPhase = phase;
            entry.errorElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - phaseStarted).count();
            entry.requestTimeout = std::move(requestTimeout);
            entry.state = "failed"; entry.dirty = true; entry.tools.clear();
            entry.retryAt = Clock::now() + std::chrono::milliseconds(options.retryDelayMs);
        }
    }
    void publish(std::map<QString, Entry> next) {
        QList<Tool> tools; QStringList names; QJsonArray status;
        std::map<QString, std::shared_ptr<mcp::Client>> nextClients;
        for (const auto& [name, entry] : next) {
            tools.append(entry.tools); for (const auto& t : entry.tools) names.append(t.definition.name);
            QJsonObject item{{"name", name}, {"transport", entry.config.type}, {"state", entry.state},
                {"tool_count", entry.tools.size()}, {"deferred", options.deferTools && !entry.config.alwaysLoad},
                {"generation", QString::number(entry.generation)}};
            if (!entry.config.appId.isEmpty()) item["app_id"] = entry.config.appId;
            item["source"] = entry.config.applicationInstance.isEmpty() ? "configuration" : "local_application";
            if (entry.error != ErrorCode::None) {
                item["error_code"] = enumName(entry.error);
                item["error_phase"] = entry.errorPhase;
                item["error_elapsed_ms"] = entry.errorElapsedMs;
                if (!entry.requestTimeout.isEmpty()) item["request_timeout"] = entry.requestTimeout;
            }
            if (entry.httpStatus) item["http_status"] = entry.httpStatus;
            status.append(item);
            if (entry.client) nextClients[name] = entry.client;
        }
        bool changed = next.size() != entries.size();
        for (const auto& [name, entry] : next) {
            const auto old = entries.find(name);
            changed |= old == entries.end() || old->second.client != entry.client
                || old->second.toolsRevision != entry.toolsRevision || old->second.tools.size() != entry.tools.size();
        }
        if (changed) registry->replace(ownedNames, std::move(tools));
        ownedNames = std::move(names);
        auto old = std::move(entries); entries = std::move(next);
        {
            std::lock_guard lock(stateMutex); statuses = std::move(status); clients.swap(nextClients);
        }
        // Network shutdown/destructors stay outside the registry and state locks.
        for (const auto& [name, entry] : old) {
            const auto found = entries.find(name);
            if (entry.client && (found == entries.end() || found->second.client != entry.client)) entry.client->close();
        }
    }
    void refresh(bool reload, CancellationToken cancellation) {
        std::lock_guard lock(operation);
        {
            std::lock_guard stateLock(stateMutex);
            if (stopping) throw Error(ErrorCode::ShuttingDown, "MCP connections are closed");
        }
        cancellation.throwIfCancelled();
        const auto authorized = reload ? readConfigs(options) : configured;
        auto configs = authorized;
        if (!options.localApplicationsDirectory.isEmpty()) {
            const auto discovered = mcp::discoverLocalApplications(options.localApplicationsDirectory, options.maxServers);
            if (discovered.error != applicationDiscoveryError) {
                applicationDiscoveryError = discovered.error;
                enqueue({{"method", "iilocal/apps/discovery_state"}, {"error_code", enumName(discovered.error)}});
            }
            for (const auto& application : discovered.applications) {
                if (configs.contains(application.serverName) || configs.size() >= static_cast<size_t>(options.maxServers)) continue;
                Config config; config.name = application.serverName; config.type = "http";
                config.appId = application.application.id; config.applicationInstance = application.instanceId;
                config.endpoint = application.endpoint; config.cwd = options.workingDirectory;
                config.headers["authorization"] = "Bearer " + application.bearerToken;
                config.fingerprint = QCryptographicHash::hash(QJsonDocument(QJsonObject{
                    {"instance", config.applicationInstance}, {"app_id", config.appId},
                    {"url", config.endpoint.toString()}, {"token", QString::fromLatin1(application.bearerToken)},
                    {"name", application.application.name}, {"version", application.application.version}
                }).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
                configs.emplace(config.name, std::move(config));
            }
        }
        std::map<QString, Entry> next;
        for (const auto& [name, config] : configs) {
            const auto old = entries.find(name);
            if (old != entries.end() && old->second.config.disabled == config.disabled
                && old->second.config.fingerprint == config.fingerprint) {
                next[name] = old->second; next[name].config = config;
            }
            else { Entry entry; entry.config = config; next.emplace(name, std::move(entry)); }
        }
        for (auto& [name, entry] : next) {
            if (stopping) throw Error(ErrorCode::ShuttingDown, "MCP connections are closed");
            cancellation.throwIfCancelled(); update(entry, cancellation, reload);
        }
        cancellation.throwIfCancelled();
        if (stopping) throw Error(ErrorCode::ShuttingDown, "MCP connections are closed");
        publish(std::move(next));
        configured = authorized;
    }
    void start() {
        if (!options.refreshIntervalMs) return;
        worker = std::thread([this] {
            while (!stopping) {
                std::unique_lock lock(stateMutex);
                if (wake.wait_for(lock, std::chrono::milliseconds(options.refreshIntervalMs), [this] { return stopping.load(); })) break;
                lock.unlock();
                try { refresh(false, {}); }
                catch (const Error& error) {
                    if (!stopping) enqueue({{"method", "iilocal/mcp/refresh_error"}, {"error_code", enumName(error.code())}});
                } catch (...) {
                    if (!stopping) enqueue({{"method", "iilocal/mcp/refresh_error"}, {"error_code", "RuntimeFailure"}});
                }
            }
        });
    }
    void close() {
        std::lock_guard stopLock(stoppingMutex);
        std::map<QString, std::shared_ptr<mcp::Client>> closing;
        { std::lock_guard lock(stateMutex); stopping = true; closing = clients; }
        wake.notify_all();
        // Interrupt our clients, never cancel the host's potentially shared token.
        // New unpublished clients remain bounded by the initialization deadline.
        for (const auto& [name, client] : closing) client->close();
        if (worker.joinable()) worker.join();
        std::lock_guard lock(operation);
        registry->replace(ownedNames, {}); ownedNames.clear();
        auto old = std::move(entries);
        { std::lock_guard stateLock(stateMutex); clients.clear(); statuses = {}; }
        for (const auto& [name, entry] : old) if (entry.client) entry.client->close();
    }
};
McpConnections::McpConnections(std::shared_ptr<ToolRegistry> registry, McpConnectionOptions options)
    : d(std::make_unique<Impl>(std::move(registry), std::move(options))) { d->refresh(true, {}); d->start(); }
McpConnections::~McpConnections() { close(); }
void McpConnections::reload(CancellationToken token) { d->refresh(true, std::move(token)); }
void McpConnections::refresh(CancellationToken token) { d->refresh(false, std::move(token)); }
QJsonArray McpConnections::status() const { std::lock_guard lock(d->stateMutex); return d->statuses; }
QList<QJsonObject> McpConnections::takeNotifications() {
    std::lock_guard lock(d->stateMutex); QList<QJsonObject> result; result.swap(d->notifications); d->notificationBytes = 0; return result;
}
std::shared_ptr<mcp::Client> McpConnections::client(const QString& name) const {
    std::lock_guard lock(d->stateMutex); const auto it = d->clients.find(name);
    if (it == d->clients.end()) throw Error(ErrorCode::NotFound, "MCP server is not connected");
    return it->second;
}
void McpConnections::close() { d->close(); }
}
