#include "SessionStore.h"
#include "ProtocolState.h"
#include "Compaction.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QLockFile>
#include <QtCore/QUuid>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>

namespace iiLocalLLM::agent {
namespace {
void storage(bool condition, const QString& message) { if (!condition) throw Error(ErrorCode::StorageFailure, message); }
QByteArray line(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n'; }
QJsonObject object(const QByteArray& bytes) {
    QJsonParseError error; auto doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) throw Error(ErrorCode::ProtocolError, "Corrupt transcript JSON record");
    return doc.object();
}
QString safeId(const QString& id) {
    static const QRegularExpression uuid("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    if (!uuid.match(id).hasMatch()) throw Error(ErrorCode::InvalidArgument, "Invalid agent session ID");
    return id;
}
Session publish(const QString& root, qint64 maximum, Session value) {
    const auto header = line({{"type", "session"}, {"version", 2}, {"id", value.id}, {"model", value.model},
        {"system_prompt", value.systemPrompt}, {"working_directory", value.workingDirectory}});
    if (header.size() > 4 * 1024 * 1024 || header.size() > maximum)
        throw Error(ErrorCode::ResourceLimit, "Agent transcript header exceeds limit");
    const auto directory = QDir(root).filePath(value.id);
    storage(QDir().mkdir(directory), "Cannot create agent session directory");
    struct DirectoryGuard { QString path; bool keep = false; ~DirectoryGuard() { if (!keep) QDir().rmdir(path); } } guard{directory};
    QSaveFile file(QDir(directory).filePath("transcript.jsonl"));
    storage(file.open(QIODevice::WriteOnly) && file.write(header) == header.size(), "Cannot create agent transcript");
    detail::ProtocolState state; QString parent; qint64 size = header.size();
    Session prefix = value; prefix.messages.clear(); prefix.compactions.clear(); qsizetype checkpoint = 0;
    for (const auto& message : value.messages) {
        state.accept(message);
        const auto bytes = line({{"type", "message"}, {"parent_id", parent}, {"message", toJson(message)}});
        if (bytes.size() > 4 * 1024 * 1024 || size > maximum - bytes.size())
            throw Error(ErrorCode::ResourceLimit, "Forked transcript exceeds limit");
        storage(file.write(bytes) == bytes.size(), "Cannot write forked transcript");
        size += bytes.size(); parent = message.id; prefix.messages.append(message);
        while (checkpoint < value.compactions.size() && value.compactions[checkpoint].atMessageId == message.id) {
            const auto& c = value.compactions[checkpoint++]; detail::validateCompaction(prefix, c);
            const auto record = line({{"type", "compaction"}, {"checkpoint", toJson(c)}});
            if (record.size() > 4 * 1024 * 1024 || size > maximum - record.size())
                throw Error(ErrorCode::ResourceLimit, "Forked compaction record exceeds limit");
            storage(file.write(record) == record.size(), "Cannot write forked compaction");
            size += record.size(); prefix.compactions.append(c);
        }
    }
    if (checkpoint != value.compactions.size()) throw Error(ErrorCode::ProtocolError, "Fork has an invalid compaction boundary");
    storage(file.commit(), "Cannot publish agent transcript"); guard.keep = true; return value;
}
}
class SessionLease::Impl {
public:
    Session value;
    QString directory;
    std::unique_ptr<QLockFile> lock;
    QFile file;
    qint64 maximum = 0;
    detail::ProtocolState state;
    int version = 2;
};
SessionLease::SessionLease(std::unique_ptr<Impl> impl) : d(std::move(impl)) {}
SessionLease::~SessionLease() = default;
const Session& SessionLease::session() const { return d->value; }
QString SessionLease::artifactsDirectory() const { return QDir(d->directory).filePath("artifacts"); }
void SessionLease::compact(Compaction c) {
    detail::validateCompaction(d->value, c);
    const auto bytes = line({{"type", "compaction"}, {"checkpoint", toJson(c)}});
    if (bytes.size() > 4 * 1024 * 1024 || d->file.size() > d->maximum - bytes.size())
        throw Error(ErrorCode::ResourceLimit, "Compaction transcript size limit exceeded");
    if (d->version == 1) {
        storage(d->file.seek(0), "Cannot seek legacy transcript");
        auto header = object(d->file.readLine()); header["version"] = 2;
        const auto remaining = d->file.readAll();
        QSaveFile replacement(d->file.fileName());
        storage(replacement.open(QIODevice::WriteOnly), "Cannot migrate legacy transcript");
        const auto first = line(header);
        storage(replacement.write(first) == first.size() && replacement.write(remaining) == remaining.size()
            && replacement.commit(), "Cannot atomically migrate legacy transcript");
        d->file.close(); storage(d->file.open(QIODevice::ReadWrite), "Cannot reopen migrated transcript"); d->version = 2;
    }
    storage(d->file.seek(d->file.size()), "Cannot seek compaction transcript");
    const auto originalSize = d->file.size();
    if (d->file.write(bytes) != bytes.size() || !d->file.flush()) {
        (void)d->file.resize(originalSize);
        throw Error(ErrorCode::StorageFailure, "Cannot append compaction checkpoint");
    }
    d->value.compactions.append(std::move(c));
}
void SessionLease::append(Message message) {
    if (message.id.isEmpty()) message.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    d->state.check(message);
    if (message.id.startsWith("iilocal.compaction:")) throw Error(ErrorCode::InvalidArgument, "Message ID uses the reserved compaction prefix");
    const auto parent = d->value.messages.isEmpty() ? QString() : d->value.messages.back().id;
    const auto bytes = line({{"type", "message"}, {"parent_id", parent}, {"message", toJson(message)}});
    if (bytes.size() > 4 * 1024 * 1024 || d->file.size() > d->maximum - bytes.size())
        throw Error(ErrorCode::ResourceLimit, "Agent transcript size limit exceeded");
    storage(d->file.seek(d->file.size()), "Cannot seek agent transcript");
    const auto originalSize = d->file.size();
    if (d->file.write(bytes) != bytes.size() || !d->file.flush()) {
        (void)d->file.resize(originalSize);
        throw Error(ErrorCode::StorageFailure, "Cannot append agent transcript");
    }
    d->state.accept(message);
    d->value.messages.append(std::move(message));
}
SessionStore::SessionStore(QString directory, qint64 maximum) : maxTranscriptBytes_(maximum) {
    if (directory.trimmed().isEmpty() || maximum < 1024) throw Error(ErrorCode::InvalidArgument, "Invalid agent transcript store");
    storage(QDir().mkpath(directory), "Cannot create agent sessions directory");
    directory_ = QFileInfo(directory).canonicalFilePath();
    storage(!directory_.isEmpty(), "Cannot resolve agent sessions directory");
}
Session SessionStore::create(QString model, QString prompt, QString workingDirectory) const {
    const auto workspace = QFileInfo(workingDirectory).canonicalFilePath();
    if (model.trimmed().isEmpty() || workspace.isEmpty() || !QFileInfo(workspace).isDir())
        throw Error(ErrorCode::InvalidArgument, "Model and existing workspace are required");
    Session value{QUuid::createUuid().toString(QUuid::WithoutBraces), std::move(model), std::move(prompt), workspace, {}};
    return publish(directory_, maxTranscriptBytes_, std::move(value));
}
Session SessionStore::fork(const QString& id, const QString& throughMessageId) const {
    const auto source = acquire(id); auto value = source->session();
    if (!QDir(source->artifactsDirectory()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty())
        throw Error(ErrorCode::RuntimeUnavailable, "Forking sessions with artifacts requires artifact cloning, which is not yet supported");
    if (!throughMessageId.isEmpty()) {
        qsizetype end = 0;
        while (end < value.messages.size() && value.messages[end].id != throughMessageId) ++end;
        if (end == value.messages.size()) throw Error(ErrorCode::NotFound, "Fork message was not found");
        value.messages = value.messages.first(end + 1);
        QSet<QString> kept; for (const auto& message : value.messages) kept.insert(message.id);
        value.compactions.removeIf([&](const Compaction& c) { return !kept.contains(c.atMessageId); });
    }
    if (!pendingToolCalls(value.messages).isEmpty())
        throw Error(ErrorCode::InvalidArgument, "Cannot fork across an unresolved tool call");
    value.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return publish(directory_, maxTranscriptBytes_, std::move(value));
}
std::unique_ptr<SessionLease> SessionStore::acquire(const QString& id) const {
    const auto directory = QDir(directory_).filePath(safeId(id));
    const QFileInfo info(directory);
    if (!info.isDir()) throw Error(ErrorCode::NotFound, "Agent session not found");
    storage(!info.isSymLink() && info.canonicalFilePath() == directory, "Agent session directory must not be a symlink");
    auto d = std::make_unique<SessionLease::Impl>(); d->directory = directory; d->maximum = maxTranscriptBytes_;
    d->lock = std::make_unique<QLockFile>(QDir(directory).filePath("session.lock"));
    d->lock->setStaleLockTime(0);
    if (!d->lock->tryLock(0)) throw Error(ErrorCode::ModelInUse, "Agent session is already in use");
    const auto path = QDir(directory).filePath("transcript.jsonl");
    storage(!QFileInfo(path).isSymLink(), "Transcript must not be a symlink");
    d->file.setFileName(path);
    storage(d->file.open(QIODevice::ReadWrite), "Cannot open agent transcript");
    if (d->file.size() > maxTranscriptBytes_) throw Error(ErrorCode::ResourceLimit, "Agent transcript exceeds configured limit");
    const auto headerLine = d->file.readLine(4 * 1024 * 1024 + 1);
    if (!headerLine.endsWith('\n')) throw Error(ErrorCode::ProtocolError, "Missing complete transcript header");
    const auto header = object(headerLine);
    if (header["type"] != "session" || (header["version"] != 1 && header["version"] != 2) || header["id"] != id
        || !header["model"].isString() || header["model"].toString().isEmpty()
        || !header["system_prompt"].isString() || !header["working_directory"].isString())
        throw Error(ErrorCode::ProtocolError, "Invalid agent transcript header");
    d->version = header["version"].toInt();
    d->value = {id, header["model"].toString(), header["system_prompt"].toString(), header["working_directory"].toString(), {}};
    while (!d->file.atEnd()) {
        const auto start = d->file.pos();
        const auto bytes = d->file.readLine(4 * 1024 * 1024 + 1);
        if (!bytes.endsWith('\n')) {
            if (!d->file.atEnd()) throw Error(ErrorCode::ResourceLimit, "Agent transcript record exceeds 4 MiB");
            storage(d->file.resize(start), "Cannot discard interrupted transcript tail");
            break;
        }
        if (bytes.size() > 4 * 1024 * 1024) throw Error(ErrorCode::ResourceLimit, "Agent transcript record exceeds 4 MiB");
        const auto record = object(bytes);
        if (d->version == 2 && record["type"] == "compaction") {
            if (!record["checkpoint"].isObject()) throw Error(ErrorCode::ProtocolError, "Missing compaction checkpoint");
            auto c = compactionFromJson(record["checkpoint"].toObject());
            detail::validateCompaction(d->value, c); d->value.compactions.append(std::move(c)); continue;
        }
        const auto expectedParent = d->value.messages.isEmpty() ? QString() : d->value.messages.back().id;
        if (record["type"] != "message" || !record["message"].isObject() || record["parent_id"] != expectedParent)
            throw Error(ErrorCode::ProtocolError, "Invalid transcript chain");
        auto message = messageFromJson(record["message"].toObject());
        if (message.id.startsWith("iilocal.compaction:")) throw Error(ErrorCode::ProtocolError, "Reserved transcript message ID");
        d->state.accept(message); d->value.messages.append(std::move(message));
    }
    return std::unique_ptr<SessionLease>(new SessionLease(std::move(d)));
}
Session SessionStore::load(const QString& id) const { return acquire(id)->session(); }
QStringList SessionStore::list() const {
    QStringList result;
    for (const auto& name : QDir(directory_).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        try { safeId(name); if (QFileInfo::exists(QDir(directory_).filePath(name + "/transcript.jsonl"))) result.append(name); }
        catch (const Error&) {}
    }
    return result;
}
}
