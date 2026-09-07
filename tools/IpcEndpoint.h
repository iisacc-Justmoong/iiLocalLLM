#pragma once
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
namespace iiLocalLLMClient {
inline QString defaultEndpoint()
{
    const auto configured = qEnvironmentVariable("IILLM_SOCKET");
    if (!configured.isEmpty()) return configured;
    const auto user = QCryptographicHash::hash(QDir::homePath().toUtf8(), QCryptographicHash::Sha256).toHex().first(12);
    const auto name = QStringLiteral("iiLocalLLMD-") + QString::fromLatin1(user);
#ifdef Q_OS_WIN
    return name;
#else
    return QDir::temp().filePath(name + QStringLiteral(".sock"));
#endif
}
}
