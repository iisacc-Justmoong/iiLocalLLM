#include "PermissionSettings.h"
#include "PermissionRules.h"
#include "ContextFile.h"
#include "PermissionResponses.h"
#include "PermissionSettingsFile.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QSet>
#include <algorithm>
#include <mutex>
#include <map>
#ifdef Q_OS_UNIX
#include <dirent.h>
#include <cerrno>
#endif

namespace iiLocalLLM::agent {
namespace {
void require(bool condition, const QString& text, ErrorCode code = ErrorCode::InvalidArgument) {
    if (!condition) throw Error(code, text);
}
QString modeName(PermissionMode mode) {
    switch (mode) {
    case PermissionMode::AcceptEdits: return "acceptEdits";
    case PermissionMode::DontAsk: return "dontAsk";
    case PermissionMode::Bypass: return "bypassPermissions";
    case PermissionMode::Plan: return "plan";
    default: return "default";
    }
}
PermissionMode parseMode(const QString& value) {
    for (auto mode : {PermissionMode::Default, PermissionMode::AcceptEdits, PermissionMode::DontAsk, PermissionMode::Bypass, PermissionMode::Plan})
        if (modeName(mode) == value) return mode;
    throw Error(ErrorCode::RuntimeUnavailable, "Unsupported permission mode: " + value);
}
QString behaviorName(PermissionBehavior behavior) {
    return behavior == PermissionBehavior::Allow ? "allow" : behavior == PermissionBehavior::Deny ? "deny" : "ask";
}
struct Layer { QString source, path, root, sha; QJsonObject data; };
std::optional<QByteArray> settingsBytes(const QString& root, const QString& path, int maximum, const CancellationToken& token) {
    token.throwIfCancelled();
#ifdef Q_OS_UNIX
    // The host chooses root. Every component below it is opened without
    // following links. Only ENOENT means an optional source is absent.
    struct stat status{};
    if (::stat(QFile::encodeName(root).constData(),&status)) {
        if(errno==ENOENT)return std::nullopt;
        throw Error(ErrorCode::StorageFailure,"Cannot inspect settings root");
    }
    require(S_ISDIR(status.st_mode),"Settings root is not a directory",ErrorCode::StorageFailure);
    const auto relative=QDir(root).relativeFilePath(path);
    require(relative!=".."&&!relative.startsWith("../")&&!QDir::isAbsolutePath(relative),"Settings path escapes its root");
    const auto canonical=QFileInfo(root).canonicalFilePath();
    int fd=::open(QFile::encodeName(canonical).constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    require(fd>=0,"Cannot open settings root",ErrorCode::StorageFailure);
    const auto parts=relative.split('/');
    for(qsizetype i=0;i<parts.size();++i) {
        const int next=::openat(fd,QFile::encodeName(parts[i]).constData(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|(i+1<parts.size()?O_DIRECTORY:O_NONBLOCK));
        const int error=errno;::close(fd);fd=next;
        if(fd<0) { if(error==ENOENT)return std::nullopt; throw Error(ErrorCode::StorageFailure,"Cannot safely open settings file"); }
    }
    if(::fstat(fd,&status)||!S_ISREG(status.st_mode)) { ::close(fd);throw Error(ErrorCode::StorageFailure,"Settings must be a regular file"); }
    QFile file;
    if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)) { ::close(fd);throw Error(ErrorCode::StorageFailure,"Cannot read settings file"); }
    auto bytes=file.read(qint64(maximum)+1); token.throwIfCancelled();
    require(file.error()==QFileDevice::NoError,"Cannot read settings file",ErrorCode::StorageFailure);
    require(bytes.size()<=maximum&&file.atEnd(),"Settings file exceeds byte limit",ErrorCode::ResourceLimit);
    return bytes;
#else
    const QFileInfo info(path);
    if(!info.exists()&&!info.isSymLink())return std::nullopt;
    require(!info.isSymLink(),"Settings links are unsupported");
    return detail::readContextFile(QFileInfo(root).canonicalFilePath(),path,maximum,token);
#endif
}
QStringList fragments(const QString& root,int maximum,const CancellationToken& token) {
    QStringList paths; const auto path=root+"/managed-settings.d";
#ifdef Q_OS_UNIX
    const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0) { if(errno==ENOENT)return {};throw Error(ErrorCode::StorageFailure,"Cannot open managed settings fragments"); }
    DIR* directory=::fdopendir(fd);
    if(!directory) { ::close(fd);throw Error(ErrorCode::StorageFailure,"Cannot enumerate managed settings fragments"); }
    try {
        for(;;) {
            token.throwIfCancelled();errno=0;const auto* entry=::readdir(directory);
            if(!entry) { require(errno==0,"Cannot enumerate managed settings fragments",ErrorCode::StorageFailure);break; }
            const auto name=QFile::decodeName(entry->d_name);
            if(name.startsWith('.')||!name.endsWith(".json"))continue;
            require(paths.size()<maximum,"Too many managed settings fragments",ErrorCode::ResourceLimit);
            paths.append(path+'/'+name);
        }
    } catch(...) { ::closedir(directory);throw; }
    ::closedir(directory);
#else
    const QDir directory(path);
    require(!directory.exists()||directory.isReadable(),"Cannot enumerate managed settings fragments",ErrorCode::StorageFailure);
    for(const auto& name:directory.entryList({"*.json"},QDir::Files|QDir::System|QDir::NoDotAndDotDot)) {
        require(paths.size()<maximum,"Too many managed settings fragments",ErrorCode::ResourceLimit);paths.append(directory.filePath(name));
    }
#endif
    std::sort(paths.begin(),paths.end());return paths;
}
QJsonObject merge(QJsonObject base, const QJsonObject& next) {
    for (auto i = next.begin(); i != next.end(); ++i) {
        if (i->isObject() && base.value(i.key()).isObject()) base[i.key()] = merge(base.value(i.key()).toObject(), i->toObject());
        else if (i->isArray() && base.value(i.key()).isArray()) {
            auto values = base.value(i.key()).toArray();
            for (const auto& value : i->toArray()) if (!values.contains(value)) values.append(value);
            base[i.key()] = values;
        } else base[i.key()] = i.value();
    }
    return base;
}
void validate(const QJsonObject& data) {
    const auto managed = data.value("allowManagedPermissionRulesOnly");
    require(managed.isUndefined() || managed.isBool(), "allowManagedPermissionRulesOnly must be boolean");
    const auto value = data.value("permissions");
    require(value.isUndefined() || value.isObject(), "permissions must be an object");
    const auto p = value.toObject();
    for (const auto& key : {"allow", "deny", "ask", "additionalDirectories"}) {
        const auto list = p.value(key); require(list.isUndefined() || list.isArray(), QString("permissions.") + key + " must be an array");
        QStringList strings;
        for (const auto& item : list.toArray()) {
            require(item.isString() && !item.toString().contains(QChar::Null), "Permission list entries must be strings");
            strings.append(item.toString());
        }
        if (QString(key) != "additionalDirectories") parsePermissionRules(strings);
    }
    const auto mode = p.value("defaultMode");
    require(mode.isUndefined() || mode.isString(), "permissions.defaultMode must be a string");
    if (mode.isString()) parseMode(mode.toString());
    for (const auto& key : {"disableBypassPermissionsMode", "disableAutoMode"}) {
        const auto flag = p.value(key); require(flag.isUndefined() || flag == "disable", QString("Invalid permissions.") + key);
    }
}
}
struct SettingsPermissionPolicy::Runtime {
    QJsonObject permissions;
    std::optional<QList<PermissionRule>> cliRules;
    std::optional<QStringList> cliDirectories;
    std::optional<PermissionMode> mode;
    QSet<QString> removedDirectories;
    QHash<QString,QString> targets;
};
struct SettingsPermissionPolicy::State {
    std::mutex mutex;
    QHash<QString,QString> targets;
    QHash<QString,Runtime> sessions;
};
SettingsPermissionPolicy::SettingsPermissionPolicy(PermissionSettingsOptions options, QList<PermissionRule> cli, QList<PermissionRule> host)
    : options_(std::move(options)), cliRules_(std::move(cli)), hostRules_(std::move(host)),state_(std::make_shared<State>()) {
    require(options_.maxFileBytes > 0 && options_.maxFileBytes <= 1024 * 1024 && options_.maxTotalBytes >= options_.maxFileBytes
        && options_.maxTotalBytes <= 16 * 1024 * 1024 && options_.maxFiles > 0 && options_.maxFiles <= 1024
        && options_.maxDirectories > 0 && options_.maxDirectories <= 1024
        && options_.additionalDirectories.size() <= options_.maxDirectories
        && options_.maxRuntimeSessions>0&&options_.maxRuntimeSessions<=65536
        && options_.updateLockTimeoutMs>0&&options_.updateLockTimeoutMs<=60000, "Invalid permission settings limits");
    options_.workingDirectory = QFileInfo(options_.workingDirectory).canonicalFilePath();
    require(!options_.workingDirectory.isEmpty() && QFileInfo(options_.workingDirectory).isDir(), "Permission settings workspace must exist");
    for (const auto& source : options_.enabledSources) require(QStringList{"user", "project", "local"}.contains(source), "Unknown permission setting source: " + source);
    options_.enabledSources.removeDuplicates();
    for (auto* path : {&options_.userDirectory, &options_.managedDirectory, &options_.homeDirectory})
        if (!path->isEmpty()) *path = QDir::cleanPath(QFileInfo(*path).absoluteFilePath());
    for (auto& path : options_.flagFiles) { require(!path.trimmed().isEmpty(), "Empty settings file path"); path = QFileInfo(path).absoluteFilePath(); }
    RulePolicy(PermissionMode::Default, cliRules_ + hostRules_);
}
PermissionSettingsSnapshot SettingsPermissionPolicy::snapshot(const CancellationToken& token) const {
    std::lock_guard guard(state_->mutex);Runtime runtime;return snapshotLocked({{}, {},options_.workingDirectory,{},token},runtime,{});
}
PermissionSettingsSnapshot SettingsPermissionPolicy::sessionSnapshot(const ToolContext& context) const {
    std::lock_guard guard(state_->mutex);auto runtime=state_->sessions.value(context.sessionId);
    auto result=snapshotLocked(context,runtime,{});
    if(state_->sessions.contains(context.sessionId))state_->sessions[context.sessionId].targets=std::move(runtime.targets);
    return result;
}
PermissionSettingsSnapshot SettingsPermissionPolicy::snapshotLocked(const ToolContext& context,Runtime& runtime,const QHash<QString,QJsonObject>& replacements) const {
    const auto& token=context.cancellation;token.throwIfCancelled();
    auto directoryTargets=state_->targets, runtimeTargets=runtime.targets;
    QList<Layer> layers; qint64 total = 0;
    auto add = [&](QString source, QString path, QString root, QJsonObject data, QString sha = {}) {
        token.throwIfCancelled(); require(layers.size() < options_.maxFiles, "Too many permission settings files", ErrorCode::ResourceLimit);
        validate(data); layers.append({std::move(source), std::move(path), std::move(root), std::move(sha), std::move(data)});
    };
    auto file = [&](const QString& source, const QString& path, const QString& root, bool required = false) {
        token.throwIfCancelled(); const QFileInfo info(path);
        const auto loaded=replacements.contains(path)?std::optional<QByteArray>(QJsonDocument(replacements[path]).toJson()):settingsBytes(root,path,options_.maxFileBytes,token);
        if(!loaded) { require(!required,"Settings file does not exist: "+path);return; }
        const auto& bytes=*loaded;
        total += bytes.size(); require(total <= options_.maxTotalBytes, "Permission settings exceed byte limit", ErrorCode::ResourceLimit);
        QJsonObject data;
        if (!bytes.trimmed().isEmpty()) {
            QJsonParseError error; const auto document = QJsonDocument::fromJson(bytes, &error);
            require(error.error == QJsonParseError::NoError && document.isObject(), "Settings file must contain a JSON object: " + path);
            data = document.object();
        }
        add(source, path, source == "userSettings" ? options_.userDirectory : source == "flagSettings" ? info.absolutePath() : options_.workingDirectory,
            data, QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
    };
    if (options_.enabledSources.contains("user") && !options_.userDirectory.isEmpty()) file("userSettings", options_.userDirectory + "/settings.json", options_.userDirectory);
    if (options_.enabledSources.contains("project")) file("projectSettings", options_.workingDirectory + "/.claude/settings.json", options_.workingDirectory);
    if (options_.enabledSources.contains("local")) file("localSettings", options_.workingDirectory + "/.claude/settings.local.json", options_.workingDirectory);
    for (const auto& path : options_.flagFiles) file("flagSettings", path, QFileInfo(path).absolutePath(), true);
    if (!options_.inlineSettings.isEmpty()) {
        const auto bytes = QJsonDocument(options_.inlineSettings).toJson(QJsonDocument::Compact); total += bytes.size();
        require(bytes.size() <= options_.maxFileBytes && total <= options_.maxTotalBytes, "Inline settings exceed byte limit", ErrorCode::ResourceLimit);
        add("flagSettings", {}, options_.workingDirectory, options_.inlineSettings, QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
    }
    if (!options_.managedDirectory.isEmpty()) {
        file("policySettings", options_.managedDirectory + "/managed-settings.json", options_.managedDirectory);
        for(const auto& path:fragments(options_.managedDirectory,options_.maxFiles,token)) file("policySettings",path,options_.managedDirectory,true);
    }
    if(!runtime.permissions.isEmpty())add("session",{},options_.workingDirectory,{{"permissions",runtime.permissions}});
    PermissionSettingsSnapshot result; QJsonObject merged, managed;
    for (const auto& layer : layers) {
        merged = merge(merged, layer.data);
        if (layer.source == "policySettings") managed = merge(managed, layer.data);
        result.sources.append(QJsonObject{{"source", layer.source}, {"path", layer.path}, {"root_directory", layer.root}, {"sha256", layer.sha}});
        for (auto i = layer.data.begin(); i != layer.data.end(); ++i)
            if (i.key() != "permissions" && i.key() != "allowManagedPermissionRulesOnly" && i.key() != "$schema")
                result.unsupportedFeatures.append(i.key());
    }
    result.managedRulesOnly = managed.value("allowManagedPermissionRulesOnly").toBool();
    const auto permissions = merged.value("permissions").toObject();
    result.bypassDisabled = permissions.value("disableBypassPermissionsMode") == "disable";
    QList<PermissionMode> modes;
    if(runtime.mode)modes.append(*runtime.mode);
    if (options_.modeOverride) modes.append(*options_.modeOverride);
    if (permissions.contains("defaultMode")) modes.append(parseMode(permissions["defaultMode"].toString()));
    modes.append(options_.fallbackMode); modes.append(PermissionMode::Default);
    for (const auto mode : modes) if (mode != PermissionMode::Bypass || !result.bypassDisabled) { result.mode = mode; break; }
    for (auto i = permissions.begin(); i != permissions.end(); ++i) {
        if (!QStringList{"allow", "deny", "ask", "defaultMode", "disableBypassPermissionsMode", "disableAutoMode", "additionalDirectories"}.contains(i.key()))
            result.unsupportedFeatures.append("permissions." + i.key());
    }
    auto absoluteDirectory=[&](QString input) {
        if(input.startsWith('~')) {
            require((input=="~"||input.startsWith("~/"))&&!options_.homeDirectory.isEmpty(),"Additional directory home expansion requires an explicit homeDirectory");
            input=input=="~"?options_.homeDirectory:QDir(options_.homeDirectory).filePath(input.mid(2));
        }
        return QDir::cleanPath(QDir::isAbsolutePath(input)?input:QDir(options_.workingDirectory).filePath(input));
    };
    QSet<QString> runtimeDirectories;
    for(const auto& input:runtime.permissions.value("additionalDirectories").toArray())runtimeDirectories.insert(absoluteDirectory(input.toString()));
    if(runtime.cliDirectories)for(const auto& input:*runtime.cliDirectories)runtimeDirectories.insert(absoluteDirectory(input));
    result.workingDirectories={options_.workingDirectory};QSet<QString> directoryInputs,activeBindings,activeRuntime;
    auto directory=[&](const QString& input,const QString& source,const QString& authority,bool ephemeral=false,bool visible=true) {
        token.throwIfCancelled();
        require(input.size()<=4096&&!input.contains(QChar::Null)&&!input.contains("://"),"Invalid additional directory path");
        const bool duplicate=directoryInputs.contains(input);
        if(!duplicate&&visible) {
            require(directoryInputs.size()<options_.maxDirectories,"Too many additional directories",ErrorCode::ResourceLimit);
            directoryInputs.insert(input);
        }
        QJsonObject entry{{"input",input},{"source",source}};
        if(input.trimmed().isEmpty()) { entry["status"]="empty";if(!duplicate&&visible)result.additionalDirectories.append(entry);return; }
        const auto path=absoluteDirectory(input);auto& targets=ephemeral?runtimeTargets:directoryTargets;
        const auto bindingKey=authority+QChar::Null+input;(ephemeral?activeRuntime:activeBindings).insert(bindingKey);
        const QFileInfo info(path);const auto canonical=info.canonicalFilePath();entry["path"]=path;
        if(canonical.isEmpty()||!info.exists())entry["status"]="not_found";
        else if(!info.isDir())entry["status"]="not_directory";
        else {
            // A source's directory grant stays bound to its first resolved target.
            // A changed settings digest is new host authority; a changed symlink is not.
            auto bound=targets.value(bindingKey);
            if(bound.isEmpty()) {bound=canonical;targets.insert(bindingKey,bound);}
            entry["canonical_path"]=bound;
            if(bound!=canonical)entry["observed_canonical_path"]=canonical;
            auto covered=[&](const QString& value) {return std::any_of(result.workingDirectories.cbegin(),result.workingDirectories.cend(),[&](const auto& root) {
                return value==root||value.startsWith(root.endsWith('/')?root:root+'/');
            });};
            const bool already=covered(path)&&covered(bound);
            const bool removed=runtime.removedDirectories.contains(path);
            entry["status"]=removed?"removed":bound!=canonical?"target_changed":already?"already_covered":"active";
            if(!already&&!removed&&visible) {result.workingDirectories.append(path);result.workingDirectories.append(bound);result.workingDirectories.removeDuplicates();}
        }
        // Deduplicate inspection entries, never the authority bindings: removing
        // one settings source must not make an existing CLI grant bind anew.
        if(!duplicate&&visible)result.additionalDirectories.append(entry);
    };
    // Directory arrays are merged independently of the managed-only tool-rule filter.
    for(const auto& layer:layers)
        for(const auto& value:layer.data.value("permissions").toObject().value("additionalDirectories").toArray())
            directory(value.toString(),layer.source,layer.source=="session"?QString("session"):layer.source+QChar::Null+layer.path+QChar::Null+layer.sha,
                layer.source=="session",layer.source=="session"||!runtimeDirectories.contains(absoluteDirectory(value.toString())));
    for(const auto& value:options_.additionalDirectories)directory(value,"cliArg","cliArg",false,!runtime.cliDirectories&&!runtimeDirectories.contains(absoluteDirectory(value)));
    if(runtime.cliDirectories)for(const auto& value:*runtime.cliDirectories)directory(value,"cliArg","cliArg",true);
    for(auto i=directoryTargets.begin();i!=directoryTargets.end();)
        if(!activeBindings.contains(i.key()))i=directoryTargets.erase(i);else ++i;
    for(auto i=runtimeTargets.begin();i!=runtimeTargets.end();)
        if(!activeRuntime.contains(i.key()))i=runtimeTargets.erase(i);else ++i;
    for (const auto& layer : layers) {
        if (result.managedRulesOnly && layer.source != "policySettings") continue;
        const auto p = layer.data.value("permissions").toObject();
        for (auto behavior : {PermissionBehavior::Allow, PermissionBehavior::Deny, PermissionBehavior::Ask}) {
            QStringList values; for (const auto& value : p.value(behaviorName(behavior)).toArray()) values.append(value.toString());
            for (const auto& value : parsePermissionRules(values)) result.rules.append({value, behavior, layer.source, layer.root, options_.homeDirectory, true});
        }
    }
    if (!result.managedRulesOnly) for(auto rule:runtime.cliRules.value_or(cliRules_)) { rule.source="cliArg";result.rules.append(std::move(rule)); }
    for(auto rule:hostRules_) { rule.source="host";result.rules.append(std::move(rule)); }
    // Ordinary edit grants cannot rewrite the authority that grants them.
    // An embedding host's interactive approval callback can approve this Ask;
    // standalone DontAsk hosts deny it. Existing explicit denies still win.
    for(const auto& name:{"settings.json","settings.local.json",".settings.json.iillm-permissions.lock",".settings.local.json.iillm-permissions.lock",".settings.json.*.tmp",".settings.local.json.*.tmp"})
        result.rules.append({"Edit(//**/.claude/"+QString(name)+")",PermissionBehavior::Ask,"host.settingsProtection",options_.workingDirectory,options_.homeDirectory,true});
    for(const auto& layer:layers)if(!layer.path.isEmpty()) {
        auto path=layer.path;path.replace("\\","\\\\").replace("(","\\(").replace(")","\\)");
        for(const auto& tool:{"Write","Edit"})result.rules.append({QString(tool)+'('+path+')',PermissionBehavior::Ask,"host.settingsProtection"});
        const auto lockPath=QFileInfo(layer.path).absolutePath()+"/."+QFileInfo(layer.path).fileName()+".iillm-permissions.lock";
        auto escaped=lockPath;escaped.replace("\\","\\\\").replace("(","\\(").replace(")","\\)");
        for(const auto& tool:{"Write","Edit"})result.rules.append({QString(tool)+'('+escaped+')',PermissionBehavior::Ask,"host.settingsProtection"});
    }
    result.unsupportedFeatures.removeDuplicates();
    RulePolicy(result.mode, result.rules);state_->targets=std::move(directoryTargets);runtime.targets=std::move(runtimeTargets);return result;
}
PermissionDecision SettingsPermissionPolicy::decide(const ToolDefinition& tool, const QJsonObject& args, const ToolContext& context) const {
    require(QFileInfo(context.workingDirectory).canonicalFilePath() == options_.workingDirectory, "Permission settings workspace mismatch");
    const auto current = sessionSnapshot(context);
    for (const auto& feature : current.unsupportedFeatures) if (feature.startsWith("permissions."))
        throw Error(ErrorCode::RuntimeUnavailable, "Unsupported permission settings feature: " + feature);
    auto scoped=context;scoped.workingDirectories=current.workingDirectories+PermissionPolicy::workingDirectories(context);
    scoped.workingDirectories.removeDuplicates();
    return RulePolicy(current.mode, current.rules).decide(tool, args, scoped);
}
QStringList SettingsPermissionPolicy::workingDirectories(const ToolContext& context) const {
    require(QFileInfo(context.workingDirectory).canonicalFilePath() == options_.workingDirectory, "Permission settings workspace mismatch");
    const auto current=sessionSnapshot(context);
    for(const auto& feature:current.unsupportedFeatures)if(feature.startsWith("permissions."))
        throw Error(ErrorCode::RuntimeUnavailable,"Unsupported permission settings feature: "+feature);
    auto paths=current.workingDirectories+PermissionPolicy::workingDirectories(context);paths.removeDuplicates();return paths;
}
QJsonObject SettingsPermissionPolicy::describe(const ToolContext& context) const {
    require(QFileInfo(context.workingDirectory).canonicalFilePath() == options_.workingDirectory, "Permission settings workspace mismatch");
    auto current=sessionSnapshot(context);if(context.permissionMode)current.mode=*context.permissionMode;
    return current.toJson();
}
namespace {
QString normalizedRule(QString value) {
    const auto parsed=parsePermissionRules({value});require(parsed.size()==1,"Permission update requires one rule");value=parsed.first();
    for(const auto& suffix:{QString("(*)"),QString("()")})if(value.endsWith(suffix)&&!value.left(value.size()-suffix.size()).contains('('))value.chop(suffix.size());
    return value;
}
QString updateRule(const QJsonObject& value) {
    const auto tool=value["toolName"].toString();
    require(!tool.contains('(')&&!tool.contains(')')&&parsePermissionRules({tool})==QStringList{tool},"Invalid permission update tool name");
    auto content=value["ruleContent"].toString();if(content.isEmpty()||content=="*")return tool;
    content.replace("\\","\\\\").replace("(","\\(").replace(")","\\)");return normalizedRule(tool+'('+content+')');
}
QStringList strings(const QJsonArray& values){QStringList result;for(const auto& value:values)result.append(value.toString());return result;}
void checkUpdateContext(const PermissionSettingsOptions& options,const ToolContext& context) {
    context.cancellation.throwIfCancelled();
    require(!context.sessionId.isEmpty()&&context.sessionId.size()<=512&&!context.sessionId.contains(QChar::Null),"Permission updates require a session identity");
    require(QFileInfo(context.workingDirectory).canonicalFilePath()==options.workingDirectory,"Permission settings workspace mismatch");
}
}
void SettingsPermissionPolicy::applyUpdates(const QJsonArray& updates,const ToolContext& context) const {
    checkUpdateContext(options_,context);detail::validatePermissionUpdates(updates);if(updates.isEmpty())return;
    std::lock_guard guard(state_->mutex);
    require(state_->sessions.contains(context.sessionId)||state_->sessions.size()<options_.maxRuntimeSessions,"Permission runtime session limit reached",ErrorCode::ResourceLimit);
    auto candidate=state_->sessions.value(context.sessionId);const auto initial=snapshotLocked(context,candidate,{});
    for(const auto& feature:initial.unsupportedFeatures)if(feature.startsWith("permissions."))throw Error(ErrorCode::RuntimeUnavailable,"Unsupported permission settings feature: "+feature);
    auto absolute=[&](QString input) {
        require(!input.trimmed().isEmpty()&&input.size()<=4096&&!input.contains(QChar::Null)&&!input.contains("://"),"Invalid permission update directory");
        if(input.startsWith('~')) {
            require((input=="~"||input.startsWith("~/"))&&!options_.homeDirectory.isEmpty(),"Additional directory home expansion requires an explicit homeDirectory");
            input=input=="~"?options_.homeDirectory:QDir(options_.homeDirectory).filePath(input.mid(2));
        }
        return QDir::cleanPath(QDir::isAbsolutePath(input)?input:QDir(options_.workingDirectory).filePath(input));
    };
    struct Target {QString root,destination;std::unique_ptr<detail::PermissionSettingsFile> file;};
    std::map<QString,Target> targets;QHash<QString,QString> paths;
    // Resolve and validate every destination before opening any file for write.
    for(const auto& value:updates) {
        const auto update=value.toObject();const auto destination=update["destination"].toString();QString path,root;
        if(destination=="userSettings") {
            require(options_.enabledSources.contains("user")&&!options_.userDirectory.isEmpty(),"Permission update destination is disabled: userSettings");
            root=options_.userDirectory;path=root+"/settings.json";
        } else if(destination=="projectSettings"||destination=="localSettings") {
            const bool project=destination=="projectSettings";require(options_.enabledSources.contains(project?"project":"local"),"Permission update destination is disabled: "+destination);
            root=options_.workingDirectory;path=root+(project?"/.claude/settings.json":"/.claude/settings.local.json");
        }
        if(update["type"]=="setMode"&&update["mode"]=="bypassPermissions")require(!initial.bypassDisabled,"Managed policy disables bypass permissions");
        for(const auto& rule:update["rules"].toArray())(void)updateRule(rule.toObject());
        for(const auto& directory:update["directories"].toArray())(void)absolute(directory.toString());
        if(path.isEmpty())continue;
        require(!targets.contains(path)||targets.at(path).destination==destination,"Permission destinations refer to the same file");
        targets.try_emplace(path,Target{root,destination,{}});paths[destination]=path;
    }
    QHash<QString,QJsonObject> proposed;
    for(auto& [path,target]:targets) {
        target.file=std::make_unique<detail::PermissionSettingsFile>(target.root,path,options_.maxFileBytes,options_.updateLockTimeoutMs,context.cancellation);
        QJsonObject data;
        if(target.file->bytes()&&!target.file->bytes()->trimmed().isEmpty()) {
            QJsonParseError error;const auto document=QJsonDocument::fromJson(*target.file->bytes(),&error);
            require(error.error==QJsonParseError::NoError&&document.isObject(),"Settings file must contain a JSON object");data=document.object();
        }
        validate(data);proposed[path]=data;
    }
    for(const auto& value:updates) {
        context.cancellation.throwIfCancelled();const auto update=value.toObject();
        const auto type=update["type"].toString(),destination=update["destination"].toString(),behavior=update["behavior"].toString();
        const auto filePath=paths.value(destination);auto permissions=filePath.isEmpty()?candidate.permissions:proposed[filePath]["permissions"].toObject();
        if(type.endsWith("Rules")) {
            const auto flag=behavior=="allow"?PermissionBehavior::Allow:behavior=="deny"?PermissionBehavior::Deny:PermissionBehavior::Ask;
            QStringList wanted;for(const auto& item:update["rules"].toArray()){const auto rule=updateRule(item.toObject());if(!wanted.contains(rule))wanted.append(rule);}
            if(destination=="cliArg") {
                auto cli=candidate.cliRules.value_or(cliRules_);
                cli.removeIf([&](const PermissionRule& rule){return rule.behavior==flag&&(type=="replaceRules"||(type=="removeRules"&&wanted.contains(normalizedRule(rule.toolPattern))));});
                if(type!="removeRules")for(const auto& rule:wanted) {
                    const bool exists=std::any_of(cli.cbegin(),cli.cend(),[&](const auto& r){return r.behavior==flag&&normalizedRule(r.toolPattern)==rule;});
                    if(!exists)cli.append({rule,flag,"cliArg",options_.workingDirectory,options_.homeDirectory,true});
                }
                candidate.cliRules=std::move(cli);
            } else {
                auto previous=parsePermissionRules(strings(permissions[behavior].toArray()));for(auto& rule:previous)rule=normalizedRule(rule);previous.removeDuplicates();
                if(type=="replaceRules")previous=wanted;
                else if(type=="removeRules")previous.removeIf([&](const QString& rule){return wanted.contains(rule);});
                else for(const auto& rule:wanted)if(!previous.contains(rule))previous.append(rule);
                permissions[behavior]=QJsonArray::fromStringList(previous);
            }
        } else if(type=="setMode") {
            candidate.mode=parseMode(update["mode"].toString());if(!filePath.isEmpty())permissions["defaultMode"]=update["mode"];
        } else {
            QStringList wanted;for(const auto& item:update["directories"].toArray()){const auto path=absolute(item.toString());if(!wanted.contains(path))wanted.append(path);}
            auto previous=destination=="cliArg"?candidate.cliDirectories.value_or(options_.additionalDirectories):strings(permissions["additionalDirectories"].toArray());
            if(destination=="cliArg"&&!candidate.cliDirectories)for(const auto& input:previous) {
                const auto key=QString("cliArg")+QChar::Null+input;if(state_->targets.contains(key))candidate.targets[key]=state_->targets[key];
            }
            if(type=="addDirectories")for(const auto& path:wanted) {
                previous.removeIf([&](const QString& old){return absolute(old)==path;});previous.append(path);candidate.removedDirectories.remove(path);
                // This explicit new grant may bind to a newly selected target.
                for(auto it=candidate.targets.begin();it!=candidate.targets.end();)
                    if(it.key().startsWith(destination+QChar::Null)&&absolute(it.key().section(QChar::Null,1))==path)it=candidate.targets.erase(it);else ++it;
            } else {
                previous.removeIf([&](const QString& old){return wanted.contains(absolute(old));});
                for(const auto& path:wanted)candidate.removedDirectories.insert(path);
            }
            if(destination=="cliArg")candidate.cliDirectories=std::move(previous);
            else permissions["additionalDirectories"]=QJsonArray::fromStringList(previous);
        }
        if(!filePath.isEmpty()) {auto document=proposed[filePath];document["permissions"]=permissions;validate(document);proposed[filePath]=document;}
        else if(destination=="session")candidate.permissions=std::move(permissions);
    }
    qint64 runtimeBytes=QJsonDocument(candidate.permissions).toJson(QJsonDocument::Compact).size();
    if(candidate.cliRules)for(const auto& rule:*candidate.cliRules)runtimeBytes+=rule.toolPattern.size()*2;
    if(candidate.cliDirectories)for(const auto& directory:*candidate.cliDirectories)runtimeBytes+=directory.size()*2;
    for(const auto& directory:candidate.removedDirectories)runtimeBytes+=directory.size()*2;
    require(runtimeBytes<=options_.maxFileBytes&&candidate.removedDirectories.size()<=options_.maxDirectories,"Permission runtime exceeds limit",ErrorCode::ResourceLimit);
    for(const auto& data:proposed)require(QJsonDocument(data).toJson().size()<=options_.maxFileBytes,"Updated permission settings exceed file limit",ErrorCode::ResourceLimit);
    const auto previousTargets=state_->targets;QStringList committed;
    try {
        // Preflight the complete effective policy under the same in-process lock.
        // No persistent document is changed until every proposed layer validates.
        const auto effective=snapshotLocked(context,candidate,proposed);
        if(candidate.mode==PermissionMode::Bypass)require(!effective.bypassDisabled,"Managed policy disables bypass permissions");
        context.cancellation.throwIfCancelled();
        // Once publication starts, finish the admitted batch despite cancellation.
        // Files are individually atomic; an I/O failure can still leave a prefix.
        for(auto& [path,target]:targets){target.file->commit(QJsonDocument(proposed[path]).toJson());committed.append(target.destination);}
        state_->sessions[context.sessionId]=std::move(candidate);
    } catch(const Error& error) {
        state_->targets=previousTargets;
        throw Error(error.code(),QString::fromUtf8(error.what())+(committed.isEmpty()?QString():"; already committed: "+committed.join(", ")));
    } catch(...) {state_->targets=previousTargets;throw;}
}
void SettingsPermissionPolicy::inheritSession(const ToolContext& parent,const ToolContext& child) const {
    checkUpdateContext(options_,parent);checkUpdateContext(options_,child);if(parent.sessionId==child.sessionId)return;
    std::lock_guard guard(state_->mutex);
    if(!state_->sessions.contains(parent.sessionId)){state_->sessions.remove(child.sessionId);return;}
    require(state_->sessions.contains(child.sessionId)||state_->sessions.size()<options_.maxRuntimeSessions,"Permission runtime session limit reached",ErrorCode::ResourceLimit);
    state_->sessions[child.sessionId]=state_->sessions.value(parent.sessionId);
}
void SettingsPermissionPolicy::forgetSession(const ToolContext& context) const {
    checkUpdateContext(options_,context);std::lock_guard guard(state_->mutex);state_->sessions.remove(context.sessionId);
}
QJsonObject PermissionSettingsSnapshot::toJson() const {
    QJsonArray entries;
    for (const auto& rule : rules) entries.append(QJsonObject{{"rule", rule.toolPattern}, {"behavior", behaviorName(rule.behavior)},
        {"source", rule.source}, {"root_directory", rule.rootDirectory}, {"settings_syntax", rule.settingsSyntax}});
    return {{"provider","settings"},{"inspection_supported",true},{"mode", modeName(mode)}, {"managed_rules_only", managedRulesOnly}, {"bypass_disabled", bypassDisabled},
        {"rules", entries}, {"sources", sources}, {"unsupported_features", QJsonArray::fromStringList(unsupportedFeatures)},
        {"working_directories",QJsonArray::fromStringList(workingDirectories)},{"additional_directories",additionalDirectories}};
}
}
