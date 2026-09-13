#include "Tools.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QSaveFile>
#include <QtCore/QStringConverter>
#include <QtCore/QRegularExpression>
#include <QtCore/QUuid>
#include <mutex>
#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

namespace iiLocalLLM::agent {
namespace {
constexpr qsizetype maxFileBytes = 1024 * 1024;
void require(bool ok, const QString& message) { if (!ok) throw Error(ErrorCode::InvalidArgument, message); }
QJsonObject stringSchema() { return {{"type", "string"}}; }
QJsonObject integerSchema(int minimum, int maximum) { return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}}; }
QJsonObject inputSchema(QJsonObject properties, QJsonArray required) {
    return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
}
QByteArray readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw Error(ErrorCode::StorageFailure, "Cannot read file: " + path);
    const auto bytes = file.read(maxFileBytes + 1);
    if (bytes.size() > maxFileBytes) throw Error(ErrorCode::ResourceLimit, "Text file exceeds 1 MiB");
    return bytes;
}
QString decode(const QByteArray& bytes) {
    QStringDecoder utf8(QStringDecoder::Utf8); QString text = utf8(bytes);
    require(!utf8.hasError(), "File is not valid UTF-8 text"); return text;
}
bool inside(const QString& path, const QString& root) {
    return !root.isEmpty() && (path == root || path.startsWith(root.endsWith('/') ? root : root + '/'));
}
class Workspace {
public:
    QString root;
    struct ReadState { QByteArray digest; bool complete; };
    std::mutex mutex;
    QHash<QString, ReadState> reads;
    QString key(const ToolContext& c, const QString& path) const { return c.sessionId + QChar(0) + path; }
    QString resolve(QString path, const ToolContext& c, bool write = false) const {
        require(c.workingDirectory.isEmpty() || QFileInfo(c.workingDirectory).canonicalFilePath() == root,
            "Tool registry belongs to a different workspace");
        require(!path.isEmpty() && !path.contains(QChar(0)), "A nonempty path is required");
        if (QDir::isRelativePath(path)) path = QDir(root).absoluteFilePath(path);
        path = QDir::cleanPath(path);
        QFileInfo info(path);
        QString canonical;
        if (info.exists()) canonical = info.canonicalFilePath();
        else {
            require(!info.isSymLink(), "Broken symlinks are not writable");
            QStringList tail;
            auto ancestor = info;
            while (!ancestor.exists()) {
                require(!ancestor.isSymLink(), "Broken ancestor symlink");
                tail.prepend(ancestor.fileName());
                const auto parent = ancestor.dir().absolutePath();
                require(parent != ancestor.absoluteFilePath(), "Cannot resolve path ancestor");
                ancestor = QFileInfo(parent);
            }
            canonical = QDir(ancestor.canonicalFilePath()).filePath(tail.join('/'));
        }
        const auto artifacts = write ? QString() : QFileInfo(c.artifactsDirectory).canonicalFilePath();
        require(inside(canonical, root) || inside(canonical, artifacts), "Path is outside the configured workspace");
        if (write) require(canonical != root, "Cannot replace the workspace root");
        return canonical;
    }
    void remember(const ToolContext& c, const QString& path, const QByteArray& bytes, bool complete = true) {
        const auto id = key(c, path);
        if (reads.size() >= 256 && !reads.contains(id)) reads.erase(reads.begin());
        reads.insert(id, {QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), complete});
    }
    QByteArray writable(const ToolContext& c, const QString& path) const {
        const auto found = reads.constFind(key(c, path));
        require(found != reads.cend() && found->complete, "Read the complete file before changing it");
        const auto bytes = readFile(path);
        require(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) == found->digest, "File changed after it was read; read it again");
        return bytes;
    }
    QString write(const ToolContext& c, const QString& path, const QByteArray& bytes, const std::optional<QByteArray>& before) {
        require(bytes.size() <= maxFileBytes, "Text output exceeds 1 MiB");
        QString backup;
        if (before && !c.artifactsDirectory.isEmpty()) {
            require(QDir().mkpath(c.artifactsDirectory), "Cannot create backup directory");
            backup = QDir(c.artifactsDirectory).filePath(uuid() + ".before");
            QSaveFile old(backup);
            if (!old.open(QIODevice::WriteOnly) || old.write(*before) != before->size() || !old.commit())
                throw Error(ErrorCode::StorageFailure, "Cannot save pre-edit backup");
        }
        require(QDir().mkpath(QFileInfo(path).absolutePath()), "Cannot create parent directory");
        require(resolve(path, c, true) == path, "File path changed before writing");
        if (before) require(readFile(path) == *before, "File changed before writing");
        else require(!QFileInfo::exists(path), "File appeared before writing; read it first");
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            throw Error(ErrorCode::StorageFailure, "Cannot write file: " + path);
        remember(c, path, bytes); return backup;
    }
    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
};
}
void registerWorkspaceTools(ToolRegistry& registry, const QString& workspaceRoot) {
    auto workspace = std::make_shared<Workspace>(); workspace->root = QFileInfo(workspaceRoot).canonicalFilePath();
    require(!workspace->root.isEmpty() && QFileInfo(workspace->root).isDir(), "Workspace must be an existing directory");
    Tool read;
    read.definition = {"Read", "Read a UTF-8 file (up to 1 MiB). Read the full file before editing it.",
        inputSchema({{"path", stringSchema()}, {"offset", integerSchema(1, 1000000)}, {"limit", integerSchema(1, 20000)}}, {"path"}), {}, true, true};
    read.execute = [workspace](const QJsonObject& a, const ToolContext& c) {
        c.cancellation.throwIfCancelled();
        std::lock_guard lock(workspace->mutex);
        const auto path = workspace->resolve(a["path"].toString(), c);
        const auto bytes = readFile(path); const auto text = decode(bytes); const auto lines = text.split('\n');
        const int offset = a["offset"].toInt(1), limit = a["limit"].toInt(2000);
        QStringList output;
        for (qsizetype n = offset - 1; n < lines.size() && n < qsizetype(offset - 1) + limit; ++n) output.append(lines[n]);
        const bool complete = offset == 1 && limit >= lines.size();
        workspace->remember(c, path, bytes, complete);
        const QJsonObject contextPaths = path == workspace->root || path.startsWith(workspace->root + '/')
            ? QJsonObject{{"iilocal.context_paths", QJsonArray{path}}} : QJsonObject{};
        return ToolResult{output.join('\n'), {{"path", path}, {"offset", offset}, {"lines", output.size()}, {"complete", complete}}, false, {}, contextPaths};
    }; registry.add(std::move(read));
    Tool write;
    write.definition = {"Write", "Write a UTF-8 file. Existing files must have been read completely and remain unchanged.",
        inputSchema({{"path", stringSchema()}, {"content", stringSchema()}}, {"path", "content"}), {}, false, false, true};
    write.execute = [workspace](const QJsonObject& a, const ToolContext& c) {
        c.cancellation.throwIfCancelled(); std::lock_guard lock(workspace->mutex);
        const auto path = workspace->resolve(a["path"].toString(), c, true);
        std::optional<QByteArray> before;
        if (QFileInfo::exists(path)) before = workspace->writable(c, path);
        const auto backup = workspace->write(c, path, a["content"].toString().toUtf8(), before);
        return ToolResult{"Wrote " + path, {{"path", path}, {"backup_path", backup}}, false, {}, {{"iilocal.context_paths", QJsonArray{path}}}};
    }; registry.add(std::move(write));
    Tool edit;
    edit.definition = {"Edit", "Replace exact text in a previously read UTF-8 file. Multiple matches require replace_all=true.",
        inputSchema({{"path", stringSchema()}, {"old_string", QJsonObject{{"type", "string"}, {"minLength", 1}}},
            {"new_string", stringSchema()}, {"replace_all", QJsonObject{{"type", "boolean"}}}}, {"path", "old_string", "new_string"}), {}, false, false, true};
    edit.execute = [workspace](const QJsonObject& a, const ToolContext& c) {
        c.cancellation.throwIfCancelled(); std::lock_guard lock(workspace->mutex);
        const auto path = workspace->resolve(a["path"].toString(), c, true);
        const auto before = workspace->writable(c, path); auto text = decode(before);
        const auto old = a["old_string"].toString(), replacement = a["new_string"].toString();
        const auto matches = text.count(old);
        require(matches > 0, "old_string was not found"); require(matches == 1 || a["replace_all"].toBool(), "Multiple matches require replace_all=true");
        text.replace(old, replacement);
        const auto backup = workspace->write(c, path, text.toUtf8(), before);
        return ToolResult{"Edited " + path, {{"path", path}, {"replacements", matches}, {"backup_path", backup}}, false, {}, {{"iilocal.context_paths", QJsonArray{path}}}};
    }; registry.add(std::move(edit));
    Tool glob;
    glob.definition = {"Glob", "List matching relative file paths in the workspace (up to 1000 results).",
        inputSchema({{"pattern", stringSchema()}}, {"pattern"}), {}, true, true};
    glob.execute = [workspace](const QJsonObject& a, const ToolContext& c) {
        workspace->resolve(".", c);
        const auto regex = QRegularExpression(QRegularExpression::wildcardToRegularExpression(a["pattern"].toString()));
        require(regex.isValid(), "Invalid glob pattern"); QStringList paths;
        QDirIterator iterator(workspace->root, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (iterator.hasNext() && paths.size() < 1000) {
            c.cancellation.throwIfCancelled(); const auto path = iterator.next(); const auto relative = QDir(workspace->root).relativeFilePath(path);
            if (regex.match(relative).hasMatch()) paths.append(relative);
        }
        paths.sort(); QJsonArray values; for (const auto& path : paths) values.append(path);
        return ToolResult{paths.join('\n'), {{"paths", values}, {"truncated", iterator.hasNext()}}};
    }; registry.add(std::move(glob));
    Tool grep;
    grep.definition = {"Grep", "Search a regular expression in UTF-8 workspace files (up to 100 matches, 1 MiB per file).",
        inputSchema({{"pattern", stringSchema()}, {"path", stringSchema()}}, {"pattern"}), {}, true, true};
    grep.execute = [workspace](const QJsonObject& a, const ToolContext& c) {
        QRegularExpression regex(a["pattern"].toString()); require(regex.isValid(), "Invalid regular expression");
        const auto root = workspace->resolve(a["path"].toString("."), c);
        QStringList files;
        if (QFileInfo(root).isFile()) files.append(root);
        else { QDirIterator iterator(root, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
            while (iterator.hasNext() && files.size() < 10000) { c.cancellation.throwIfCancelled(); files.append(iterator.next()); } }
        QStringList matches; QJsonArray values;
        for (const auto& path : files) {
            c.cancellation.throwIfCancelled();
            if (QFileInfo(path).size() > maxFileBytes) continue;
            QString text; try { text = decode(readFile(path)); } catch (const Error&) { continue; }
            const auto lines = text.split('\n');
            for (qsizetype n = 0; n < lines.size() && matches.size() < 100; ++n) if (regex.match(lines[n]).hasMatch()) {
                const auto relative = QDir(workspace->root).relativeFilePath(path);
                matches.append(relative + ':' + QString::number(n + 1) + ':' + lines[n]);
                values.append(QJsonObject{{"path", relative}, {"line", n + 1}, {"text", lines[n]}});
            }
            if (matches.size() >= 100) break;
        }
        return ToolResult{matches.join('\n'), {{"matches", values}, {"limit_reached", matches.size() >= 100}}};
    }; registry.add(std::move(grep));
    Tool shell;
    shell.definition = {"Bash", "Execute a shell command in the workspace; this requires tool permission and is not an OS sandbox.",
        inputSchema({{"command", QJsonObject{{"type", "string"}, {"minLength", 1}}}, {"timeout_ms", integerSchema(1, 600000)}}, {"command"})};
    shell.execute = [workspace](const QJsonObject& a, const ToolContext& c) {
        workspace->resolve(".", c);
        c.cancellation.throwIfCancelled(); QProcess process; process.setWorkingDirectory(workspace->root);
#ifdef Q_OS_UNIX
        process.setChildProcessModifier([] { if (::setsid() < 0) ::_exit(126); });
        process.start("/bin/bash", {"--noprofile", "--norc", "-c", a["command"].toString()});
#else
        process.start("cmd.exe", {"/D", "/S", "/C", a["command"].toString()});
#endif
        if (!process.waitForStarted(5000)) throw Error(ErrorCode::RuntimeFailure, "Could not start shell: " + process.errorString());
        const auto pid = process.processId();
        auto terminate = [&] {
#ifdef Q_OS_UNIX
            ::kill(-pid_t(pid), SIGTERM);
#endif
            process.terminate(); process.waitForFinished(200);
#ifdef Q_OS_UNIX
            ::kill(-pid_t(pid), SIGKILL);
#endif
            process.kill(); process.waitForFinished(1000);
        };
        QByteArray output, errors; QElapsedTimer elapsed; elapsed.start();
        const int timeout = a["timeout_ms"].toInt(30000);
        for (;;) {
            process.waitForFinished(20); output += process.readAllStandardOutput(); errors += process.readAllStandardError();
            if (c.cancellation.isCancelled()) { terminate(); c.cancellation.throwIfCancelled(); }
            if (elapsed.elapsed() > timeout) { terminate(); throw Error(ErrorCode::Timeout, "Shell command timed out"); }
            if (output.size() + errors.size() > maxFileBytes) { terminate(); throw Error(ErrorCode::ResourceLimit, "Shell output exceeds 1 MiB"); }
            if (process.state() == QProcess::NotRunning) break;
        }
        const auto text = QString::fromUtf8(output) + (errors.isEmpty() ? QString() : "\n[stderr]\n" + QString::fromUtf8(errors));
        return ToolResult{text, {{"exit_code", process.exitCode()}, {"crashed", process.exitStatus() == QProcess::CrashExit}},
            process.exitCode() != 0 || process.exitStatus() == QProcess::CrashExit};
    }; registry.add(std::move(shell));
}
}
