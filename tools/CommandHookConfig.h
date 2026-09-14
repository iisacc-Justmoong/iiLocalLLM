#pragma once
#include "PrivateFile.h"
#include <agent/CommandHooks.h>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>

namespace iiLocalLLMClient {
inline QList<iiLocalLLM::agent::Hook> commandHookConfig(const QString& path,const QString& workspace) {
    if(path.isEmpty())return {};
    const auto canonical=QFileInfo(path).canonicalFilePath();
    if(canonical.isEmpty()||canonical==workspace||canonical.startsWith(workspace.endsWith('/')?workspace:workspace+'/'))
        throw iiLocalLLM::Error(iiLocalLLM::ErrorCode::InvalidArgument,"Command hook configuration must be outside the workspace");
    QJsonParseError error;const auto document=QJsonDocument::fromJson(readPrivateFile(path,128*1024),&error);
    if(error.error!=QJsonParseError::NoError||!document.isObject())
        throw iiLocalLLM::Error(iiLocalLLM::ErrorCode::InvalidArgument,"Command hook configuration must be a private JSON object");
    iiLocalLLM::agent::CommandHookOptions options;options.workingDirectory=workspace;
    return {iiLocalLLM::agent::CommandHooks(document.object(),options).callback()};
}
}
