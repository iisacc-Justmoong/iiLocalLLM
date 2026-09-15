#pragma once
#include "PrivateFile.h"
#include <agent/CommandHooks.h>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>

namespace iiLocalLLMClient {
inline QList<iiLocalLLM::agent::Hook> commandHookConfig(const QString& path,const QString& workspace,bool modelAvailable=true) {
    if(path.isEmpty())return {};
    const auto canonical=QFileInfo(path).canonicalFilePath();
    if(canonical.isEmpty()||canonical==workspace||canonical.startsWith(workspace.endsWith('/')?workspace:workspace+'/'))
        throw iiLocalLLM::Error(iiLocalLLM::ErrorCode::InvalidArgument,"Command hook configuration must be outside the workspace");
    QJsonParseError error;const auto document=QJsonDocument::fromJson(readPrivateFile(path,128*1024),&error);
    if(error.error!=QJsonParseError::NoError||!document.isObject())
        throw iiLocalLLM::Error(iiLocalLLM::ErrorCode::InvalidArgument,"Command hook configuration must be a private JSON object");
    iiLocalLLM::agent::CommandHookOptions options;options.workingDirectory=workspace;
    const iiLocalLLM::agent::CommandHooks hooks(document.object(),options);
    if(!modelAvailable)for(const auto& hook:hooks.describe()["hooks"].toArray())if(hook.toObject()["hook_type"]=="prompt"||hook.toObject()["hook_type"]=="agent")
        throw iiLocalLLM::Error(iiLocalLLM::ErrorCode::InvalidArgument,hook.toObject()["hook_type"]=="agent"?"Agent hooks require --model and --models":"Prompt hooks require --model and --models");
    return {hooks.callback()};
}
}
