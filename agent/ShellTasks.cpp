#include "ShellTasks.h"
#include "ShellProcess.h"
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QUuid>
#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace iiLocalLLM::agent {
namespace {
const auto privateFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
const auto privateDirectory = privateFile | QFileDevice::ExeOwner;
void require(bool ok, const QString& message, ErrorCode code = ErrorCode::InvalidArgument) { if (!ok) throw Error(code, message); }
bool sessionValid(const QString& id) { return !id.isEmpty() && id.size() <= 512 && !id.contains(QChar(0)); }
bool taskValid(const QString& id) { static const QRegularExpression pattern("\\Ash-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\z"); return pattern.match(id).hasMatch(); }
bool active(const QJsonObject& metadata) { return metadata["status"] == "pending" || metadata["status"] == "running"; }
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
QJsonObject schema(QJsonObject fields, QJsonArray required = {}) {
    return {{"type", "object"}, {"properties", fields}, {"required", required}, {"additionalProperties", false}};
}
QJsonObject integer(qint64 minimum, qint64 maximum) { return {{"type", "integer"}, {"minimum", double(minimum)}, {"maximum", double(maximum)}}; }
QJsonObject taskSchema(bool stored) {
    const QJsonObject string{{"type", "string"}}, nullableInteger{{"type", QJsonArray{"integer", "null"}}};
    QJsonObject fields{{"task_id", QJsonObject{{"type", "string"}, {"pattern", "^sh-[0-9a-f-]{36}$"}}},
        {"task_type", QJsonObject{{"const", "local_bash"}}}, {"run_id", string}, {"command", QJsonObject{{"type", "string"}, {"maxLength", 65536}}},
        {"description", QJsonObject{{"type", "string"}, {"maxLength", 4096}}},
        {"status", QJsonObject{{"enum", QJsonArray{"pending", "running", "completed", "failed", "killed", "interrupted"}}}},
        {"created_at", integer(0, 9007199254740991LL)}, {"started_at", nullableInteger}, {"finished_at", nullableInteger},
        {"exitCode", nullableInteger}, {"output_bytes", integer(0, 64 * 1024 * 1024)}, {"timeout_ms", integer(1, 86400000)},
        {"error_code", string}, {"error", string}, {"persisted", QJsonObject{{"type", "boolean"}}}};
    QJsonArray required{"task_id", "task_type", "run_id", "description", "status", "created_at", "started_at", "finished_at",
        "exitCode", "output_bytes", "timeout_ms", "error_code", "error"};
    if (stored) {
        fields["schema"] = QJsonObject{{"const", "iisacc.agent.shell/1"}};
        fields["session_id"] = QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 512}};
        fields["working_directory"] = string;
        for (const auto* key : {"schema", "session_id", "working_directory", "command"}) required.append(QString::fromLatin1(key));
    } else {
        fields["output_file"] = string; required.append("output_file");
        fields["output"] = string; fields["output_base64"] = string;
        fields["offset"] = fields["next_offset"] = integer(0, 64 * 1024 * 1024);
        fields["has_more"] = QJsonObject{{"type", "boolean"}};
    }
    return schema(fields, required);
}
void validateRecord(const QJsonObject& value) {
    static const auto validator = [] {
        auto r = std::make_unique<ToolRegistry>(); Tool tool; tool.definition.name = "ShellState";
        tool.definition.inputSchema = taskSchema(true); tool.execute = [](const auto&, const auto&) { return ToolResult{}; };
        r->add(std::move(tool)); return r;
    }();
    try { validator->validateInput("ShellState", value); }
    catch (const Error&) { throw Error(ErrorCode::ProtocolError, "Corrupt shell task record fields"); }
}
ToolResult result(QJsonObject data) { return {QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)), std::move(data)}; }
}
class ShellTasks::Impl {
public:
    struct Job {
        mutable std::mutex mutex;
        std::condition_variable changed;
        QJsonObject metadata;
        QString directory;
        CancellationToken cancel;
        std::thread worker;
        bool ready = false, done = false;
    };
    QString workspace, directory;
    ShellTaskOptions options;
    mutable std::mutex mutex;
    std::mutex joining;
    std::map<QString, std::shared_ptr<Job>> jobs;
    std::unique_ptr<QLockFile> lock;
    bool closing = false;
    static QString outputPath(const Job& job) { return QDir(job.directory).filePath("output.log"); }
    static void save(const Job& job) {
        const auto path = QDir(job.directory).filePath("state.json");
        require(!QFileInfo(path).isSymLink(), "Shell task state must not be a symlink", ErrorCode::StorageFailure);
        const auto bytes = QJsonDocument(job.metadata).toJson(QJsonDocument::Compact) + '\n';
        QSaveFile file(path); file.setDirectWriteFallback(false);
        require(file.open(QIODevice::WriteOnly) && file.setPermissions(privateFile) && file.write(bytes) == bytes.size() && file.commit(),
            "Cannot publish shell task state", ErrorCode::StorageFailure);
    }
    static QJsonObject view(const Job& job) {
        auto value = job.metadata; value.remove("schema"); value.remove("session_id"); value.remove("working_directory");
        value["output_file"] = outputPath(job); return value;
    }
    std::shared_ptr<Job> find(const QString& session, const QString& id) const {
        require(sessionValid(session) && taskValid(id), "Invalid shell task identity");
        std::lock_guard guard(mutex); const auto found = jobs.find(id);
        require(found != jobs.end(), "Shell task not found", ErrorCode::NotFound);
        std::lock_guard item(found->second->mutex);
        require(found->second->metadata["session_id"] == session, "Shell task not found", ErrorCode::NotFound);
        return found->second;
    }
    Impl(QString root, QString state, ShellTaskOptions o) : options(o) {
#if !defined(Q_OS_UNIX) || defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
        throw Error(ErrorCode::RuntimeUnavailable, "Background shell tasks currently require a desktop POSIX host");
#endif
        require(o.maxConcurrent > 0 && o.maxConcurrent <= 64 && o.maxRecords > 0 && o.maxRecords <= 10000
            && o.maxOutputBytes >= 1024 && o.maxOutputBytes <= 64 * 1024 * 1024 && o.maxRuntimeMs > 0 && o.maxRuntimeMs <= 86400000,
            "Invalid shell task limits");
        workspace = QFileInfo(root).canonicalFilePath();
        require(!workspace.isEmpty() && QFileInfo(workspace).isDir(), "Shell workspace must exist");
        require(!state.trimmed().isEmpty() && !QFileInfo(state).isSymLink() && QDir().mkpath(state), "Cannot create shell state directory", ErrorCode::StorageFailure);
        directory = QFileInfo(state).canonicalFilePath();
        require(directory != workspace && !workspace.startsWith(directory + '/'), "Shell state cannot contain the workspace");
        require(!directory.isEmpty() && QFile::setPermissions(directory, privateDirectory), "Cannot make shell state private", ErrorCode::StorageFailure);
        const auto lockPath = QDir(directory).filePath("shell.lock");
        require(!QFileInfo(lockPath).isSymLink(), "Shell state lock must not be a symlink", ErrorCode::StorageFailure);
        lock = std::make_unique<QLockFile>(lockPath); lock->setStaleLockTime(0);
        require(lock->tryLock(0), "Shell state is already owned or inaccessible", ErrorCode::AlreadyExists);
        const auto entries = QDir(directory).entryInfoList({"sh-*"}, QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
        require(entries.size() <= o.maxRecords, "Shell history exceeds configured record capacity", ErrorCode::ResourceLimit);
        for (const auto& entry : entries) {
            require(taskValid(entry.fileName()) && entry.isDir() && !entry.isSymLink()
                && entry.canonicalFilePath() == entry.absoluteFilePath(), "Invalid shell task directory", ErrorCode::StorageFailure);
            auto job = std::make_shared<Job>(); job->directory = entry.absoluteFilePath();
            const auto path = QDir(job->directory).filePath("state.json"); QFile file(path);
            require(!QFileInfo(path).isSymLink() && QFileInfo(path).isFile() && file.open(QIODevice::ReadOnly), "Cannot read shell state", ErrorCode::StorageFailure);
            const auto bytes = file.read(1024 * 1024 + 1); QJsonParseError error;
            const auto doc = QJsonDocument::fromJson(bytes, &error); auto m = doc.object();
            validateRecord(m);
            require(bytes.size() <= 1024 * 1024 && file.atEnd() && error.error == QJsonParseError::NoError && doc.isObject()
                && m["schema"] == "iisacc.agent.shell/1" && m["task_id"] == entry.fileName() && m["task_type"] == "local_bash"
                && m["working_directory"] == workspace && sessionValid(m["session_id"].toString())
                && m["command"].isString() && m["command"].toString().toUtf8().size() <= 65536 && m["description"].isString()
                && m["description"].toString().size() <= 4096 && m["created_at"].isDouble()
                && QStringList{"pending", "running", "completed", "failed", "killed", "interrupted"}.contains(m["status"].toString()),
                "Corrupt shell task state", ErrorCode::ProtocolError);
            const auto output = outputPath(*job); const QFileInfo outputInfo(output);
            require(outputInfo.isFile() && !outputInfo.isSymLink() && outputInfo.size() <= 64 * 1024 * 1024,
                "Invalid shell output file", ErrorCode::StorageFailure);
            const bool recover = active(m);
            require(recover || m["output_bytes"].toDouble() == outputInfo.size(), "Shell output changed after completion", ErrorCode::ProtocolError);
            m["output_bytes"] = double(outputInfo.size());
            if (recover) {
                m["status"] = "interrupted"; m["finished_at"] = double(now()); m["exitCode"] = QJsonValue::Null;
                m["error_code"] = "host_interrupted";
                m["error"] = "Previous host stopped without a terminal record; process outcome is unknown. No command was restarted or PID signalled.";
            }
            job->metadata = m; job->ready = job->done = true; if (recover) save(*job); jobs.emplace(entry.fileName(), job);
        }
    }
    void execute(const std::shared_ptr<Job>& job) noexcept {
        QString status = "failed", errorCode, errorText; QJsonValue exitCode(QJsonValue::Null);
        try {
            QFile output(outputPath(*job));
            require(!QFileInfo(output.fileName()).isSymLink() && output.open(QIODevice::WriteOnly | QIODevice::Append), "Cannot open shell output", ErrorCode::StorageFailure);
            QString command; int timeout;
            { std::lock_guard guard(job->mutex); command = job->metadata["command"].toString(); timeout = job->metadata["timeout_ms"].toInt(); }
            const auto value = detail::shellProcess(workspace, command, timeout, job->cancel, [&] {
                std::lock_guard guard(job->mutex); job->metadata["status"] = "running"; job->metadata["started_at"] = double(now());
                save(*job); job->ready = true; job->changed.notify_all();
            }, [&](const QByteArray& bytes, bool) {
                std::lock_guard guard(job->mutex);
                const auto size = qint64(job->metadata["output_bytes"].toDouble());
                const auto count = std::min<qint64>(bytes.size(), options.maxOutputBytes - size);
                require(count >= 0 && output.write(bytes.constData(), count) == count && output.flush(), "Cannot store shell output", ErrorCode::StorageFailure);
                job->metadata["output_bytes"] = double(size + count); job->changed.notify_all();
                require(count == bytes.size(), "Shell output exceeds its configured byte limit", ErrorCode::ResourceLimit);
            });
            if (!value.crashed) exitCode = value.code;
            status = !value.crashed && value.code == 0 ? "completed" : "failed";
            if (status == "failed") {
                errorCode = "runtime_failure";
                errorText = value.crashed ? "Shell process terminated abnormally" : "Shell exited unsuccessfully";
            }
        } catch (const Error& error) {
            errorCode = enumName(error.code()); errorText = QString::fromUtf8(error.what()); status = error.code() == ErrorCode::Cancelled ? "killed" : "failed";
        } catch (const std::exception& error) { errorCode = "runtime_failure"; errorText = QString::fromUtf8(error.what()); }
        catch (...) { errorCode = "runtime_failure"; errorText = "Unknown shell worker failure"; }
        std::lock_guard guard(job->mutex);
        job->metadata["status"] = status; job->metadata["exitCode"] = exitCode; job->metadata["finished_at"] = double(now());
        job->metadata["error_code"] = errorCode; job->metadata["error"] = errorText;
        try { save(*job); }
        catch (const std::exception& error) {
            job->metadata["status"] = "failed"; job->metadata["error_code"] = "storage_failure";
            job->metadata["error"] = QString::fromUtf8(error.what()); job->metadata["persisted"] = false;
        }
        job->ready = job->done = true; job->changed.notify_all();
    }
    void close() {
        std::lock_guard join(joining); std::vector<std::shared_ptr<Job>> all;
        { std::lock_guard guard(mutex); closing = true; for (const auto& [id, job] : jobs) { job->cancel.cancel(); all.push_back(job); } }
        for (const auto& job : all) if (job->worker.joinable()) job->worker.join();
        if (lock) lock->unlock();
    }
};
ShellTasks::ShellTasks(QString root, QString state, ShellTaskOptions options) : d(std::make_unique<Impl>(std::move(root), std::move(state), options)) {}
ShellTasks::~ShellTasks() { d->close(); }
QString ShellTasks::workspace() const { return d->workspace; }
void ShellTasks::close() { d->close(); }
QJsonObject ShellTasks::start(const ToolContext& context, QString command, QString description, int timeoutMs) {
    context.cancellation.throwIfCancelled();
    require(sessionValid(context.sessionId) && command.toUtf8().size() <= 65536 && !command.trimmed().isEmpty() && !command.contains(QChar(0))
        && description.toUtf8().size() <= 4096 && timeoutMs > 0 && timeoutMs <= d->options.maxRuntimeMs
        && QFileInfo(context.workingDirectory).canonicalFilePath() == d->workspace, "Invalid shell execution request");
    auto job = std::make_shared<Impl::Job>();
    const auto id = "sh-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        std::lock_guard guard(d->mutex); require(!d->closing, "Shell task manager is closed", ErrorCode::ShuttingDown);
        int running = 0;
        for (const auto& [key, existing] : d->jobs) {
            std::lock_guard item(existing->mutex); if (!existing->done) ++running;
            else if (existing->worker.joinable()) existing->worker.join();
        }
        require(running < d->options.maxConcurrent && d->jobs.size() < size_t(d->options.maxRecords), "Shell task capacity reached", ErrorCode::ResourceLimit);
        job->directory = QDir(d->directory).filePath(id);
        require(QDir().mkpath(job->directory) && QFile::setPermissions(job->directory, privateDirectory), "Cannot create private shell task", ErrorCode::StorageFailure);
        QFile output(Impl::outputPath(*job));
        require(output.open(QIODevice::WriteOnly | QIODevice::NewOnly) && output.setPermissions(privateFile), "Cannot create shell output", ErrorCode::StorageFailure); output.close();
        const auto points = command.toUcs4();
        const auto label = description.isEmpty() ? QString::fromUcs4(points.constData(), std::min(points.size(), qsizetype(1024))) : description;
        job->metadata = {{"schema", "iisacc.agent.shell/1"}, {"task_id", id}, {"task_type", "local_bash"}, {"session_id", context.sessionId},
            {"run_id", context.runId}, {"working_directory", d->workspace}, {"command", command}, {"description", label},
            {"status", "pending"}, {"created_at", double(now())}, {"started_at", QJsonValue::Null}, {"finished_at", QJsonValue::Null},
            {"exitCode", QJsonValue::Null}, {"output_bytes", 0}, {"timeout_ms", timeoutMs}, {"error_code", ""}, {"error", ""}};
        Impl::save(*job); d->jobs.emplace(id, job);
        try { job->worker = std::thread([impl = d.get(), job] { impl->execute(job); }); }
        catch (...) { job->ready = job->done = true; job->metadata["status"] = "failed"; job->metadata["error_code"] = "runtime_failure"; Impl::save(*job); throw; }
    }
    std::unique_lock guard(job->mutex);
    while (!job->ready) {
        if (context.cancellation.isCancelled()) job->cancel.cancel();
        job->changed.wait_for(guard, std::chrono::milliseconds(10));
    }
    if (context.cancellation.isCancelled()) {
        job->cancel.cancel(); while (!job->done) job->changed.wait_for(guard, std::chrono::milliseconds(10));
        context.cancellation.throwIfCancelled();
    }
    auto value = Impl::view(*job); value["backgroundTaskId"] = id; return value;
}
QJsonObject ShellTasks::output(const QString& session, const QString& id, bool block, int timeoutMs, qint64 offset, int limit, const CancellationToken& token) const {
    require(timeoutMs >= 0 && timeoutMs <= 600000 && offset >= 0 && offset <= 64 * 1024 * 1024 && limit > 0 && limit <= 65536, "Invalid shell output limits");
    token.throwIfCancelled(); auto job = d->find(session, id); std::unique_lock guard(job->mutex);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (block && !job->done && std::chrono::steady_clock::now() < deadline) {
        token.throwIfCancelled(); job->changed.wait_for(guard, std::chrono::milliseconds(10));
    }
    token.throwIfCancelled(); auto task = Impl::view(*job); const auto path = Impl::outputPath(*job);
    QFile file(path); require(!QFileInfo(path).isSymLink() && QFileInfo(path).isFile() && file.open(QIODevice::ReadOnly) && file.seek(offset),
        "Cannot read shell output", ErrorCode::StorageFailure);
    const auto bytes = file.read(limit); task["output"] = QString::fromUtf8(bytes);
    // Byte pages may split UTF-8, and shell output may be binary. Preserve exact
    // page bytes alongside the display string for lossless consumers.
    task["output_base64"] = QString::fromLatin1(bytes.toBase64()); task["offset"] = double(offset);
    task["next_offset"] = double(offset + bytes.size()); task["has_more"] = offset + bytes.size() < file.size();
    return {{"retrieval_status", job->done ? "success" : block ? "timeout" : "not_ready"}, {"task", task}};
}
QJsonObject ShellTasks::stop(const QString& session, const QString& id, const CancellationToken& token) {
    token.throwIfCancelled(); auto job = d->find(session, id); std::unique_lock guard(job->mutex);
    require(!job->done, "Shell task is not running"); job->cancel.cancel();
    while (!job->done) { job->changed.wait_for(guard, std::chrono::milliseconds(10)); token.throwIfCancelled(); }
    return {{"task_id", id}, {"task_type", "local_bash"}, {"command", job->metadata["command"]}, {"status", job->metadata["status"]},
        {"message", "Shell task reached a terminal state after stop was requested"}};
}
QJsonArray ShellTasks::list(const QString& session, int offset, int limit) const {
    require(sessionValid(session) && offset >= 0 && offset <= 1000000 && limit > 0 && limit <= 100, "Invalid shell list request");
    std::vector<QJsonObject> values;
    { std::lock_guard guard(d->mutex); for (const auto& [id, job] : d->jobs) {
        std::lock_guard item(job->mutex); if (job->metadata["session_id"] != session) continue;
        auto value = Impl::view(*job); value.remove("command"); values.push_back(value);
    } }
    std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) {
        if (active(a) != active(b)) return active(a);
        if (a["created_at"] != b["created_at"]) return a["created_at"].toDouble() > b["created_at"].toDouble();
        return a["task_id"].toString() < b["task_id"].toString();
    });
    QJsonArray result; for (size_t n = size_t(offset); n < values.size() && result.size() < limit; ++n) result.append(values[n]); return result;
}
bool ShellTasks::ownsOutput(const QString& session, const QString& path) const {
    const auto parent = QFileInfo(path).dir(); if (QFileInfo(path).fileName() != "output.log" || !taskValid(parent.dirName())) return false;
    try { auto job = d->find(session, parent.dirName()); return path == Impl::outputPath(*job) && !QFileInfo(path).isSymLink(); }
    catch (const Error&) { return false; }
}
bool ShellTasks::containsStatePath(const QString& path) const { return path == d->directory || path.startsWith(d->directory + '/'); }
void registerShellTaskControls(ToolRegistry& registry, std::shared_ptr<ShellTasks> tasks, bool deferred) {
    require(bool(tasks), "Shell task controls require a manager");
    const QJsonObject id{{"type", "string"}, {"pattern", "^sh-[0-9a-f-]{36}$"}};
    Tool output; output.definition.name = "TaskOutput";
    output.definition.description = "Read a background shell's output and status. block=true waits up to timeout milliseconds; cancelling this wait does not stop the task. Use next_offset to read further bytes.";
    output.definition.inputSchema = schema({{"task_id", id}, {"block", QJsonObject{{"type", "boolean"}}}, {"timeout", integer(0, 600000)},
        {"offset", integer(0, 64 * 1024 * 1024)}, {"limit_bytes", integer(1, 65536)}}, {"task_id"});
    output.definition.readOnly = true;
    output.definition.outputSchema = schema({{"retrieval_status", QJsonObject{{"enum", QJsonArray{"success", "timeout", "not_ready"}}}}, {"task", taskSchema(false)}}, {"retrieval_status", "task"});
    output.execute = [tasks](const QJsonObject& args, const ToolContext& context) {
        return result(tasks->output(context.sessionId, args["task_id"].toString(), args["block"].toBool(true), args["timeout"].toInt(30000),
            qint64(args["offset"].toDouble()), args["limit_bytes"].toInt(24576), context.cancellation));
    };
    Tool stop; stop.definition.name = "TaskStop"; stop.definition.description = "Stop this session's running background shell and its process group. Completed tasks cannot be stopped.";
    stop.definition.inputSchema = schema({{"task_id", id}, {"shell_id", id}});
    stop.definition.inputSchema["oneOf"] = QJsonArray{QJsonObject{{"required", QJsonArray{"task_id"}}}, QJsonObject{{"required", QJsonArray{"shell_id"}}}};
    stop.definition.outputSchema = schema({{"task_id", id}, {"task_type", QJsonObject{{"const", "local_bash"}}},
        {"command", QJsonObject{{"type", "string"}}}, {"status", QJsonObject{{"enum", QJsonArray{"completed", "failed", "killed"}}}},
        {"message", QJsonObject{{"type", "string"}}}}, {"task_id", "task_type", "command", "status", "message"});
    stop.execute = [tasks](const QJsonObject& args, const ToolContext& context) {
        return result(tasks->stop(context.sessionId, args.contains("task_id") ? args["task_id"].toString() : args["shell_id"].toString(), context.cancellation));
    };
    Tool list; list.definition.name = "ShellTaskList"; list.definition.description = "List this session's actual background shell executions, separate from the planning TaskList. Use offset/limit to page the history.";
    list.definition.inputSchema = schema({{"offset", integer(0, 1000000)}, {"limit", integer(1, 100)}}); list.definition.readOnly = true;
    list.definition.outputSchema = schema({{"tasks", QJsonObject{{"type", "array"}, {"maxItems", 100}, {"items", taskSchema(false)}}},
        {"next_offset", integer(0, 1000100)}}, {"tasks"});
    list.execute = [tasks](const QJsonObject& args, const ToolContext& context) {
        context.cancellation.throwIfCancelled(); const int offset = args["offset"].toInt(), limit = args["limit"].toInt(100);
        const auto values = tasks->list(context.sessionId, offset, limit); QJsonObject data{{"tasks", values}};
        if (values.size() == limit) data["next_offset"] = offset + limit;
        return result(data);
    };
    for (auto* tool : {&output, &stop, &list}) {
        tool->definition.deferred = deferred; tool->definition.concurrencySafe = true;
        tool->definition.metadata = {{"source", "builtin.shell.control"}, {"search_hint", "background shell task output logs stop running"}};
        registry.add(std::move(*tool));
    }
}
}
