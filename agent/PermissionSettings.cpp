#include "PermissionSettings.h"
#include "PermissionRules.h"
#include "ContextFile.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QSet>
#include <algorithm>
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
SettingsPermissionPolicy::SettingsPermissionPolicy(PermissionSettingsOptions options, QList<PermissionRule> cli, QList<PermissionRule> host)
    : options_(std::move(options)), cliRules_(std::move(cli)), hostRules_(std::move(host)) {
    require(options_.maxFileBytes > 0 && options_.maxFileBytes <= 1024 * 1024 && options_.maxTotalBytes >= options_.maxFileBytes
        && options_.maxTotalBytes <= 16 * 1024 * 1024 && options_.maxFiles > 0 && options_.maxFiles <= 1024, "Invalid permission settings limits");
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
    QList<Layer> layers; qint64 total = 0;
    auto add = [&](QString source, QString path, QString root, QJsonObject data, QString sha = {}) {
        token.throwIfCancelled(); require(layers.size() < options_.maxFiles, "Too many permission settings files", ErrorCode::ResourceLimit);
        validate(data); layers.append({std::move(source), std::move(path), std::move(root), std::move(sha), std::move(data)});
    };
    auto file = [&](const QString& source, const QString& path, const QString& root, bool required = false) {
        token.throwIfCancelled(); const QFileInfo info(path);
        const auto loaded=settingsBytes(root,path,options_.maxFileBytes,token);
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
    if (options_.modeOverride) modes.append(*options_.modeOverride);
    if (permissions.contains("defaultMode")) modes.append(parseMode(permissions["defaultMode"].toString()));
    modes.append(options_.fallbackMode); modes.append(PermissionMode::Default);
    for (const auto mode : modes) if (mode != PermissionMode::Bypass || !result.bypassDisabled) { result.mode = mode; break; }
    for (auto i = permissions.begin(); i != permissions.end(); ++i) {
        if (!QStringList{"allow", "deny", "ask", "defaultMode", "disableBypassPermissionsMode", "disableAutoMode"}.contains(i.key()))
            if (i.key() != "additionalDirectories" || !i.value().toArray().isEmpty()) result.unsupportedFeatures.append("permissions." + i.key());
    }
    for (const auto& layer : layers) {
        if (result.managedRulesOnly && layer.source != "policySettings") continue;
        const auto p = layer.data.value("permissions").toObject();
        for (auto behavior : {PermissionBehavior::Allow, PermissionBehavior::Deny, PermissionBehavior::Ask}) {
            QStringList values; for (const auto& value : p.value(behaviorName(behavior)).toArray()) values.append(value.toString());
            for (const auto& value : parsePermissionRules(values)) result.rules.append({value, behavior, layer.source, layer.root, options_.homeDirectory, true});
        }
    }
    if (!result.managedRulesOnly) for(auto rule:cliRules_) { rule.source="cliArg";result.rules.append(std::move(rule)); }
    for(auto rule:hostRules_) { rule.source="host";result.rules.append(std::move(rule)); }
    // Ordinary edit grants cannot rewrite the authority that grants them.
    // An embedding host's interactive approval callback can approve this Ask;
    // standalone DontAsk hosts deny it. Existing explicit denies still win.
    for(const auto& name:{"settings.json","settings.local.json"})
        result.rules.append({"Edit(**/.claude/"+QString(name)+")",PermissionBehavior::Ask,"host.settingsProtection",options_.workingDirectory,options_.homeDirectory,true});
    for(const auto& layer:layers)if(!layer.path.isEmpty()) {
        auto path=layer.path;path.replace("\\","\\\\").replace("(","\\(").replace(")","\\)");
        for(const auto& tool:{"Write","Edit"})result.rules.append({QString(tool)+'('+path+')',PermissionBehavior::Ask,"host.settingsProtection"});
    }
    result.unsupportedFeatures.removeDuplicates();
    RulePolicy(result.mode, result.rules); return result;
}
PermissionDecision SettingsPermissionPolicy::decide(const ToolDefinition& tool, const QJsonObject& args, const ToolContext& context) const {
    require(QFileInfo(context.workingDirectory).canonicalFilePath() == options_.workingDirectory, "Permission settings workspace mismatch");
    const auto current = snapshot(context.cancellation);
    for (const auto& feature : current.unsupportedFeatures) if (feature.startsWith("permissions."))
        throw Error(ErrorCode::RuntimeUnavailable, "Unsupported permission settings feature: " + feature);
    return RulePolicy(current.mode, current.rules).decide(tool, args, context);
}
QJsonObject SettingsPermissionPolicy::describe(const ToolContext& context) const {
    require(QFileInfo(context.workingDirectory).canonicalFilePath() == options_.workingDirectory, "Permission settings workspace mismatch");
    return snapshot(context.cancellation).toJson();
}
QJsonObject PermissionSettingsSnapshot::toJson() const {
    QJsonArray entries;
    for (const auto& rule : rules) entries.append(QJsonObject{{"rule", rule.toolPattern}, {"behavior", behaviorName(rule.behavior)},
        {"source", rule.source}, {"root_directory", rule.rootDirectory}, {"settings_syntax", rule.settingsSyntax}});
    return {{"provider","settings"},{"inspection_supported",true},{"mode", modeName(mode)}, {"managed_rules_only", managedRulesOnly}, {"bypass_disabled", bypassDisabled},
        {"rules", entries}, {"sources", sources}, {"unsupported_features", QJsonArray::fromStringList(unsupportedFeatures)}};
}
}
