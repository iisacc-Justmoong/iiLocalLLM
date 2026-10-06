#pragma once
#include "PrivateFile.h"
#include <agent/Procedures.h>
#include <QtCore/QJsonDocument>
#include <QtCore/QFileInfo>
#include <cmath>
namespace iiLocalLLMClient {
inline iiLocalLLM::agent::ProcedureOptions proceduresConfig(const QString& path,const QString& workspace){
    using namespace iiLocalLLM;agent::ProcedureOptions options;if(path.isEmpty())return options;
    const auto canonical=QFileInfo(path).canonicalFilePath();
    if(canonical.isEmpty()||canonical==workspace||canonical.startsWith(workspace.endsWith('/')?workspace:workspace+'/'))throw Error(ErrorCode::InvalidArgument,"Procedure host configuration must be outside the workspace");
    QJsonParseError error;const auto document=QJsonDocument::fromJson(readPrivateFile(path,128*1024),&error);
    if(error.error!=QJsonParseError::NoError||!document.isObject())throw Error(ErrorCode::InvalidArgument,"Procedure configuration must be a JSON object");
    const QMap<QString,int*> numbers{{"timeout_ms",&options.timeoutMs},{"max_records",&options.maxRecords},{"max_record_bytes",&options.maxRecordBytes},{"max_bytes",&options.maxBytes}};
    const auto object=document.object();for(auto it=object.begin();it!=object.end();++it){
        if(it.key()=="intercept"){
            if(!it.value().isArray())throw Error(ErrorCode::InvalidArgument,"Procedure intercept must be an array");
            for(const auto& value:it.value().toArray()){if(!value.isString())throw Error(ErrorCode::InvalidArgument,"Procedure intercept kinds must be strings");options.intercept.append(value.toString());}
        }else{
            if(!numbers.contains(it.key())||!it.value().isDouble()||it.value().toDouble()<1||it.value().toDouble()>268435456||std::floor(it.value().toDouble())!=it.value().toDouble())throw Error(ErrorCode::InvalidArgument,"Invalid procedure configuration field: "+it.key());
            *numbers[it.key()]=it.value().toInt();
        }
    }
    agent::Procedures validate(options);return options;
}
}
