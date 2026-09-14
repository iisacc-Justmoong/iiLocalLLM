#pragma once
#include "PrivateFile.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <functional>

namespace iiLocalLLMClient {
inline bool containsPath(const QString& root, const QString& path) {
    return path == root || path.startsWith(root.endsWith('/') ? root : root + '/');
}
// Immutable host-owned app identities; remote clientInfo and appId are not credentials.
inline std::function<QString(const QByteArray&)> mcpCredentials(const QString& path, const QString& workspace) {
    const auto canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty() || containsPath(workspace, canonical))
        throw std::runtime_error("MCP credentials must be an existing private file outside the workspace");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(readPrivateFile(path), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()
        || document.object().isEmpty() || document.object().size() > 64)
        throw std::runtime_error("MCP credentials must map 1 to 64 client IDs to distinct private tokens");
    const QRegularExpression id(QRegularExpression::anchoredPattern("[A-Za-z0-9][A-Za-z0-9._-]{0,127}"));
    const QRegularExpression secret(QRegularExpression::anchoredPattern("[A-Za-z0-9_-]{32,256}"));
    QMap<QString, QByteArray> entries; QSet<QByteArray> unique;
    const auto object = document.object();
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!id.match(it.key()).hasMatch() || !it.value().isString() || !secret.match(it.value().toString()).hasMatch())
            throw std::runtime_error("MCP credential client ID or token has an invalid format");
        const auto digest = QCryptographicHash::hash(it.value().toString().toUtf8(), QCryptographicHash::Sha256);
        if (unique.contains(digest)) throw std::runtime_error("MCP client tokens must be distinct");
        unique.insert(digest); entries.insert(it.key(), digest);
    }
    return [entries](const QByteArray& token) {
        if (token.size() < 32 || token.size() > 256) return QString{};
        const auto digest = QCryptographicHash::hash(token, QCryptographicHash::Sha256);
        QString principal;
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            unsigned difference = 0;
            for (qsizetype n = 0; n < digest.size(); ++n) difference |= unsigned(uchar(digest[n]) ^ uchar(it.value()[n]));
            if (!difference) principal = it.key();
        }
        return principal;
    };
}
}
