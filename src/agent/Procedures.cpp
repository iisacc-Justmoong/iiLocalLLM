#include "Procedures.h"
#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>

namespace iiLocalLLM::agent {
namespace {
using Clock=std::chrono::steady_clock;
QString now(){return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);}
void require(bool value,const QString& message,ErrorCode code=ErrorCode::InvalidArgument){if(!value)throw Error(code,message);}
bool terminal(const QJsonObject& record){return QStringList{"completed","failed","cancelled","expired"}.contains(record["phase"].toString());}
QString actionName(ProcedureAction action){return action==ProcedureAction::Replace?"replace":action==ProcedureAction::Cancel?"cancel":"continue";}
QJsonObject responseJson(const ProcedureResponse& response){
    QJsonObject result{{"action",actionName(response.action)}};
    if(response.action==ProcedureAction::Replace)result["output"]=response.output;
    if(!response.reason.isEmpty())result["reason"]=response.reason;
    return result;
}
ProcedureResponse parse(const QJsonObject& value){
    for(auto it=value.begin();it!=value.end();++it)require(QStringList{"action","output","reason"}.contains(it.key()),"Unknown procedure response field: "+it.key());
    const auto action=value["action"].toString();require(QStringList{"continue","replace","cancel"}.contains(action),"Invalid procedure action");
    require(!value.contains("reason")||value["reason"].isString(),"Procedure reason must be a string");
    require(action=="replace"?(value.contains("output")&&value["output"].isObject()):!value.contains("output"),"Only replace accepts an output object");
    return {action=="replace"?ProcedureAction::Replace:action=="cancel"?ProcedureAction::Cancel:ProcedureAction::Continue,value["output"].toObject(),value["reason"].toString()};
}
void emitRecord(const EventCallback& callback,const QJsonObject& record){
    if(!callback)return;
    try{callback({EventKind::Procedure,record["run_id"].toString(),record["session_id"].toString(),record["tool_call_id"].toString(),{},record});}
    catch(...){throw Error(ErrorCode::ConsumerFailure,"Procedure event consumer threw an exception");}
}
}
struct Procedures::Impl {
    struct Entry {QJsonObject record,response;Validator validator;Clock::time_point started,deadline;};
    ProcedureOptions options;ProcedureCallback callback;
    mutable std::mutex mutex;std::condition_variable changed;
    std::map<QString,Entry> entries;quint64 sequence=0;bool closed=false;
    void update(Entry& entry){entry.record["sequence"]=qint64(++sequence);entry.record["updated_at"]=now();}
    qint64 bytes()const{qint64 total=0;for(const auto& [_,e]:entries)total+=QJsonDocument(e.record).toJson(QJsonDocument::Compact).size()+QJsonDocument(e.response).toJson(QJsonDocument::Compact).size();return total;}
    void trim(const QString& keep={}){
        while(entries.size()>size_t(options.maxRecords)||bytes()>options.maxBytes){
            auto oldest=entries.end();for(auto it=entries.begin();it!=entries.end();++it)
                if(it->first!=keep&&terminal(it->second.record)&&(oldest==entries.end()||it->second.record["sequence"].toInteger()<oldest->second.record["sequence"].toInteger()))oldest=it;
            require(oldest!=entries.end(),"Procedure channel capacity exceeded",ErrorCode::ResourceLimit);entries.erase(oldest);
        }
    }
};
Procedures::Procedures(ProcedureOptions options,ProcedureCallback callback):d(std::make_shared<Impl>()){
    require(options.timeoutMs>=1&&options.timeoutMs<=3600000&&options.maxRecords>=8&&options.maxRecords<=65536
        &&options.maxRecordBytes>=1024&&options.maxRecordBytes<=32*1024*1024&&options.maxBytes>=options.maxRecordBytes&&options.maxBytes<=256*1024*1024,"Invalid procedure limits");
    for(const auto& kind:options.intercept)require(QStringList{"input","decision_input","decision","context","compaction","model","tool_validation","tool_preparation","permission","tool_execution","tool","completion"}.contains(kind),"Invalid intercepted procedure kind: "+kind);
    d->options=std::move(options);d->callback=std::move(callback);
}
Procedures::~Procedures(){close();}
QJsonObject Procedures::begin(const QString& kind,const ToolContext& context,const QJsonObject& input){
    context.cancellation.throwIfCancelled();
    require(!kind.isEmpty()&&kind.size()<=64&&!kind.contains(QChar::Null),"Invalid procedure kind");
    std::lock_guard lock(d->mutex);require(!d->closed,"Procedure channel is closed",ErrorCode::ShuttingDown);
    const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    Impl::Entry entry;entry.started=Clock::now();entry.record={{"schema","iisacc.procedure/1"},{"procedure_id",id},{"kind",kind},{"phase","started"},
        {"session_id",context.sessionId},{"owner_session_id",context.procedureOwnerSessionId.isEmpty()?context.sessionId:context.procedureOwnerSessionId},
        {"run_id",context.runId},{"parent_procedure_id",context.procedureId},{"tool_call_id",context.procedureToolCallId},
        {"turn",context.procedureTurn},{"agent_id",context.procedureAgentId},{"controllable",kind!="run"},{"input",input},{"created_at",now()}};
    require(QJsonDocument(entry.record).toJson(QJsonDocument::Compact).size()+256<=d->options.maxRecordBytes,"Procedure input exceeds record limit",ErrorCode::ResourceLimit);
    d->update(entry);d->entries.emplace(id,std::move(entry));
    try{d->trim(id);}catch(...){d->entries.erase(id);throw;}
    return d->entries.at(id).record;
}
QJsonObject Procedures::finish(const QString& id,const QJsonObject& output,Validator validator,const CancellationToken& token,const EventCallback& events){
    QJsonObject returned;
    {std::lock_guard lock(d->mutex);auto& e=d->entries.at(id);
        require(e.record["phase"]=="started","Procedure already returned",ErrorCode::AlreadyExists);
        auto candidate=e.record;candidate["output"]=output;candidate["replaceable"]=bool(validator);
        require(QJsonDocument(candidate).toJson(QJsonDocument::Compact).size()*2+256<=d->options.maxRecordBytes,"Procedure output exceeds record limit",ErrorCode::ResourceLimit);
        e.record=std::move(candidate);e.validator=std::move(validator);e.deadline=Clock::now()+std::chrono::milliseconds(d->options.timeoutMs);
        e.record["phase"]=d->options.intercept.contains(e.record["kind"].toString())?"waiting":"returned";d->update(e);d->trim(id);returned=e.record;
    }
    emitRecord(events,returned);token.throwIfCancelled();
    if(d->callback&&returned["kind"]!="run") {
        ProcedureResponse response;
        try{response=d->callback(returned,token);}catch(const Error&){throw;}catch(...){throw Error(ErrorCode::ConsumerFailure,"Procedure return handler threw an exception");}
        // A response delivered from the event observer may already have won.
        bool pending;{std::lock_guard lock(d->mutex);pending=d->entries.at(id).response.isEmpty();}
        if(pending){try{respond(id,responseJson(response));}catch(const Error& error){if(error.code()!=ErrorCode::AlreadyExists)throw;}}
    }
    QJsonObject record,effective;
    {std::unique_lock lock(d->mutex);auto& e=d->entries.at(id);
        if(e.record["phase"]=="returned"&&e.response.isEmpty())e.response={{"action","continue"}};
        while(e.response.isEmpty()){
            token.throwIfCancelled();require(!d->closed,"Procedure channel closed while waiting",ErrorCode::Cancelled);
            require(Clock::now()<e.deadline,"Procedure return control timed out",ErrorCode::Timeout);
            d->changed.wait_for(lock,std::chrono::milliseconds(10));
        }
        token.throwIfCancelled();const auto response=parse(e.response);effective=response.action==ProcedureAction::Replace?response.output:output;
        e.record["action"]=actionName(response.action);e.record["effective_output"]=effective;e.record["reason"]=response.reason;
        e.record["phase"]=response.action==ProcedureAction::Cancel?"cancelled":"completed";
        e.record["duration_ms"]=qint64(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-e.started).count());d->update(e);record=e.record;
        e.validator={};d->trim();
    }
    emitRecord(events,record);
    if(record["action"]=="cancel")throw Error(ErrorCode::Cancelled,record["reason"].toString("Cancelled by host procedure control"));
    return effective;
}
QJsonObject Procedures::respond(const QString& id,const QJsonObject& value,const QString& owner){
    auto response=parse(value);const auto normalized=responseJson(response);Validator validate;
    {std::lock_guard lock(d->mutex);const auto it=d->entries.find(id);require(it!=d->entries.end(),"Procedure was not found",ErrorCode::NotFound);auto& e=it->second;
        require(owner.isEmpty()||e.record["owner_session_id"]==owner,"Procedure belongs to another conversation",ErrorCode::NotFound);
        require(e.record["controllable"].toBool(),"This procedure only reports the run outcome");
        if(!e.response.isEmpty()){require(e.response==normalized,"Conflicting procedure response",ErrorCode::AlreadyExists);return {{"procedure_id",id},{"accepted",true},{"replayed",true}};}
        require(!terminal(e.record)&&e.record["phase"]!="started","Procedure is not accepting a return response",ErrorCode::AlreadyExists);
        require(!d->closed&&Clock::now()<e.deadline,"Procedure response expired",ErrorCode::Timeout);
        require(QJsonDocument(normalized).toJson(QJsonDocument::Compact).size()<=d->options.maxRecordBytes/2,"Procedure response exceeds limit",ErrorCode::ResourceLimit);
        validate=e.validator;
    }
    if(response.action==ProcedureAction::Replace){require(bool(validate),"This procedure return cannot be replaced");validate(response.output);}
    {std::lock_guard lock(d->mutex);const auto found=d->entries.find(id);
        require(found!=d->entries.end(),"Procedure is no longer retained",ErrorCode::NotFound);auto& e=found->second;
        if(!e.response.isEmpty()){require(e.response==normalized,"Conflicting procedure response",ErrorCode::AlreadyExists);return {{"procedure_id",id},{"accepted",true},{"replayed",true}};}
        require(!terminal(e.record)&&!d->closed&&Clock::now()<e.deadline,"Procedure response is no longer pending",ErrorCode::AlreadyExists);
        e.response=normalized;
        try{d->trim(id);}catch(...){e.response={};throw;}
        d->changed.notify_all();
    }
    return {{"procedure_id",id},{"accepted",true},{"replayed",false}};
}
QJsonObject Procedures::list(const QString& owner,qint64 after,int limit)const{
    require(after>=0&&limit>=1&&limit<=128,"Invalid procedure cursor or limit");
    std::lock_guard lock(d->mutex);QList<QJsonObject> matches;
    for(const auto& [_,entry]:d->entries)if(entry.record["owner_session_id"]==owner&&entry.record["sequence"].toInteger()>after)matches.append(entry.record);
    std::sort(matches.begin(),matches.end(),[](const auto& a,const auto& b){return a["sequence"].toInteger()<b["sequence"].toInteger();});
    QJsonArray records;qint64 cursor=after,bytes=256;for(const auto& record:matches){const auto size=QJsonDocument(record).toJson(QJsonDocument::Compact).size()+1;if(records.size()>=limit||bytes+size>d->options.maxRecordBytes)break;records.append(record);bytes+=size;cursor=record["sequence"].toInteger();}
    return {{"schema","iisacc.procedure-list/1"},{"procedures",records},{"next_cursor",cursor},{"has_more",records.size()<matches.size()}};
}
void Procedures::fail(const QString& id,const QString& reason,bool cancelled,const EventCallback& events,ErrorCode code)noexcept{
    try{QJsonObject record;{std::lock_guard lock(d->mutex);auto it=d->entries.find(id);if(it==d->entries.end()||terminal(it->second.record))return;
        auto& e=it->second;e.record["phase"]=code==ErrorCode::Timeout?"expired":cancelled?"cancelled":"failed";e.record["error"]=reason.left(4096);e.record["error_code"]=iiLocalLLM::enumName(code);e.validator={};d->update(e);
        if(QJsonDocument(e.record).toJson(QJsonDocument::Compact).size()+256>d->options.maxRecordBytes){e.record.remove("input");e.record.remove("output");e.record["payload_omitted"]=true;}
        while(QJsonDocument(e.record).toJson(QJsonDocument::Compact).size()+256>d->options.maxRecordBytes&&!e.record["error"].toString().isEmpty())e.record["error"]=e.record["error"].toString().left(e.record["error"].toString().size()/2);
        record=e.record;d->trim();d->changed.notify_all();}emitRecord(events,record);}catch(...){}
}
void Procedures::close(){std::lock_guard lock(d->mutex);d->closed=true;d->changed.notify_all();}
ProcedureScope::ProcedureScope(QString kind,const ToolContext& context,QJsonObject input,EventCallback events)
    :channel_(context.procedures),token_(context.cancellation),events_(std::move(events)){
    if(channel_){const auto record=channel_->begin(kind,context,input);id_=record["procedure_id"].toString();try{emitRecord(events_,record);}catch(...){fail("Procedure observer failed");throw;}}
}
ProcedureScope::~ProcedureScope(){if(!ended_)fail("Procedure exited before its return was committed",token_.isCancelled());}
QJsonObject ProcedureScope::returned(QJsonObject output,Procedures::Validator validator){
    if(!channel_){ended_=true;return output;}
    try{auto effective=channel_->finish(id_,output,std::move(validator),token_,events_);ended_=true;return effective;}
    catch(const Error& error){fail(QString::fromUtf8(error.what()),error.code()==ErrorCode::Cancelled,error.code());throw;}
}
void ProcedureScope::fail(QString reason,bool cancelled,ErrorCode code)noexcept{if(channel_)channel_->fail(id_,reason,cancelled,events_,cancelled?ErrorCode::Cancelled:code);ended_=true;}
}
