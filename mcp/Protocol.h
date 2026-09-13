#pragma once
#include "../Types.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QDir>
#include <QtCore/QSet>
#include <QtCore/QUrl>
#include <cmath>

namespace iiLocalLLM::mcp::detail {
inline void require(bool condition, const QString& message, ErrorCode code = ErrorCode::ProtocolError) {
    if (!condition) throw Error(code, message);
}
inline QString key(const QJsonValue& id) {
    if (id.isString()) return "s:" + id.toString();
    if (id.isDouble() && std::isfinite(id.toDouble()) && std::trunc(id.toDouble()) == id.toDouble()
        && std::abs(id.toDouble()) <= 9007199254740991.) return "n:" + QString::number(id.toDouble(), 'f', 0);
    throw Error(ErrorCode::ProtocolError, "JSON-RPC IDs must be strings or safe integers");
}
inline QByteArray encode(const QJsonObject& message, int limit) {
    auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    require(bytes.size() <= limit, "MCP message exceeds the configured frame limit", ErrorCode::ResourceLimit);
    return bytes + '\n';
}
inline QByteArray encodeValue(const QJsonValue& message, int limit) {
    require(message.isObject() || message.isArray(), "Invalid JSON-RPC frame");
    const auto bytes = (message.isObject() ? QJsonDocument(message.toObject()) : QJsonDocument(message.toArray())).toJson(QJsonDocument::Compact);
    require(bytes.size() <= limit, "MCP message exceeds the configured frame limit", ErrorCode::ResourceLimit);
    return bytes + '\n';
}
inline QJsonObject notification(const QString& method, const QJsonObject& params = {}) {
    QJsonObject o{{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.isEmpty()) o["params"] = params;
    return o;
}
inline QJsonObject rpcError(const QJsonValue& id, int code, const QString& message, const QJsonValue& data = {}) {
    QJsonObject error{{"code", code}, {"message", message}};
    if (!data.isUndefined() && !data.isNull()) error["data"] = data;
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", error}};
}
inline void validateRoots(const QJsonArray& roots) {
    QSet<QString> seen;
    for (const auto& value : roots) {
        const auto root = value.toObject(); const QUrl url(root["uri"].toString(), QUrl::StrictMode);
        require(value.isObject() && root["uri"].isString() && url.isValid() && url.isLocalFile()
            && !url.hasQuery() && !url.hasFragment() && url.userInfo().isEmpty()
            && QDir::isAbsolutePath(url.toLocalFile()) && !url.toLocalFile().contains(QChar::Null)
            && !url.toLocalFile().split('/').contains("..")
            && !seen.contains(root["uri"].toString()) && (!root.contains("name") || root["name"].isString()),
            "MCP roots must have distinct file: URIs and optional string names", ErrorCode::InvalidArgument);
        seen.insert(root["uri"].toString());
    }
}
inline void validateContent(const QJsonValue& value) {
    require(value.isObject(), "MCP content must be an object");
    const auto b = value.toObject(); const auto type = b["type"].toString();
    if (type == "text") require(b["text"].isString(), "MCP text content is missing text");
    else if (type == "image" || type == "audio")
        require(b["data"].isString() && b["mimeType"].isString(), "MCP media content is incomplete");
    else if (type == "resource_link")
        require(b["uri"].isString() && b["name"].isString(), "MCP resource link is incomplete");
    else if (type == "resource") {
        const auto r = b["resource"].toObject();
        require(r["uri"].isString() && (r["text"].isString() != r["blob"].isString()), "MCP embedded resource is incomplete");
    } else throw Error(ErrorCode::ProtocolError, "Unknown MCP content type: " + type);
}
}
