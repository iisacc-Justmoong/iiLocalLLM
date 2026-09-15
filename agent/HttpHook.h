#pragma once
#include "CommandHooks.h"
#include <QtCore/QMap>
#include <QtCore/QSet>
#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>

namespace iiLocalLLM::agent::detail {
struct HttpHook {
    QString url;
    QMap<QByteArray,QString> headers;
    QSet<QString> allowedEnvVars;
    bool permitted=true;
};
HttpHook parseHttpHook(const QJsonObject&,const QJsonObject& settings);
void validateHttpHookSettings(const QJsonObject&);
struct HttpHookResult {int status=0;QByteArray body;};
HttpHookResult postHttpHook(const HttpHook&,const QByteArray&,const CommandHookOptions&,
    int timeoutMs,const CancellationToken&,const std::function<void()>& started);
// Same direct-network ranges as the reference, including IPv4-mapped IPv6.
bool blockedHookAddress(const QHostAddress&);
}
