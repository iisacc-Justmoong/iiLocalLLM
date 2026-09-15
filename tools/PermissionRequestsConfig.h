#pragma once
#include "PrivateFile.h"
#include <agent/PermissionRequests.h>
#include <QtCore/QJsonDocument>
#include <QtCore/QFileInfo>
#include <cmath>

namespace iiLocalLLMClient {
inline std::optional<iiLocalLLM::agent::PermissionRequestsOptions> permissionRequestsConfig(const QString& path,const QString& workspace) {
    using namespace iiLocalLLM;
    if(path.isEmpty())return std::nullopt;
    const auto canonical=QFileInfo(path).canonicalFilePath();
    if(canonical.isEmpty()||canonical==workspace||canonical.startsWith(workspace.endsWith('/')?workspace:workspace+'/'))
        throw Error(ErrorCode::InvalidArgument,"Permission request host configuration must be outside the workspace");
    QJsonParseError error;const auto document=QJsonDocument::fromJson(readPrivateFile(path,128*1024),&error);
    if(error.error!=QJsonParseError::NoError||!document.isObject())throw Error(ErrorCode::InvalidArgument,"Permission request configuration must be a JSON object");
    agent::PermissionRequestsOptions options;
    const QMap<QString,int*> fields{{"timeout_ms",&options.timeoutMs},{"max_pending",&options.maxPending},
        {"max_history",&options.maxHistory},{"max_request_bytes",&options.maxRequestBytes},{"max_pending_bytes",&options.maxPendingBytes}};
    const auto object=document.object();
    for(auto it=object.begin();it!=object.end();++it) {
        if(!fields.contains(it.key())||!it.value().isDouble()||it.value().toDouble()<1||it.value().toDouble()>67108864
            ||std::floor(it.value().toDouble())!=it.value().toDouble())throw Error(ErrorCode::InvalidArgument,"Invalid permission request setting: "+it.key());
        *fields[it.key()]=it.value().toInt();
    }
    agent::PermissionRequests validate(options);return options;
}
}
