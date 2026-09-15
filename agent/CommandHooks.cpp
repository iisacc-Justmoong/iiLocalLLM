#include "CommandHooks.h"
#include "HttpHook.h"
#include "ModelHook.h"
#include "PermissionRules.h"
#include "PermissionRulesInternal.h"
#include "PermissionResponses.h"
#include "ShellProcess.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSemaphore>
#include <QtCore/QSet>
#include <QtCore/QStringDecoder>
#include <QtCore/QThreadPool>
#include <chrono>
#include <mutex>

namespace iiLocalLLM::agent {
namespace {
void require(bool value,const QString& message,ErrorCode code=ErrorCode::InvalidArgument) {
    if(!value)throw Error(code,message);
}
void keys(const QJsonObject& object,const QStringList& known) {
    for(auto i=object.begin();i!=object.end();++i)require(known.contains(i.key()),"Unsupported command hook field: "+i.key());
}
QString string(const QJsonValue& value,int limit=65536) {
    require(value.isString()&&!value.toString().contains(QChar::Null)&&value.toString().size()<=limit,"Invalid hook string");return value.toString();
}
QString text(const QByteArray& bytes) {
    QStringDecoder decoder(QStringDecoder::Utf8,QStringDecoder::Flag::Stateless);const QString result=decoder(bytes);
    require(!decoder.hasError(),"Hook output is not UTF-8");return result;
}
QString eventName(const HookInput& input) {
    switch(input.kind) {
    case HookKind::BeforeTool:return "PreToolUse";
    case HookKind::AfterTool:return input.result.isError?"PostToolUseFailure":"PostToolUse";
    case HookKind::Stop:return "Stop";
    case HookKind::BeforeCompact:return "PreCompact";
    case HookKind::AfterCompact:return "PostCompact";
    case HookKind::TaskCreated:return "TaskCreated";
    case HookKind::TaskCompleted:return "TaskCompleted";
    case HookKind::SubagentStart:return "SubagentStart";
    case HookKind::SubagentStop:return "SubagentStop";
    case HookKind::BeforeModel:return "BeforeModel";
    case HookKind::AfterModel:return "AfterModel";
    case HookKind::UserPromptSubmit:return "UserPromptSubmit";
    case HookKind::SessionStart:return "SessionStart";
    case HookKind::SessionEnd:return "SessionEnd";
    case HookKind::PermissionRequest:return "PermissionRequest";
    }
    throw Error(ErrorCode::InvalidArgument,"Unknown hook event");
}
void merge(HookResult& target,const HookResult& value,bool request) {
    if(request) {
        // First completed decision owns its whole payload. Later hooks may log
        // diagnostics but cannot mix a deny or new input into that decision.
        if(!target.permissionResponse&&!target.block&&!target.stop) {
            target.permissionResponse=value.permissionResponse;target.block=value.block;target.stop=value.stop;
            target.stopReason=value.stopReason;target.feedback=value.feedback;
        }
        for(const auto& event:value.diagnostics)target.diagnostics.append(event);
        return;
    }
    target.block|=value.block;target.stop|=value.stop;
    if(!value.stopReason.isEmpty())target.stopReason=value.stopReason;
    if(!value.feedback.isEmpty()) {if(!target.feedback.isEmpty())target.feedback+='\n';target.feedback+=value.feedback;}
    if(value.updatedArguments)target.updatedArguments=value.updatedArguments;
    if(value.initialUserMessage)target.initialUserMessage=value.initialUserMessage;
    if(value.permission) {
        auto priority=[](PermissionBehavior b){return b==PermissionBehavior::Deny?3:b==PermissionBehavior::Ask?2:1;};
        if(!target.permission||priority(value.permission->behavior)>=priority(target.permission->behavior))target.permission=value.permission;
    }
    for(const auto& event:value.diagnostics)target.diagnostics.append(event);
}
HookResult response(const QJsonObject& object,const QString& event,bool& suppress) {
    HookResult result;
    keys(object,{"continue","suppressOutput","stopReason","decision","reason","systemMessage","hookSpecificOutput"});
    for(const auto& key:{"continue","suppressOutput"})if(object.contains(key))require(object[key].isBool(),"Hook boolean expected");
    for(const auto& key:{"stopReason","reason","systemMessage"})if(object.contains(key))string(object[key]);
    suppress=object["suppressOutput"].toBool();result.stop=object["continue"]==false;
    if(result.stop)result.stopReason=object["stopReason"].toString("Stopped by command hook");
    auto permission=[&](const QString& behavior,const QString& reason) {
        require(QStringList{"allow","deny","ask","passthrough"}.contains(behavior),"Unknown hook permission decision");
        if(behavior=="passthrough")return;
        result.permission=PermissionDecision{behavior=="allow"?PermissionBehavior::Allow:behavior=="ask"?PermissionBehavior::Ask:PermissionBehavior::Deny,reason};
        result.block=behavior=="deny";
        if(result.block&&event!="SessionStart"&&event!="SessionEnd")result.feedback=reason.isEmpty()?QString("Blocked by command hook"):reason;
    };
    if(object.contains("decision")) {
        const auto decision=string(object["decision"]);require(decision=="approve"||decision=="block","Invalid hook decision");
        permission(decision=="approve"?"allow":"deny",object["reason"].toString());
    }
    if(object.contains("hookSpecificOutput")) {
        require(object["hookSpecificOutput"].isObject(),"hookSpecificOutput must be an object");const auto specific=object["hookSpecificOutput"].toObject();
        require(specific["hookEventName"]==event,"Hook output event does not match input event");
        const bool pre=event=="PreToolUse";
        auto known=pre?QStringList{"hookEventName","permissionDecision","permissionDecisionReason","updatedInput","additionalContext"}
                      :QStringList{"hookEventName","additionalContext"};
        if(event=="SessionStart")known.append("initialUserMessage");
        if(event=="PermissionRequest")known={"hookEventName","decision"};keys(specific,known);
        if(event=="PermissionRequest") {
            require(specific["decision"].isObject(),"PermissionRequest requires a decision object");const auto decision=specific["decision"].toObject();
            const auto behavior=string(decision["behavior"]);require(behavior=="allow"||behavior=="deny","PermissionRequest must allow or deny");
            PermissionResponse response;response.behavior=behavior=="allow"?PermissionBehavior::Allow:PermissionBehavior::Deny;
            keys(decision,behavior=="allow"?QStringList{"behavior","updatedInput","updatedPermissions"}:QStringList{"behavior","message","interrupt"});
            if(decision.contains("message"))response.message=string(decision["message"]);
            if(decision.contains("interrupt")){require(decision["interrupt"].isBool(),"Permission interrupt must be boolean");response.interrupt=decision["interrupt"].toBool();}
            if(decision.contains("updatedInput")){require(decision["updatedInput"].isObject(),"Permission updatedInput must be an object");response.updatedArguments=decision["updatedInput"].toObject();}
            if(decision.contains("updatedPermissions")){require(decision["updatedPermissions"].isArray(),"Permission updatedPermissions must be an array");response.updatedPermissions=decision["updatedPermissions"].toArray();}
            detail::validatePermissionResponse(response);result.permissionResponse=response;
        }
        if(specific.contains("permissionDecision"))permission(string(specific["permissionDecision"]),specific.contains("permissionDecisionReason")?string(specific["permissionDecisionReason"]):object["reason"].toString());
        if(specific.contains("updatedInput")) {require(specific["updatedInput"].isObject(),"Hook updatedInput must be an object");if(!result.block)result.updatedArguments=specific["updatedInput"].toObject();}
        if(specific.contains("additionalContext")) {const auto value=string(specific["additionalContext"]);if(!result.feedback.isEmpty())result.feedback+='\n';result.feedback+=value;}
        if(specific.contains("initialUserMessage"))result.initialUserMessage=string(specific["initialUserMessage"]);
    }
    if(object.contains("systemMessage"))result.diagnostics.append(QJsonObject{{"system_message",object["systemMessage"]}});
    return result;
}
}
class CommandHooks::Impl {
public:
    struct Entry {QString event,matcher,command,condition,status,sha;std::optional<detail::HttpHook> http;std::optional<detail::PromptHook> prompt;QRegularExpression regex;bool literal=false,once=false;int timeout=0,index=0;};
    CommandHookOptions options;
    QList<Entry> entries;
    QSemaphore permits;
    std::mutex mutex;
    QSet<QString> once;
    Impl(QJsonObject settings,CommandHookOptions value):options(std::move(value)) {
        require(options.timeoutMs>0&&options.timeoutMs<=3600000&&options.maxHooks>0&&options.maxHooks<=1024
            &&options.maxConcurrentProcesses>0&&options.maxConcurrentProcesses<=32&&options.maxInputBytes>0&&options.maxInputBytes<=4*1024*1024
            &&options.maxOutputBytes>0&&options.maxOutputBytes<=4*1024*1024&&options.maxOnceEntries>0&&options.maxOnceEntries<=1048576
            &&options.maxModelTokens>0&&options.maxModelTokens<=16384,"Invalid command hook limits");
        options.workingDirectory=QFileInfo(options.workingDirectory).canonicalFilePath();
        require(!options.workingDirectory.isEmpty()&&QFileInfo(options.workingDirectory).isDir(),"Command hook workspace must exist");
        permits.release(options.maxConcurrentProcesses);keys(settings,{"hooks","allowedHttpHookUrls","httpHookAllowedEnvVars"});
        detail::validateHttpHookSettings(settings);
        require(settings["hooks"].isObject(),"Command hook settings require a hooks object");
        const QStringList events{"PreToolUse","PostToolUse","PostToolUseFailure","Stop","PreCompact","PostCompact","TaskCreated","TaskCompleted","SubagentStart","SubagentStop","BeforeModel","AfterModel","UserPromptSubmit","SessionStart","SessionEnd","PermissionRequest"};
        const auto hooks=settings["hooks"].toObject();
        for(auto i=hooks.begin();i!=hooks.end();++i) {
            require(events.contains(i.key()),"Unsupported command hook event: "+i.key(),ErrorCode::RuntimeUnavailable);
            require(i->isArray(),"Hook matchers must be an array");
            for(const auto& v:i->toArray()) {
                require(v.isObject(),"Hook matcher must be an object");const auto group=v.toObject();keys(group,{"matcher","hooks"});
                const auto matcher=group.contains("matcher")?string(group["matcher"],512):QString();
                const bool literal=QRegularExpression("^[a-zA-Z0-9_|]+$").match(matcher).hasMatch();
                QRegularExpression regex;if(!literal&&!matcher.isEmpty()&&matcher!="*") {regex=QRegularExpression(matcher);require(regex.isValid(),"Invalid hook matcher expression");}
                require(group["hooks"].isArray(),"Matcher hooks must be an array");
                for(const auto& h:group["hooks"].toArray()) {
                    require(entries.size()<options.maxHooks,"Too many command hooks",ErrorCode::ResourceLimit);
                    require(h.isObject(),"Command hook must be an object");const auto object=h.toObject();
                    const bool http=object["type"]=="http";
                    const bool prompt=object["type"]=="prompt";
                    require(http||prompt||object["type"]=="command","Unsupported hook type",ErrorCode::RuntimeUnavailable);
                    keys(object,prompt?QStringList{"type","prompt","model","if","timeout","statusMessage","once"}
                        :http?QStringList{"type","url","if","timeout","headers","allowedEnvVars","statusMessage","once"}
                        :QStringList{"type","command","if","shell","timeout","statusMessage","once","async","asyncRewake"});
                    for(const auto& flag:{"once","async","asyncRewake"})if(object.contains(flag))require(object[flag].isBool(),"Hook flag must be boolean");
                    require(!object["async"].toBool()&&!object["asyncRewake"].toBool(),"Background command hooks are not implemented",ErrorCode::RuntimeUnavailable);
                    require(!object.contains("shell")||object["shell"]=="bash","Only POSIX command hook shells are implemented",ErrorCode::RuntimeUnavailable);
#if !defined(Q_OS_UNIX) || defined(Q_OS_IOS) || defined(Q_OS_ANDROID) || defined(Q_OS_WASM)
                    if(!http&&!prompt)throw Error(ErrorCode::RuntimeUnavailable,"Command hooks require a desktop POSIX host");
#endif
                    Entry entry;entry.event=i.key();entry.matcher=matcher;entry.literal=literal;entry.regex=regex;
                    if(prompt) {
                        entry.prompt=detail::PromptHook{string(object["prompt"]),{}};
                        require(!entry.prompt->prompt.trimmed().isEmpty(),"Empty model hook prompt");
                        if(object.contains("model")){entry.prompt->model=string(object["model"],256);require(!entry.prompt->model.trimmed().isEmpty(),"Empty hook model");}
                    } else if(http)entry.http=detail::parseHttpHook(object,settings);
                    else {entry.command=string(object["command"]);require(!entry.command.trimmed().isEmpty(),"Empty hook command");}
                    entry.timeout=prompt?30000:options.timeoutMs;entry.once=object["once"].toBool();entry.index=entries.size();
                    if(object.contains("timeout")) {const auto seconds=object["timeout"].toDouble(-1);require(seconds>0&&seconds<=3600,"Invalid hook timeout");entry.timeout=qMax(1,int(seconds*1000));}
                    if(object.contains("statusMessage"))entry.status=string(object["statusMessage"],1024);
                    if(object.contains("if")) {entry.condition=string(object["if"],4096);require(parsePermissionRules({entry.condition}).size()==1,"Hook if requires one permission rule");}
                    entry.sha=QString::fromLatin1(QCryptographicHash::hash((prompt?entry.prompt->prompt:http?entry.http->url:entry.command).toUtf8(),QCryptographicHash::Sha256).toHex());entries.append(std::move(entry));
                }
            }
        }
    }
    bool matches(const Entry& entry,const HookInput& input,const QString& event) const {
        if(entry.event!=event)return false;
        QString query=input.call.name;
        if(event=="SubagentStart"||event=="SubagentStop")query=input.context["agent_type"].toString();
        if(event=="PreCompact"||event=="PostCompact")query=input.context["trigger"].toString();
        if(event=="SessionStart")query=input.context["source"].toString();
        if(event=="SessionEnd")query=input.context["reason"].toString();
        if(!entry.matcher.isEmpty()&&entry.matcher!="*") {
            if(entry.literal) {if(!entry.matcher.split('|').contains(query))return false;}
            else if(!entry.regex.match(query).hasMatch())return false;
        }
        if(!entry.condition.isEmpty()) {
            if(input.call.name.isEmpty())return false;
            ToolContext context{input.sessionId,input.runId,options.workingDirectory};
            if(!detail::permissionRulesMatch({{entry.condition,PermissionBehavior::Allow}},ToolDefinition{input.call.name},input.call.arguments,context,false))return false;
        }
        return true;
    }
    HookResult execute(const Entry& entry,const HookInput& input,const QString& event,const QByteArray& payload,const CancellationToken& token) {
        bool acquired=false,reserved=false,started=false;const auto key=input.sessionId+QChar::Null+QString::number(entry.index);
        const auto begin=std::chrono::steady_clock::now();QByteArray stdoutBytes,stderrBytes;HookResult result;
        QJsonObject diagnostic{{"hook_event_name",event},{"hook_index",entry.index},{"hook_type",entry.prompt?"prompt":entry.http?"http":"command"},
            {entry.prompt?"prompt_sha256":entry.http?"url_sha256":"command_sha256",entry.sha},{"status_message",entry.status}};
        auto release=[&] {if(acquired)permits.release();if(reserved&&!started) {std::lock_guard lock(mutex);once.remove(key);}};
        try {
            if(entry.once) {
                require(!input.sessionId.isEmpty(),"Once hooks require a session identity");std::lock_guard lock(mutex);
                if(once.contains(key))return {};
                require(once.size()<options.maxOnceEntries,"Command hook once table is full",ErrorCode::ResourceLimit);once.insert(key);reserved=true;
            }
            while(!permits.tryAcquire(1,20))token.throwIfCancelled();acquired=true;token.throwIfCancelled();
            if(entry.prompt) {
                const auto outcome=detail::evaluatePromptHook(*entry.prompt,input,payload,options,entry.timeout,token,[&]{started=true;});
                result=outcome.result;diagnostic["model"]=outcome.model;
                diagnostic["history_messages"]=input.modelContext->session?modelMessages(*input.modelContext->session).size():0;
                diagnostic["usage"]=QJsonObject{{"prompt_tokens",outcome.usage.promptTokens},{"generated_tokens",outcome.usage.generatedTokens},{"cached_tokens",outcome.usage.cachedTokens}};
                diagnostic["outcome"]=result.block?"blocked":"success";
                if(event=="SessionStart"||event=="SessionEnd"){result.block=false;result.stop=false;result.feedback.clear();result.stopReason.clear();}
            } else if(entry.http) {
                const auto outcome=detail::postHttpHook(*entry.http,payload,options,entry.timeout,token,[&]{started=true;});
                diagnostic["http_status"]=outcome.status;diagnostic["response_bytes"]=outcome.body.size();
                require(outcome.status>=200&&outcome.status<300,"HTTP hook returned a non-success status",ErrorCode::ProtocolError);
                const auto decoded=text(outcome.body);const auto trimmed=decoded.trimmed();
                QJsonParseError error;const auto document=QJsonDocument::fromJson(trimmed.isEmpty()?QByteArray("{}"):trimmed.toUtf8(),&error);
                require(error.error==QJsonParseError::NoError&&document.isObject(),"HTTP hook must return a JSON object",ErrorCode::ProtocolError);
                const auto object=document.object();bool suppress=false;
                if(object.contains("async")) {
                    keys(object,{"async","asyncTimeout"});require(object["async"]==true,"Invalid HTTP hook async acknowledgement");
                    require(!object.contains("asyncTimeout")||object["asyncTimeout"].isDouble(),"Invalid HTTP hook asyncTimeout");
                    // The endpoint acknowledged asynchronous work. No local
                    // background process, later decision or callback is implied.
                } else result=response(object,event,suppress);
                diagnostic["outcome"]=result.block||(result.permissionResponse&&result.permissionResponse->behavior==PermissionBehavior::Deny)?"blocked":"success";
            } else {
            auto environment=options.environment;environment.insert("CLAUDE_PROJECT_DIR",options.workingDirectory);environment.insert("IILOCALLLM_PROJECT_DIR",options.workingDirectory);
            const auto outcome=detail::shellProcess(options.workingDirectory,entry.command,entry.timeout,token,[&]{started=true;},[&](const QByteArray& bytes,bool error) {
                require(stdoutBytes.size()+stderrBytes.size()+bytes.size()<=options.maxOutputBytes,"Command hook output exceeds byte limit",ErrorCode::ResourceLimit);
                (error?stderrBytes:stdoutBytes).append(bytes);
            },payload,&environment,"/bin/sh",{"-c",entry.command});
            auto stdoutText=text(stdoutBytes),stderrText=text(stderrBytes);bool suppress=false;
            diagnostic["exit_code"]=outcome.code;diagnostic["crashed"]=outcome.crashed;
            const auto trimmed=stdoutBytes.trimmed();QJsonParseError error;QJsonDocument document;
            if(trimmed.startsWith('{'))document=QJsonDocument::fromJson(trimmed,&error);
            if(event=="PermissionRequest"&&outcome.code==2)result.permissionResponse=PermissionResponse{PermissionBehavior::Deny,stderrText.isEmpty()?QString("Permission denied by command hook"):stderrText};
            else if(document.isObject()&&error.error==QJsonParseError::NoError&&(event!="PermissionRequest"||outcome.code==0))result=response(document.object(),event,suppress);
            else if(outcome.code==2&&event!="SessionStart"&&event!="SessionEnd") {result.block=true;result.feedback=stderrText.isEmpty()?QString("Blocked by command hook"):stderrText;}
            else if(outcome.code==0&&QStringList{"BeforeModel","PreCompact","UserPromptSubmit","SessionStart"}.contains(event))result.feedback=stdoutText.trimmed();
            diagnostic["outcome"]=result.block||(result.permissionResponse&&result.permissionResponse->behavior==PermissionBehavior::Deny)?"blocked":outcome.code==0?"success":"non_blocking_error";
            if(!suppress)diagnostic["stdout"]=stdoutText.left(4096);diagnostic["stderr"]=stderrText.left(4096);
            }
        } catch(const Error& error) {
            if((error.code()==ErrorCode::Cancelled&&(!entry.prompt||token.isCancelled()))||error.code()==ErrorCode::ConsumerFailure) {release();throw;}
            diagnostic["outcome"]=entry.prompt&&(error.code()==ErrorCode::Timeout||error.code()==ErrorCode::Cancelled)?"cancelled":"non_blocking_error";diagnostic["error_code"]=enumName(error.code());diagnostic["error"]=QString::fromUtf8(error.what());
        } catch(const std::exception& error) {diagnostic["outcome"]="non_blocking_error";diagnostic["error"]=QString::fromUtf8(error.what());}
        release();diagnostic["duration_ms"]=double(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count());
        result.diagnostics.append(diagnostic);return result;
    }
    HookResult invoke(const HookInput& input,const CancellationToken& token) {
        token.throwIfCancelled();const auto event=eventName(input);
        QList<Entry> matching;QHash<QString,qsizetype> httpPositions;
        for(const auto& entry:entries)if(matches(entry,input,event)) {
            if(entry.http||entry.prompt) {
                const auto key=(entry.prompt?QString("prompt")+QChar::Null+entry.prompt->prompt:QString("http")+QChar::Null+entry.http->url)+QChar::Null+entry.condition;
                const auto found=httpPositions.constFind(key);
                if(found!=httpPositions.cend()){matching[*found]=entry;continue;}
                httpPositions.insert(key,matching.size());
            }
            matching.append(entry);
        }
        matching.removeIf([&](const Entry& entry){
            if(!entry.once)return false;
            std::lock_guard lock(mutex);return once.contains(input.sessionId+QChar::Null+QString::number(entry.index));
        });
        if(matching.isEmpty())return {};
        QJsonObject body=input.context;body["hook_event_name"]=event;body["session_id"]=input.sessionId;body["run_id"]=input.runId;
        body["cwd"]=options.workingDirectory;
        if(!body.contains("transcript_path"))body["transcript_path"]="";
        if(!body.contains("permission_mode"))body["permission_mode"]="unknown";
        if(!input.call.name.isEmpty()) {body["tool_name"]=input.call.name;body["tool_input"]=input.call.arguments;body["tool_use_id"]=input.call.id;}
        if(input.kind==HookKind::AfterTool) {
            if(input.result.isError) {body["error"]=input.result.text;body["is_interrupt"]=false;}
            else body["tool_response"]=QJsonObject{{"text",input.result.text},{"data",input.result.data},{"content",input.result.content}};
        }
        if(event=="Stop"||event=="SubagentStop") {body["last_assistant_message"]=input.text;if(!body.contains("stop_hook_active"))body["stop_hook_active"]=false;}
        if(input.kind==HookKind::BeforeModel)body["prompt"]=input.text;
        if(input.kind==HookKind::UserPromptSubmit)body["prompt"]=input.text;
        if(input.kind==HookKind::AfterModel)body["response"]=input.text;
        if(input.kind==HookKind::BeforeCompact)body["custom_instructions"]=input.text;
        if(input.kind==HookKind::AfterCompact)body["compact_summary"]=input.text;
        if(event=="TaskCreated"||event=="TaskCompleted") {
            body["task"]=input.call.arguments;body["task_id"]=input.call.arguments["id"];
            body["task_subject"]=input.call.arguments["subject"];body["task_description"]=input.call.arguments["description"];
        }
        const auto payload=QJsonDocument(body).toJson(QJsonDocument::Compact)+'\n';
        require(payload.size()<=options.maxInputBytes,"Command hook input exceeds byte limit",ErrorCode::ResourceLimit);
        HookResult result;std::mutex resultMutex;std::exception_ptr failure;QThreadPool pool;pool.setMaxThreadCount(options.maxConcurrentProcesses);
        for(const auto& entry:matching)pool.start([&,entry] {
            try {auto value=execute(entry,input,event,payload,token);std::lock_guard lock(resultMutex);merge(result,value,event=="PermissionRequest");}
            catch(...) {std::lock_guard lock(resultMutex);if(!failure)failure=std::current_exception();}
        });
        pool.waitForDone();if(failure)std::rethrow_exception(failure);token.throwIfCancelled();return result;
    }
};
CommandHooks::CommandHooks(QJsonObject settings,CommandHookOptions options):d(std::make_shared<Impl>(std::move(settings),std::move(options))) {}
Hook CommandHooks::callback() const {return [impl=d](const HookInput& input,const CancellationToken& token){return impl->invoke(input,token);};}
QJsonObject CommandHooks::describe() const {
    bool http=false,command=false,prompt=false;for(const auto& e:d->entries){http|=e.http.has_value();prompt|=e.prompt.has_value();command|=!e.http&&!e.prompt;}
    QJsonArray entries;for(const auto& e:d->entries)entries.append(QJsonObject{{"event",e.event},{"matcher",e.matcher},{"condition",e.condition},{"once",e.once},
        {"timeout_ms",e.timeout},{"hook_type",e.prompt?"prompt":e.http?"http":"command"},{e.prompt?"prompt_sha256":e.http?"url_sha256":"command_sha256",e.sha},{"status_message",e.status}});
    return {{"provider",int(http)+int(command)+int(prompt)>1?"configured":prompt?"prompt":http?"http":"command"},{"hooks",entries},
        {"max_concurrent_hooks",d->options.maxConcurrentProcesses},{"max_concurrent_processes",d->options.maxConcurrentProcesses}};
}
}
