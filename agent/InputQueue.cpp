#include "InputQueue.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QThread>
#include <QtCore/QUuid>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace iiLocalLLM::agent {
namespace {
constexpr qint64 maximum = 9007199254740991LL;
const auto filePermissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
const auto directoryPermissions = filePermissions | QFileDevice::ExeOwner;
void require(bool ok, const QString& text, ErrorCode code = ErrorCode::InvalidArgument) { if (!ok) throw Error(code, text); }
bool validId(const QString& id) {
    static const QRegularExpression pattern("\\A[A-Za-z0-9][A-Za-z0-9_.-]{0,127}\\z");
    return pattern.match(id).hasMatch();
}
int priority(const QJsonObject& input) {
    const auto p = input["priority"].toString(); return p == "now" ? 0 : p == "next" ? 1 : 2;
}
QJsonObject normalized(QJsonObject input, const InputQueueOptions& o) {
    const QSet<QString> allowed{"text", "priority", "kind", "context_paths"};
    for (auto it = input.begin(); it != input.end(); ++it) require(allowed.contains(it.key()), "Unknown queued input field: " + it.key());
    require(input["text"].isString() && !input["text"].toString().trimmed().isEmpty()
        && input["text"].toString().size() <= o.maxTextCharacters, "Invalid queued input text");
    if (!input.contains("kind")) input["kind"] = "prompt";
    require(input["kind"] == "prompt" || input["kind"] == "notification", "Invalid queued input kind");
    if (!input.contains("priority")) input["priority"] = input["kind"] == "notification" ? "later" : "next";
    require(input["priority"] == "now" || input["priority"] == "next" || input["priority"] == "later", "Invalid queued input priority");
    if (!input.contains("context_paths")) input["context_paths"] = QJsonArray{};
    require(input["context_paths"].isArray() && input["context_paths"].toArray().size() <= 128, "Invalid queued context paths");
    for (const auto& p : input["context_paths"].toArray())
        require(p.isString() && !p.toString().isEmpty() && p.toString().size() <= 4096, "Invalid queued context path");
    return input;
}
struct State {
    qint64 revision = 0, nextSequence = 1;
    QList<QJsonObject> inputs;
    QList<QJsonObject> ordered() const {
        auto sorted = inputs;
        std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return priority(a) < priority(b); });
        return sorted;
    }
};
class LockedState {
public:
    QString path, session;
    QLockFile lock;
    InputQueueOptions options;
    static QString sessionPath(const QString& root, const QString& id) {
        require(validId(id), "Invalid input queue session ID"); const auto path = QDir(root).filePath(id);
        require(!QFileInfo(path).isSymLink() && QDir().mkpath(path) && QFileInfo(path).canonicalFilePath() == path
            && QFile::setPermissions(path, directoryPermissions), "Cannot open private input queue", ErrorCode::StorageFailure);
        require(!QFileInfo(QDir(path).filePath("queue.lock")).isSymLink(), "Input queue lock is a symlink", ErrorCode::StorageFailure);
        return path;
    }
    LockedState(const QString& root, const QString& id, InputQueueOptions o, const CancellationToken& token,
        const QString& lockName = "queue.lock")
        : path(sessionPath(root, id)), session(id), lock(QDir(path).filePath(lockName)), options(o) {
        require(!QFileInfo(QDir(path).filePath(lockName)).isSymLink(),"Input queue lock is a symlink",ErrorCode::StorageFailure);
        token.throwIfCancelled(); lock.setStaleLockTime(0); const auto start = std::chrono::steady_clock::now();
        while (!lock.tryLock(0)) {
            token.throwIfCancelled(); require(lock.error() == QLockFile::LockFailedError, "Cannot lock input queue", ErrorCode::StorageFailure);
            require(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(o.lockTimeoutMs), "Input queue lock timed out", ErrorCode::Timeout);
            QThread::msleep(5);
        }
        token.throwIfCancelled();
    }
    State read() const {
        State state; const auto name = QDir(path).filePath("queue.json"); const QFileInfo info(name);
        require(!info.isSymLink(), "Input queue is a symlink", ErrorCode::StorageFailure);
        if (!info.exists()) return state;
        require(info.isFile(), "Input queue is not a regular file", ErrorCode::StorageFailure);
        QFile file(name); require(file.open(QIODevice::ReadOnly), "Cannot read input queue", ErrorCode::StorageFailure);
        const auto bytes = file.read(options.maxBytes + 1);
        require(bytes.size() <= options.maxBytes && file.atEnd(), "Input queue byte limit exceeded", ErrorCode::ResourceLimit);
        QJsonParseError error; const auto doc = QJsonDocument::fromJson(bytes, &error); const auto data = doc.object();
        require(error.error == QJsonParseError::NoError && doc.isObject() && data.size() == 5
            && data["schema"] == "iisacc.agent.inputs/1" && data["session_id"] == session && data["inputs"].isArray(),
            "Corrupt input queue", ErrorCode::ProtocolError);
        auto number = [&](const char* key, qint64 minimum) {
            const auto v = data[key]; require(v.isDouble() && v.toDouble() >= minimum && v.toDouble() <= maximum
                && v.toDouble() == std::floor(v.toDouble()), "Corrupt queue sequence", ErrorCode::ProtocolError); return qint64(v.toDouble());
        };
        state.revision = number("revision", 0); state.nextSequence = number("next_sequence", 1);
        // schema, session_id, revision, next_sequence, inputs
        require(data["inputs"].toArray().size() <= options.maxPending, "Input queue capacity exceeded", ErrorCode::ResourceLimit);
        QSet<QString> ids; qint64 previous = 0;
        for (const auto& v : data["inputs"].toArray()) {
            auto item = v.toObject(); const auto id = item.take("id"); const auto seq = item.take("sequence");
            require(v.isObject() && validId(id.toString()) && id.isString() && !ids.contains(id.toString()) && seq.isDouble()
                && seq.toDouble() > previous && seq.toDouble() < state.nextSequence && seq.toDouble() == std::floor(seq.toDouble()),
                "Corrupt queued input identity", ErrorCode::ProtocolError);
            try { require(normalized(item, options) == item, "Incomplete queued input"); }
            catch (const Error&) { throw Error(ErrorCode::ProtocolError, "Corrupt queued input fields"); }
            previous = qint64(seq.toDouble()); ids.insert(id.toString()); state.inputs.append(v.toObject());
        }
        return state;
    }
    void write(State& state, const CancellationToken& token) const {
        require(state.revision < maximum && state.nextSequence <= maximum, "Input queue sequence exhausted", ErrorCode::ResourceLimit);
        QJsonArray items; for (const auto& input : state.inputs) items.append(input);
        const QJsonObject data{{"schema", "iisacc.agent.inputs/1"}, {"session_id", session}, {"revision", double(state.revision + 1)},
            {"next_sequence", double(state.nextSequence)}, {"inputs", items}};
        const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact) + '\n';
        require(bytes.size() <= options.maxBytes, "Input queue byte limit exceeded", ErrorCode::ResourceLimit);
        QSaveFile file(QDir(path).filePath("queue.json")); file.setDirectWriteFallback(false);
        require(!QFileInfo(file.fileName()).isSymLink(), "Input queue is a symlink", ErrorCode::StorageFailure);
        require(file.open(QIODevice::WriteOnly) && file.setPermissions(filePermissions) && file.write(bytes) == bytes.size(),
            "Cannot write input queue", ErrorCode::StorageFailure);
        token.throwIfCancelled(); require(file.commit(), "Cannot publish input queue", ErrorCode::StorageFailure); ++state.revision;
    }
};
}
InputQueue::InputQueue(QString directory, InputQueueOptions options) : options_(options) {
    require(!directory.trimmed().isEmpty() && options.maxPending >= 1 && options.maxPending <= 10000
        && options.maxTextCharacters >= 1 && options.maxTextCharacters <= 1048576 && options.maxBytes >= 1024
        && options.maxBytes <= 64 * 1024 * 1024 && options.lockTimeoutMs >= 1 && options.lockTimeoutMs <= 60000, "Invalid input queue configuration");
    require(QDir().mkpath(directory), "Cannot create input queue root", ErrorCode::StorageFailure);
    directory_ = QFileInfo(directory).canonicalFilePath();
    require(!directory_.isEmpty() && QFile::setPermissions(directory_, directoryPermissions), "Cannot protect input queue root", ErrorCode::StorageFailure);
}
QJsonObject InputQueue::enqueue(const QString& id, const QJsonObject& input, const CancellationToken& token) const {
    return enqueueIdentified(id,QUuid::createUuid().toString(QUuid::WithoutBraces),input,token);
}
QJsonObject InputQueue::enqueueIdentified(const QString& id,const QString& inputId,const QJsonObject& input,const CancellationToken& token)const {
    require(validId(inputId),"Invalid producer input identity");
    token.throwIfCancelled(); auto item = normalized(input, options_); LockedState file(directory_, id, options_, token); auto state = file.read();
    for(const auto& existing:state.inputs)if(existing["id"]==inputId) {
        auto payload=existing;payload.remove("id");payload.remove("sequence");
        require(payload==item,"Producer input identity has a conflicting payload",ErrorCode::AlreadyExists);
        return {{"input",existing},{"revision",double(state.revision)}};
    }
    require(state.inputs.size() < options_.maxPending, "Input queue is full", ErrorCode::QueueFull);
    require(state.nextSequence < maximum, "Input queue sequence exhausted", ErrorCode::ResourceLimit);
    item["id"] = inputId; item["sequence"] = double(state.nextSequence++);
    state.inputs.append(item); file.write(state, token); return {{"input", item}, {"revision", double(state.revision)}};
}
QJsonObject InputQueue::snapshot(const QString& id, int offset, int limit, const CancellationToken& token) const {
    require(offset >= 0 && limit >= 1 && limit <= 1000, "Invalid input queue page"); LockedState file(directory_, id, options_, token);
    const auto state = file.read(); const auto ordered = state.ordered(); QJsonArray items;
    for (qsizetype n = offset; n < ordered.size() && items.size() < limit; ++n) items.append(ordered[n]);
    QJsonObject result{{"revision", double(state.revision)}, {"count", ordered.size()}, {"inputs", items}};
    if (qsizetype(offset) + items.size() < ordered.size()) result["next_offset"] = offset + items.size();
    return result;
}
QJsonObject InputQueue::remove(const QString& id, const QString& inputId, const CancellationToken& token) const {
    require(validId(inputId), "Invalid queued input ID"); LockedState file(directory_, id, options_, token); auto state = file.read();
    const auto old = state.inputs.size(); state.inputs.removeIf([&](const auto& input) { return input["id"] == inputId; });
    require(state.inputs.size() != old, "Queued input was not found", ErrorCode::NotFound); file.write(state, token);
    return {{"removed", true}, {"input_id", inputId}, {"revision", double(state.revision)}};
}
int InputQueue::deliver(const QString& id, bool includeLater, int limit,
    const std::function<void(const QJsonObject&)>& persist, const CancellationToken& token) const {
    require(bool(persist),"Invalid input delivery callback");
    return deliver(id,includeLater,limit,[](const QJsonObject&){return QJsonObject{};},
        [&](const QJsonObject& input,const QJsonObject&){persist(input);return true;},token);
}
int InputQueue::transferNotifications(const QString& from,const QString& to,const QStringList& inputIds,const CancellationToken& token,QStringList* pendingAtDestination) const {
    require(validId(from)&&validId(to)&&inputIds.size()<=10000,"Invalid notification transfer");
    QSet<QString> ids;for(const auto& id:inputIds){require(validId(id),"Invalid notification identity");ids.insert(id);}
    if(pendingAtDestination)pendingAtDestination->clear();
    if(ids.isEmpty())return 0;
    if(from==to) {
        LockedState queue(directory_,from,options_,token);
        for(const auto& input:queue.read().inputs)if(ids.contains(input["id"].toString())) {
            require(input["kind"]=="notification","Only notification identities can be queried");
            if(pendingAtDestination)pendingAtDestination->append(input["id"].toString());
        }
        return 0;
    }
    const auto first=std::min(from,to),second=std::max(from,to);
    LockedState firstDelivery(directory_,first,options_,token,"delivery.lock"),secondDelivery(directory_,second,options_,token,"delivery.lock");
    LockedState firstQueue(directory_,first,options_,token),secondQueue(directory_,second,options_,token);
    auto& source=from==first?firstQueue:secondQueue;auto& destination=to==first?firstQueue:secondQueue;
    auto previous=source.read(),next=destination.read();int moved=0;bool inserted=false;
    auto identity=[](QJsonObject value){value.remove("sequence");return value;};
    for(auto input:previous.inputs)if(ids.contains(input["id"].toString())) {
        require(input["kind"]=="notification","Only notifications can follow a session clear");
        const auto present=std::find_if(next.inputs.begin(),next.inputs.end(),[&](const auto& item){return item["id"]==input["id"];});
        if(present!=next.inputs.end())require(identity(*present)==identity(input),"Conflicting notification transfer",ErrorCode::ProtocolError);
        else {
            require(next.inputs.size()<options_.maxPending,"Destination input queue is full",ErrorCode::QueueFull);
            require(next.nextSequence<maximum,"Input queue sequence exhausted",ErrorCode::ResourceLimit);
            input["sequence"]=double(next.nextSequence++);next.inputs.append(input);inserted=true;
        }
        ++moved;
    }
    if(pendingAtDestination)for(const auto& input:next.inputs)if(ids.contains(input["id"].toString())) {
        require(input["kind"]=="notification","Transferred identity is not a notification",ErrorCode::ProtocolError);
        pendingAtDestination->append(input["id"].toString());
    }
    if(!moved)return 0;
    token.throwIfCancelled();if(inserted)destination.write(next,token);
    previous.inputs.removeIf([&](const auto& input){return ids.contains(input["id"].toString());});
    source.write(previous,{});return moved;
}
int InputQueue::deliver(const QString& id,bool includeLater,int limit,
    const std::function<QJsonObject(const QJsonObject&)>& prepare,
    const std::function<bool(const QJsonObject&,const QJsonObject&)>& persist,const CancellationToken& token) const {
    require(bool(prepare)&&bool(persist)&&limit>=1&&limit<=256,"Invalid input delivery request");
    LockedState delivery(directory_,id,options_,token,"delivery.lock");
    QList<QJsonObject> selected;
    {LockedState file(directory_,id,options_,token);selected=file.read().ordered();}
    int delivered=0,attempted=0;QString kind;
    for(const auto& input:selected) {
        if ((!includeLater && priority(input) == 2) || attempted == limit) break;
        if (kind.isEmpty()) kind = input["kind"].toString();
        if (input["kind"] != kind) continue;
        ++attempted;token.throwIfCancelled();const auto prepared=prepare(input);token.throwIfCancelled();
        LockedState file(directory_,id,options_,token);auto state=file.read();
        const auto current=std::find_if(state.inputs.begin(),state.inputs.end(),[&](const auto& item){return item["id"]==input["id"];});
        if(current==state.inputs.end())continue;
        require(*current==input,"Queued input changed during preparation",ErrorCode::ProtocolError);
        token.throwIfCancelled();const auto proceed=persist(input,prepared);
        state.inputs.removeIf([&](const auto& item) { return item["id"] == input["id"]; });
        // After persistence the acknowledgement is not cancelled midway; a failed
        // write leaves the durable input pending for ID-based transcript recovery.
        file.write(state, {}); ++delivered;
        if(!proceed)break;
    }
    return delivered;
}
}
