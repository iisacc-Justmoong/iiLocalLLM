#include "LocalApplications.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtCore/QUuid>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#if defined(Q_OS_UNIX) && !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace iiLocalLLM::mcp {
namespace {
constexpr int recordLimit = 65536;
constexpr int scanLimit = 4096;
bool validIdentity(const LocalApplicationIdentity& identity) {
    static const QRegularExpression id(QRegularExpression::anchoredPattern("[A-Za-z][A-Za-z0-9.-]{0,79}"));
    const auto text = [](const QString& value, int limit) {
        return !value.trimmed().isEmpty() && value.size() <= limit
            && !value.contains(QRegularExpression("[\\x{0}-\\x{1f}\\x{7f}]"));
    };
    return id.match(identity.id).hasMatch() && text(identity.name, 128) && text(identity.version, 64);
}
QString serverName(const LocalApplicationIdentity& identity, const QString& instance) {
    return "app." + identity.id + '.' + QString(instance).remove('-');
}
QString checkedDirectory(const QString& directory) {
    if (directory.isEmpty() || !QDir::isAbsolutePath(directory) || directory.contains(QChar(0)))
        throw Error(ErrorCode::InvalidArgument, "Local application registry requires an absolute directory");
    return QDir::cleanPath(directory);
}
#if defined(Q_OS_UNIX) && !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
struct Descriptor {
    int value = -1;
    explicit Descriptor(int fd = -1) : value(fd) {}
    ~Descriptor() { if (value >= 0) ::close(value); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};
bool privateStat(const struct stat& info, bool directory) {
    return info.st_uid == geteuid() && !(info.st_mode & 0077)
        && (directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode) && info.st_nlink == 1);
}
int openDirectory(const QString& path, ErrorCode& error) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        error = errno == ENOENT ? ErrorCode::None : ErrorCode::StorageFailure;
        return -1;
    }
    struct stat info{};
    if (fstat(fd, &info) || !privateStat(info, true)) {
        ::close(fd); error = ErrorCode::Unauthorized; return -1;
    }
    return fd;
}
QByteArray readRecord(int directory, const QByteArray& name) {
    Descriptor file(openat(directory, name.constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    struct stat info{};
    if (file.value < 0 || fstat(file.value, &info) || !privateStat(info, false)
        || info.st_size < 1 || info.st_size > recordLimit) return {};
    QByteArray bytes; bytes.resize(recordLimit + 1);
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(file.value, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return {};
        if (!count) break;
        offset += count;
    }
    if (offset > recordLimit) return {};
    bytes.resize(offset); return bytes;
}
#endif
}
QString localApplicationsDirectory() {
    const auto override = qEnvironmentVariable("IILOCALLLM_APP_ENDPOINTS");
    if (!override.isEmpty()) return checkedDirectory(override);
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath("iisacc/AgentEndpoints");
}
LocalApplicationDiscovery discoverLocalApplications(const QString& directory, int maxApplications) {
    const auto path = checkedDirectory(directory);
    if (maxApplications < 1 || maxApplications > 256)
        throw Error(ErrorCode::InvalidArgument, "Invalid local application discovery limit");
    LocalApplicationDiscovery result;
#if defined(Q_OS_UNIX) && !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
    Descriptor folder(openDirectory(path, result.error));
    if (folder.value < 0) return result;
    const int scan = dup(folder.value);
    DIR* stream = scan < 0 ? nullptr : fdopendir(scan);
    if (!stream) { if (scan >= 0) ::close(scan); result.error = ErrorCode::StorageFailure; return result; }
    QList<QByteArray> names; int visited = 0;
    while (const auto* entry = readdir(stream)) {
        const QByteArray name(entry->d_name);
        if (name == "." || name == "..") continue;
        if (++visited > scanLimit) { result.truncated = true; break; }
        if (name.endsWith(".json")) names.append(name);
    }
    closedir(stream); std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        const auto bytes = readRecord(folder.value, name);
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(bytes, &parseError);
        const auto object = document.object();
        LocalApplicationEndpoint endpoint;
        endpoint.application = {object["app_id"].toString(), object["name"].toString(), object["version"].toString()};
        endpoint.instanceId = object["instance_id"].toString();
        endpoint.endpoint = QUrl(object["endpoint"].toString(), QUrl::StrictMode);
        endpoint.bearerToken = object["bearer_token"].toString().toLatin1();
        endpoint.processId = object["pid"].toInteger(-1);
        const auto instance = QUuid(endpoint.instanceId);
        const auto& url = endpoint.endpoint;
        static const QRegularExpression credential(QRegularExpression::anchoredPattern("[A-Za-z0-9_-]{43}"));
        if (parseError.error != QJsonParseError::NoError || !document.isObject() || object.size() != 8
            || object["schema"] != "iisacc.mcp-app/1" || !validIdentity(endpoint.application)
            || instance.isNull() || instance.toString(QUuid::WithoutBraces) != endpoint.instanceId
            || name != endpoint.instanceId.toLatin1() + ".json"
            || !url.isValid() || url.scheme() != "http" || url.host() != "127.0.0.1"
            || url.port() < 1 || url.port() > 65535 || url.path() != "/mcp"
            || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()
            || !credential.match(object["bearer_token"].toString()).hasMatch()
            || endpoint.processId < 1 || endpoint.processId > std::numeric_limits<pid_t>::max()
            || object["pid"].toDouble() != static_cast<double>(endpoint.processId)) {
            ++result.rejectedRecords; continue;
        }
        if (kill(static_cast<pid_t>(endpoint.processId), 0) != 0) { ++result.staleRecords; continue; }
        if (result.applications.size() >= maxApplications) { result.truncated = true; break; }
        endpoint.serverName = serverName(endpoint.application, endpoint.instanceId);
        result.applications.append(std::move(endpoint));
    }
#else
    Q_UNUSED(path);
    result.error = ErrorCode::RuntimeUnavailable;
#endif
    return result;
}
class LocalApplicationServer::Impl {
public:
    LocalApplicationIdentity identity;
    ServerOptions server;
    LocalApplicationServerOptions options;
    std::unique_ptr<HttpServer> http;
    QString instance, name, registration, error;
#if defined(Q_OS_UNIX) && !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
    std::unique_ptr<Descriptor> directory;
    ino_t recordInode = 0;
#endif
    Impl(LocalApplicationIdentity id, ServerOptions s, LocalApplicationServerOptions o)
        : identity(std::move(id)), server(std::move(s)), options(std::move(o)) {
        if (!validIdentity(identity) || options.http.authenticate)
            throw Error(ErrorCode::InvalidArgument, "Invalid local application identity or authentication override");
        options.directory = checkedDirectory(options.directory.isEmpty() ? localApplicationsDirectory() : options.directory);
    }
    bool listen() {
        if (http) { error = "Local application server is already listening"; return false; }
        error.clear();
#if defined(Q_OS_UNIX) && !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
        const auto path = QFile::encodeName(options.directory);
        if (!QDir().mkpath(QFileInfo(options.directory).absolutePath())) {
            error = "Cannot create the local application registry parent"; return false;
        }
        if (::mkdir(path.constData(), 0700) && errno != EEXIST) {
            error = "Cannot create the local application registry"; return false;
        }
        ErrorCode code = ErrorCode::None;
        directory = std::make_unique<Descriptor>(openDirectory(options.directory, code));
        if (directory->value < 0) { error = "Local application registry is not a private owned directory"; directory.reset(); return false; }
        instance = QUuid::createUuid().toString(QUuid::WithoutBraces);
        name = mcp::serverName(identity, instance);
        QByteArray random(32, Qt::Uninitialized);
        for (int i = 0; i < 4; ++i) {
            const auto word = QRandomGenerator::system()->generate64();
            std::memcpy(random.data() + i * sizeof(word), &word, sizeof(word));
        }
        const auto token = random.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        const auto digest = QCryptographicHash::hash(token, QCryptographicHash::Sha256);
        auto httpOptions = options.http;
        httpOptions.authenticate = [digest, principal = identity.id](const QByteArray& value) {
            const auto supplied = QCryptographicHash::hash(value, QCryptographicHash::Sha256);
            unsigned char difference = 0;
            for (qsizetype i = 0; i < digest.size(); ++i)
                difference |= static_cast<unsigned char>(digest[i]) ^ static_cast<unsigned char>(supplied[i]);
            return difference ? QString() : principal;
        };
        auto serverOptions = server;
        serverOptions.implementation = {{"name", identity.name}, {"version", identity.version}};
        http = std::make_unique<HttpServer>([serverOptions](const QString&) { return serverOptions; }, std::move(httpOptions));
        if (!http->listen()) { error = http->errorString(); http.reset(); directory.reset(); return false; }
        const auto object = QJsonObject{{"schema", "iisacc.mcp-app/1"}, {"app_id", identity.id},
            {"name", identity.name}, {"version", identity.version}, {"instance_id", instance},
            {"pid", QCoreApplication::applicationPid()}, {"endpoint", http->endpoint().toString()},
            {"bearer_token", QString::fromLatin1(token)}};
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        const auto temporary = ('.' + instance + ".tmp").toLatin1();
        const auto published = (instance + ".json").toLatin1();
        Descriptor file(openat(directory->value, temporary.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        bool ok = file.value >= 0; qsizetype offset = 0;
        while (ok && offset < bytes.size()) {
            const auto count = ::write(file.value, bytes.constData() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { ok = false; break; }
            offset += count;
        }
        struct stat info{};
        ok = ok && !fsync(file.value) && !fstat(file.value, &info)
            && !linkat(directory->value, temporary.constData(), directory->value, published.constData(), 0);
        if (file.value >= 0) unlinkat(directory->value, temporary.constData(), 0);
        if (!ok) { error = "Cannot publish the local application endpoint"; close(); return false; }
        recordInode = info.st_ino;
        registration = QDir(options.directory).filePath(QString::fromLatin1(published));
        return true;
#else
        error = "Private local application discovery is not implemented on this platform";
        return false;
#endif
    }
    void close() {
#if defined(Q_OS_UNIX) && !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
        if (directory && !registration.isEmpty()) {
            const auto file = (instance + ".json").toLatin1(); struct stat info{};
            if (!fstatat(directory->value, file.constData(), &info, AT_SYMLINK_NOFOLLOW)
                && info.st_ino == recordInode) unlinkat(directory->value, file.constData(), 0);
        }
        registration.clear(); directory.reset(); recordInode = 0;
#endif
        if (http) http->close();
        http.reset();
    }
};
LocalApplicationServer::LocalApplicationServer(LocalApplicationIdentity id, ServerOptions server, LocalApplicationServerOptions options)
    : d(std::make_unique<Impl>(std::move(id), std::move(server), std::move(options))) {}
LocalApplicationServer::~LocalApplicationServer() { d->close(); }
bool LocalApplicationServer::listen() { return d->listen(); }
QString LocalApplicationServer::errorString() const { return d->error; }
QString LocalApplicationServer::registrationPath() const { return d->registration; }
QString LocalApplicationServer::serverName() const { return d->name; }
QUrl LocalApplicationServer::endpoint() const { return d->http ? d->http->endpoint() : QUrl(); }
void LocalApplicationServer::close() { d->close(); }
}
