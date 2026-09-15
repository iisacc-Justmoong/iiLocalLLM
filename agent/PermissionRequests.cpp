#include "PermissionRequests.h"
#include "PermissionResponses.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QHash>
#include <QtCore/QUuid>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace iiLocalLLM::agent {
namespace {
using Clock=std::chrono::steady_clock;
void require(bool value,const QString& message,ErrorCode code=ErrorCode::InvalidArgument) {if(!value)throw Error(code,message);}
QByteArray encoded(const QJsonObject& value){return QJsonDocument(value).toJson(QJsonDocument::Compact);}
QString now(){return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);}
bool validId(const QString& value){return !value.isEmpty()&&value.size()<=512&&!value.contains(QChar::Null);}
PermissionResponse responseFromJson(const QJsonObject& object) {
    const auto behavior=object.value("behavior").toString();require(behavior=="allow"||behavior=="deny","Invalid permission response behavior");
    const QStringList keys=behavior=="allow"?QStringList{"behavior","updatedInput","updatedPermissions","toolUseID","decisionClassification"}
        :QStringList{"behavior","message","interrupt","toolUseID","decisionClassification"};
    for(auto i=object.begin();i!=object.end();++i)require(keys.contains(i.key()),"Unknown permission response field: "+i.key());
    PermissionResponse result;result.behavior=behavior=="allow"?PermissionBehavior::Allow:PermissionBehavior::Deny;
    if(object.contains("updatedInput")) {
        require(object["updatedInput"].isObject(),"Permission updatedInput must be an object");
        // Reference SDK mobile replies use {} when they do not have the input.
        if(!object["updatedInput"].toObject().isEmpty())result.updatedArguments=object["updatedInput"].toObject();
    }
    if(object.contains("updatedPermissions")){require(object["updatedPermissions"].isArray(),"Permission updates must be an array");result.updatedPermissions=object["updatedPermissions"].toArray();}
    if(object.contains("message")){require(object["message"].isString(),"Permission message must be a string");result.message=object["message"].toString();}
    if(object.contains("interrupt")){require(object["interrupt"].isBool(),"Permission interrupt must be boolean");result.interrupt=object["interrupt"].toBool();}
    if(object.contains("toolUseID"))require(object["toolUseID"].isString()&&validId(object["toolUseID"].toString()),"Invalid permission toolUseID");
    if(object.contains("decisionClassification"))require(QStringList{"user_temporary","user_permanent","user_reject"}.contains(object["decisionClassification"].toString()),"Invalid permission decision classification");
    detail::validatePermissionResponse(result);return result;
}
}
struct PermissionRequests::Entry {
    QString channel,id;quint64 sequence=0;qint64 bytes=0;
    Clock::time_point deadline;
    QJsonObject request,resolution;
    std::optional<PermissionResponse> response;
};
struct PermissionRequests::Impl {
    struct History {QJsonObject resolution;QByteArray fingerprint;};
    PermissionRequestsOptions options;
    QString channel=QUuid::createUuid().toString(QUuid::WithoutBraces);
    std::mutex mutex;std::condition_variable changed;bool closed=false;
    quint64 sequence=0;qint64 pendingBytes=0;
    QHash<QString,std::shared_ptr<Entry>> pending;
    QHash<QString,History> history;std::deque<QString> order;
    explicit Impl(PermissionRequestsOptions value):options(value) {
        require(options.timeoutMs>=1&&options.timeoutMs<=3600000&&options.maxPending>=1&&options.maxPending<=1024
            &&options.maxHistory>=1&&options.maxHistory<=65536&&options.maxRequestBytes>=1024&&options.maxRequestBytes<=4*1024*1024
            &&options.maxPendingBytes>=options.maxRequestBytes&&options.maxPendingBytes<=64*1024*1024,"Invalid permission request limits");
    }
    bool finish(std::shared_ptr<Entry> entry,const PermissionResponse& response,const QString& status,const QString& source,
        const QByteArray& fingerprint={},const QString& classification={}) {
        if(entry->response)return false;
        entry->response=response;
        entry->resolution={{"request_id",entry->id},{"session_id",entry->request["session_id"]},{"run_id",entry->request["run_id"]},
            {"tool_use_id",entry->request["request"].toObject()["tool_use_id"]},{"status",status},{"source",source},
            {"behavior",response.behavior==PermissionBehavior::Allow?"allow":"deny"},{"resolved_at",now()}};
        if(!classification.isEmpty())entry->resolution["decision_classification"]=classification;
        pendingBytes-=entry->bytes;pending.remove(entry->id);history.insert(entry->id,{entry->resolution,fingerprint});order.push_back(entry->id);
        while(order.size()>size_t(options.maxHistory)){history.remove(order.front());order.pop_front();}
        changed.notify_all();return true;
    }
    void expire() {
        const auto time=Clock::now();const auto entries=pending.values();
        for(const auto& entry:entries)if(time>=entry->deadline)finish(entry,{PermissionBehavior::Deny,"Permission request timed out"},"expired","timeout");
    }
};
PermissionRequests::PermissionRequests(PermissionRequestsOptions options):d(std::make_shared<Impl>(options)){}
PermissionRequests::~PermissionRequests(){close();}
PermissionRequests::Ticket PermissionRequests::begin(const ToolCall& call,const PermissionDecision& decision,const ToolContext& context,const QJsonObject& preview) {
    context.cancellation.throwIfCancelled();require(decision.behavior==PermissionBehavior::Ask,"Only Ask decisions can request permission");
    require(validId(context.sessionId)&&validId(call.id)&&validId(call.name)&&context.runId.size()<=512&&!context.runId.contains(QChar::Null),"Invalid permission request identity");
    detail::validatePermissionUpdates(decision.suggestions);
    auto entry=std::make_shared<Entry>();entry->channel=d->channel;entry->id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject request{{"subtype","can_use_tool"},{"tool_name",call.name},{"tool_use_id",call.id},{"input",call.arguments},
        {"permission_suggestions",decision.suggestions},{"decision_reason",decision.reason}};
    if(!preview.isEmpty())request["permission_preview"]=preview;
    entry->deadline=Clock::now()+std::chrono::milliseconds(d->options.timeoutMs);
    entry->request={{"schema","iisacc.permission-request/1"},{"request_id",entry->id},{"status","pending"},
        {"session_id",context.sessionId},{"run_id",context.runId},{"cwd",context.workingDirectory},
        {"created_at",now()},{"expires_at",QDateTime::currentDateTimeUtc().addMSecs(d->options.timeoutMs).toString(Qt::ISODateWithMs)},
        {"request",request}};
    std::lock_guard lock(d->mutex);d->expire();require(!d->closed,"Permission channel is closed",ErrorCode::ShuttingDown);
    require(d->sequence<9007199254740991ULL,"Permission request sequence exhausted",ErrorCode::ResourceLimit);
    entry->sequence=++d->sequence;entry->request["sequence"]=double(entry->sequence);entry->bytes=encoded(entry->request).size();
    require(entry->bytes<=d->options.maxRequestBytes,"Permission request exceeds size limit",ErrorCode::ResourceLimit);
    require(d->pending.size()<d->options.maxPending&&d->pendingBytes+entry->bytes<=d->options.maxPendingBytes,"Permission request capacity reached",ErrorCode::QueueFull);
    d->pending.insert(entry->id,entry);d->pendingBytes+=entry->bytes;
    Ticket ticket;ticket.request=entry->request;ticket.entry=std::move(entry);return ticket;
}
PermissionResponse PermissionRequests::wait(const Ticket& ticket,const CancellationToken& token,QJsonObject* resolution) {
    require(ticket.entry&&ticket.entry->channel==d->channel,"Permission ticket belongs to another channel");
    std::unique_lock lock(d->mutex);
    for(;;) {
        if(token.isCancelled()) {
            d->finish(ticket.entry,{PermissionBehavior::Deny,"Permission request cancelled"},"cancelled","cancellation");
            if(resolution)*resolution=ticket.entry->resolution;
            throw Error(ErrorCode::Cancelled,"Permission request cancelled");
        }
        d->expire();
        if(ticket.entry->response){if(resolution)*resolution=ticket.entry->resolution;return *ticket.entry->response;}
        d->changed.wait_for(lock,std::chrono::milliseconds(10));
    }
}
QJsonObject PermissionRequests::pending(qint64 after,int limit,int maxBytes) {
    require(after>=0&&after<=9007199254740991LL&&limit>=1&&limit<=128&&maxBytes>=1024&&maxBytes<=64*1024*1024,"Invalid permission request page");
    std::lock_guard lock(d->mutex);d->expire();auto entries=d->pending.values();
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a->sequence<b->sequence;});
    QJsonArray values;qint64 bytes=256;quint64 cursor=after;bool more=false;
    for(const auto& entry:entries)if(entry->sequence>quint64(after)) {
        if(values.size()==limit||bytes+entry->bytes>maxBytes){require(!values.isEmpty(),"Permission request exceeds response page size",ErrorCode::ResourceLimit);more=true;break;}
        values.append(entry->request);bytes+=entry->bytes;cursor=entry->sequence;
    }
    QJsonObject result{{"requests",values},{"closed",d->closed},{"pending_count",d->pending.size()}};
    if(more)result["next_cursor"]=double(cursor);return result;
}
QJsonObject PermissionRequests::respond(const QString& id,const QJsonObject& object) {
    require(validId(id),"Invalid permission request ID");
    require(encoded(object).size()<=d->options.maxRequestBytes,"Permission response exceeds size limit",ErrorCode::ResourceLimit);
    const auto response=responseFromJson(object);const auto fingerprint=QCryptographicHash::hash(encoded(object),QCryptographicHash::Sha256);
    std::lock_guard lock(d->mutex);d->expire();const auto found=d->pending.find(id);
    if(found==d->pending.end()) {
        const auto previous=d->history.constFind(id);require(previous!=d->history.cend(),"Permission request was not found",ErrorCode::NotFound);
        auto result=previous->resolution;
        if(result["source"]=="client") {
            require(previous->fingerprint==fingerprint,"Permission request already has a different response",ErrorCode::AlreadyExists);
            result["accepted"]=true;result["replayed"]=true;
        } else {result["accepted"]=false;result["replayed"]=false;}
        return result;
    }
    const auto entry=found.value();
    if(object.contains("toolUseID"))require(object["toolUseID"]==entry->request["request"].toObject()["tool_use_id"],"Permission response toolUseID mismatch");
    d->finish(entry,response,"answered","client",fingerprint,object.value("decisionClassification").toString());
    auto result=entry->resolution;result["accepted"]=true;result["replayed"]=false;return result;
}
bool PermissionRequests::settle(const Ticket& ticket,const PermissionResponse& response,const QString& source) {
    require(ticket.entry&&ticket.entry->channel==d->channel,"Permission ticket belongs to another channel");detail::validatePermissionResponse(response);
    std::lock_guard lock(d->mutex);d->expire();return d->finish(ticket.entry,response,"answered",source);
}
void PermissionRequests::dismiss(const Ticket& ticket,const QString& reason) {
    require(ticket.entry&&ticket.entry->channel==d->channel,"Permission ticket belongs to another channel");
    std::lock_guard lock(d->mutex);d->finish(ticket.entry,{PermissionBehavior::Deny,reason},"cancelled","cancellation");
}
void PermissionRequests::close() {
    std::lock_guard lock(d->mutex);d->closed=true;
    while(!d->pending.isEmpty())d->finish(d->pending.begin().value(),{PermissionBehavior::Deny,"Permission channel closed"},"closed","channel");
}
}
