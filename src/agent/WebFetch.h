#pragma once
#include "Tools.h"
#include <QtNetwork/QSslConfiguration>

namespace iiLocalLLM::agent {
struct WebFetchOptions {
    QString model; // Empty selects the host-bound session model. Never taken from tool arguments.
    bool deferred = true;
    int timeoutMs = 60000, summaryTimeoutMs = 60000;
    int maxContentBytes = 10 * 1024 * 1024;
    int maxMarkdownCharacters = 100000, maxOutputBytes = 400000, maxTokens = 2048;
    int maxRedirects = 10, maxConcurrentFetches = 2;
    int cacheTtlMs = 15 * 60 * 1000, maxCacheBytes = 50 * 1024 * 1024, maxCacheEntries = 128;
    // Exact host-configured origins, including scheme and effective port. These
    // permit private DNS results and HTTP only for that origin, not its redirects.
    QStringList privateOrigins;
    QSslConfiguration sslConfiguration = QSslConfiguration::defaultConfiguration();
};
// Anonymous bounded GET, HTML-to-Markdown and isolated local-model extraction.
// Each instance owns a bounded cache partitioned by ToolContext::sessionId.
// fetch() is a trusted C++ entrypoint. Use tool()/ToolRunner for policy and hooks.
class IILOCALLLM_EXPORT WebFetch {
public:
    WebFetch(std::shared_ptr<Model>, WebFetchOptions = {});
    Tool tool(bool deferred = true) const;
    ToolResult fetch(const QJsonObject&, const ToolContext&) const;
    void clearCache() const;
private:
    class Impl;
    std::shared_ptr<Impl> d;
};
}
