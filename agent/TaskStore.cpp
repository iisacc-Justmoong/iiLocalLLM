#include "TaskStore.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QThread>
#include <QtCore/QSet>
#include <QtCore/QMap>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace iiLocalLLM::agent {
namespace {
constexpr qint64 maxInteger = 9007199254740991LL;
const auto ownerFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
const auto ownerDirectory = ownerFile | QFileDevice::ExeOwner;
void require(bool ok, const QString& text, ErrorCode code = ErrorCode::InvalidArgument) {
    if (!ok) throw Error(code, text);
}
QJsonObject stringSchema(int maximum, int minimum = 0) {
    return {{"type", "string"}, {"minLength", minimum}, {"maxLength", maximum}};
}
QJsonObject integerSchema(int minimum, int maximum) { return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}}; }
QJsonObject schema(QJsonObject properties, QJsonArray required = {}) {
    return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
}
const QJsonObject& todoSchema() {
    static const auto value = schema({{"content", stringSchema(4096, 1)}, {"activeForm", stringSchema(1024, 1)},
        {"status", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"pending", "in_progress", "completed"}}}}},
        {"content", "activeForm", "status"});
    return value;
}
QJsonObject todosSchema() { return {{"type", "array"}, {"maxItems", 1000}, {"items", todoSchema()}}; }
QJsonObject revisionSchema() { return {{"type", "integer"}, {"minimum", 0}, {"maximum", double(maxInteger)}}; }
QJsonObject metadataSchema() { return {{"type", "object"}, {"maxProperties", 256}}; }
bool validId(const QString& id) {
    static const QRegularExpression pattern("\\A[A-Za-z0-9][A-Za-z0-9_.-]{0,127}\\z");
    return pattern.match(id).hasMatch();
}
bool validTaskId(const QString& id) {
    static const QRegularExpression pattern("\\A[1-9][0-9]{0,15}\\z");
    return pattern.match(id).hasMatch() && id.toLongLong() <= maxInteger;
}
bool validStatus(const QJsonValue& status) { return status == "pending" || status == "in_progress" || status == "completed"; }
qint64 integer(const QJsonValue& v) {
    require(v.isDouble() && v.toDouble() >= 0 && v.toDouble() <= double(maxInteger)
        && v.toDouble() == std::floor(v.toDouble()), "Corrupt task integer", ErrorCode::ProtocolError);
    return qint64(v.toDouble());
}
QJsonArray unresolved(const QJsonObject& task, const QMap<QString, QJsonObject>& tasks) {
    QJsonArray out;
    for (const auto& id : task["blockedBy"].toArray()) if (tasks[id.toString()]["status"] != "completed") out.append(id);
    return out;
}
QJsonObject taskView(QJsonObject task, const QMap<QString, QJsonObject>& tasks) {
    task["unresolvedBlockedBy"] = unresolved(task, tasks); return task;
}
QJsonArray sorted(const QMap<QString, QJsonObject>& tasks) {
    auto keys = tasks.keys(); std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.toLongLong() < b.toLongLong(); });
    QJsonArray out; for (const auto& id : keys) out.append(tasks[id]); return out;
}
void validateGraph(const QMap<QString, QJsonObject>& tasks, ErrorCode code) {
    QMap<QString, int> incoming;
    for (auto it = tasks.begin(); it != tasks.end(); ++it) {
        incoming[it.key()] = it.value()["blockedBy"].toArray().size();
        for (const auto* field : {"blocks", "blockedBy"}) {
            QSet<QString> seen;
            for (const auto& v : it.value()[field].toArray()) {
                const auto id = v.toString();
                require(v.isString() && id != it.key() && tasks.contains(id) && !seen.contains(id), "Invalid task dependency", code);
                seen.insert(id);
                const auto reverse = QString::fromLatin1(field) == "blocks" ? "blockedBy" : "blocks";
                require(tasks[id][reverse].toArray().contains(it.key()), "Inconsistent task dependency", code);
            }
        }
    }
    QStringList ready; for (auto it = incoming.begin(); it != incoming.end(); ++it) if (!it.value()) ready.append(it.key());
    qsizetype visited = 0;
    while (!ready.isEmpty()) {
        const auto id = ready.takeLast(); ++visited;
        for (const auto& child : tasks[id]["blocks"].toArray()) if (--incoming[child.toString()] == 0) ready.append(child.toString());
    }
    require(visited == tasks.size(), "Task dependency cycle", code);
}
const ToolRegistry& validators() {
    static const auto registry = [] {
        auto r = std::make_unique<ToolRegistry>();
        for (auto d : taskToolDefinitions()) {
            Tool t; t.definition = std::move(d); t.execute = [](const auto&, const auto&) { return ToolResult{}; }; r->add(std::move(t));
        }
        return r;
    }();
    return *registry;
}
struct Board {
    QString listId;
    qint64 revision = 0, nextId = 1;
    QMap<QString, QJsonObject> tasks;
    QJsonArray todos;
    QJsonObject json() const { return {{"schema", "iisacc.agent.tasks/1"}, {"listId", listId}, {"revision", double(revision)},
        {"nextId", double(nextId)}, {"tasks", sorted(tasks)}, {"todos", todos}}; }
};
QString listDirectory(const QString& root, const QString& list) {
    require(validId(list), "Invalid task list ID");
    const auto path = QDir(root).filePath(list);
    require(!QFileInfo(path).isSymLink(), "Task list must not be a symlink", ErrorCode::StorageFailure);
    require(QDir().mkpath(path) && QFileInfo(path).canonicalFilePath() == path
        && QFile::setPermissions(path, ownerDirectory), "Cannot open private task list", ErrorCode::StorageFailure);
    return path;
}
class LockedBoard {
public:
    QString path;
    QLockFile lock;
    LockedBoard(const QString& root, const QString& list, const TaskStoreOptions& o, const CancellationToken& token)
        : path(listDirectory(root, list)), lock(QDir(path).filePath("board.lock")) {
        token.throwIfCancelled();
        require(!QFileInfo(QDir(path).filePath("board.lock")).isSymLink(), "Task lock must not be a symlink", ErrorCode::StorageFailure);
        lock.setStaleLockTime(0); const auto start = std::chrono::steady_clock::now();
        while (!lock.tryLock(0)) {
            token.throwIfCancelled();
            require(lock.error() == QLockFile::LockFailedError, "Cannot lock task list", ErrorCode::StorageFailure);
            require(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(o.lockTimeoutMs), "Task list lock timed out", ErrorCode::Timeout);
            QThread::msleep(10);
        }
        token.throwIfCancelled();
    }
    Board read(const QString& list, const TaskStoreOptions& options) {
        Board b; b.listId = list; const auto filePath = QDir(path).filePath("board.json"); const QFileInfo info(filePath);
        require(!info.isSymLink(), "Task state must not be a symlink", ErrorCode::StorageFailure);
        if (!info.exists()) return b;
        require(info.isFile(), "Task state is not a regular file", ErrorCode::StorageFailure);
        QFile file(filePath); require(file.open(QIODevice::ReadOnly), "Cannot read task state", ErrorCode::StorageFailure);
        const auto bytes = file.read(options.maxBytes + 1);
        require(bytes.size() <= options.maxBytes && file.atEnd(), "Task state exceeds limit", ErrorCode::ResourceLimit);
        QJsonParseError error; const auto doc = QJsonDocument::fromJson(bytes, &error); const auto o = doc.object();
        require(error.error == QJsonParseError::NoError && doc.isObject() && o.size() == 6 && o["schema"] == "iisacc.agent.tasks/1"
            && o["listId"] == list && o["tasks"].isArray() && o["todos"].isArray(), "Corrupt task state", ErrorCode::ProtocolError);
        b.revision = integer(o["revision"]); b.nextId = integer(o["nextId"]);
        require(b.nextId >= 1 && o["tasks"].toArray().size() <= options.maxTasks && o["todos"].toArray().size() <= options.maxTodos,
            "Task state exceeds configured capacity", ErrorCode::ResourceLimit);
        for (const auto& v : o["tasks"].toArray()) {
            const auto t = v.toObject(); const auto id = t["id"].toString();
            require(v.isObject() && t.size() == 9 && validTaskId(id) && id.toLongLong() < b.nextId && !b.tasks.contains(id)
                && validStatus(t["status"]) && t["blocks"].isArray() && t["blockedBy"].isArray()
                && t["owner"].isString() && t["activeForm"].isString()
                && t["metadata"].isObject(), "Corrupt task record", ErrorCode::ProtocolError);
            try {
                validators().validateInput("TaskCreate", {{"subject", t["subject"]}, {"description", t["description"]},
                    {"activeForm", t["activeForm"]}, {"metadata", t["metadata"]}});
                validators().validateInput("TaskUpdate", {{"taskId", id}, {"owner", t["owner"]}});
            }
            catch (const Error&) { throw Error(ErrorCode::ProtocolError, "Corrupt task fields"); }
            require(!t["subject"].toString().trimmed().isEmpty() && QJsonDocument(t["metadata"].toObject()).toJson(QJsonDocument::Compact).size() <= 65536,
                "Corrupt task metadata or subject", ErrorCode::ProtocolError);
            b.tasks.insert(id, t);
        }
        b.todos = o["todos"].toArray();
        try { validators().validateInput("TodoWrite", {{"todos", b.todos}}); }
        catch (const Error&) { throw Error(ErrorCode::ProtocolError, "Corrupt todo list"); }
        validateGraph(b.tasks, ErrorCode::ProtocolError); return b;
    }
    void write(const Board& board, const TaskStoreOptions& options, const CancellationToken& token) {
        const auto bytes = QJsonDocument(board.json()).toJson(QJsonDocument::Compact) + '\n';
        require(bytes.size() <= options.maxBytes, "Task state exceeds byte limit", ErrorCode::ResourceLimit);
        QSaveFile file(QDir(path).filePath("board.json")); file.setDirectWriteFallback(false);
        require(!QFileInfo(file.fileName()).isSymLink(), "Task state must not be a symlink", ErrorCode::StorageFailure);
        require(file.open(QIODevice::WriteOnly) && file.setPermissions(ownerFile) && file.write(bytes) == bytes.size(),
            "Cannot write task state", ErrorCode::StorageFailure);
        token.throwIfCancelled(); require(file.commit(), "Cannot publish task state", ErrorCode::StorageFailure);
    }
};
ToolResult result(QJsonObject data) {
    return {QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)), std::move(data)};
}
}
QList<ToolDefinition> taskToolDefinitions(bool deferred) {
    const auto id = QJsonObject{{"type", "string"}, {"pattern", "^[1-9][0-9]{0,15}$"}};
    const auto ids = QJsonObject{{"type", "array"}, {"maxItems", 1000}, {"uniqueItems", true}, {"items", id}};
    const auto status = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"pending", "in_progress", "completed", "deleted"}}};
    const auto page = QJsonObject{{"offset", integerSchema(0, 1000000)}, {"limit", integerSchema(1, 100)}};
    QList<ToolDefinition> definitions;
    auto add = [&](QString name, QString description, QJsonObject input, bool readOnly = false) {
        ToolDefinition d; d.name = std::move(name); d.description = std::move(description); d.inputSchema = std::move(input);
        d.readOnly = readOnly; d.concurrencySafe = true; d.deferred = deferred;
        d.metadata = {{"source", "builtin.task"}, {"search_hint", "tasks todo plan checklist dependencies owner"}};
        definitions.append(std::move(d));
    };
    add("TaskCreate", "Create a persistent task with a pending status. Task state tracks work; it does not execute the work.",
        schema({{"subject", stringSchema(1024, 1)}, {"description", stringSchema(65536)}, {"activeForm", stringSchema(1024)},
            {"metadata", metadataSchema()}, {"expectedRevision", revisionSchema()}}, {"subject", "description"}));
    add("TaskGet", "Read a task, its owner and all dependency links, including unresolved blockers.", schema({{"taskId", id}}, {"taskId"}), true);
    add("TaskList", "List tasks in numeric ID order. blockedBy contains unresolved blockers. Follow nextOffset for more tasks.", schema(page), true);
    add("TaskUpdate", "Update task fields, merge metadata (null removes a key), add or remove dependency links, or delete a task. "
        "A completion status is a recorded claim; verify the work before marking it completed.",
        schema({{"taskId", id}, {"subject", stringSchema(1024, 1)}, {"description", stringSchema(65536)}, {"activeForm", stringSchema(1024)},
            {"status", status}, {"owner", stringSchema(128)}, {"metadata", metadataSchema()}, {"expectedRevision", revisionSchema()},
            {"addBlocks", ids}, {"addBlockedBy", ids}, {"removeBlocks", ids}, {"removeBlockedBy", ids}}, {"taskId"}));
    add("TaskClaim", "Atomically claim an unfinished, unblocked task and mark it in_progress. Refuses another owner; optionally refuses when the owner has another open task.",
        schema({{"taskId", id}, {"owner", stringSchema(128, 1)}, {"checkOwnerBusy", QJsonObject{{"type", "boolean"}}},
            {"expectedRevision", revisionSchema()}}, {"taskId", "owner"}));
    add("TodoWrite", "Replace this session's entire checklist. Completed entries remain until explicitly removed. This records progress without performing the work.",
        schema({{"todos", todosSchema()}, {"expectedRevision", revisionSchema()}}, {"todos"}));
    add("TodoRead", "Read this session's current checklist. Follow nextOffset for more entries.", schema(page), true);
    const auto regularStatus = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"pending", "in_progress", "completed"}}};
    const auto summaryFields = QJsonObject{{"id", id}, {"subject", stringSchema(1024, 1)}, {"status", regularStatus},
        {"owner", stringSchema(128)}, {"blockedBy", ids}};
    auto taskFields = summaryFields;
    taskFields["description"] = stringSchema(65536); taskFields["activeForm"] = stringSchema(1024);
    taskFields["metadata"] = metadataSchema(); taskFields["blocks"] = ids; taskFields["unresolvedBlockedBy"] = ids;
    QJsonArray taskRequired; for (auto it = taskFields.begin(); it != taskFields.end(); ++it) taskRequired.append(it.key());
    const auto task = schema(taskFields, taskRequired);
    for (auto& d : definitions) {
        QJsonObject fields{{"revision", revisionSchema()}}; QJsonArray required{"revision"};
        if (d.name == "TaskCreate" || d.name == "TaskGet" || d.name == "TaskClaim") {
            fields["task"] = task; required.append("task");
            if (d.name == "TaskClaim") {
                fields["success"] = QJsonObject{{"type", "boolean"}}; required.append("success");
                fields["reason"] = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"already_claimed", "completed", "blocked", "owner_busy"}}};
            }
        } else if (d.name == "TaskUpdate") {
            d.outputSchema = {{"oneOf", QJsonArray{
                schema({{"revision", revisionSchema()}, {"task", task}}, {"revision", "task"}),
                schema({{"revision", revisionSchema()}, {"deleted", QJsonObject{{"const", true}}}, {"taskId", id}}, {"revision", "deleted", "taskId"})}}};
            continue;
        } else if (d.name == "TodoWrite") {
            fields["oldTodos"] = todosSchema(); fields["newTodos"] = todosSchema(); required.append("oldTodos"); required.append("newTodos");
        } else {
            const auto name = d.name == "TaskList" ? "tasks" : "todos";
            const auto item = d.name == "TaskList" ? schema(summaryFields, {"id", "subject", "status", "owner", "blockedBy"}) : todoSchema();
            fields[name] = QJsonObject{{"type", "array"}, {"maxItems", 100}, {"items", item}}; required.append(name);
            fields["total"] = integerSchema(0, 1000); required.append("total"); fields["nextOffset"] = integerSchema(0, 1000);
        }
        d.outputSchema = schema(fields, required);
    }
    return definitions;
}
TaskStore::TaskStore(QString directory, TaskStoreOptions options) : options_(options) {
    require(!directory.trimmed().isEmpty() && options.maxTasks > 0 && options.maxTasks <= 1000 && options.maxTodos > 0 && options.maxTodos <= 1000
        && options.maxBytes >= 1024 && options.maxBytes <= 64 * 1024 * 1024 && options.lockTimeoutMs > 0 && options.lockTimeoutMs <= 60000,
        "Invalid task store configuration");
    require(!QFileInfo(directory).isSymLink() && QDir().mkpath(directory), "Cannot create task store", ErrorCode::StorageFailure);
    directory_ = QFileInfo(directory).canonicalFilePath();
    require(!directory_.isEmpty() && QFile::setPermissions(directory_, ownerDirectory), "Cannot make task store private", ErrorCode::StorageFailure);
}
QJsonObject TaskStore::snapshot(const QString& listId, const CancellationToken& token) const {
    token.throwIfCancelled(); LockedBoard locked(directory_, listId, options_, token);
    return locked.read(listId, options_).json();
}
ToolResult TaskStore::execute(const QString& listId, const QString& operation, const QJsonObject& args,
    const CancellationToken& token, const TaskCommitCallback& beforeCommit) const {
    token.throwIfCancelled(); validators().validateInput(operation, args);
    LockedBoard locked(directory_, listId, options_, token); auto b = locked.read(listId, options_);
    const auto original = b.json();
    if (args.contains("expectedRevision")) require(integer(args["expectedRevision"]) == b.revision,
        "Task list revision changed; read the current state before retrying", ErrorCode::AlreadyExists);
    QJsonObject data{{"revision", double(b.revision)}};
    TaskChange change{listId, operation};
    const auto id = args["taskId"].toString();
    if (operation == "TaskCreate") {
        require(!args["subject"].toString().trimmed().isEmpty(), "Task subject must not be blank");
        require(b.tasks.size() < options_.maxTasks && b.nextId < maxInteger, "Task capacity reached", ErrorCode::ResourceLimit);
        QJsonObject t{{"id", QString::number(b.nextId++)}, {"subject", args["subject"]}, {"description", args["description"]},
            {"activeForm", args["activeForm"].toString()}, {"status", "pending"}, {"owner", ""},
            {"metadata", args["metadata"].toObject()}, {"blocks", QJsonArray{}}, {"blockedBy", QJsonArray{}}};
        require(QJsonDocument(t["metadata"].toObject()).toJson(QJsonDocument::Compact).size() <= 65536, "Task metadata exceeds 64 KiB", ErrorCode::ResourceLimit);
        b.tasks[t["id"].toString()] = t; change.after = t; data["task"] = taskView(t, b.tasks);
    } else if (operation == "TaskGet") {
        require(b.tasks.contains(id), "Task not found", ErrorCode::NotFound); data["task"] = taskView(b.tasks[id], b.tasks);
    } else if (operation == "TaskList" || operation == "TodoRead") {
        const bool todos = operation == "TodoRead"; const auto values = todos ? b.todos : sorted(b.tasks);
        const int offset = args["offset"].toInt(), limit = args["limit"].toInt(100); QJsonArray page;
        for (auto n = qsizetype(offset); n < std::min(values.size(), qsizetype(offset) + limit); ++n) {
            if (todos) { page.append(values[n]); continue; }
            const auto t = values[n].toObject();
            page.append(QJsonObject{{"id", t["id"]}, {"subject", t["subject"]}, {"status", t["status"]},
                {"owner", t["owner"]}, {"blockedBy", unresolved(t, b.tasks)}});
        }
        data[todos ? "todos" : "tasks"] = page; data["total"] = values.size();
        if (qsizetype(offset) + limit < values.size()) data["nextOffset"] = offset + limit;
    } else if (operation == "TodoWrite") {
        require(args["todos"].toArray().size() <= options_.maxTodos, "Todo capacity reached", ErrorCode::ResourceLimit);
        change.before = {{"todos", b.todos}}; data["oldTodos"] = b.todos; b.todos = args["todos"].toArray();
        change.after = {{"todos", b.todos}}; data["newTodos"] = b.todos;
    } else {
        require(b.tasks.contains(id), "Task not found", ErrorCode::NotFound);
        auto t = b.tasks[id]; change.before = t;
        if (operation == "TaskClaim") {
            const auto owner = args["owner"].toString(); require(!owner.trimmed().isEmpty(), "Task owner must not be blank");
            QString reason;
            if (!t["owner"].toString().isEmpty() && t["owner"] != owner) reason = "already_claimed";
            else if (t["status"] == "completed") reason = "completed";
            else if (!unresolved(t, b.tasks).isEmpty()) reason = "blocked";
            else if (args["checkOwnerBusy"].toBool()) {
                for (auto it = b.tasks.begin(); it != b.tasks.end(); ++it)
                    if (it.key() != id && it.value()["owner"] == owner && it.value()["status"] != "completed") { reason = "owner_busy"; break; }
            }
            data["success"] = reason.isEmpty();
            if (!reason.isEmpty()) { data["reason"] = reason; data["task"] = taskView(t, b.tasks); return result(data); }
            t["owner"] = owner; t["status"] = "in_progress"; b.tasks[id] = t; change.after = t; data["task"] = taskView(t, b.tasks);
        } else if (args["status"] == "deleted") {
            for (auto it = args.begin(); it != args.end(); ++it)
                require(it.key() == "taskId" || it.key() == "status" || it.key() == "expectedRevision", "Deletion cannot include other updates");
            b.tasks.remove(id);
            for (auto it = b.tasks.begin(); it != b.tasks.end(); ++it) for (const auto* field : {"blocks", "blockedBy"}) {
                auto values = it.value()[field].toArray(); for (auto n = values.size(); n-- > 0;) if (values[n] == id) values.removeAt(n);
                it.value()[field] = values;
            }
            data["deleted"] = true; data["taskId"] = id;
        } else {
            if (args.contains("subject")) require(!args["subject"].toString().trimmed().isEmpty(), "Task subject must not be blank");
            for (const auto* field : {"subject", "description", "activeForm", "status", "owner"}) if (args.contains(field)) t[field] = args[field];
            auto metadata = t["metadata"].toObject(); const auto changes = args["metadata"].toObject();
            for (auto it = changes.begin(); it != changes.end(); ++it) { if (it.value().isNull()) metadata.remove(it.key()); else metadata[it.key()] = it.value(); }
            require(metadata.size() <= 256 && QJsonDocument(metadata).toJson(QJsonDocument::Compact).size() <= 65536,
                "Task metadata exceeds limit", ErrorCode::ResourceLimit);
            t["metadata"] = metadata; b.tasks[id] = t;
            auto edge = [&](const QString& from, const QString& to, bool add) {
                require(from != to && b.tasks.contains(from) && b.tasks.contains(to), "Task dependency endpoint is missing or self-referencing");
                auto update = [&](const QString& key, const char* field, const QString& value) {
                    auto values = b.tasks[key][field].toArray();
                    if (add && !values.contains(value)) values.append(value);
                    if (!add) for (auto n = values.size(); n-- > 0;) if (values[n] == value) values.removeAt(n);
                    b.tasks[key][field] = values;
                };
                update(from, "blocks", to); update(to, "blockedBy", from);
            };
            for (const auto* field : {"removeBlocks", "removeBlockedBy", "addBlocks", "addBlockedBy"}) {
                const auto key = QString::fromLatin1(field); const bool add = key.startsWith("add"), reverse = key.endsWith("BlockedBy");
                for (const auto& other : args[field].toArray()) edge(reverse ? other.toString() : id, reverse ? id : other.toString(), add);
            }
            validateGraph(b.tasks, ErrorCode::InvalidArgument); change.after = b.tasks[id]; data["task"] = taskView(b.tasks[id], b.tasks);
        }
    }
    if (b.json() != original) {
        require(b.revision < maxInteger, "Task revision capacity reached", ErrorCode::ResourceLimit);
        ++b.revision;
        require(QJsonDocument(b.json()).toJson(QJsonDocument::Compact).size() + 1 <= options_.maxBytes, "Task state exceeds byte limit", ErrorCode::ResourceLimit);
        token.throwIfCancelled();
        if(beforeCommit) {
            // Verification may inspect or change this board. Never hold its
            // lock across host/model/tool callbacks or overwrite their changes.
            locked.lock.unlock();beforeCommit(change,token);token.throwIfCancelled();
            LockedBoard current(directory_,listId,options_,token);
            require(current.read(listId,options_).json()==original,
                "Task list changed during verification; read the current state before retrying",ErrorCode::AlreadyExists);
            current.write(b,options_,token);
        } else locked.write(b,options_,token);
        data["revision"] = double(b.revision);
    }
    return result(data);
}
QList<Tool> taskTools(std::shared_ptr<TaskStore> store, QString listId, bool deferred, TaskCommitCallback callback) {
    require(bool(store) && (listId.isEmpty() || validId(listId)), "Task tools require a store and valid host-selected list ID");
    QList<Tool> out;
    for (auto definition : taskToolDefinitions(deferred)) {
        Tool tool; tool.definition = definition;
        tool.execute = [store, listId, callback, name = definition.name](const QJsonObject& args, const ToolContext& context) {
            return store->execute(listId.isEmpty() ? context.sessionId : listId, name, args, context.cancellation, callback);
        };
        out.append(std::move(tool));
    }
    return out;
}
}
