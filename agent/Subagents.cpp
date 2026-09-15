#include "Subagents.h"
#include "SkillsInternal.h"
#include "PermissionRules.h"
#include "SessionOwners.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QLockFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QRunnable>
#include <QtCore/QThreadPool>
#include <QtCore/QUuid>
#include <QtCore/QDateTime>
#include <QtCore/QSet>
#include <condition_variable>
#include <map>
#include <mutex>
#include <chrono>
#include <algorithm>
#include <cmath>

namespace iiLocalLLM::agent {
namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
void require(bool condition, const QString& message, ErrorCode code = ErrorCode::InvalidArgument) { if (!condition) throw Error(code,message); }
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool nested(const QString& path,const QString& root) { return path==root || path.startsWith(root+'/'); }
bool terminal(const QString& s) { return s!="running" && s!="queued"; }
QJsonObject publicState(const QJsonObject& state) {
    QJsonObject result;
    for(const auto& key:{"agentId","agent_type","session_id","model","status","description","finished","retrieval_status","tool_uses","duration_ms","result","error","delivery_error","notification_id","notification_transfer_error","skill"})
        if(state.contains(key))result.insert(key,state[key]);
    return result;
}
QJsonObject profileJson(const SubagentDefinition& p) {
    return p.toJson(true);
}
bool matches(const QString& name,const QJsonArray& patterns) {
    for(const auto& p:patterns) if(QRegularExpression(QRegularExpression::wildcardToRegularExpression(p.toString(),QRegularExpression::NonPathWildcardConversion)).match(name).hasMatch()) return true;
    return false;
}
bool allowed(const ToolDefinition& t,const QJsonObject& p) {
    return t.name!="Agent" && t.metadata["source"]!="builtin.subagent" && matches(t.name,p["tools"].toArray())
        && !matches(t.name,p["disallowed_tools"].toArray()) && (!p["read_only"].toBool() || t.readOnly);
}
class ScopedModel final : public Model {
    std::shared_ptr<Model> model; QJsonObject original,current;
    ModelRequest filtered(ModelRequest r) const { r.tools.removeIf([&](const auto& t){return !allowed(t,original)||!allowed(t,current);});return r; }
public:
    ScopedModel(std::shared_ptr<Model> m,QJsonObject o,QJsonObject c):model(std::move(m)),original(std::move(o)),current(std::move(c)){}
    ModelReply generate(const ModelRequest& r,const CancellationToken& t,const TextCallback& cb) override {return model->generate(filtered(r),t,cb);}
    std::optional<ContextBudget> measure(const ModelRequest& r,const CancellationToken& t) override {return model->measure(filtered(r),t);}
};
class ScopedPolicy final : public PermissionPolicy {
    std::shared_ptr<const PermissionPolicy> parent; QJsonObject original,current;
public:
    ScopedPolicy(std::shared_ptr<const PermissionPolicy> p,QJsonObject o,QJsonObject c):parent(std::move(p)),original(std::move(o)),current(std::move(c)){}
    QStringList workingDirectories(const ToolContext& c) const override { return parent->workingDirectories(c); }
    QJsonObject describe(const ToolContext& c) const override {
        auto result=parent->describe(c);result["parent_mode"]=result["mode"];
        for(const auto& profile:{original,current}) {
            if(profile["permission_mode"]=="plan")result["mode"]="plan";
            else if(profile["permission_mode"]=="dontAsk"&&result["mode"]!="plan")result["mode"]="dontAsk";
        }
        return result;
    }
    void applyUpdates(const QJsonArray& updates,const ToolContext& c) const override {parent->applyUpdates(updates,c);}
    void inheritSession(const ToolContext& from,const ToolContext& to) const override {parent->inheritSession(from,to);}
    void forgetSession(const ToolContext& c) const override {parent->forgetSession(c);}
    PermissionDecision decide(const ToolDefinition& t,const QJsonObject& a,const ToolContext& c) const override {
        if(!allowed(t,original)||!allowed(t,current)) return {PermissionBehavior::Deny,"Tool is outside this subagent's scope"};
        auto decision=parent->decide(t,a,c);
        for(const auto& p:{original,current}) {
            const auto mode=p["permission_mode"].toString();
            if(mode=="plan" && RulePolicy(PermissionMode::Plan).decide(t,a,c).behavior==PermissionBehavior::Deny)
                return {PermissionBehavior::Deny,"Subagent plan mode forbids this tool"};
            if(mode=="dontAsk" && decision.behavior==PermissionBehavior::Ask)
                return {PermissionBehavior::Deny,"Subagent dontAsk mode cannot request permission"};
        }
        return decision;
    }
};
void checkProfile(const SubagentDefinition& p) {
    static const QRegularExpression name("\\A[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}\\z");
    require(name.match(p.name).hasMatch() && !p.description.trimmed().isEmpty() && p.description.size()<=4096
        && p.systemPrompt.size()<=1024*1024 && p.model.size()<=256 && p.maxTurns>=1 && p.maxTurns<=10000
        && p.tools.size()<=256 && p.disallowedTools.size()<=256 && p.skills.size()<=128
        && p.initialPrompt.size()<=65536 && !p.initialPrompt.contains(QChar::Null),"Invalid subagent definition");
    require(QStringList{"","default","inherit","acceptEdits","dontAsk","bypassPermissions","plan","auto"}.contains(p.permissionMode),"Invalid subagent permission mode");
    for(const auto& pattern:p.tools+p.disallowedTools) require(!pattern.isEmpty() && pattern.size()<=256 && !pattern.contains(QChar::Null),"Invalid subagent tool pattern");
}
}
class Subagents::Impl {
public:
    std::shared_ptr<Model> model;
    std::shared_ptr<ToolRegistry> registry;
    std::shared_ptr<const PermissionPolicy> policy;
    EngineOptions parent;
    SubagentOptions options;
    SessionStore parentStore,children;
    InputQueue notifications;
    QLockFile ownership;
    QThreadPool pool;
    mutable std::mutex mutex;
    std::mutex transferring;
    mutable std::condition_variable changed;
    bool stopping=false;
    struct Job { QJsonObject state; CancellationToken token; bool done=true; bool awaitingForeground=false; RunResult outcome; };
    std::map<QString,std::shared_ptr<Job>> jobs;
    std::unique_ptr<detail::SessionOwners> owners;
    static QString rootPath(QString path,const QString& workspace) {
        require(!path.trimmed().isEmpty() && !QFileInfo(path).isSymLink() && QDir().mkpath(path),"Cannot create subagent state",ErrorCode::StorageFailure);
        path=QFileInfo(path).canonicalFilePath();
        require(!path.isEmpty() && !nested(path,workspace) && !nested(workspace,path),"Subagent state and workspace must be disjoint");
        require(QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner),"Cannot protect subagent state",ErrorCode::StorageFailure);
        return path;
    }
    static QString sessionPath(const QString& path,const QString& workspace) {
        const auto root=rootPath(path,workspace);const auto sessions=QDir(root).filePath("sessions");
        require(!QFileInfo(sessions).isSymLink(),"Subagent sessions directory is a symlink",ErrorCode::StorageFailure);return sessions;
    }
    Impl(std::shared_ptr<Model> m,std::shared_ptr<ToolRegistry> r,std::shared_ptr<const PermissionPolicy> p,EngineOptions e,SubagentOptions o)
        :model(std::move(m)),registry(std::move(r)),policy(std::move(p)),parent(std::move(e)),options(std::move(o)),
         parentStore(parent.sessionsDirectory),children(sessionPath(options.stateDirectory,QFileInfo(options.workingDirectory).canonicalFilePath())),
         notifications(QDir(parent.sessionsDirectory).filePath("inputs"),parent.inputQueue),ownership(QDir(options.stateDirectory).filePath("agents.lock")) {
        require(model && registry && policy && options.maxConcurrent>=1 && options.maxConcurrent<=64 && options.maxRecords>=1 && options.maxRecords<=10000
            && options.maxTurns>=1 && options.maxTurns<=10000 && options.maxRuntimeMs>=1 && options.maxRuntimeMs<=86400000
            && options.definitions.size()<=128 && options.allowedModels.size()<=128 && options.modelAliases.size()<=128,"Invalid subagent host configuration");
        options.workingDirectory=QFileInfo(options.workingDirectory).canonicalFilePath(); options.stateDirectory=QFileInfo(options.stateDirectory).canonicalFilePath();
        require(!options.workingDirectory.isEmpty() && QFileInfo(options.workingDirectory).isDir() && !QDir(options.workingDirectory).isRoot(),"Invalid subagent workspace");
        require(!QFileInfo(ownership.fileName()).isSymLink(),"Subagent ownership lock is a symlink",ErrorCode::StorageFailure);
        ownership.setStaleLockTime(0); require(ownership.tryLock(0),"Subagent store already owned or inaccessible",ErrorCode::AlreadyExists);
        owners=std::make_unique<detail::SessionOwners>(QDir(options.stateDirectory).filePath("session-owners.json"),options.maxRecords);
        if(options.definitions.isEmpty() && !options.profiles.enabled) options.definitions.append(SubagentDefinition{});
        QSet<QString> names; for(const auto& definition:options.definitions) {checkProfile(definition);require(!names.contains(definition.name),"Duplicate subagent definition");names.insert(definition.name);}
        for(const auto& name:options.allowedModels) require(!name.trimmed().isEmpty() && name.size()<=256,"Invalid allowed subagent model");
        for(auto i=options.modelAliases.begin();i!=options.modelAliases.end();++i)
            require(!i.key().trimmed().isEmpty()&&i.key().size()<=256&&!i.key().contains(QChar::Null)
                &&!i.value().trimmed().isEmpty()&&i.value().size()<=256&&!i.value().contains(QChar::Null),"Invalid subagent model alias");
        // Additional tools include the parent's orchestration owner. Children get
        // a fresh engine and a scoped snapshot of the underlying tool registry.
        parent.forkedSkill = {}; // Children cannot recursively delegate through Skill.
        parent.additionalTools.removeIf([](const Tool& t){return t.definition.name=="Agent"||t.definition.metadata["source"]=="builtin.subagent";});
        if(parent.additionalToolsProvider) parent.additionalToolsProvider=[provider=std::move(parent.additionalToolsProvider)]{
            auto tools=provider();tools.removeIf([](const Tool& t){return t.definition.name=="Agent"||t.definition.metadata["source"]=="builtin.subagent";});return tools;
        };
        pool.setMaxThreadCount(options.maxConcurrent);
        const auto files=QDir(options.stateDirectory).entryList({"agent-*.json"},QDir::Files|QDir::Hidden,QDir::Name);
        require(files.size()<=options.maxRecords,"Subagent record limit exceeded",ErrorCode::ResourceLimit);
        for(const auto& name:files) {
            const auto id=name.chopped(5); safeId(id); const auto path=QDir(options.stateDirectory).filePath(name);
            require(!QFileInfo(path).isSymLink(),"Subagent record is a symlink",ErrorCode::StorageFailure);
            QFile file(path);require(file.open(QIODevice::ReadOnly),"Cannot read subagent record",ErrorCode::StorageFailure);
            const auto bytes=file.read(8*1024*1024+1);require(bytes.size()<=8*1024*1024 && file.atEnd(),"Subagent record exceeds limit",ErrorCode::ResourceLimit);
            QJsonParseError error;const auto doc=QJsonDocument::fromJson(bytes,&error);auto state=doc.object();
            require(error.error==QJsonParseError::NoError && doc.isObject() && state["schema"]=="iisacc.subagent/1" && state["agentId"]==id
                && state["parent_session_id"].isString() && state["session_id"].isString() && state["profile"].isObject()
                && QStringList{"queued","running","completed","cancelled","failed","turn_limit","interrupted"}.contains(state["status"].toString()),"Corrupt subagent record",ErrorCode::ProtocolError);
            const auto child=children.metadata(state["session_id"].toString());require(child.model==state["model"] && child.workingDirectory==options.workingDirectory,"Subagent transcript identity mismatch",ErrorCode::ProtocolError);
            if(state.contains("notification_refs")) {
                require(state["notification_refs"].isArray()&&state["notification_refs"].toArray().size()<=10000,"Invalid notification receipts",ErrorCode::ProtocolError);
                QSet<QString> seen;
                for(const auto& value:state["notification_refs"].toArray()) {
                    const auto ref=value.toObject();const auto id=ref["id"].toString(),owner=ref["owner"].toString();
                    require(value.isObject()&&ref.size()==2&&!id.isEmpty()&&id.size()<=128&&!owner.isEmpty()&&owner.size()<=128&&!seen.contains(id),"Invalid notification receipt",ErrorCode::ProtocolError);seen.insert(id);
                }
            }
            auto job=std::make_shared<Job>();job->state=state;
            if(!terminal(state["status"].toString())) {job->state["status"]="interrupted";job->state["error"]="Previous host ended without a final outcome; execution was not repeated.";write(*job);}
            jobs.emplace(id,std::move(job));
        }
        QSet<QString> known;for(const auto& [id,_]:jobs)known.insert(id);owners->validateKnown(known);
        for(const auto& [_,job]:jobs)try {moveNotification(*job);}catch(const std::exception& error) {
            job->state["notification_transfer_error"]=QString::fromUtf8(error.what());
        }
    }
    static void safeId(const QString& id) {
        static const QRegularExpression pattern("\\Aagent-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\z");
        require(pattern.match(id).hasMatch(),"Invalid subagent ID");
    }
    void write(const Job& job) const {
        const auto id=job.state["agentId"].toString();safeId(id);const auto path=QDir(options.stateDirectory).filePath(id+".json");
        require(!QFileInfo(path).isSymLink(),"Subagent record is a symlink",ErrorCode::StorageFailure);
        const auto bytes=QJsonDocument(job.state).toJson(QJsonDocument::Compact)+'\n';require(bytes.size()<=8*1024*1024,"Subagent record exceeds limit",ErrorCode::ResourceLimit);
        QSaveFile file(path);file.setDirectWriteFallback(false);
        require(file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner)
            && file.write(bytes)==bytes.size() && file.commit(),"Cannot commit subagent record",ErrorCode::StorageFailure);
    }
    std::shared_ptr<Job> owned(const QString& parentId,const QString& id) const {
        safeId(id);const auto it=jobs.find(id);require(it!=jobs.end() && owner(*it->second)==parentId,"Subagent not found",ErrorCode::NotFound);return it->second;
    }
    QString owner(const Job& job) const {return owners->owner(job.state["agentId"].toString(),job.state["parent_session_id"].toString());}
    static QJsonArray notificationRefs(const Job& job) {
        if(job.state.contains("notification_refs"))return job.state["notification_refs"].toArray();
        const auto id=job.state["notification_id"].toString();
        return id.isEmpty()?QJsonArray{}:QJsonArray{QJsonObject{{"id",id},{"owner",job.state["notification_owner"].toString(job.state["parent_session_id"].toString())}}};
    }
    QJsonArray pendingRefs(const QJsonArray& refs) {
        QMap<QString,QStringList> groups;for(const auto& value:refs){const auto ref=value.toObject();groups[ref["owner"].toString()].append(ref["id"].toString());}
        QJsonArray result;for(auto it=groups.cbegin();it!=groups.cend();++it) {
            QStringList pending;notifications.transferNotifications(it.key(),it.key(),it.value(),{},&pending);
            for(const auto& id:pending)result.append(QJsonObject{{"id",id},{"owner",it.key()}});
        }
        return result;
    }
    void moveNotification(Job& job) {
        QString to;QJsonArray refs;
        {std::lock_guard lock(mutex);to=owner(job);refs=notificationRefs(job);}
        if(refs.isEmpty())return;
        QMap<QString,QStringList> groups;for(const auto& value:refs){const auto ref=value.toObject();groups[ref["owner"].toString()].append(ref["id"].toString());}
        // Delivery preparation may inspect child state. Never hold the child
        // mutex while waiting on an input delivery lock.
        QJsonArray remaining;
        for(auto it=groups.cbegin();it!=groups.cend();++it) {
            QStringList pending;notifications.transferNotifications(it.key(),to,it.value(),{},&pending);
            for(const auto& id:pending)remaining.append(QJsonObject{{"id",id},{"owner",to}});
        }
        {std::lock_guard lock(mutex);
            // A trusted host may resume a child during queue I/O. Keep any new
            // invocation's receipts, replacing only the captured stable IDs.
            QSet<QString> captured;for(const auto& value:refs)captured.insert(value.toObject()["id"].toString());
            for(const auto& value:notificationRefs(job))if(!captured.contains(value.toObject()["id"].toString()))remaining.append(value);
            auto updated=job;updated.state["notification_refs"]=remaining;updated.state["notification_owner"]=to;updated.state.remove("notification_transfer_error");
            write(updated);job.state=std::move(updated.state);}
    }
    bool modelAllowed(const QString& name,const QString& parentModel,const SubagentDefinition& p) const {
        return name==parentModel || options.allowedModels.contains(name) || options.modelAliases.values().contains(name)
            || ((p.source=="host" || !options.profiles.enabled) && !p.model.isEmpty() && name==p.model);
    }
    void stopShells(const QString& id) const {
        const auto source=registry->snapshot();Tool list,stop;
        for(const auto& t:source->definitions()) if(t.metadata["source"]=="builtin.shell.control") {
            if(t.name=="ShellTaskList")list=source->get(t.name);
            if(t.name=="TaskStop")stop=source->get(t.name);
        }
        if(!list.execute||!stop.execute)return;
        const ToolContext context{id,{},options.workingDirectory};
        const auto tasks=[&]{
            QJsonArray values;int offset=0;
            do {
                const auto page=list.execute({{"offset",offset},{"limit",100}},context);
                require(!page.isError,"Cannot list child background shells",ErrorCode::RuntimeFailure);
                for(const auto& value:page.data["tasks"].toArray())values.append(value);
                const auto next=page.data["next_offset"].toInt(-1);
                require(next==-1||(next>offset&&next<=1000000),"Invalid child shell list cursor",ErrorCode::ProtocolError);
                offset=next;
            }while(offset!=-1);
            return values;
        };
        for(const auto& value:tasks()) {
            const auto task=value.toObject();
            if(task["status"]=="pending"||task["status"]=="running") {
                try {
                    const auto result=stop.execute({{"task_id",task["task_id"]}},context);
                    require(!result.isError,"Cannot stop child background shell",ErrorCode::RuntimeFailure);
                }catch(const Error&) {
                    bool finished=false;
                    for(const auto& fresh:tasks()) {
                        const auto observed=fresh.toObject();
                        if(observed["task_id"]==task["task_id"])
                            finished=QStringList{"completed","failed","killed","interrupted"}.contains(observed["status"].toString());
                    }
                    if(!finished)throw;
                }
            }
        }
    }
    void execute(std::shared_ptr<Job> job,RunRequest request,SubagentDefinition definition,
        std::function<void(const QJsonObject&)> progress) noexcept {
        const auto started=Clock::now(); RunResult result;result.sessionId=request.sessionId;
        try {
            QJsonObject original,current=profileJson(definition),hookContext;
            {std::lock_guard lock(mutex);job->state["status"]="running";original=job->state["profile"].toObject();write(*job);
                hookContext={{"agent_id",job->state["agentId"]},{"agent_type",definition.name},{"parent_session_id",job->state["parent_session_id"]}};}
            hookContext["transcript_path"]=QDir(options.stateDirectory).filePath("sessions/"+request.sessionId+"/transcript.jsonl");
            auto scopedPolicy=std::make_shared<ScopedPolicy>(policy,original,current);
            QString startFeedback;
            for(const auto& hook:parent.hooks) {
                job->token.throwIfCancelled();
                auto startContext=hookContext;startContext["permission_mode"]=scopedPolicy->describe({request.sessionId,{},options.workingDirectory,{},job->token})["mode"].toString("unknown");
                const auto value=hook({HookKind::SubagentStart,request.sessionId,{}, {},{},request.prompt,startContext},job->token);
                if(progress)for(const auto& diagnostic:value.diagnostics)
                    progress(QJsonObject{{"agentId",hookContext["agent_id"]},{"event",toJson(Event{EventKind::Hook,{},request.sessionId,{}, {},diagnostic.toObject()})}});
                if(value.stop)throw Error(ErrorCode::Cancelled,value.stopReason.isEmpty()?QString("Stopped by SubagentStart hook"):value.stopReason);
                require(!value.block,"SubagentStart hook blocked execution: "+value.feedback);
                if(!value.feedback.isEmpty()){if(!startFeedback.isEmpty())startFeedback+='\n';startFeedback+=value.feedback;}
                require(startFeedback.size()<=parent.maxInputCharacters,"SubagentStart context exceeds input limit",ErrorCode::ResourceLimit);
            }
            if(!startFeedback.isEmpty()) {
                job->token.throwIfCancelled();auto lease=children.acquire(request.sessionId);
                Message message{uuid(),MessageRole::User,startFeedback};message.metadata={{"iilocal.subagent_start",hookContext}};lease->append(message);
            }
            auto scoped=std::make_shared<ToolRegistry>();const auto source=registry->snapshot();
            for(const auto& t:source->definitions()) if(allowed(t,original)&&allowed(t,current)) {
                scoped->add(source->get(t.name));
            }
            auto eo=parent;eo.sessionsDirectory=QDir(options.stateDirectory).filePath("sessions");eo.maxConcurrentRuns=1;eo.maxQueuedRuns=0;
            eo.sessionStartHooks=false;request.userPrompt=false;
            eo.hooks.clear();for(const auto& hook:parent.hooks)eo.hooks.append([hook,hookContext](HookInput input,const CancellationToken& token){
                for(auto i=hookContext.begin();i!=hookContext.end();++i)input.context[i.key()]=i.value();
                if(input.kind==HookKind::Stop)input.kind=HookKind::SubagentStop;
                return hook(input,token);
            });
            eo.toolFilter=[original,current,parentFilter=parent.toolFilter](const ToolDefinition& t){
                return allowed(t,original)&&allowed(t,current)&&(!parentFilter||parentFilter(t));
            };
            const ToolDefinition search{"ToolSearch",{}, {}, {},true};eo.toolSearch.enabled &= eo.toolFilter(search);
            const ToolDefinition skill{"Skill",{}, {}, {},true};eo.skills.enabled &= allowed(skill,original)&&allowed(skill,current);
            Engine engine(std::make_shared<ScopedModel>(model,original,current),scoped,scopedPolicy,eo);
            job->token.throwIfCancelled();
            require(Clock::now()-started<std::chrono::milliseconds(options.maxRuntimeMs),"Subagent runtime deadline exceeded",ErrorCode::Timeout);
            auto handle=engine.run(request,[&](const Event& event){
                if(event.kind==EventKind::ToolStarted) {std::lock_guard lock(mutex);job->state["tool_uses"]=job->state["tool_uses"].toInt()+1;}
                if(progress) progress(QJsonObject{{"agentId",job->state["agentId"]},{"event",toJson(event)}});
            });
            bool deadline=false;
            while(handle.result.wait_for(5ms)!=std::future_status::ready) {
                if(job->token.isCancelled()) handle.cancel();
                if(Clock::now()-started>=std::chrono::milliseconds(options.maxRuntimeMs)) {deadline=true;handle.cancel();}
            }
            result=handle.result.get();
            if(deadline || Clock::now()-started>=std::chrono::milliseconds(options.maxRuntimeMs)) {result.status=RunStatus::Failed;result.errorCode=ErrorCode::Timeout;result.errorMessage="Subagent runtime deadline exceeded";}
        } catch(const Error& error) {result.status=error.code()==ErrorCode::Cancelled?RunStatus::Cancelled:RunStatus::Failed;result.errorCode=error.code();result.errorMessage=QString::fromUtf8(error.what());}
          catch(const std::exception& error) {result.status=RunStatus::Failed;result.errorCode=ErrorCode::RuntimeFailure;result.errorMessage=QString::fromUtf8(error.what());}
          catch(...) {result.status=RunStatus::Failed;result.errorCode=ErrorCode::RuntimeFailure;result.errorMessage="Unknown subagent failure";}
        try { stopShells(request.sessionId);policy->forgetSession({request.sessionId,{},options.workingDirectory}); }
        catch(const Error& error) {result.status=RunStatus::Failed;result.errorCode=error.code();result.errorMessage="Child cleanup: "+QString::fromUtf8(error.what());}
        catch(const std::exception& error) {result.status=RunStatus::Failed;result.errorCode=ErrorCode::RuntimeFailure;result.errorMessage="Child cleanup: "+QString::fromUtf8(error.what());}
        try {
            // Serialize completion publication with trusted ownership transfer.
            // enqueue has no external callbacks; the queue never borrows us.
            std::lock_guard lock(mutex);job->state["status"]=enumName(result.status);job->state["result"]=toJson(result);
                job->state["duration_ms"]=double(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count());
            const auto destination=owner(*job),id=job->state["agentId"].toString();write(*job);
            if(options.completionNotifications&&job->state["background"].toBool()) {
                const QJsonObject notice{{"agentId",id},{"status",enumName(result.status)},{"session_id",result.sessionId},
                    {"text",result.text.left(16000)},{"error_code",iiLocalLLM::enumName(result.errorCode)},{"error_message",result.errorMessage}};
                const auto queued=notifications.enqueue(destination,{{"kind","notification"},{"priority","later"},
                    {"text","Subagent execution finished. Inspect AgentOutput for the full recorded result.\n"+QString::fromUtf8(QJsonDocument(notice).toJson(QJsonDocument::Compact))}});
                auto receipts=notificationRefs(*job);receipts.append(QJsonObject{{"id",queued["input"].toObject()["id"]},{"owner",destination}});
                job->state["notification_refs"]=pendingRefs(receipts);
                job->state["notification_id"]=queued["input"].toObject()["id"];job->state["notification_owner"]=destination;write(*job);
            }
        } catch(const std::exception& error) {std::lock_guard lock(mutex);job->state["delivery_error"]=QString::fromUtf8(error.what());}
        {std::lock_guard lock(mutex);
            if(job->state.contains("delivery_error")) {result.status=RunStatus::Failed;result.errorCode=ErrorCode::StorageFailure;result.errorMessage=job->state["delivery_error"].toString();}
            job->outcome=std::move(result);job->done=true;}changed.notify_all();
    }
};
Subagents::Subagents(std::shared_ptr<Model> m,std::shared_ptr<ToolRegistry> r,std::shared_ptr<const PermissionPolicy> p,EngineOptions e,SubagentOptions o)
    :d(std::make_unique<Impl>(std::move(m),std::move(r),std::move(p),std::move(e),std::move(o))){}
Subagents::~Subagents(){close();}
void Subagents::close(){ {std::lock_guard lock(d->mutex);d->stopping=true;for(const auto& [_,job]:d->jobs) if(!job->done)job->token.cancel();}d->pool.waitForDone(); }
ToolResult Subagents::run(const ToolContext& context,const QJsonObject& args) {
    return runImpl(context,args,nullptr,nullptr);
}
SkillForkResult Subagents::runSkill(const SkillForkRequest& request,const ToolContext& context) {
    const auto& prompt=request.prompt;const auto metadata=prompt.metadata["iilocal.skill"].toObject();
    require(prompt.role==MessageRole::User && prompt.toolCalls.isEmpty() && prompt.toolCallId.isEmpty()
        && metadata["format_version"]==1 && metadata["context"]=="fork" && metadata["unsupported_features"].toArray().isEmpty(),"Invalid forked skill snapshot");
    require(request.maxTurns>=1 && request.maxTurns<=10000,"Invalid forked skill turn limit");
    QJsonObject args{{"prompt",prompt.text},{"description","Skill: "+metadata["name"].toString()}};
    if(!metadata["agent"].toString().isEmpty())args["subagent_type"]=metadata["agent"];
    const auto model=metadata["model"].toString();
    if(!model.isEmpty())args["model"]=model=="inherit"&&context.sessionSnapshot?context.sessionSnapshot->model:model;
    SkillForkResult result;const auto output=runImpl(context,args,&request,&result.result);result.execution=output.data;return result;
}
ToolResult Subagents::runImpl(const ToolContext& context,const QJsonObject& args,const SkillForkRequest* skill,RunResult* outcome) {
    context.cancellation.throwIfCancelled();
    const QSet<QString> fields{"prompt","description","subagent_type","model","run_in_background","resume","fork_context","max_turns"};
    for(auto it=args.begin();it!=args.end();++it) require(fields.contains(it.key()),"Unknown subagent argument: "+it.key());
    for(const auto& key:{"prompt","description","subagent_type","model","resume"}) if(args.contains(key)) require(args[key].isString(),"Subagent text field must be a string");
    for(const auto& key:{"run_in_background","fork_context"}) if(args.contains(key)) require(args[key].isBool(),"Subagent flag must be boolean");
    require(!args["prompt"].toString().trimmed().isEmpty() && args["prompt"].toString().size()<=d->parent.maxInputCharacters
        && args["description"].toString().size()<=512,"Invalid subagent prompt or description");
    require(context.sessionSnapshot && context.sessionSnapshot->id==context.sessionId
        && context.sessionSnapshot->workingDirectory==d->options.workingDirectory && context.workingDirectory==d->options.workingDirectory,"Missing or mismatched subagent parent context");
    const auto parent=d->parentStore.metadata(context.sessionId);
    require(parent.model==context.sessionSnapshot->model && parent.workingDirectory==d->options.workingDirectory,"Subagent parent identity mismatch");
    bool background=args["run_in_background"].toBool();const bool fork=args["fork_context"].toBool();
    const auto resume=args["resume"].toString();std::shared_ptr<Impl::Job> job;SubagentDefinition definition;RunRequest request;QString id;
    const auto catalog=profiles(context.cancellation);
    const auto configureRequest=[&]{
        const int cap=std::min(d->options.maxTurns,definition.maxTurns);
        if(args.contains("max_turns")) require(args["max_turns"].isDouble() && args["max_turns"].toDouble()==std::floor(args["max_turns"].toDouble()) && args["max_turns"].toDouble()>=1 && args["max_turns"].toDouble()<=cap,"Invalid subagent turn limit");
        request.prompt=args["prompt"].toString();request.maxTurns=args["max_turns"].toInt(cap);
        require(definition.unsupportedFeatures.isEmpty()&&definition.permissionMode!="auto","Unsupported agent profile execution features: "+definition.unsupportedFeatures.join(", "),ErrorCode::RuntimeUnavailable);
        checkProfile(definition);background|=definition.background;
        if(skill) background=false; // Forked commands synchronously return the observed result.
        if(resume.isEmpty()&&!definition.initialPrompt.isEmpty()) request.prompt=definition.initialPrompt+"\n\n"+request.prompt;
        if(fork) request.prompt="You are the delegated child. The earlier conversation belongs to the parent. Work directly on the task below with your available tools and report your own observed result.\n\n"+request.prompt;
        require(request.prompt.size()<=d->parent.maxInputCharacters,"Expanded subagent prompt exceeds input limit",ErrorCode::ResourceLimit);
        request.generation=d->options.generation;
        request.allowedTools=parsePermissionRules(context.allowedTools);
        if(skill) {
            request.generation=skill->generation;request.maxTurns=std::min(request.maxTurns,skill->maxTurns);
            request.contextPaths=skill->contextPaths;request.promptMetadata=skill->prompt.metadata;
            request.allowedTools=parsePermissionRules(request.allowedTools+detail::skillAllowedTools(skill->prompt));
        }
    };
    {
        std::lock_guard lock(d->mutex);require(!d->stopping,"Subagent host is closing",ErrorCode::ShuttingDown);
        int active=0;for(const auto& [_,item]:d->jobs)active+=!item->done;
        require(active<d->options.maxConcurrent,"Subagent concurrency limit reached",ErrorCode::QueueFull);
        if(!resume.isEmpty()) {
            require(!fork&&!args.contains("subagent_type")&&!args.contains("model"),"Resume cannot replace context, profile or model");
            job=d->owned(context.sessionId,resume);require(job->done&&!job->awaitingForeground,"Subagent is already active or returning its result",ErrorCode::ModelInUse);
            definition=catalog.find(job->state["agent_type"].toString());request.sessionId=job->state["session_id"].toString();
            configureRequest();
        } else {
            require(int(d->jobs.size())<d->options.maxRecords,"Subagent record limit reached",ErrorCode::ResourceLimit);
            QString selected=args["subagent_type"].toString("general-purpose");
            if(skill) {
                const auto exists=[&](const QString& name) {
                    for(const auto& p:catalog.profiles)if(p.name==name)return true;
                    // A malformed higher-priority profile must not silently fall back.
                    for(const auto& v:catalog.failedFiles) {
                        const auto f=v.toObject();
                        require(f["name"]!=name && QFileInfo(f["path"].toString()).completeBaseName()!=name,
                            "Requested skill agent profile is invalid: "+name,ErrorCode::RuntimeUnavailable);
                    }
                    return false;
                };
                if(!exists(selected)) {
                    selected="general-purpose";
                    if(!exists(selected)) {require(!catalog.profiles.isEmpty(),"No agent is available for forked skill",ErrorCode::RuntimeUnavailable);selected=catalog.profiles.first().name;}
                }
            }
            definition=catalog.find(selected);job=std::make_shared<Impl::Job>();
            configureRequest();
            const auto requestedModel=args["model"].toString(definition.model.isEmpty()?parent.model:definition.model);
            const auto model=d->options.modelAliases.value(requestedModel,requestedModel);
            require(d->modelAllowed(model,parent.model,definition),"Subagent model override is not authorized by the host");
            Session seed=fork?*context.sessionSnapshot:Session{};seed.model=model;seed.workingDirectory=parent.workingDirectory;
            seed.systemPrompt=fork?context.sessionSnapshot->systemPrompt:definition.systemPrompt;
            if(!fork && seed.systemPrompt.isEmpty()) seed.systemPrompt="Perform the delegated task using the available tools and report the observed result.";
            if(fork) {
                require(context.artifactsDirectory.isEmpty() || QDir(context.artifactsDirectory).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden).isEmpty(),"Forking parent artifacts is not implemented",ErrorCode::RuntimeUnavailable);
                for(const auto& call:pendingToolCalls(seed.messages)) seed.messages.append({uuid(),MessageRole::Tool,"Parent operation is still pending. This child has not executed it.",{},call.id,true,{{"parent_pending",true}}});
            }
            const auto child=d->children.createFromSnapshot(std::move(seed),[&](const QString& childId,QList<Message>& messages){
            qsizetype skillCharacters=0;
            for(const auto& skill:definition.skills) {
                auto message=loadSkill(parent.workingDirectory,skill,{},childId,SkillInvocationSource::Model,d->parent.skills,context.cancellation);
                skillCharacters+=message.text.size();require(skillCharacters<=d->parent.maxInputCharacters,"Preloaded agent skills exceed input limit",ErrorCode::ResourceLimit);
                auto metadata=message.metadata["iilocal.skill"].toObject();metadata["agent_profile"]=definition.name;message.metadata["iilocal.skill"]=metadata;
                messages.append(std::move(message));
            }
            });request.sessionId=child.id;
            job->state={{"schema","iisacc.subagent/1"},{"agentId","agent-"+uuid()},{"parent_session_id",parent.id},{"session_id",child.id},
                {"agent_type",definition.name},{"profile",profileJson(definition)},{"model",model},{"fork_context",fork},{"created_at",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
            if(skill)job->state["skill"]=skill->prompt.metadata["iilocal.skill"];
        }
        const auto model=job->state["model"].toString();require(d->modelAllowed(model,parent.model,definition),"Resumed subagent model is no longer authorized");
        Impl::Job accepted;accepted.state=job->state;
        accepted.state["status"]="queued";accepted.state["prompt"]=request.prompt;accepted.state["description"]=args["description"].toString(job->state["description"].toString());
        accepted.state["background"]=background;accepted.state["tool_uses"]=0;
        accepted.state["notification_refs"]=d->pendingRefs(Impl::notificationRefs(accepted));
        for(const auto& key:{"result","notification_id","notification_owner","notification_transfer_error","delivery_error","error","duration_ms"})accepted.state.remove(key);
        try {d->policy->inheritSession(context,{request.sessionId,{},d->options.workingDirectory});d->write(accepted);}catch(...) {
            if(resume.isEmpty()) {
                try {d->policy->forgetSession({request.sessionId,{},d->options.workingDirectory});}catch(...) {}
                const auto directory=QDir(d->options.stateDirectory).filePath("sessions/"+request.sessionId);
                QFile::remove(QDir(directory).filePath("transcript.jsonl"));QDir().rmdir(directory);
            }
            throw;
        }
        job->state=std::move(accepted.state);job->token=background?CancellationToken{}:CancellationToken::linkedTo(context.cancellation);job->done=false;
        job->awaitingForeground=!background;
        id=job->state["agentId"].toString();d->jobs[id]=job;
        d->pool.start(QRunnable::create([impl=d.get(),job,request,definition,progress=background?std::function<void(const QJsonObject&)>{}:context.progress]{impl->execute(job,request,definition,progress);}));
    }
    if(background) return {"Subagent accepted for background execution.",{{"status","async_launched"},{"agentId",id},{"session_id",request.sessionId}}};
    // A foreground progress callback borrows its parent's event lifetime. Keep
    // waiting after requesting cancellation until the cooperative worker joins.
    QJsonObject result;
    {std::unique_lock lock(d->mutex);d->changed.wait(lock,[&]{return job->done;});
        result=job->state;result["finished"]=true;result["retrieval_status"]="success";
        if(outcome)*outcome=job->outcome;job->awaitingForeground=false;}
    const auto response=result["result"].toObject()["text"].toString();
    const auto text=result["status"]=="completed" ? "Child agent completed. The final response is already available below; use it directly without polling AgentOutput for this completed invocation.\n\n"+response
        : "Child agent ended with status "+result["status"].toString()+".\n"+response;
    return {text,publicState(result),result["status"]!="completed"||result.contains("delivery_error")};
}
QJsonObject Subagents::output(const QString& parent,const QString& id,bool block,int timeoutMs,const CancellationToken& token) const {
    require(timeoutMs>=0&&timeoutMs<=86401000,"Invalid subagent output timeout");token.throwIfCancelled();
    std::unique_lock lock(d->mutex);const auto job=d->owned(parent,id);const auto end=Clock::now()+std::chrono::milliseconds(timeoutMs);
    while(block&&!job->done&&Clock::now()<end){token.throwIfCancelled();d->changed.wait_for(lock,5ms);}
    // A trusted transfer can occur while a direct C++ output wait is asleep.
    (void)d->owned(parent,id);
    auto result=job->state;result["owner_session_id"]=d->owner(*job);result["finished"]=job->done;result["retrieval_status"]=job->done?"success":block?"timeout":"not_ready";return result;
}
QJsonObject Subagents::stop(const QString& parent,const QString& id,const CancellationToken& token) {
    token.throwIfCancelled();std::lock_guard lock(d->mutex);const auto job=d->owned(parent,id);if(!job->done)job->token.cancel();return {{"agentId",id},{"stop_requested",!job->done},{"status",job->state["status"]}};
}
QJsonArray Subagents::list(const QString& parent) const {
    std::lock_guard lock(d->mutex);QJsonArray items;
    for(const auto& [id,job]:d->jobs) if(d->owner(*job)==parent) items.append(QJsonObject{{"agentId",id},{"agent_type",job->state["agent_type"]},
        {"status",job->state["status"]},{"description",job->state["description"]},{"session_id",job->state["session_id"]},{"model",job->state["model"]},{"finished",job->done}});
    return items;
}
QJsonArray Subagents::transferSession(const QString& from,const QString& to,const CancellationToken& token) {
    token.throwIfCancelled();require(from!=to,"A background transfer requires a new session");
    const auto source=d->parentStore.metadata(from),target=d->parentStore.metadata(to);
    require(source.workingDirectory==d->options.workingDirectory&&target.workingDirectory==d->options.workingDirectory,"Background transfer crosses workspaces",ErrorCode::NotFound);
    std::lock_guard transfer(d->transferring);QStringList ids;QList<std::shared_ptr<Impl::Job>> jobs;
    {std::lock_guard lock(d->mutex);require(!d->stopping,"Subagent host is shutting down",ErrorCode::ShuttingDown);
    for(const auto& [id,job]:d->jobs) {
        const auto owner=d->owner(*job);bool retry=false;
        for(const auto& value:Impl::notificationRefs(*job))retry|=value.toObject()["owner"]==from;
        if(job->state["background"].toBool() && (owner==from||(owner==to&&retry))){ids.append(id);jobs.append(job);}
    }
    }
    // Recover any earlier destination before advancing ownership again. Otherwise
    // a second clear after a partial first transfer could orphan a queued notice.
    for(const auto& job:jobs)d->moveNotification(*job);
    {std::lock_guard lock(d->mutex);token.throwIfCancelled();require(!d->stopping,"Subagent host is shutting down",ErrorCode::ShuttingDown);d->owners->transfer(ids,to);}
    // After the ownership commit, finish acknowledgement independently of caller
    // cancellation. Queue errors leave the stable notice at source for retry.
    for(const auto& job:jobs)try {d->moveNotification(*job);}
        catch(const std::exception& error){std::lock_guard lock(d->mutex);job->state["notification_transfer_error"]=QString::fromUtf8(error.what());throw;}
    d->changed.notify_all();return QJsonArray::fromStringList(ids);
}
AgentProfileCatalog Subagents::profiles(const CancellationToken& token) const {
    return discoverAgentProfiles(d->options.workingDirectory,d->options.profiles,d->options.definitions,token);
}
QList<Tool> Subagents::tools(std::shared_ptr<Subagents> owner) {return makeTools(std::move(owner),true);}
void Subagents::attach(EngineOptions& options,std::shared_ptr<Subagents> owner) {
    require(bool(owner),"Missing subagent owner");
    options.additionalTools.append(makeTools(owner,false));
    options.forkedSkill=[owner](const SkillForkRequest& request,const ToolContext& context){return owner->runSkill(request,context);};
    options.additionalToolsProvider=[owner,previous=options.additionalToolsProvider]{
        auto tools=previous?previous():QList<Tool>{};auto live=makeTools(owner,true);
        live.removeIf([](const Tool& t){return t.definition.name!="Agent";});tools.append(live);return tools;
    };
}
QList<Tool> Subagents::makeTools(std::shared_ptr<Subagents> owner,bool includeAgent) {
    require(bool(owner),"Missing subagent owner");QList<Tool> tools;
    const QJsonObject text{{"type","string"},{"minLength",1},{"maxLength",1048576}},id{{"type","string"},{"minLength",1},{"maxLength",128}};
    if(includeAgent) {
    QJsonArray types;QString descriptions;QJsonObject discoveryError;
    try {
        for(const auto& p:owner->profiles().profiles)if(p.unsupportedFeatures.isEmpty()){types.append(p.name);descriptions+=p.name+": "+p.description+'\n';}
    }catch(const Error& error) {
        discoveryError={{"code",iiLocalLLM::enumName(error.code())},{"message",QString::fromUtf8(error.what())}};
        descriptions="Profile discovery failed. AgentProfiles reports the failure; existing child state controls remain available.";
    }
    Tool agent;agent.definition.name="Agent";agent.definition.description="Delegate a task to a separate local agent conversation. A foreground status of completed already includes the final response; use it directly without polling. Only async_launched means background work is pending; use AgentOutput to wait or AgentStop to cancel it. Child agents cannot delegate again.\n"+descriptions;
    agent.definition.concurrencySafe=true;agent.definition.metadata={{"source","builtin.subagent"}};
    if(!discoveryError.isEmpty())agent.definition.metadata["profile_discovery_error"]=discoveryError;
    agent.definition.inputSchema={{"type","object"},{"additionalProperties",false},{"required",QJsonArray{"prompt"}},{"properties",QJsonObject{
        {"prompt",text},{"description",QJsonObject{{"type","string"},{"maxLength",512}}},{"subagent_type",QJsonObject{{"type","string"},{"enum",types}}},
        {"model",QJsonObject{{"type","string"},{"maxLength",256}}},{"run_in_background",QJsonObject{{"type","boolean"}}},
        {"fork_context",QJsonObject{{"type","boolean"}}},{"resume",id},{"max_turns",QJsonObject{{"type","integer"},{"minimum",1},{"maximum",owner->d->options.maxTurns}}}}}};
    agent.execute=[owner](const auto& args,const auto& context){return owner->run(context,args);};tools.append(std::move(agent));
    }
    for(const auto& name:{QStringLiteral("AgentOutput"),QStringLiteral("AgentStop"),QStringLiteral("AgentList"),QStringLiteral("AgentProfiles")}) {
        Tool tool;tool.definition.name=name;tool.definition.readOnly=name!="AgentStop";tool.definition.concurrencySafe=true;tool.definition.metadata={{"source","builtin.subagent"}};
        tool.definition.description=name=="AgentOutput"?"Inspect or wait for a pending child agent. retrieval_status success and finished true mean the recorded result is final; read it and stop polling that invocation.":name=="AgentStop"?"Request cancellation of a child agent.":name=="AgentProfiles"?"Discover current agent profile metadata, provenance, shadowed definitions and unsupported fields. Profile prompts are excluded.":"List this conversation's child agents.";
        const bool needsId=name=="AgentOutput"||name=="AgentStop";
        QJsonObject props;if(needsId)props["agent_id"]=id;
        if(name=="AgentOutput"){props["block"]=QJsonObject{{"type","boolean"}};props["timeout_ms"]=QJsonObject{{"type","integer"},{"minimum",0},{"maximum",60000}};}
        tool.definition.inputSchema={{"type","object"},{"additionalProperties",false},{"properties",props}};if(needsId)tool.definition.inputSchema["required"]=QJsonArray{"agent_id"};
        if(name=="AgentList")tool.transferSession=[owner](const auto& from,const auto& to,const auto& token){return owner->transferSession(from,to,token);};
        tool.execute=[owner,name](const auto& args,const auto& context){QJsonObject result;
            if(name=="AgentProfiles")result=owner->profiles(context.cancellation).toJson();
            else if(name=="AgentList")result={{"agents",owner->list(context.sessionId)}};
            else if(name=="AgentStop")result=owner->stop(context.sessionId,args["agent_id"].toString(),context.cancellation);
            else result=publicState(owner->output(context.sessionId,args["agent_id"].toString(),args["block"].toBool(),args["timeout_ms"].toInt(30000),context.cancellation));
            const auto text=name=="AgentOutput" && result["finished"].toBool()
                ? "Child agent reached final status "+result["status"].toString()+". This result is final for the invocation; do not poll again.\n\n"+result["result"].toObject()["text"].toString()
                : QStringLiteral("Subagent execution state");
            return ToolResult{text,result};};tools.append(std::move(tool));
    }
    return tools;
}
}
