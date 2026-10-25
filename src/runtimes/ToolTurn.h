#pragma once
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>
#include <optional>

namespace iiLocalLLM::detail {
// A single-call JSON/XML turn yields to its executor at the closed envelope.
// The payload must be a complete JSON object; a delimiter inside a quoted
// argument is not a boundary. Other native template formats keep EOS handling.
inline std::optional<qsizetype> completeSingleToolTurn(const QString& prefill,const QString& generated,const QStringList& tools) {
    static const QString open="<tool_call>",close="</tool_call>";
    qsizetype begin=0;
    if(prefill.trimmed().endsWith(open)||generated.trimmed().startsWith('{'))begin=0;
    else {const auto tag=generated.indexOf(open);if(tag<0)return {};begin=tag+open.size();}
    for(auto end=generated.indexOf(close,begin);end>=0;end=generated.indexOf(close,end+close.size())) {
        QJsonParseError error;const auto document=QJsonDocument::fromJson(generated.mid(begin,end-begin).trimmed().toUtf8(),&error);
        if(error.error!=QJsonParseError::NoError||!document.isObject())continue;
        const auto call=document.object();
        if(call.size()==2&&call.value("name").isString()&&tools.contains(call.value("name").toString())&&call.value("arguments").isObject())
            return end+close.size();
    }
    return {};
}
}
