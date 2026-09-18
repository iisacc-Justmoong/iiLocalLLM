#include "LspProtocol.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <QtCore/QUrl>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#ifdef Q_OS_UNIX
#include <signal.h>
#endif

namespace iiLocalLLM::agent::detail {
namespace {
using Clock=std::chrono::steady_clock;
void require(bool ok,const QString& message,ErrorCode code=ErrorCode::ProtocolError){if(!ok)throw Error(code,message);}
QJsonObject position(int line,int character){return {{"line",line},{"character",character}};}
bool validPosition(const QJsonValue& value){const auto p=value.toObject();for(const auto& key:{"line","character"})if(!p[key].isDouble()||p[key].toDouble()<0||p[key].toDouble()>2147483647||p[key].toDouble()!=p[key].toInt())return false;return true;}
bool validRange(const QJsonValue& value){const auto r=value.toObject();if(!validPosition(r["start"])||!validPosition(r["end"]))return false;const auto s=r["start"].toObject(),e=r["end"].toObject();return e["line"].toInt()>s["line"].toInt()||(e["line"]==s["line"]&&e["character"].toInt()>=s["character"].toInt());}
bool locationResult(const QJsonObject& item){return (item["uri"].isString()&&validRange(item["range"]))||(item["targetUri"].isString()&&validRange(item["targetRange"])&&validRange(item["targetSelectionRange"]));}
bool callItem(const QJsonObject& item){return item["name"].isString()&&item["kind"].isDouble()&&item["uri"].isString()&&validRange(item["range"])&&validRange(item["selectionRange"]);}
void validateResult(const QString& operation,const QJsonValue& result,int depth=0){require(depth<=48,"LSP result nesting exceeds limit",ErrorCode::ResourceLimit);if(result.isNull())return;
    if(operation=="hover"){require(result.isObject()&&result.toObject().contains("contents"),"Invalid LSP hover result");return;}
    const bool location=operation=="goToDefinition"||operation=="goToImplementation";
    require(result.isArray()||(location&&result.isObject()),"Invalid LSP result type");const auto items=result.isArray()?result.toArray():QJsonArray{result};
    for(const auto& value:items){require(value.isObject(),"Invalid LSP result item");const auto item=value.toObject();
        if(location||operation=="findReferences")require(locationResult(item),"Invalid LSP location");
        else if(operation=="prepareCallHierarchy")require(callItem(item),"Invalid LSP call hierarchy item");
        else if(operation=="incomingCalls"||operation=="outgoingCalls"){require(callItem(item[operation=="incomingCalls"?"from":"to"].toObject())&&item["fromRanges"].isArray(),"Invalid LSP call hierarchy result");for(const auto& range:item["fromRanges"].toArray())require(validRange(range),"Invalid LSP call range");}
        else {require(item["name"].isString()&&item["kind"].isDouble(),"Invalid LSP symbol");
            if(item.contains("location"))require(item["location"].isObject()&&item["location"].toObject()["uri"].isString(),"Invalid LSP symbol location");
            else require(validRange(item["range"])&&validRange(item["selectionRange"]),"Invalid LSP symbol range");
            if(item.contains("children"))validateResult("documentSymbol",item["children"],depth+1);}
    }
}
struct RpcError {int code;QString message;};
struct Document {QByteArray hash;int version;QJsonObject end;};
struct Connection {
    LspServerOptions config;LspOptions limits;QString root;CancellationToken stop;
    std::unique_ptr<QProcess> process;QByteArray input,errors; qint64 pid=0,nextId=0;
    QJsonObject capabilities;QMap<QString,Document> documents;QMap<QString,QJsonObject> diagnostics;int restartCount=0;
    std::function<void(const QJsonObject&)> publish;
    void check(const CancellationToken& token,Clock::time_point deadline,bool stopping=false) {
        token.throwIfCancelled();if(!stopping)stop.throwIfCancelled();
        require(Clock::now()<deadline,"LSP request deadline exceeded",ErrorCode::Timeout);
    }
    void send(const QJsonObject& value) {
        const auto bytes=QJsonDocument(value).toJson(QJsonDocument::Compact);
        require(bytes.size()<=limits.maxMessageBytes,"LSP outgoing message exceeds limit",ErrorCode::ResourceLimit);
        const auto frame="Content-Length: "+QByteArray::number(bytes.size())+"\r\n\r\n"+bytes;
        require(process&&process->state()!=QProcess::NotRunning&&process->bytesToWrite()+frame.size()<=2LL*limits.maxMessageBytes+8192,"LSP server input is unavailable",ErrorCode::ResourceLimit);
        require(process->write(frame)==frame.size(),"LSP write failed",ErrorCode::RuntimeFailure);
    }
    void notify(const QString& method,const QJsonObject& params={}){send({{"jsonrpc","2.0"},{"method",method},{"params",params}});}
    void read() {
        process->waitForReadyRead(10);
        process->setReadChannel(QProcess::StandardError);errors=(errors+process->read(65536)).right(16384);
        process->setReadChannel(QProcess::StandardOutput);input+=process->read(65536);
        require(input.size()<=limits.maxMessageBytes+8192LL+65536,"LSP incoming buffer exceeds limit",ErrorCode::ResourceLimit);
    }
    std::optional<QJsonObject> frame() {
        const auto end=input.indexOf("\r\n\r\n");require(end>=0||input.size()<=8192,"LSP header exceeds limit",ErrorCode::ResourceLimit);
        if(end<0)return {};require(end<=8192,"LSP header exceeds limit",ErrorCode::ResourceLimit);qint64 length=-1;
        for(auto header:input.first(end).split('\n')){
            header=header.trimmed();const auto colon=header.indexOf(':');require(colon>0,"Invalid LSP header");
            const auto name=header.first(colon).trimmed().toLower(),value=header.sliced(colon+1).trimmed();
            if(name=="content-length"){bool ok=false;const auto size=value.toLongLong(&ok);require(length==-1&&ok&&size>0&&size<=limits.maxMessageBytes,"Invalid LSP Content-Length",ErrorCode::ResourceLimit);length=size;}
            if(name=="content-type"&&value.toLower().contains("charset="))require(value.toLower().endsWith("charset=utf-8")||value.toLower().endsWith("charset=utf8"),"Unsupported LSP content encoding");
        }
        require(length>0,"LSP Content-Length missing");if(input.size()<end+4+length)return {};
        QJsonParseError error;const auto parsed=QJsonDocument::fromJson(input.mid(end+4,length),&error);input.remove(0,end+4+length);
        require(error.error==QJsonParseError::NoError&&parsed.isObject()&&parsed.object()["jsonrpc"]=="2.0","Invalid LSP JSON-RPC message");return parsed.object();
    }
    QJsonValue setting(QString section)const {QJsonValue value=config.settings;for(const auto& part:section.split('.',Qt::SkipEmptyParts))value=value.toObject().value(part);return value.isUndefined()?QJsonValue(QJsonValue::Null):value;}
    bool handle(const QJsonObject& message) {
        const auto method=message["method"].toString();if(method.isEmpty())return false;const auto params=message["params"].toObject();
        if(message.contains("id")) {
            QJsonObject answer{{"jsonrpc","2.0"},{"id",message["id"]}};
            if(method=="workspace/configuration") {QJsonArray values;const auto items=params["items"].toArray();require(items.size()<=128,"LSP configuration query exceeds limit");for(const auto& item:items)values.append(setting(item.toObject()["section"].toString()));answer["result"]=values;}
            else if(method=="workspace/workspaceFolders")answer["result"]=QJsonArray{QJsonObject{{"uri",QUrl::fromLocalFile(root).toString()},{"name",root}}};
            else if(method=="window/workDoneProgress/create"||method=="window/showMessageRequest")answer["result"]=QJsonValue::Null;
            else if(method=="workspace/applyEdit")answer["result"]=QJsonObject{{"applied",false},{"failureReason","LSP analysis cannot apply workspace edits"}};
            else answer["error"]=QJsonObject{{"code",-32601},{"message","Client method not supported"}};
            send(answer);
        } else if(method=="textDocument/publishDiagnostics") {
            const auto uri=params["uri"].toString();auto document=documents.constFind(uri);
            if(document!=documents.cend()&&(!params.contains("version")||params["version"].toInt(-1)==document->version)) {
                const auto items=params["diagnostics"].toArray();
                if(items.size()<=512&&QJsonDocument(params).toJson(QJsonDocument::Compact).size()<=65536)diagnostics[uri]=params;
                update();
            }
        }
        return true;
    }
    QJsonValue request(const QString& method,const QJsonObject& params,const CancellationToken& token,Clock::time_point deadline,bool stopping=false) {
        check(token,deadline,stopping);const auto id=++nextId;send({{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}});int messages=0;
        try{for(;;){check(token,deadline,stopping);read();while(auto value=frame()) {
            check(token,deadline,stopping);require(++messages<=8192,"LSP message flood",ErrorCode::ResourceLimit);
            if(handle(*value))continue;
            require(value->contains("id"),"LSP response id missing");if((*value)["id"]!=QJsonValue(id))continue;
            require(value->contains("result")!=value->contains("error"),"Invalid LSP response result");
            if(value->contains("error")){const auto e=(*value)["error"].toObject();throw RpcError{e["code"].toInt(),e["message"].toString().left(4096)};}
            return (*value)["result"];
        }require(process->state()!=QProcess::NotRunning,"LSP server exited",ErrorCode::RuntimeFailure);}}
        catch(...){try{notify("$/cancelRequest",{{"id",id}});process->waitForBytesWritten(10);}catch(...){}throw;}
    }
    void update(QString state="running",int restarts=-1) {
        if(restarts>=0)restartCount=restarts;
        QJsonArray values;for(const auto& value:diagnostics)values.append(value);
        publish({{"name",config.name},{"state",state},{"pid",pid},{"open_documents",documents.size()},{"capabilities",capabilities},{"diagnostics",values},{"restarts",restartCount}});
    }
    void start(const CancellationToken& token,Clock::time_point deadline) {
        check(token,deadline);process=std::make_unique<QProcess>();process->setWorkingDirectory(root);process->setProcessEnvironment(config.environment);process->setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_UNIX
        process->setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession|QProcess::UnixProcessFlag::CloseFileDescriptors);
#endif
        process->start(config.command,config.arguments);const auto startup=std::min(deadline,Clock::now()+std::chrono::milliseconds(limits.startupTimeoutMs));
        while(!process->waitForStarted(10)){check(token,startup);require(process->state()!=QProcess::NotRunning,"Cannot start LSP server",ErrorCode::RuntimeUnavailable);}
        pid=process->processId();update("starting");
        QJsonObject textCapabilities;for(const auto& feature:{"definition","references","hover","documentSymbol","implementation","callHierarchy"})textCapabilities[feature]=QJsonObject{{"dynamicRegistration",false}};
        textCapabilities["documentSymbol"]=QJsonObject{{"dynamicRegistration",false},{"hierarchicalDocumentSymbolSupport",true}};
        textCapabilities["synchronization"]=QJsonObject{{"dynamicRegistration",false},{"didSave",false}};
        textCapabilities["publishDiagnostics"]=QJsonObject{{"versionSupport",true}};
        const auto result=request("initialize",{{"processId",QCoreApplication::applicationPid()},{"clientInfo",QJsonObject{{"name","iiLocalLLM"},{"version","0.43.0"}}},
            {"rootUri",QUrl::fromLocalFile(root).toString()},{"rootPath",root},{"workspaceFolders",QJsonArray{QJsonObject{{"uri",QUrl::fromLocalFile(root).toString()},{"name",root}}}},
            {"initializationOptions",config.initializationOptions},{"capabilities",QJsonObject{{"general",QJsonObject{{"positionEncodings",QJsonArray{"utf-16"}}}},{"textDocument",textCapabilities},
            {"workspace",QJsonObject{{"configuration",true},{"workspaceFolders",true},{"symbol",QJsonObject{{"dynamicRegistration",false}}},{"applyEdit",false}}},{"window",QJsonObject{{"workDoneProgress",true}}}}}},token,startup);
        require(result.isObject()&&result.toObject()["capabilities"].isObject(),"LSP initialization capabilities missing");capabilities=result.toObject()["capabilities"].toObject();
        require(capabilities["positionEncoding"].toString("utf-16")=="utf-16","LSP server must support UTF-16 positions",ErrorCode::RuntimeUnavailable);
        notify("initialized");notify("workspace/didChangeConfiguration",{{"settings",config.settings}});update();
    }
    void sync(const QString& uri,const QString& language,const QString& text) {
        const auto hash=QCryptographicHash::hash(text.toUtf8(),QCryptographicHash::Sha256);const auto found=documents.constFind(uri);if(found!=documents.cend()&&found->hash==hash)return;
        const int version=found==documents.cend()?1:found->version+1;const int last=text.lastIndexOf('\n');const auto end=position(text.count('\n'),last<0?text.size():text.size()-last-1);
        if(found==documents.cend()) {
            if(documents.size()>=limits.maxDocuments){const auto old=documents.firstKey();notify("textDocument/didClose",{{"textDocument",QJsonObject{{"uri",old}}}});documents.remove(old);diagnostics.remove(old);}
            notify("textDocument/didOpen",{{"textDocument",QJsonObject{{"uri",uri},{"languageId",language},{"version",version},{"text",text}}}});
        } else {
            const auto sync=capabilities["textDocumentSync"];const auto kind=sync.isObject()?sync.toObject()["change"].toInt():sync.toInt();
            if(kind==1||kind==2){QJsonObject change{{"text",text}};if(kind==2)change["range"]=QJsonObject{{"start",position(0,0)},{"end",found->end}};
                notify("textDocument/didChange",{{"textDocument",QJsonObject{{"uri",uri},{"version",version}}},{"contentChanges",QJsonArray{change}}});}
            else {notify("textDocument/didClose",{{"textDocument",QJsonObject{{"uri",uri}}}});notify("textDocument/didOpen",{{"textDocument",QJsonObject{{"uri",uri},{"languageId",language},{"version",version},{"text",text}}}});}
            diagnostics.remove(uri);
        }
        documents[uri]={hash,version,end};update();
    }
    QJsonValue query(const QString& operation,const QString& file,const QString& language,const QString& text,int line,int character,const QString& search,const CancellationToken& token,Clock::time_point deadline) {
        if(!process)start(token,deadline);check(token,deadline);
        const QMap<QString,QString> methods{{"goToDefinition","definition"},{"findReferences","references"},{"hover","hover"},{"documentSymbol","documentSymbol"},{"workspaceSymbol","workspaceSymbol"},{"goToImplementation","implementation"},
            {"prepareCallHierarchy","callHierarchy"},{"incomingCalls","callHierarchy"},{"outgoingCalls","callHierarchy"}};
        const auto feature=methods.value(operation);const auto cap=capabilities[feature+"Provider"];require(cap.isObject()||cap.toBool(),"LSP server does not support "+operation,ErrorCode::RuntimeUnavailable);
        const auto uri=QUrl::fromLocalFile(file).toString();sync(uri,language,text);QString method="textDocument/"+(feature=="callHierarchy"?QString("prepareCallHierarchy"):feature);
        QJsonObject params{{"textDocument",QJsonObject{{"uri",uri}}},{"position",position(line,character)}};
        if(operation=="findReferences")params["context"]=QJsonObject{{"includeDeclaration",true}};
        if(operation=="documentSymbol")params.remove("position");if(operation=="workspaceSymbol"){method="workspace/symbol";params={{"query",search}};}
        auto call=[&](const QString& name,const QJsonObject& values){for(int attempt=0;;++attempt){try{return request(name,values,token,deadline);}catch(const RpcError& e){
            if(e.code!=-32801||attempt==3)throw Error(ErrorCode::RuntimeFailure,QString("LSP error %1: %2").arg(e.code).arg(e.message));
            const auto retry=Clock::now()+std::chrono::milliseconds(500*(1<<attempt));while(Clock::now()<retry){check(token,deadline);std::this_thread::sleep_for(std::chrono::milliseconds(10));}}}};
        auto result=call(method,params);
        if(operation=="incomingCalls"||operation=="outgoingCalls") {
            validateResult("prepareCallHierarchy",result);
            require(result.isNull()||result.isArray(),"Invalid LSP call hierarchy preparation");const auto items=result.toArray();
            result=items.isEmpty()?QJsonValue(QJsonArray{}):call("callHierarchy/"+operation,{{"item",items.first()}});
        }
        validateResult(operation,result);
        update();return result;
    }
    void idle(){if(!process)return;read();int count=0;while(auto item=frame()){require(++count<=1024,"LSP idle notification flood",ErrorCode::ResourceLimit);handle(*item);}require(process->state()!=QProcess::NotRunning,"LSP server exited",ErrorCode::RuntimeFailure);}
    void close(bool graceful)noexcept {
        if(!process)return;
        if(graceful&&process->state()!=QProcess::NotRunning)try{request("shutdown",{},CancellationToken{},Clock::now()+std::chrono::milliseconds(300),true);notify("exit");process->waitForBytesWritten(50);process->waitForFinished(200);}catch(...){}
#ifdef Q_OS_UNIX
        if(pid>0)::kill(-pid_t(pid),SIGTERM);
#endif
        if(process->state()!=QProcess::NotRunning){process->terminate();process->waitForFinished(100);}
#ifdef Q_OS_UNIX
        if(pid>0)::kill(-pid_t(pid),SIGKILL);
#endif
        if(process->state()!=QProcess::NotRunning){process->kill();process->waitForFinished(500);}
        process.reset();pid=0;input.clear();errors.clear();documents.clear();diagnostics.clear();capabilities={};
    }
    ~Connection(){close(true);}
};
}
class LanguageServer::Impl {
public:
    struct Job {QString operation,file,language,text,search;int line,character;CancellationToken token;Clock::time_point deadline;std::promise<QJsonValue> result;};
    LspServerOptions config;LspOptions limits;QString root;CancellationToken stop;
    mutable std::mutex mutex;std::mutex joining;std::condition_variable changed;std::deque<std::shared_ptr<Job>> jobs;QJsonObject state;std::thread worker;
    Impl(LspServerOptions c,LspOptions l,QString r):config(std::move(c)),limits(std::move(l)),root(std::move(r)) {
        state={{"name",config.name},{"state","stopped"},{"pid",0}};
        worker=std::thread([this]{Connection connection{config,limits,root,stop};connection.publish=[this](const QJsonObject& value){std::lock_guard lock(mutex);state=value;};int restarts=0;
            for(;;){std::shared_ptr<Job> job;{std::unique_lock lock(mutex);changed.wait_for(lock,std::chrono::milliseconds(10),[&]{return stop.isCancelled()||!jobs.empty();});if(stop.isCancelled())break;
                if(!jobs.empty()){job=jobs.front();jobs.pop_front();}}
                try{if(job){require(restarts<=3,"LSP restart limit exceeded; close the session to retry",ErrorCode::ResourceLimit);
                        job->token.throwIfCancelled();auto result=connection.query(job->operation,job->file,job->language,job->text,job->line,job->character,job->search,job->token,job->deadline);job->result.set_value(std::move(result));}
                    else connection.idle();}
                catch(...){auto error=std::current_exception();bool cancelled=false;
                    try{std::rethrow_exception(error);}catch(const RpcError& e){error=std::make_exception_ptr(Error(ErrorCode::RuntimeFailure,QString("LSP error %1: %2").arg(e.code).arg(e.message)));}
                    catch(const Error& e){cancelled=e.code()==ErrorCode::Cancelled;}catch(...){}
                    if(connection.process){if(!cancelled)++restarts;connection.close(false);}connection.update(cancelled?"stopped":"error",restarts);if(job)job->result.set_exception(error);}
            }
            connection.close(true);connection.update("stopped",restarts);
            std::deque<std::shared_ptr<Job>> pending;{std::lock_guard lock(mutex);pending.swap(jobs);}for(const auto& job:pending)job->result.set_exception(std::make_exception_ptr(Error(ErrorCode::Cancelled,"LSP session closed")));
        });
    }
    void close(){std::lock_guard join(joining);stop.cancel();changed.notify_all();if(worker.joinable())worker.join();}
    ~Impl(){close();}
};
LanguageServer::LanguageServer(LspServerOptions options,LspOptions limits,QString root):d(std::make_unique<Impl>(std::move(options),std::move(limits),std::move(root))){}
LanguageServer::~LanguageServer()=default;
QJsonValue LanguageServer::query(QString op,QString file,QString language,QString text,int line,int character,QString search,const CancellationToken& token){
    auto job=std::make_shared<Impl::Job>();job->operation=std::move(op);job->file=std::move(file);job->language=std::move(language);job->text=std::move(text);job->line=line;job->character=character;job->search=std::move(search);job->token=CancellationToken::linkedTo(token);job->deadline=Clock::now()+std::chrono::milliseconds(d->limits.requestTimeoutMs);
    auto result=job->result.get_future();{std::lock_guard lock(d->mutex);d->stop.throwIfCancelled();token.throwIfCancelled();require(d->jobs.size()<size_t(d->limits.maxQueuedRequests),"LSP queue limit exceeded",ErrorCode::ResourceLimit);d->jobs.push_back(job);}d->changed.notify_all();
    while(result.wait_for(std::chrono::milliseconds(10))!=std::future_status::ready) {
        if(token.isCancelled()||d->stop.isCancelled()||Clock::now()>=job->deadline) {
            job->token.cancel();{std::lock_guard lock(d->mutex);auto found=std::find(d->jobs.begin(),d->jobs.end(),job);if(found!=d->jobs.end())d->jobs.erase(found);}
            token.throwIfCancelled();d->stop.throwIfCancelled();throw Error(ErrorCode::Timeout,"LSP request deadline exceeded");
        }
    }
    token.throwIfCancelled();return result.get();
}
QJsonObject LanguageServer::snapshot()const{std::lock_guard lock(d->mutex);return d->state;}
void LanguageServer::close(){d->close();}
}
