#include "Tools.h"
#include "ShellTasks.h"
#include "ShellProcess.h"
#include "PermissionRulesInternal.h"
#include "Notebook.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QStringConverter>
#include <QtCore/QRegularExpression>
#include <QtCore/QUuid>
#include <mutex>
#include <algorithm>

namespace iiLocalLLM::agent {
namespace {
constexpr qsizetype maxFileBytes = 1024 * 1024;
void require(bool ok, const QString& message) { if (!ok) throw Error(ErrorCode::InvalidArgument, message); }
QJsonObject stringSchema() { return {{"type", "string"}}; }
QJsonObject integerSchema(int minimum, int maximum) { return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}}; }
QJsonObject inputSchema(QJsonObject properties, QJsonArray required) {
    return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
}
QRegularExpression globExpression(const QString& pattern) {
    require(pattern.size()<=4096&&!pattern.contains(QChar::Null),"Invalid glob pattern");
    const auto parts=pattern.split('/');QString expression;
    for(qsizetype i=0;i<parts.size();++i) {
        const bool last=i+1==parts.size();
        if(parts[i]=="**")expression+=last?"[\\s\\S]*":"(?:[^/]+/)*";
        else {
            expression+=QRegularExpression::wildcardToRegularExpression(parts[i],QRegularExpression::UnanchoredWildcardConversion);
            if(!last)expression+='/';
        }
    }
    return QRegularExpression(QRegularExpression::anchoredPattern(expression));
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
    QStringList privatePaths;
    std::shared_ptr<ShellTasks> shells;
    struct ReadState { QByteArray digest; bool complete; };
    std::mutex mutex;
    QHash<QString, ReadState> reads;
    QHash<QString,QHash<QString,ReadState>> forkReads;
    QHash<QString,ReadState>& observations(const ToolContext& c) {
        auto it=forkReads.find(c.sessionId);return it==forkReads.end()?reads:it.value();
    }
    const QHash<QString,ReadState>& observations(const ToolContext& c)const {
        auto it=forkReads.constFind(c.sessionId);return it==forkReads.cend()?reads:it.value();
    }
    QString rootFor(const ToolContext& context) const {
        if(context.workingDirectory.isEmpty())return root;
        const auto active=QFileInfo(context.workingDirectory).canonicalFilePath();
        require(!active.isEmpty()&&(active==root||QFileInfo(context.originalWorkingDirectory).canonicalFilePath()==root),"Tool registry belongs to a different workspace");
        return active;
    }
    bool isPrivate(const QString& path,const ToolContext& context) const {
        if(inside(path,context.plansDirectory))return true;
        for(const auto& denied:context.protectedPaths)
            if(inside(path,denied)||inside(path,QFileInfo(denied).canonicalFilePath()))return true;
        return std::any_of(privatePaths.cbegin(),privatePaths.cend(),[&](const auto& denied) {
            const auto active=rootFor(context);const auto rebound=inside(denied,root)?QDir(active).filePath(QDir(root).relativeFilePath(denied)):denied;
            return inside(path,denied)||inside(path,QFileInfo(denied).canonicalFilePath())||inside(path,rebound)||inside(path,QFileInfo(rebound).canonicalFilePath());
        });
    }
    QJsonObject contextPaths(const QString& path,const ToolContext& context) const {
        return inside(path,rootFor(context))?QJsonObject{{"iilocal.context_paths",QJsonArray{path}}}:QJsonObject{};
    }
    QString key(const ToolContext& c, const QString& path) const { return c.sessionId + QChar(0) + QString::number(c.contextRevision) + QChar(0) + QString::number(c.workspaceRevision) + QChar(0) + path; }
    QString resolve(QString path, const ToolContext& c, bool write = false) const {
        const auto root=rootFor(c);
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
        const auto artifactRoot=c.artifactsDirectory.isEmpty()?QString():QFileInfo(c.artifactsDirectory).absoluteFilePath();
        const auto canonicalArtifacts=artifactRoot.isEmpty()?QString():QFileInfo(artifactRoot).canonicalFilePath();
        const bool planArea=inside(path,c.plansDirectory)||inside(canonical,c.plansDirectory);
        const bool ownPlan=!c.planFilePath.isEmpty()&&path==c.planFilePath&&canonical==c.planFilePath;
        if(planArea) {
            require(ownPlan,"Only this session's exact plan file is accessible");
            require(!write||c.planModeActive,"An approved plan is read-only; enter plan mode to revise it");
            require(!info.isSymLink(),"Plan file must not be a symlink");return canonical;
        }
        if(write)require(!inside(path,artifactRoot)&&!inside(canonical,canonicalArtifacts),"Tool artifacts are read-only to workspace tools");
        const auto artifacts = write ? QString() : canonicalArtifacts;
        if (shells && shells->containsStatePath(canonical)) {
            require(!write && shells->ownsOutput(c.sessionId, canonical), "Shell state is private; only this session's output files can be read");
            return canonical;
        }
        const bool ownArtifact=inside(canonical,artifacts);
        require((!isPrivate(path,c)&&!isPrivate(canonical,c))||ownArtifact,"Host-private path is inaccessible to workspace tools");
        auto covered=[&](const QString& value) {return inside(value,root)||std::any_of(c.workingDirectories.cbegin(),c.workingDirectories.cend(),[&](const auto& directory){return inside(value,directory);});};
        require((covered(path)&&covered(canonical))||ownArtifact, "Path is outside the configured working directories");
        if (write) require(canonical != root&&!c.workingDirectories.contains(canonical), "Cannot replace a working directory root");
        return canonical;
    }
    void remember(const ToolContext& c, const QString& path, const QByteArray& bytes, bool complete = true) {
        auto& reads=observations(c);
        const auto id = key(c, path);
        if (reads.size() >= 256 && !reads.contains(id)) reads.erase(reads.begin());
        reads.insert(id, {QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), complete});
    }
    QByteArray writable(const ToolContext& c, const QString& path) const {
        const auto& reads=observations(c);
        const auto found = reads.constFind(key(c, path));
        require(found != reads.cend() && found->complete, "Read the complete file before changing it");
        const auto bytes = readFile(path);
        require(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) == found->digest, "File changed after it was read; read it again");
        return bytes;
    }
    QString artifact(const ToolContext& c,const QByteArray& bytes,const QString& extension) const {
        if(c.artifactsDirectory.isEmpty())return {};
        require(QDir().mkpath(c.artifactsDirectory),"Cannot create backup directory");
        const auto path=QDir(c.artifactsDirectory).filePath(uuid()+'.'+extension);QSaveFile file(path);
        if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())
            throw Error(ErrorCode::StorageFailure,"Cannot save file edit artifact");
        return path;
    }
    QString write(const ToolContext& c, const QString& path, const QByteArray& bytes, const std::optional<QByteArray>& before) {
        require(bytes.size() <= maxFileBytes, "Text output exceeds 1 MiB");
        if(path==c.planFilePath)require(bytes.size()<=c.maxPlanBytes&&!bytes.contains('\0'),"Plan text exceeds its byte limit or contains NUL");
        QString backup;
        if(before)backup=artifact(c,*before,"before");
        require(QDir().mkpath(QFileInfo(path).absolutePath()), "Cannot create parent directory");
        require(resolve(path, c, true) == path, "File path changed before writing");
        if (before) require(readFile(path) == *before, "File changed before writing");
        else require(!QFileInfo::exists(path), "File appeared before writing; read it first");
        if(c.beforeFileWrite&&path!=c.planFilePath)c.beforeFileWrite(path,before,c);
        require(resolve(path,c,true)==path,"File path changed while checkpointing");
        if(before)require(readFile(path)==*before,"File changed while checkpointing");
        else require(!QFileInfo::exists(path),"File appeared while checkpointing");
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            throw Error(ErrorCode::StorageFailure, "Cannot write file: " + path);
        remember(c, path, bytes); return backup;
    }
    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
};
void preparePath(Tool& tool, const std::shared_ptr<Workspace>& workspace, bool write,QString defaultPath={},QString pathKey="path") {
    tool.prepare = [workspace, write, defaultPath, pathKey, definition = tool.definition, execute = tool.execute](const QJsonObject& args, const ToolContext& context) {
        QString path;
        { std::lock_guard lock(workspace->mutex); path = workspace->resolve(args[pathKey].toString(defaultPath), context, write); }
        auto preview = definition; preview.metadata["canonical_path"] = path;
        return PreparedTool{std::move(preview), [workspace, write, defaultPath, pathKey, execute, args, context, path] {
            {
                std::lock_guard lock(workspace->mutex);
                require(workspace->resolve(args[pathKey].toString(defaultPath), context, write) == path
                    && workspace->resolve(path, context, write) == path, "File permission target changed before execution");
            }
            auto frozen = args; frozen[pathKey] = path;
            return execute(frozen, context);
        }};
    };
}
}
void registerWorkspaceTools(ToolRegistry& registry, const QString& workspaceRoot) {
    registerWorkspaceTools(registry, workspaceRoot, {});
}
void registerWorkspaceTools(ToolRegistry& registry, const QString& workspaceRoot, std::shared_ptr<ShellTasks> shells) {
    registerWorkspaceTools(registry,workspaceRoot,std::move(shells),{});
}
void registerWorkspaceTools(ToolRegistry& registry, const QString& workspaceRoot, std::shared_ptr<ShellTasks> shells,const QStringList& privatePaths) {
    auto workspace = std::make_shared<Workspace>(); workspace->root = QFileInfo(workspaceRoot).canonicalFilePath();
    require(!workspace->root.isEmpty() && QFileInfo(workspace->root).isDir(), "Workspace must be an existing directory");
    require(!shells || shells->workspace() == workspace->root, "Shell manager belongs to a different workspace");
    workspace->shells = shells;
    require(privatePaths.size()<=128,"Too many host-private paths");
    for(const auto& path:privatePaths) {
        require(!path.isEmpty()&&!path.contains(QChar::Null)&&path.size()<=4096,"Invalid host-private path");
        workspace->privatePaths.append(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
        const auto canonical=QFileInfo(path).canonicalFilePath();if(!canonical.isEmpty())workspace->privatePaths.append(canonical);
    }
    workspace->privatePaths.removeDuplicates();
    Tool read;
    read.definition = {"Read", "Read a UTF-8 file (up to 1 MiB). Read the full file before editing it.",
        inputSchema({{"path", stringSchema()}, {"offset", integerSchema(1, 1000000)}, {"limit", integerSchema(1, 20000)}}, {"path"}), {}, true, true};
    read.execute = [workspace](const QJsonObject& a, const ToolContext& c) {
        c.cancellation.throwIfCancelled();
        std::lock_guard lock(workspace->mutex);
        const auto path = workspace->resolve(a["path"].toString(), c);
        const auto bytes = readFile(path); const auto text = decode(bytes); const auto lines = text.split('\n');
        const auto sha=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
        require(c.expectedReadSha256.isEmpty()||c.expectedReadSha256==sha,"File changed before it could be read");
        require(c.maxReadBytes>=1&&c.maxReadBytes<=maxFileBytes,"Invalid host read byte limit");
        const int offset = a["offset"].toInt(1), limit = a["limit"].toInt(2000);
        QStringList output;
        for (qsizetype n = offset - 1; n < lines.size() && n < qsizetype(offset - 1) + limit; ++n) output.append(lines[n]);
        auto excerpt=output.join('\n').toUtf8();const bool byteTruncated=excerpt.size()>c.maxReadBytes;
        if(byteTruncated) {
            excerpt.truncate(c.maxReadBytes);
            for(;;) {QStringDecoder decoder(QStringDecoder::Utf8,QStringConverter::Flag::Stateless);const QString decoded=decoder(excerpt);if(!decoder.hasError())break;excerpt.chop(1);}
        }
        const bool complete = offset == 1 && limit >= lines.size()&&!byteTruncated;
        workspace->remember(c, path, bytes, complete);
        return ToolResult{QString::fromUtf8(excerpt), {{"path", path}, {"offset", offset}, {"lines", byteTruncated?(excerpt.isEmpty()?0:excerpt.count('\n')+1):output.size()}, {"complete", complete},
            {"sha256",sha},{"truncated",!complete},{"bytes",excerpt.size()}}, false, {}, workspace->contextPaths(path,c)};
    }; read.definition.metadata = {{"source", "builtin.workspace"}}; preparePath(read, workspace, false);
    read.captureReadState=[workspace](const ToolContext& parent) {
        QHash<QString,Workspace::ReadState> observations;const auto prefix=workspace->key(parent,{});
        {std::lock_guard lock(workspace->mutex);const auto& source=workspace->observations(parent);for(auto it=source.cbegin();it!=source.cend();++it)
            if(it.key().startsWith(prefix))observations.insert(it.key().mid(prefix.size()),it.value());}
        return [workspace,observations](const ToolContext& child) {
            std::lock_guard lock(workspace->mutex);
            // Rebinding the same memory router may reach this native owner
            // twice. Install its first frozen copy once for this unique job ID.
            if(workspace->forkReads.contains(child.sessionId))return;
            require(workspace->forkReads.size()<64,"Native read-state fork capacity exhausted");
            auto& reads=workspace->forkReads[child.sessionId];
            for(auto it=observations.cbegin();it!=observations.cend();++it) {
                const auto id=workspace->key(child,it.key());
                reads.insert(id,it.value());
            }
        };
    };
    read.clearReadState=[workspace](const ToolContext& context) {
        std::lock_guard lock(workspace->mutex);const auto prefix=context.sessionId+QChar(0);
        workspace->forkReads.remove(context.sessionId);
        for(auto it=workspace->reads.begin();it!=workspace->reads.end();)if(it.key().startsWith(prefix))it=workspace->reads.erase(it);else ++it;
    };
    registry.add(std::move(read));
    Tool write;
    write.definition = {"Write", "Write a UTF-8 file. Existing files must have been read completely and remain unchanged.",
        inputSchema({{"path", stringSchema()}, {"content", stringSchema()}}, {"path", "content"}), {}, false, false, true};
    write.execute = [workspace](const QJsonObject& a, const ToolContext& c) {
        c.cancellation.throwIfCancelled(); std::lock_guard lock(workspace->mutex);
        const auto path = workspace->resolve(a["path"].toString(), c, true);
        std::optional<QByteArray> before;
        if (QFileInfo::exists(path)) before = workspace->writable(c, path);
        const auto backup = workspace->write(c, path, a["content"].toString().toUtf8(), before);
        return ToolResult{"Wrote " + path, {{"path", path}, {"backup_path", backup},
            {"sha256",QString::fromLatin1(QCryptographicHash::hash(a["content"].toString().toUtf8(),QCryptographicHash::Sha256).toHex())}}, false, {}, workspace->contextPaths(path,c)};
    }; write.definition.metadata = {{"source", "builtin.workspace"}}; preparePath(write, workspace, true); registry.add(std::move(write));
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
        return ToolResult{"Edited " + path, {{"path", path}, {"replacements", matches}, {"backup_path", backup},
            {"sha256",QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(),QCryptographicHash::Sha256).toHex())}}, false, {}, workspace->contextPaths(path,c)};
    }; edit.definition.metadata = {{"source", "builtin.workspace"}}; preparePath(edit, workspace, true); registry.add(std::move(edit));
    Tool notebook;
    notebook.definition={"NotebookEdit","Edit an observed Jupyter notebook cell. Read the complete notebook first. Actual cell IDs take precedence over zero-based cell-N selectors. Insert after cell_id, or at the beginning if omitted. Replacing code clears execution results; cells are never executed. Paths resolve in the current working directory.",
        inputSchema({{"notebook_path",stringSchema()},
            {"new_source",QJsonObject{{"type","string"},{"description","The actual decoded cell source, not a JSON string literal. When Read returns notebook JSON, decode the source field before editing. Do not copy the notebook JSON's escaping into the source characters. The tool stores this string exactly as supplied."}}},
            {"cell_id",stringSchema()},
            {"cell_type",QJsonObject{{"type","string"},{"enum",QJsonArray{"code","markdown"}}}},
            {"edit_mode",QJsonObject{{"type","string"},{"enum",QJsonArray{"replace","insert","delete"}}}}},{"notebook_path","new_source"}),{},false,false,true,true};
    notebook.validate=[](const QJsonObject& args,const ToolContext&){validateNotebookEditArguments(args);};
    notebook.execute=[workspace](const QJsonObject& args,const ToolContext& context) {
        context.cancellation.throwIfCancelled();std::lock_guard lock(workspace->mutex);
        const auto path=workspace->resolve(args["notebook_path"].toString(),context,true);const auto before=workspace->writable(context,path);
        const auto edited=editNotebook(before,args,context.cancellation);auto data=edited.metadata;
        const bool inlineFiles=before.size()+edited.content.size()<=32768;
        const auto snapshot=inlineFiles?QString():workspace->artifact(context,edited.content,"ipynb");
        const auto backup=workspace->write(context,path,edited.content,before);
        data["notebook_path"]=path;data["backup_path"]=backup;
        data["original_sha256"]=QString::fromLatin1(QCryptographicHash::hash(before,QCryptographicHash::Sha256).toHex());
        data["sha256"]=QString::fromLatin1(QCryptographicHash::hash(edited.content,QCryptographicHash::Sha256).toHex());
        data["files_inlined"]=inlineFiles;data["updated_file_path"]=snapshot;
        if(inlineFiles){data["original_file"]=decode(before);data["updated_file"]=decode(edited.content);}
        return ToolResult{"Notebook cell "+data["cell_id"].toString()+": "+data["edit_mode"].toString()+" completed.",data,false,{},workspace->contextPaths(path,context)};
    };
    notebook.definition.metadata={{"source","builtin.workspace"},{"search_hint","edit Jupyter notebook cells ipynb"}};
    preparePath(notebook,workspace,true,{},"notebook_path");registry.add(std::move(notebook));
    Tool glob;
    glob.definition = {"Glob", "List matching file paths relative to path (default: workspace). A ** path component matches zero or more directory levels; * and ? stay within one component. path must be an authorized working directory. Up to 1000 results and 10000 scanned files.",
        inputSchema({{"pattern", stringSchema()},{"path",stringSchema()}}, {"pattern"}), {}, true, true};
    glob.execute = [workspace](const QJsonObject& a, const ToolContext& c) {
        const auto root=workspace->resolve(a["path"].toString("."),c);require(QFileInfo(root).isDir(),"Glob path must be a directory");
        const auto regex = globExpression(a["pattern"].toString());
        require(regex.isValid(), "Invalid glob pattern"); QStringList paths;
        QDirIterator iterator(root, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);int visited=0;
        while (iterator.hasNext() && paths.size() < 1000 && visited++<10000) {
            c.cancellation.throwIfCancelled(); const auto path = iterator.next(); const auto relative = QDir(root).relativeFilePath(path);
            if(workspace->isPrivate(path,c)||(workspace->shells&&workspace->shells->containsStatePath(path)))continue;
            try { workspace->resolve(path,c); } catch(const Error&) { continue; }
            if (regex.match(relative).hasMatch()) paths.append(relative);
        }
        paths.sort(); QJsonArray values; for (const auto& path : paths) values.append(path);
        return ToolResult{paths.join('\n'), {{"paths", values}, {"truncated", iterator.hasNext()},{"root",root}}};
    }; glob.definition.metadata = {{"source", "builtin.workspace"}}; preparePath(glob,workspace,false,".");registry.add(std::move(glob));
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
            if(workspace->isPrivate(path,c)||(workspace->shells&&workspace->shells->containsStatePath(path)))continue;
            try { workspace->resolve(path,c); } catch(const Error&) { continue; }
            if (QFileInfo(path).size() > maxFileBytes) continue;
            QString text; try { text = decode(readFile(path)); } catch (const Error&) { continue; }
            const auto lines = text.split('\n');
            for (qsizetype n = 0; n < lines.size() && matches.size() < 100; ++n) if (regex.match(lines[n]).hasMatch()) {
                const auto relative = QDir(workspace->rootFor(c)).relativeFilePath(path);
                matches.append(relative + ':' + QString::number(n + 1) + ':' + lines[n]);
                values.append(QJsonObject{{"path", relative}, {"line", n + 1}, {"text", lines[n]}});
            }
            if (matches.size() >= 100) break;
        }
        return ToolResult{matches.join('\n'), {{"matches", values}, {"limit_reached", matches.size() >= 100}}};
    }; grep.definition.metadata = {{"source", "builtin.workspace"}}; preparePath(grep,workspace,false,".");registry.add(std::move(grep));
    Tool shell;
    shell.definition = {"Bash", "Execute a shell command in the workspace; this requires tool permission and is not an OS sandbox.",
        inputSchema({{"command", QJsonObject{{"type", "string"}, {"minLength", 1}}}, {"timeout_ms", integerSchema(1, 600000)}}, {"command"})};
    shell.definition.metadata = {{"source", "builtin.shell"}};
    if (shells) {
        auto properties = shell.definition.inputSchema["properties"].toObject();
        properties["run_in_background"] = QJsonObject{{"type", "boolean"}};
        properties["description"] = QJsonObject{{"type", "string"}, {"maxLength", 1024}};
        properties["timeout_ms"] = integerSchema(1, 86400000);
        shell.definition.inputSchema["properties"] = properties;
        shell.definition.description += " run_in_background=true returns a task ID and output file; use TaskOutput or TaskStop later. Foreground timeout_ms is at most 600000, background at most the host limit.";
    }
    shell.execute = [workspace](const QJsonObject& a, const ToolContext& c) {
        workspace->resolve(".", c);
        QProcessEnvironment environment;
        if(c.readOnlyShell) {
            auto scope=c;scope.protectedPaths.append(workspace->privatePaths);
            require(detail::readOnlyShell(a,scope),"Shell command is outside the host's read-only scope");
            environment.insert("PATH","/usr/bin:/bin");environment.insert("LANG","C");environment.insert("LC_ALL","C");
        }
        if (a["run_in_background"].toBool()) {
            require(bool(workspace->shells), "Background shell execution is disabled");
            auto data = workspace->shells->start(c, a["command"].toString(), a["description"].toString(), a["timeout_ms"].toInt(3600000));
            return ToolResult{QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)), data};
        }
        const int timeout = a["timeout_ms"].toInt(30000); require(timeout <= 600000, "Foreground shell timeout exceeds 600000 ms");
        QByteArray output, errors;
        const auto value = detail::shellProcess(workspace->rootFor(c), a["command"].toString(), timeout, c.cancellation, {}, [&](const QByteArray& bytes, bool error) {
            if (output.size() + errors.size() + bytes.size() > maxFileBytes) throw Error(ErrorCode::ResourceLimit, "Shell output exceeds 1 MiB");
            (error ? errors : output).append(bytes);
        },{},c.readOnlyShell?&environment:nullptr);
        const auto text = QString::fromUtf8(output) + (errors.isEmpty() ? QString() : "\n[stderr]\n" + QString::fromUtf8(errors));
        return ToolResult{text, {{"exit_code", value.code}, {"crashed", value.crashed}}, value.code != 0 || value.crashed};
    }; registry.add(std::move(shell));
    if (shells) registerShellTaskControls(registry, shells);
}
}
