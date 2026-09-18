#include "MemoryWorker.h"
#include "PermissionRulesInternal.h"
#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QSet>
#include <QtCore/QUuid>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <algorithm>
namespace iiLocalLLM::agent::detail {
namespace {
using Clock=std::chrono::steady_clock;
void require(bool value,const QString& message,ErrorCode code=ErrorCode::InvalidArgument){if(!value)throw Error(code,message);}
QString uuid(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
bool inside(const QString& path,const QString& root){return !root.isEmpty()&&(path==root||path.startsWith(root+'/'));}
QJsonObject usage(const Usage& value){return {{"prompt_tokens",double(value.promptTokens)},{"generated_tokens",double(value.generatedTokens)},{"cached_tokens",double(value.cachedTokens)}};}
QString preview(QString text,int limit) {
    if(text.size()>limit){if(text.at(limit-1).isHighSurrogate())--limit;text.truncate(limit);}return text;
}
qsizetype contextBytes(const ModelRequest& request) {
    QJsonArray messages,tools;for(const auto& m:request.messages)messages.append(toJson(m));for(const auto& t:request.tools)tools.append(toJson(t));
    return QJsonDocument(QJsonObject{{"model",request.model},{"system",request.systemPrompt},{"messages",messages},{"tools",tools},
        {"response_schema",request.responseSchema}}).toJson(QJsonDocument::Compact).size();
}
bool permittedIdentity(const Tool& tool,bool history) {
    if(tool.isMcp||tool.requiresPermission)return false;
    return (tool.definition.metadata["source"]=="builtin.workspace"&&QStringList{"Read","Write","Edit","Glob","Grep"}.contains(tool.definition.name))
        ||(tool.definition.name=="Bash"&&tool.definition.metadata["source"]=="builtin.shell")
        ||(history&&tool.definition.name=="SessionSearch"&&tool.definition.metadata["source"]=="builtin.session-history");
}
class MemoryPolicy final:public PermissionPolicy {
    std::shared_ptr<const PermissionPolicy> parent;QString sessionId,directory;bool history;
    ToolContext bound(ToolContext context)const{context.sessionId=sessionId;return context;}
public:
    MemoryPolicy(std::shared_ptr<const PermissionPolicy> p,QString id,QString path,bool history):parent(std::move(p)),sessionId(std::move(id)),directory(std::move(path)),history(history){}
    QStringList workingDirectories(const ToolContext& context)const override{return parent->workingDirectories(bound(context));}
    QJsonObject describe(const ToolContext& context)const override{return parent->describe(bound(context));}
    PermissionDecision decide(const ToolDefinition& tool,const QJsonObject& args,const ToolContext& context)const override {
        auto preview=tool;auto scoped=bound(context);scoped.allowedTools.clear();
        if(tool.name=="Write"||tool.name=="Edit") {
            const auto path=tool.metadata["canonical_path"].toString();
            if(tool.metadata["source"]!="builtin.workspace"||tool.metadata["memory_directory"]!=directory||!inside(path,directory)||path==directory)
                return {PermissionBehavior::Deny,"Memory maintenance writes only to this project's owned memory directory"};
            scoped.permissionMode=PermissionMode::AcceptEdits;
        } else if(tool.name=="Bash") {
            if(tool.metadata["source"]!="builtin.shell"||!detail::readOnlyShell(args,context))
                return {PermissionBehavior::Deny,"Memory maintenance permits only classified read-only foreground shell commands"};
            preview.readOnly=true;scoped.permissionMode=PermissionMode::DontAsk;
        } else if(history&&tool.name=="SessionSearch"&&tool.metadata["source"]=="builtin.session-history")scoped.permissionMode=PermissionMode::DontAsk;
        else if(tool.metadata["source"]!="builtin.workspace"||!QStringList{"Read","Glob","Grep"}.contains(tool.name))
            return {PermissionBehavior::Deny,"Tool is unavailable to memory maintenance"};
        else scoped.permissionMode=PermissionMode::DontAsk;
        auto decision=parent->decide(preview,args,scoped);
        if(decision.behavior==PermissionBehavior::Ask)decision={PermissionBehavior::Deny,"Memory maintenance cannot request interactive permission"};
        return decision;
    }
};
class Deadline {
    std::mutex mutex;std::condition_variable changed;bool done=false;std::thread worker;
public:
    Clock::time_point at;CancellationToken token;
    Deadline(const CancellationToken& parent,int timeout):at(Clock::now()+std::chrono::milliseconds(timeout)),token(CancellationToken::linkedTo(parent)) {
        worker=std::thread([this]{std::unique_lock lock(mutex);if(!changed.wait_until(lock,at,[&]{return done;}))token.cancel();});
    }
    ~Deadline(){{std::lock_guard lock(mutex);done=true;}changed.notify_all();worker.join();}
    bool expired()const{return Clock::now()>=at;}
};
}
qsizetype memoryContextBytes(const ModelRequest& request){return contextBytes(request);}
std::shared_ptr<const FrozenMemoryContext> freezeMemoryContext(MemoryContext snapshot,const ProjectMemory& memory,int maxInputBytes) {
    require(!snapshot.sessionId.isEmpty()&&snapshot.sessionId.size()<=256&&!snapshot.sessionId.contains(QChar::Null)&&snapshot.registry
        &&!snapshot.messages.isEmpty()&&!snapshot.messages.last().id.isEmpty(),"Invalid parent extraction context");
    require(snapshot.messages.last().role==MessageRole::Assistant&&snapshot.messages.last().toolCalls.isEmpty(),"Extraction requires a completed parent response");
    require(snapshot.request.messages.size()>=snapshot.messages.size()&&snapshot.request.messages.last()==snapshot.messages.last(),"Parent extraction request must include its final assistant message");
    require(contextBytes(snapshot.request)<=maxInputBytes,"Parent extraction context exceeds byte limit",ErrorCode::ResourceLimit);
    require(pendingToolCalls(snapshot.messages).isEmpty(),"Parent extraction context has pending tools");
    snapshot.workspace=QFileInfo(snapshot.workspace).canonicalFilePath();require(!snapshot.workspace.isEmpty(),"Memory workspace is unavailable");
    (void)memory.directory(snapshot.workspace);
    auto stable=std::make_shared<FrozenMemoryContext>();static_cast<MemoryContext&>(*stable)=std::move(snapshot);stable->registry=stable->registry->snapshot();
    try {
        const auto read=stable->registry->get("Read");
        if(!read.isMcp&&read.definition.metadata["source"]=="builtin.workspace"&&read.captureReadState) {
            auto parent=stable->context;parent.sessionId=stable->sessionId;parent.workingDirectory=stable->workspace;parent.cancellation={};
            stable->inheritReads=read.captureReadState(parent);stable->clearReads=read.clearReadState;
        }
    }catch(const Error& error){if(error.code()!=ErrorCode::NotFound)throw;}
    // Drop live callbacks, cancellation and parent transcript/artifact access.
    ToolContext context;context.contextRevision=stable->context.contextRevision;context.protectedPaths=stable->context.protectedPaths;
    context.plansDirectory=stable->context.plansDirectory;context.maxReadBytes=stable->context.maxReadBytes;stable->context=std::move(context);
    return stable;
}
QJsonObject runMemoryWorker(const FrozenMemoryContext& snapshot,std::shared_ptr<ProjectMemory> memory,std::shared_ptr<Model> model,
    std::shared_ptr<const PermissionPolicy> policy,const MemoryWorkerOptions& options,const CancellationToken& token,const QString& jobId,
    const std::function<Message(const QString&,const CancellationToken&)>& instruction) {
    QJsonObject result;const auto started=Clock::now();Deadline deadline(token,options.timeoutMs);
    Usage total;QStringList written;int turns=0,toolCalls=0,toolErrors=0,validationRetries=0;bool validated=false;qsizetype outputBytes=0;
    try {
        deadline.token.throwIfCancelled();const auto directory=memory->directory(snapshot.workspace,deadline.token);
        auto request=snapshot.request;request.messages.append(instruction(directory,deadline.token));
                ToolContext context;context.sessionId=options.activity+"/"+jobId;context.runId=jobId;context.workingDirectory=snapshot.workspace;
                context.contextRevision=snapshot.context.contextRevision;context.protectedPaths=snapshot.context.protectedPaths;
                context.plansDirectory=snapshot.context.plansDirectory;context.cancellation=deadline.token;context.forceSynchronousHooks=true;
                context.hookCancellation=deadline.token;context.maxReadBytes=snapshot.context.maxReadBytes;context.readOnlyShell=true;
                context.protectedPaths.append(QFileInfo(directory).absolutePath());
                struct ReadScope {
                    const FrozenMemoryContext& snapshot;ToolContext context;
                    ~ReadScope(){if(snapshot.clearReads)try{context.cancellation={};snapshot.clearReads(context);}catch(...) {}}
                } reads{snapshot,context};
                if(snapshot.inheritReads)snapshot.inheritReads(context);
                auto scopedPolicy=std::make_shared<MemoryPolicy>(policy,snapshot.sessionId,directory,options.history);
                ToolRunnerOptions runnerOptions;runnerOptions.hooks=options.hooks;runnerOptions.hookModel=model;runnerOptions.hookModelName=request.model;
                runnerOptions.hookAgent=options.hookAgent;
                runnerOptions.maxResultCharacters=std::min(options.maxOutputBytes,24000);auto registry=snapshot.registry->snapshot();
                if(options.history) {
                    const auto native=registry->get("SessionSearch");require(permittedIdentity(native,true),"Owned native history search is required");
                    auto history=native;history.execute=[execute=native.execute,owner=snapshot.sessionId](const QJsonObject& args,ToolContext c){c.sessionId=owner;return execute(args,c);};
                    registry->replace({"SessionSearch"},{std::move(history)});
                }
                ToolRunner runner(registry,scopedPolicy,runnerOptions);
                QSet<QString> ids;for(const auto& message:request.messages)for(const auto& call:message.toolCalls)ids.insert(call.id);
                result["status"]="turn_limit";
                while(turns<options.maxTurns) {
                    deadline.token.throwIfCancelled();require(contextBytes(request)<=options.maxInputBytes,"Memory maintenance context exceeds byte limit",ErrorCode::ResourceLimit);
                    qsizetype streamed=0;++turns;
                    const auto reply=model->generate(request,deadline.token,[&](const QString& text){deadline.token.throwIfCancelled();streamed+=text.toUtf8().size();
                        require(streamed<=options.maxOutputBytes,"Memory maintenance stream exceeds byte limit",ErrorCode::ResourceLimit);return true;});
                    total.promptTokens+=reply.usage.promptTokens;total.generatedTokens+=reply.usage.generatedTokens;total.cachedTokens+=reply.usage.cachedTokens;
                    deadline.token.throwIfCancelled();Message assistant{uuid(),MessageRole::Assistant,reply.text,reply.toolCalls};
                    outputBytes+=QJsonDocument(toJson(assistant)).toJson(QJsonDocument::Compact).size();
                    require(outputBytes<=options.maxOutputBytes&&reply.toolCalls.size()<=options.maxToolCallsPerTurn,"Memory maintenance output exceeds limit",ErrorCode::ResourceLimit);
                    for(auto& call:assistant.toolCalls){if(call.id.isEmpty())call.id=uuid();require(!ids.contains(call.id),"Duplicate extraction tool call ID",ErrorCode::ProtocolError);ids.insert(call.id);}
                    request.messages.append(assistant);
                    if(options.progress) {
                        QJsonArray paths;for(const auto& call:assistant.toolCalls)if((call.name=="Write"||call.name=="Edit")&&call.arguments["path"].isString())paths.append(preview(call.arguments["path"].toString(),4096));
                        try{options.progress({{"text",preview(assistant.text,1024)},{"text_truncated",assistant.text.size()>1024},
                            {"tool_use_count",assistant.toolCalls.size()},{"attempted_paths",paths}});}catch(...){}
                    }
                    if(assistant.toolCalls.isEmpty()) {
                        if(options.completionCheck) {
                            const auto feedback=options.completionCheck(deadline.token);
                            if(!feedback.isEmpty()){++validationRetries;request.messages.append({uuid(),MessageRole::User,feedback});continue;}
                            validated=true;
                        }
                        result["status"]="completed";break;
                    }
                    for(const auto& call:assistant.toolCalls) {
                        deadline.token.throwIfCancelled();++toolCalls;ToolResult observation;
                        auto fork=std::make_shared<Session>();fork->id=context.sessionId;fork->model=request.model;fork->systemPrompt=request.systemPrompt;
                        fork->workingDirectory=snapshot.workspace;fork->messages=request.messages;fork->parentSessionId=snapshot.sessionId;context.sessionSnapshot=std::move(fork);
                        try {
                            const auto tool=registry->get(call.name);
                            require(permittedIdentity(tool,options.history),"Tool is outside the memory maintenance capability scope");
                            // Never invoke unknown/MCP validation, preparation or callbacks just to reject them.
                            observation=runner.run(call,context);
                        }catch(const std::exception& error){observation={QString::fromUtf8(error.what()),{},true};}
                        if(observation.isError)++toolErrors;
                        else if(call.name=="Write"||call.name=="Edit") {
                            const auto path=observation.data["path"].toString();
                            if(inside(path,directory)&&!written.contains(path))written.append(path);
                        }
                        Message message{uuid(),MessageRole::Tool,observation.text};message.toolCallId=call.id;message.isError=observation.isError;
                        message.data=observation.data;message.content=observation.content;message.metadata=observation.metadata;
                        request.messages.append(message);
                    }
                }
        }catch(const std::exception& error) {
            result["status"]=token.isCancelled()?"cancelled":deadline.expired()?"timeout":"failed";
            auto diagnostics=result["diagnostics"].toArray();diagnostics.append(QJsonObject{{"error",QString::fromUtf8(error.what()).left(2048)}});result["diagnostics"]=diagnostics;
        }catch(...) {result["status"]="failed";result["diagnostics"]=QJsonArray{QJsonObject{{"error","Unknown memory maintenance failure"}}};}
        result["usage"]=usage(total);result["turns"]=turns;result["tool_calls"]=toolCalls;result["tool_errors"]=toolErrors;
        if(options.completionCheck){result["validation_retries"]=validationRetries;result["completion_validated"]=validated;}
        result["written_paths"]=QJsonArray::fromStringList(written);QStringList topics;
        for(const auto& path:written)if(QFileInfo(path).fileName()!="MEMORY.md")topics.append(path);
        result["saved_topics"]=QJsonArray::fromStringList(topics);result["written_count"]=written.size();result["saved_topic_count"]=topics.size();
        // A paged status response must also remain bounded when filenames are long.
        while(QJsonDocument(result).toJson(QJsonDocument::Compact).size()>65536&&(!written.isEmpty()||!topics.isEmpty())) {
            if(!written.isEmpty())written.removeLast();if(!topics.isEmpty())topics.removeLast();result["paths_truncated"]=true;
            result["written_paths"]=QJsonArray::fromStringList(written);result["saved_topics"]=QJsonArray::fromStringList(topics);
        }
        result["duration_ms"]=double(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count());return result;
}
}
