#pragma once
#include "Tools.h"
#include <QtCore/QProcessEnvironment>

namespace iiLocalLLM::agent {
struct LspServerOptions {
    QString name, command;
    QStringList arguments;
    QMap<QString,QString> extensions; // Including leading dot; value is the LSP languageId.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    QJsonObject initializationOptions, settings;
};
struct LspOptions {
    QList<LspServerOptions> servers; // Trusted host configuration, never model arguments.
    bool deferred=true;
    QStringList protectedPaths;
    int requestTimeoutMs=30000, startupTimeoutMs=15000;
    int maxMessageBytes=16*1024*1024, maxDocumentBytes=10000000;
    int maxResultCharacters=100000, maxInstances=16, maxDocuments=128, maxQueuedRequests=16;
};
// Persistent language servers are isolated by owner session, workspace and config.
// Construct/destroy off the UI thread. query() is a trusted host entrypoint;
// tool()/ToolRunner additionally apply hooks and invocation permission requests.
class IILOCALLLM_EXPORT Lsp {
public:
    explicit Lsp(LspOptions,std::shared_ptr<const PermissionPolicy> = {});
    ~Lsp();
    Lsp(const Lsp&)=delete;
    Lsp& operator=(const Lsp&)=delete;
    Tool tool(bool deferred=true) const;
    ToolResult query(const QJsonObject&,const ToolContext&) const;
    QJsonObject status(const ToolContext&) const;
    void closeSession(const QString&);
    void close();
private:
    class Impl;
    std::shared_ptr<Impl> d;
};
// Strict schema for explicitly host-selected configuration JSON. No discovery,
// downloads or evaluation of project commands/environment expansion occurs.
IILOCALLLM_EXPORT LspOptions lspOptionsFromJson(const QJsonObject&);
}
