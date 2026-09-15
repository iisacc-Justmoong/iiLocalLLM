#include "QuestionInbox.h"
#include "UserQuestions.h"
#include <QtCore/QTimer>
#include <QtCore/QThread>

namespace iiLocalLLM::agent {
class QuestionInbox::Impl {
public:
    std::shared_ptr<PermissionRequests> broker;
    bool owned;
    QJsonArray snapshot;
    QVariantList rows;
    QString error;
    explicit Impl(std::shared_ptr<PermissionRequests> value,bool own):broker(std::move(value)),owned(own) {
        if(!broker)throw Error(ErrorCode::InvalidArgument,"Question inbox requires a request channel");
    }
};
QuestionInbox::QuestionInbox(PermissionRequestsOptions options,QObject* parent)
    :QuestionInbox(std::make_shared<PermissionRequests>(options),true,parent){}
QuestionInbox::QuestionInbox(std::shared_ptr<PermissionRequests> broker,QObject* parent)
    :QuestionInbox(std::move(broker),false,parent){}
QuestionInbox::QuestionInbox(std::shared_ptr<PermissionRequests> broker,bool owned,QObject* parent)
    :QObject(parent),d(std::make_unique<Impl>(std::move(broker),owned)) {
    auto* timer=new QTimer(this);timer->setInterval(100);
    connect(timer,&QTimer::timeout,this,&QuestionInbox::refresh);timer->start();refresh();
}
QuestionInbox::~QuestionInbox(){if(d->owned)d->broker->close();}
Hook QuestionInbox::hook() const {
    return [broker=d->broker](const HookInput& input,const CancellationToken& token) {
        HookResult result;
        if(input.kind!=HookKind::PermissionRequest||input.call.name!="AskUserQuestion")return result;
        const auto preview=input.context["permission_preview"].toObject();
        if(preview["_meta"].toObject()["source"]!="builtin.user-question")return result;
        ToolContext context{input.sessionId,input.runId,input.context["cwd"].toString(),{},token};
        auto ticket=broker->begin(input.call,{PermissionBehavior::Ask,input.text,input.context["permission_suggestions"].toArray()},context,preview);
        result.permissionResponse=broker->wait(ticket,token);return result;
    };
}
QVariantList QuestionInbox::requests() const{return d->rows;}
QString QuestionInbox::errorString() const{return d->error;}
void QuestionInbox::error(QString value){if(d->error!=value){d->error=std::move(value);emit errorChanged();}}
void QuestionInbox::refresh() {
    Q_ASSERT(QThread::currentThread()==thread());
    try {
        QJsonArray rows;qint64 cursor=0;
        do {
            const auto page=d->broker->pending(cursor,128,64*1024*1024);
            for(const auto& item:page["requests"].toArray()) {
                const auto request=item.toObject()["request"].toObject();
                if(request["tool_name"]=="AskUserQuestion"&&request["permission_preview"].toObject()["_meta"].toObject()["source"]=="builtin.user-question")rows.append(item);
            }
            cursor=page["next_cursor"].toInteger();
        }while(cursor>0);
        if(rows!=d->snapshot){d->snapshot=rows;d->rows=rows.toVariantList();emit requestsChanged();}
    }catch(const std::exception& failure){error(QString::fromUtf8(failure.what()));}
}
bool QuestionInbox::finish(const QString& id,const QVariantMap& answers,const QVariantMap& annotations,const std::optional<QString>& reason) {
    Q_ASSERT(QThread::currentThread()==thread());refresh();
    try {
        QJsonObject pending;
        for(const auto& row:d->snapshot)if(row.toObject()["request_id"]==id){pending=row.toObject();break;}
        if(pending.isEmpty())throw Error(ErrorCode::NotFound,"Question is no longer pending");
        QJsonObject decision;
        if(reason)decision={{"behavior","deny"},{"message",reason->isEmpty()?QString("The user declined to answer"):*reason}};
        else {
            const auto request=pending["request"].toObject();auto arguments=request["input"].toObject();
            arguments["answers"]=QJsonObject::fromVariantMap(answers);
            if(!annotations.isEmpty())arguments["annotations"]=QJsonObject::fromVariantMap(annotations);
            const auto metadata=request["permission_preview"].toObject()["_meta"].toObject();
            auto tool=userQuestionTool({false,metadata["user_question"].toObject()["preview_format"].toString("markdown")});
            ToolRegistry validation;validation.add(tool);validation.validateInput(tool.definition.name,arguments);
            ToolContext context;context.approvedToolPreview=metadata;tool.validate(arguments,context);
            decision={{"behavior","allow"},{"updatedInput",arguments}};
        }
        const auto response=d->broker->respond(id,decision);refresh();
        if(!response["accepted"].toBool())throw Error(ErrorCode::Cancelled,"Question is no longer pending");
        error({});return true;
    }catch(const std::exception& failure){error(QString::fromUtf8(failure.what()));return false;}
}
bool QuestionInbox::submit(const QString& id,const QVariantMap& answers,const QVariantMap& annotations){return finish(id,answers,annotations,{});}
bool QuestionInbox::reject(const QString& id,const QString& reason){return finish(id,{},{},reason);}
void QuestionInbox::close(){Q_ASSERT(QThread::currentThread()==thread());d->broker->close();refresh();}
}
