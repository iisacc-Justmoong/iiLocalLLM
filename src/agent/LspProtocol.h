#pragma once
#include "Lsp.h"
namespace iiLocalLLM::agent::detail {
class LanguageServer {
public:
    LanguageServer(LspServerOptions,LspOptions,QString root);
    ~LanguageServer();
    QJsonValue query(QString operation,QString file,QString language,QString text,int line,int character,QString query,const CancellationToken&);
    QJsonObject snapshot() const;
    void close();
private:
    class Impl;
    std::unique_ptr<Impl> d;
};
}
