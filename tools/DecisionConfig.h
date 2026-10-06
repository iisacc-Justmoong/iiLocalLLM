#pragma once
#include "PrivateFile.h"
#include <agent/DecisionProtocol.h>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
namespace iiLocalLLMClient {
inline iiLocalLLM::agent::DecisionOptions decisionConfig(const QString& path,const QString& workspace){
    using namespace iiLocalLLM;if(path.isEmpty())return {};
    const auto canonical=QFileInfo(path).canonicalFilePath();
    if(canonical.isEmpty()||canonical==workspace||canonical.startsWith(workspace.endsWith('/')?workspace:workspace+'/'))throw Error(ErrorCode::InvalidArgument,"Decision host configuration must be outside the agent workspace");
    QJsonParseError error;auto document=QJsonDocument::fromJson(readPrivateFile(path,1024*1024),&error);
    if(error.error!=QJsonParseError::NoError||!document.isObject())throw Error(ErrorCode::InvalidArgument,"Decision configuration must be a JSON object");
    return agent::decisionOptionsFromJson(document.object());
}
}
