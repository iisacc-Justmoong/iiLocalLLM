#pragma once
#include "PrivateFile.h"
#include <agent/PermissionSettings.h>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QSet>

namespace iiLocalLLMClient {
// Only a host command-line configuration can choose settings authority roots.
// Remote callers can inspect the result; they cannot provide this file or mode.
inline std::shared_ptr<const iiLocalLLM::agent::PermissionPolicy> permissionConfig(
    const QString& path,const QString& workspace,QList<iiLocalLLM::agent::PermissionRule> cli,
    QList<iiLocalLLM::agent::PermissionRule> host={},const QStringList& additionalDirectories={},
    iiLocalLLM::agent::PermissionMode fallback=iiLocalLLM::agent::PermissionMode::DontAsk) {
    using namespace iiLocalLLM; namespace a=agent;
    if(path.isEmpty()) {
        a::PermissionSettingsOptions options;options.workingDirectory=workspace;options.enabledSources.clear();
        options.fallbackMode=fallback;options.additionalDirectories=additionalDirectories;
        auto policy=std::make_shared<a::SettingsPermissionPolicy>(options,std::move(cli),std::move(host));
        (void)policy->snapshot();return policy;
    }
    const auto canonical=QFileInfo(path).canonicalFilePath();
    if(canonical.isEmpty()||canonical==workspace||canonical.startsWith(workspace.endsWith('/')?workspace:workspace+'/'))
        throw Error(ErrorCode::InvalidArgument,"Permission settings host configuration must be outside the workspace");
    QJsonParseError error; const auto document=QJsonDocument::fromJson(readPrivateFile(path,128*1024),&error);
    if(error.error!=QJsonParseError::NoError||!document.isObject())throw Error(ErrorCode::InvalidArgument,"Permission settings host configuration must be a JSON object");
    const auto object=document.object();
    const QSet<QString> known{"user_directory","managed_directory","home_directory","enabled_sources","flag_files","settings","mode"};
    for(auto i=object.begin();i!=object.end();++i)if(!known.contains(i.key()))throw Error(ErrorCode::InvalidArgument,"Unknown permission settings host field: "+i.key());
    a::PermissionSettingsOptions options;options.workingDirectory=workspace;options.fallbackMode=fallback;
    options.additionalDirectories=additionalDirectories;
    auto text=[](const QJsonValue& value) {
        if(!value.isString()||value.toString().trimmed().isEmpty()||value.toString().size()>4096||value.toString().contains(QChar::Null))
            throw Error(ErrorCode::InvalidArgument,"Invalid permission settings host string");
        return value.toString();
    };
    auto absolute=[&](const QJsonValue& value) {
        const auto s=text(value);if(s.startsWith('~')||s.contains("://"))throw Error(ErrorCode::InvalidArgument,"Permission settings directory must be a filesystem path");
        return QDir::cleanPath(QDir::isAbsolutePath(s)?s:QDir(QFileInfo(path).absolutePath()).filePath(s));
    };
    for(auto pair:{qMakePair("user_directory",&options.userDirectory),qMakePair("managed_directory",&options.managedDirectory),qMakePair("home_directory",&options.homeDirectory)})
        if(object.contains(pair.first))*pair.second=absolute(object[pair.first]);
    for(const auto& key:{"enabled_sources","flag_files"})if(object.contains(key)) {
        if(!object[key].isArray()||object[key].toArray().size()>128)throw Error(ErrorCode::InvalidArgument,"Invalid permission settings host array");
        QStringList values;for(const auto& value:object[key].toArray())values.append(QString(key)=="flag_files"?absolute(value):text(value));
        if(QString(key)=="flag_files")options.flagFiles=values;else options.enabledSources=values;
    }
    if(object.contains("settings")) {
        if(!object["settings"].isObject())throw Error(ErrorCode::InvalidArgument,"Host settings must be an object");
        options.inlineSettings=object["settings"].toObject();
    }
    if(object.contains("mode")) {
        const QMap<QString,a::PermissionMode> modes{{"default",a::PermissionMode::Default},{"acceptEdits",a::PermissionMode::AcceptEdits},
            {"dontAsk",a::PermissionMode::DontAsk},{"bypassPermissions",a::PermissionMode::Bypass},{"plan",a::PermissionMode::Plan}};
        const auto name=text(object["mode"]);if(!modes.contains(name))throw Error(ErrorCode::InvalidArgument,"Unsupported host permission mode");options.modeOverride=modes[name];
    }
    auto policy=std::make_shared<a::SettingsPermissionPolicy>(options,std::move(cli),std::move(host));
    // Validate before constructing the service or exposing a transport.
    const auto snapshot=policy->snapshot();
    for(const auto& feature:snapshot.unsupportedFeatures)if(feature.startsWith("permissions."))
        throw Error(ErrorCode::RuntimeUnavailable,"Unsupported permission settings feature: "+feature);
    return policy;
}
}
