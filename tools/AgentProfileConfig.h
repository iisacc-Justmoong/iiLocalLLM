#pragma once
#include "PrivateFile.h"
#include <agent/Subagents.h>
#include <QtCore/QJsonDocument>
#include <QtCore/QDir>
#include <QtCore/QSet>
namespace iiLocalLLMClient {
// Standalone hosts opt in to project profiles. User/managed/plugin directories
// and model grants come only from this private, host-owned configuration file.
inline iiLocalLLM::agent::SubagentOptions profileConfig(const QString& path, bool disabled) {
    using namespace iiLocalLLM;agent::SubagentOptions result;result.profiles.enabled=!disabled;
    if(path.isEmpty())return result;
    if(disabled)throw Error(ErrorCode::InvalidArgument,"--agent-profiles and --no-agent-profiles cannot be combined");
    QJsonParseError error;const auto doc=QJsonDocument::fromJson(readPrivateFile(path,1024*1024),&error);
    if(error.error!=QJsonParseError::NoError||!doc.isObject())throw Error(ErrorCode::InvalidArgument,"Agent profiles configuration must be a JSON object");
    const auto o=doc.object();const QSet<QString> known{"include_builtins","include_project","project_boundary","user_directory","managed_directory","directories","plugin_directories","overrides","model_aliases","allowed_models"};
    for(auto i=o.begin();i!=o.end();++i)if(!known.contains(i.key()))throw Error(ErrorCode::InvalidArgument,"Unknown agent profiles configuration field: "+i.key());
    auto flag=[&](const QString& key,bool& target){if(!o.contains(key))return;if(!o[key].isBool())throw Error(ErrorCode::InvalidArgument,"Agent profiles flag must be boolean: "+key);target=o[key].toBool();};
    auto text=[](const QJsonValue& value){if(!value.isString()||value.toString().trimmed().isEmpty()||value.toString().size()>4096||value.toString().contains(QChar::Null))throw Error(ErrorCode::InvalidArgument,"Invalid agent profiles configuration string");return value.toString();};
    const auto base=QFileInfo(path).absolutePath();
    auto absolute=[&](const QJsonValue& value){const auto s=text(value);if(s.startsWith('~')||s.contains("://"))throw Error(ErrorCode::InvalidArgument,"Agent profile directory must be a filesystem path");return QDir::isAbsolutePath(s)?s:QDir(base).filePath(s);};
    auto directory=[&](const QString& key,QString& target){if(o.contains(key))target=absolute(o[key]);};
    auto strings=[&](const QString& key,QStringList& target,bool paths){if(!o.contains(key))return;if(!o[key].isArray()||o[key].toArray().size()>128)throw Error(ErrorCode::InvalidArgument,"Invalid agent profiles configuration array: "+key);for(const auto& v:o[key].toArray())target.append(paths?absolute(v):text(v));};
    flag("include_builtins",result.profiles.includeBuiltins);flag("include_project",result.profiles.includeProject);
    directory("project_boundary",result.profiles.projectBoundary);directory("user_directory",result.profiles.userDirectory);directory("managed_directory",result.profiles.managedDirectory);
    strings("directories",result.profiles.directories,true);strings("plugin_directories",result.profiles.pluginDirectories,true);strings("allowed_models",result.allowedModels,false);
    for(const auto& key:{"overrides","model_aliases"})if(o.contains(key)&&!o[key].isObject())throw Error(ErrorCode::InvalidArgument,"Agent profiles field must be an object: "+QString(key));
    result.profiles.overrides=o["overrides"].toObject();const auto aliases=o["model_aliases"].toObject();
    if(aliases.size()>128)throw Error(ErrorCode::ResourceLimit,"Too many agent model aliases");
    for(auto i=aliases.begin();i!=aliases.end();++i){const auto key=text(i.key()),value=text(i.value());if(key.size()>256||value.size()>256)throw Error(ErrorCode::InvalidArgument,"Agent model alias exceeds limit");result.modelAliases[key]=value;}
    for(const auto& model:result.allowedModels)if(model.size()>256)throw Error(ErrorCode::InvalidArgument,"Allowed agent model exceeds limit");
    return result;
}
}
