#pragma once
#include "PermissionRequests.h"
#include <QtCore/QObject>
#include <QtCore/QVariant>

namespace iiLocalLLM::agent {
// GUI-thread adapter for native AskUserQuestion. Hooks block only their worker;
// the UI reads stable snapshots and submits answers on this object's thread.
class IILOCALLLM_EXPORT QuestionInbox final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList requests READ requests NOTIFY requestsChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorChanged)
public:
    explicit QuestionInbox(PermissionRequestsOptions = {},QObject* parent = nullptr);
    explicit QuestionInbox(std::shared_ptr<PermissionRequests>,QObject* parent = nullptr);
    ~QuestionInbox() override;
    Hook hook() const; // Only handles native AskUserQuestion permission requests.
    QVariantList requests() const;
    QString errorString() const;
    Q_INVOKABLE void refresh();
    Q_INVOKABLE bool submit(const QString& requestId,const QVariantMap& answers,const QVariantMap& annotations = {});
    Q_INVOKABLE bool reject(const QString& requestId,const QString& reason = {});
    Q_INVOKABLE void close();
signals:
    void requestsChanged();
    void errorChanged();
private:
    QuestionInbox(std::shared_ptr<PermissionRequests>,bool owned,QObject* parent);
    bool finish(const QString&,const QVariantMap&,const QVariantMap&,const std::optional<QString>&);
    void error(QString);
    class Impl;
    std::unique_ptr<Impl> d;
};
}
