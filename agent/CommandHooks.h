#pragma once
#include "Tools.h"
#include "AsyncHooks.h"
#include <QtCore/QProcessEnvironment>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QSslConfiguration>

namespace iiLocalLLM::agent {
struct CommandHookOptions {
    QString workingDirectory;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    int timeoutMs = 600000;
    int maxHooks = 128;
    int maxConcurrentProcesses = 4;
    int maxInputBytes = 1024 * 1024;
    int maxOutputBytes = 1024 * 1024;
    int maxOnceEntries = 32768;
    QSslConfiguration httpSslConfiguration = QSslConfiguration::defaultConfiguration();
    // nullopt reads captured HTTPS_PROXY/HTTP_PROXY and NO_PROXY. An explicit
    // NoProxy disables proxies; a host proxy delegates destination DNS/policy.
    std::optional<QNetworkProxy> httpProxy;
    int maxModelTokens = 1024;
    int maxAsyncRecords = 128;
};
// Explicit host configuration, frozen at construction. Supports command, HTTP
// and prompt/agent hooks. Commands receive JSON on stdin, HTTP endpoints a POST;
// model hooks use bounded host inference with an isolated decision schema.
// Model data is never substituted into commands, URLs or header templates.
class IILOCALLLM_EXPORT CommandHooks {
public:
    CommandHooks(QJsonObject settings, CommandHookOptions);
    Hook callback() const;
    QJsonObject describe() const; // Event/matcher metadata; no commands, prompts or environment values.
    QJsonArray asyncResults(const QString& sessionId = {}, bool consume = false) const;
    void close(); // Stop/join this configuration's command workers; affects all copies/callbacks.
private:
    class Impl;
    std::shared_ptr<Impl> d;
};
}
