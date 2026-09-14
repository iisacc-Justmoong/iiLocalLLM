#pragma once
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace iiLocalLLM::agent::detail {
// MCP structuredContent is model-facing output, unlike host-only _meta.
// Keep an existing JSON text block (including pretty-printed JSON) exactly once.
inline QJsonArray withStructuredText(QJsonArray content, const QJsonObject& data) {
    if (data.isEmpty()) return content;
    for (const auto& value : content) {
        const auto block = value.toObject();
        if (block["type"] != "text") continue;
        const auto parsed = QJsonDocument::fromJson(block["text"].toString().toUtf8());
        if (parsed.isObject() && parsed.object() == data) return content;
    }
    content.append(QJsonObject{{"type", "text"},
        {"text", QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact))}});
    return content;
}
}
