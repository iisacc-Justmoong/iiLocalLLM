#include "AsyncHooks.h"
#include <map>
#include <mutex>
#include <condition_variable>
#include <algorithm>

namespace iiLocalLLM::agent {
namespace {thread_local const AsyncHookScope* completingScope=nullptr;}
class AsyncHookScope::Impl {
public:
    struct Record {QJsonObject data;CancellationToken token;bool running=true;quint64 sequence=0;};
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::map<QString,Record> records;
    std::function<void(const QJsonObject&)> completion;
    int maxRecords;
    quint64 sequence=0;
    bool closed=false;
    Impl(std::function<void(const QJsonObject&)> callback,int count):completion(std::move(callback)),maxRecords(count) {
        if(count<1||count>4096)throw Error(ErrorCode::InvalidArgument,"Invalid async hook record limit");
    }
};
AsyncHookScope::AsyncHookScope(std::function<void(const QJsonObject&)> callback,int count):d(std::make_unique<Impl>(std::move(callback),count)) {}
AsyncHookScope::~AsyncHookScope(){try{close();}catch(...) {}}
void AsyncHookScope::attach(const QString& id,const QJsonObject& data,const CancellationToken& token) {
    std::lock_guard lock(d->mutex);token.throwIfCancelled();
    if(d->closed)throw Error(ErrorCode::ShuttingDown,"Async hook scope is closed");
    if(d->records.size()>=size_t(d->maxRecords)) {
        auto oldest=d->records.end();
        for(auto it=d->records.begin();it!=d->records.end();++it)
            if(!it->second.running&&(oldest==d->records.end()||it->second.sequence<oldest->second.sequence))oldest=it;
        if(oldest==d->records.end())throw Error(ErrorCode::QueueFull,"Async hook record capacity is full");
        d->records.erase(oldest);
    }
    auto value=data;value["hook_id"]=id;value["state"]="running";value["background"]=false;
    d->records.emplace(id,Impl::Record{value,token,true,++d->sequence});
}
void AsyncHookScope::background(const QString& id,int timeout) {
    std::lock_guard lock(d->mutex);auto& record=d->records.at(id);record.token.throwIfCancelled();
    record.data["background"]=true;record.data["async_timeout_ms"]=timeout;
}
void AsyncHookScope::finish(const QString& id,QJsonObject result) {
    bool background=false,notify=false;QJsonObject record;
    {
        std::lock_guard lock(d->mutex);auto& entry=d->records.at(id);record=entry.data;
        background=record["background"].toBool();notify=background&&!d->closed&&!entry.token.isCancelled();
    }
    for(auto it=result.begin();it!=result.end();++it)record[it.key()]=it.value();
    if(notify&&d->completion) {
        const auto previous=completingScope;completingScope=this;
        try {d->completion(record);record["delivery"]="accepted";}
        catch(const std::exception& error){record["delivery"]="failed";record["delivery_error"]=QString::fromUtf8(error.what()).left(4096);}
        catch(...){record["delivery"]="failed";record["delivery_error"]="Unknown completion handler failure";}
        completingScope=previous;
    }
    // Raw process output is bounded during capture, but retained diagnostics
    // are smaller. Full model context was passed to the completion above.
    for(const auto& key:{"stdout","stderr"})if(record.contains(key))record[key]=record[key].toString().left(4096);
    if(record.contains("text"))record["text"]=record["text"].toString().left(65536);
    std::lock_guard lock(d->mutex);
    if(background){auto& entry=d->records.at(id);entry.data=std::move(record);entry.running=false;}
    else d->records.erase(id);
    d->changed.notify_all();
}
QJsonArray AsyncHookScope::status(const QString& session) const {
    std::lock_guard lock(d->mutex);QList<const Impl::Record*> selected;
    for(const auto& [_,record]:d->records)if(record.data["background"].toBool()&&(session.isEmpty()||record.data["session_id"]==session))selected.append(&record);
    std::sort(selected.begin(),selected.end(),[](const auto* a,const auto* b){return a->sequence<b->sequence;});
    QJsonArray result;for(const auto* record:selected){auto value=record->data;value["cancel_requested"]=record->token.isCancelled();result.append(value);}return result;
}
QJsonArray AsyncHookScope::takeCompleted(const QString& session) {
    std::lock_guard lock(d->mutex);QJsonArray result;
    for(auto it=d->records.begin();it!=d->records.end();) {
        if(!it->second.running&&it->second.data["background"].toBool()&&(session.isEmpty()||it->second.data["session_id"]==session)) {
            result.append(it->second.data);it=d->records.erase(it);
        } else ++it;
    }return result;
}
int AsyncHookScope::cancel(const QString& id) {
    std::lock_guard lock(d->mutex);int count=0;
    if(!id.isEmpty()&&!d->records.contains(id))throw Error(ErrorCode::NotFound,"Async hook not found in this scope");
    for(const auto& [key,record]:d->records)if(record.running&&(id.isEmpty()||id==key)){record.token.cancel();++count;}return count;
}
void AsyncHookScope::close() {
    if(completingScope==this)throw Error(ErrorCode::InvalidArgument,"Cannot close async hook scope from its completion handler");
    std::unique_lock lock(d->mutex);d->closed=true;for(const auto& [_,record]:d->records)record.token.cancel();
    d->changed.wait(lock,[&]{return std::none_of(d->records.begin(),d->records.end(),[](const auto& item){return item.second.running;});});
}
}
