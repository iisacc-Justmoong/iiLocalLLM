#pragma once
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include "../mcp/Protocol.h"
#include <optional>

namespace iiLocalLLM::agent::detail {
inline QString mcpTextContent(const QJsonArray& content) {
    QStringList parts;
    for (const auto& value : content) {
        const auto block = value.toObject(); const auto type = block["type"].toString();
        if (type == "text") parts.append(block["text"].toString());
        else if (type == "resource_link") parts.append("Resource: " + block["name"].toString() + " (" + block["uri"].toString() + ")");
        else if (type == "resource" && block["resource"].toObject()["text"].isString()) {
            const auto resource = block["resource"].toObject();
            parts.append(resource["uri"].toString() + "\n" + resource["text"].toString());
        }
    }
    return parts.join('\n');
}
// Match the reference's false-value no-op; an empty array is an intentional clear.
// Validate before merging hook results so a malformed late reply cannot discard
// a successful earlier replacement. Native C++ callbacks use the same check.
inline std::optional<QJsonArray> mcpOutputContent(const QJsonValue& value) {
    if (value.isNull() || value.isUndefined() || (value.isBool() && !value.toBool())
        || (value.isDouble() && value.toDouble() == 0) || (value.isString() && value.toString().isEmpty())) return std::nullopt;
    if (value.isString()) return QJsonArray{QJsonObject{{"type", "text"}, {"text", value}}};
    if (!value.isArray()) throw Error(ErrorCode::ProtocolError, "updatedMCPToolOutput requires a string or MCP content array");
    const auto content = value.toArray();
    for (const auto& block : content) mcp::detail::validateContent(block);
    return content;
}
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
