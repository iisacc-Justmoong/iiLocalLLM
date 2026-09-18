#pragma once
#include "Tools.h"
#include <QtCore/QJsonDocument>

namespace iiLocalLLM::agent::detail {
// Shared validation for trusted C++ responses and decoded command output. This
// validates the reference wire shape, not the host's ability to persist it.
inline void validatePermissionUpdates(const QJsonArray& updates) {
    auto require=[](bool value){if(!value)throw Error(ErrorCode::InvalidArgument,"Invalid permission update");};
    require(updates.size()<=256&&QJsonDocument(updates).toJson(QJsonDocument::Compact).size()<=65536);
    auto string=[&](const QJsonValue& value,int limit=4096){require(value.isString()&&!value.toString().isEmpty()
        &&!value.toString().contains(QChar::Null)&&value.toString().size()<=limit);};
    auto keys=[&](const QJsonObject& object,const QStringList& names){for(auto i=object.begin();i!=object.end();++i)require(names.contains(i.key()));};
    for(const auto& value:updates) {
        require(value.isObject());const auto update=value.toObject();
        require(QStringList{"userSettings","projectSettings","localSettings","session","cliArg"}.contains(update["destination"].toString()));
        const auto type=update["type"].toString();
        if(QStringList{"addRules","replaceRules","removeRules"}.contains(type)) {
            keys(update,{"type","destination","behavior","rules"});require(QStringList{"allow","deny","ask"}.contains(update["behavior"].toString()));
            require(update["rules"].isArray()&&update["rules"].toArray().size()<=256);
            for(const auto& item:update["rules"].toArray()) {
                require(item.isObject());const auto rule=item.toObject();keys(rule,{"toolName","ruleContent"});string(rule["toolName"],512);
                if(rule.contains("ruleContent")) {require(rule["ruleContent"].isString());const auto content=rule["ruleContent"].toString();require(content.size()<=4096&&!content.contains(QChar::Null));}
            }
        } else if(type=="setMode") {
            keys(update,{"type","destination","mode"});require(QStringList{"default","acceptEdits","bypassPermissions","plan","dontAsk"}.contains(update["mode"].toString()));
        } else if(type=="addDirectories"||type=="removeDirectories") {
            keys(update,{"type","destination","directories"});require(update["directories"].isArray()&&update["directories"].toArray().size()<=128);
            for(const auto& directory:update["directories"].toArray())string(directory);
        } else require(false);
    }
}
inline void validatePermissionResponse(const PermissionResponse& response) {
    if((response.behavior!=PermissionBehavior::Allow&&response.behavior!=PermissionBehavior::Deny)
        ||(response.behavior==PermissionBehavior::Allow&&response.interrupt)
        ||(response.behavior==PermissionBehavior::Deny&&(response.updatedArguments||!response.updatedPermissions.isEmpty()))
        ||response.message.size()>65536||response.message.contains(QChar::Null)
        ||(response.updatedArguments&&QJsonDocument(*response.updatedArguments).toJson(QJsonDocument::Compact).size()>4*1024*1024))
        throw Error(ErrorCode::InvalidArgument,"Invalid permission response");
    validatePermissionUpdates(response.updatedPermissions);
}
}
