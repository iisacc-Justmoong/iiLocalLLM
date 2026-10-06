#pragma once
#include "Types.h"

namespace iiLocalLLM::agent {
enum class ProcedureAction { Continue, Replace, Cancel };
struct ProcedureResponse {
    ProcedureAction action = ProcedureAction::Continue;
    QJsonObject output;
    QString reason;
};
struct ProcedureOptions {
    QStringList intercept; // Host-selected return boundaries; empty observes without waiting.
    int timeoutMs = 120000;
    int maxRecords = 1024;
    int maxRecordBytes = 8 * 1024 * 1024;
    int maxBytes = 32 * 1024 * 1024;
};
using ProcedureCallback = std::function<ProcedureResponse(const QJsonObject&,const CancellationToken&)>;
// A host-owned, bounded return channel. Records are transient, not authority or
// a durable work queue. One API client owns one channel, including its children.
class IILOCALLLM_EXPORT Procedures final {
    struct Impl;
public:
    using Validator = std::function<void(const QJsonObject&)>;
    explicit Procedures(ProcedureOptions = {}, ProcedureCallback = {});
    ~Procedures();
    Procedures(const Procedures&) = delete;
    Procedures& operator=(const Procedures&) = delete;
    QJsonObject list(const QString& ownerSessionId, qint64 after = 0, int limit = 128) const;
    QJsonObject respond(const QString& procedureId,const QJsonObject& response,const QString& ownerSessionId = {});
    void close();
private:
    friend class ProcedureScope;
    QJsonObject begin(const QString&,const ToolContext&,const QJsonObject&);
    QJsonObject finish(const QString&,const QJsonObject&,Validator,const CancellationToken&,const EventCallback&);
    void fail(const QString&,const QString&,bool,const EventCallback&,ErrorCode) noexcept;
    std::shared_ptr<Impl> d;
};
// A lexical procedure. Unwinding always closes its record; callbacks must not
// synchronously wait for, destroy or re-enter the owning Engine's session.
class IILOCALLLM_EXPORT ProcedureScope final {
public:
    ProcedureScope(QString kind,const ToolContext&,QJsonObject input = {},EventCallback = {});
    ~ProcedureScope();
    ProcedureScope(const ProcedureScope&) = delete;
    ProcedureScope& operator=(const ProcedureScope&) = delete;
    QString id() const {return id_;}
    QJsonObject returned(QJsonObject,Procedures::Validator = {});
    void fail(QString reason,bool cancelled = false,ErrorCode = ErrorCode::RuntimeFailure) noexcept;
private:
    std::shared_ptr<Procedures> channel_;
    QString id_;
    CancellationToken token_;
    EventCallback events_;
    bool ended_ = false;
};
}
