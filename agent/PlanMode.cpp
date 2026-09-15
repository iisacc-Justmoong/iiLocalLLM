#include "PlanMode.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QStringConverter>
#include <chrono>
#include <map>
#include <mutex>
#include <shared_mutex>

namespace iiLocalLLM::agent {
namespace {
void require(bool value,const QString& message,ErrorCode code=ErrorCode::InvalidArgument) {
    if(!value)throw Error(code,message);
}
QString owner(const ToolContext& c){return c.planningSessionId.isEmpty()?c.sessionId:c.planningSessionId;}
QString digest(const QByteArray& bytes){return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
QString stateTag(const QJsonObject& value) {
    return value["phase"].toString()+":"+QString::number(value["revision"].toInt())+":"+(value["approval_current"].toBool()?"current":"unapproved");
}
void regular(const QString& path,bool directory=false) {
    const QFileInfo file(path);require(!file.isSymLink(),"Plan path must not be a symlink");
    if(file.exists())require(directory?file.isDir():file.isFile(),"Plan path has an invalid file type");
}
QString text(const QByteArray& bytes) {
    QStringDecoder decode(QStringDecoder::Utf8);const QString value=decode(bytes);
    require(!decode.hasError()&&!value.contains(QChar::Null),"Plan must be UTF-8 text without NUL");return value;
}
QJsonObject schema(QJsonObject properties={}) {
    return {{"type","object"},{"properties",properties},{"additionalProperties",false}};
}
}
class PlanMode::Impl {
public:
    QString directory;int maxBytes;
    std::mutex executionMutex;
    std::map<QString,std::weak_ptr<std::shared_timed_mutex>> executions;
    std::shared_ptr<std::shared_timed_mutex> execution(const QString& id) {
        std::lock_guard guard(executionMutex);
        for(auto i=executions.begin();i!=executions.end();)if(i->second.expired())i=executions.erase(i);else ++i;
        auto& entry=executions[id];auto value=entry.lock();if(!value){value=std::make_shared<std::shared_timed_mutex>();entry=value;}return value;
    }
    Impl(QString path,int max):maxBytes(max) {
        require(!path.isEmpty()&&max>=1024&&max<=262144,"Invalid planning configuration");
        const auto absolute=QFileInfo(path).absoluteFilePath();regular(absolute,true);
        require(QDir().mkpath(absolute),"Cannot create plan directory",ErrorCode::StorageFailure);
        directory=QFileInfo(absolute).canonicalFilePath();require(!directory.isEmpty(),"Cannot resolve plan directory");
    }
    QString session(const QString& id) const {
        static const QRegularExpression valid("\\A[A-Za-z0-9_-]{1,128}\\z");
        require(valid.match(id).hasMatch(),"Invalid plan session ID");
        regular(directory,true);require(QFileInfo(directory).canonicalFilePath()==directory,"Plan directory changed");
        const auto result=QDir(directory).filePath(id);regular(result,true);return result;
    }
    std::unique_ptr<QLockFile> lock(const QString& id,const CancellationToken& token,bool create) const {
        const auto path=session(id);token.throwIfCancelled();
        if(!QFileInfo::exists(path)) {
            if(!create)return {};
            require(QDir().mkpath(path),"Cannot create session plan directory",ErrorCode::StorageFailure);
            (void)session(id);
        }
        const auto name=path+"/lock";regular(name);
        auto value=std::make_unique<QLockFile>(name);value->setStaleLockTime(30000);
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!value->tryLock(10)) {
            token.throwIfCancelled();require(std::chrono::steady_clock::now()<until,"Plan state is busy",ErrorCode::ModelInUse);
        }
        (void)session(id);token.throwIfCancelled();return value;
    }
    QByteArray read(const QString& path,int limit) const {
        regular(path);QFile file(path);require(file.open(QIODevice::ReadOnly),"Cannot read plan file",ErrorCode::StorageFailure);
        const auto bytes=file.read(limit+1);require(bytes.size()<=limit,"Plan file exceeds limit",ErrorCode::ResourceLimit);
        require(file.error()==QFileDevice::NoError,"Cannot read plan file",ErrorCode::StorageFailure);return bytes;
    }
    void write(const QString& id,const QString& name,const QByteArray& bytes) const {
        const auto path=session(id)+"/"+name;regular(path);
        QSaveFile file(path);require(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit(),
            "Cannot save plan file",ErrorCode::StorageFailure);
    }
    QJsonObject state(const QString& id) const {
        const auto path=session(id)+"/state.json";regular(path);
        if(!QFileInfo::exists(path))return {{"schema","iisacc.plan/1"},{"session_id",id},{"phase","inactive"},{"revision",0}};
        QJsonParseError error;const auto document=QJsonDocument::fromJson(read(path,4096),&error);auto result=document.object();
        const auto revision=result["revision"].toDouble(-1);
        require(error.error==QJsonParseError::NoError&&document.isObject()&&result["schema"]=="iisacc.plan/1"
            &&result["session_id"]==id&&QStringList{"planning","approved"}.contains(result["phase"].toString())
            &&revision>=1&&revision<=1000000000&&revision==int(revision)
            &&QStringList{"default","acceptEdits","dontAsk","bypassPermissions","plan","unknown"}.contains(result["base_mode"].toString()),
            "Invalid persisted plan state",ErrorCode::StorageFailure);
        if(result["phase"]=="approved")require(QRegularExpression("\\A[0-9a-f]{64}\\z").match(result["approved_sha256"].toString()).hasMatch(),
            "Invalid plan approval digest",ErrorCode::StorageFailure);
        return result;
    }
    QJsonObject snapshot(const QString& id) const {
        auto value=state(id);const auto path=session(id)+"/plan.md";regular(path);
        const bool exists=QFileInfo::exists(path);const auto bytes=exists?read(path,maxBytes):QByteArray{};
        value["plan_file_path"]=path;value["plan"]=exists?QJsonValue(text(bytes)):QJsonValue(QJsonValue::Null);
        value["plan_sha256"]=digest(bytes);value["file_exists"]=exists;value["max_plan_bytes"]=maxBytes;
        value["approval_current"]=value["phase"]=="approved"&&value["approved_sha256"]==value["plan_sha256"]
            &&value["approved_file_exists"]==value["file_exists"];
        return value;
    }
    QJsonObject status(const QString& id,const CancellationToken& token) const {
        const auto guard=lock(id,token,false);return snapshot(id);
    }
    void save(const QString& id,const QJsonObject& value) const {write(id,"state.json",QJsonDocument(value).toJson(QJsonDocument::Compact));}
    void unchanged(const QJsonObject& current,const QJsonObject& reviewed) const {
        require(current["session_id"]==reviewed["session_id"]&&current["revision"]==reviewed["revision"]
            &&current["phase"]=="planning"&&current["plan_sha256"]==reviewed["plan_sha256"]&&current["file_exists"]==reviewed["file_exists"],
            "Plan changed during review; read the plan and request approval again");
    }
};
PlanMode::PlanMode(QString directory,int max):d(std::make_shared<Impl>(std::move(directory),max)){}
QJsonObject PlanMode::status(const QString& id,const CancellationToken& token) const {return d->status(id,token);}
ToolContext PlanMode::scope(ToolContext c,const PermissionPolicy& policy) const {
    const auto id=owner(c);if(id.isEmpty())return c;
    const auto value=status(id,c.cancellation);c.plansDirectory=d->directory;c.planFilePath=value["plan_file_path"].toString();c.maxPlanBytes=d->maxBytes;
    c.planningState=stateTag(value);
    const auto base=policy.describe(c)["mode"].toString("unknown");
    c.planModeActive=value["phase"]=="planning"||base=="plan";
    if(value["phase"]=="approved") {
        if(!value["approval_current"].toBool())c.planModeActive=true;
        // A host that initially selected plan mode returns to default after
        // approval. Live non-plan settings and explicit invocation scopes win.
        if(value["approval_current"].toBool()&&value["base_mode"]=="plan"&&base=="plan"&&!c.permissionMode) {
            c.permissionMode=PermissionMode::Default;c.planModeActive=false;
        }
    }
    if(c.planModeActive&&!c.permissionMode)c.permissionMode=PermissionMode::Plan;
    return c;
}
ToolResult PlanMode::execute(const ToolContext& c,const ToolDefinition& definition,const std::function<ToolResult()>& call) const {
    const auto id=owner(c);if(id.isEmpty())return call();
    // These MCP wrappers delegate into the same owner's Engine, which fences
    // its actual tools. Holding a parent read lease would trap Enter/Exit there.
    if(!c.planningSessionId.isEmpty()&&c.sessionId!=id&&QStringList{"builtin.task","builtin.agent.control",
        "builtin.subagent.run","builtin.subagent.control","builtin.session.control","builtin.input.control"}.contains(definition.metadata["source"].toString()))return call();
    const auto mutex=d->execution(id);
    std::shared_lock shared(*mutex,std::defer_lock);std::unique_lock exclusive(*mutex,std::defer_lock);
    const bool transition=definition.metadata["source"]=="builtin.plan";
    if(transition){while(!exclusive.try_lock_for(std::chrono::milliseconds(10)))c.cancellation.throwIfCancelled();}
    else {while(!shared.try_lock_for(std::chrono::milliseconds(10)))c.cancellation.throwIfCancelled();}
    c.cancellation.throwIfCancelled();require(stateTag(status(id,c.cancellation))==c.planningState,"Planning state changed before execution; retry the tool");
    return call();
}
QList<Tool> PlanMode::tools(std::shared_ptr<const PermissionPolicy> policy,bool deferred) const {
    require(bool(policy),"Planning requires a host policy");QList<Tool> result;
    Tool enter;enter.definition={"EnterPlanMode","Begin a session-owned implementation plan. Explore with read-only tools, edit only the returned plan file, then request ExitPlanMode to review it.",schema(),{},true,false,false,deferred,{{"source","builtin.plan"}}};
    enter.execute=[state=d,policy](const QJsonObject&,const ToolContext& c) {
        require(!c.verificationAgent,"Verification agents cannot enter plan mode");const auto id=owner(c);
        const auto guard=state->lock(id,c.cancellation,true);auto value=state->state(id);
        if(value["phase"]!="planning") {
            const auto revision=value["revision"].toInt();require(revision<1000000000,"Plan revision limit reached",ErrorCode::ResourceLimit);
            auto baseContext=c;baseContext.permissionMode.reset();
            const auto base=policy->describe(baseContext)["mode"].toString("unknown");
            value={{"schema","iisacc.plan/1"},{"session_id",id},{"phase","planning"},{"revision",revision+1},{"base_mode",base}};
            c.cancellation.throwIfCancelled();state->save(id,value);
        }
        const auto snapshot=state->snapshot(id);
        return ToolResult{"Planning is active. Use Read, Write or Edit on your plan file: "+snapshot["plan_file_path"].toString()
            +". Other file changes and external commands are blocked. Request ExitPlanMode when the plan is ready for review.",snapshot};
    };result.append(std::move(enter));
    Tool leave;leave.definition={"ExitPlanMode","Request host review of the current plan file. The permission preview contains its text and SHA-256. allowedPrompts describes requested Bash actions; it does not grant command permissions.",
        schema({{"allowedPrompts",QJsonObject{{"type","array"},{"maxItems",32},{"items",QJsonObject{{"type","object"},{"additionalProperties",false},
            {"properties",QJsonObject{{"tool",QJsonObject{{"type","string"},{"enum",QJsonArray{"Bash"}}}},{"prompt",QJsonObject{{"type","string"},{"minLength",1},{"maxLength",1024}}}}},{"required",QJsonArray{"tool","prompt"}}}}}},
            {"plan",QJsonObject{{"type","string"},{"maxLength",d->maxBytes},{"description","Reserved for a trusted permission response editing the reviewed plan; not valid in an initial tool call."}}}}),{},false,false,false,deferred,{{"source","builtin.plan"}}};
    leave.requiresPermission=true;
    leave.validate=[](const QJsonObject& args,const ToolContext& c) {
        require(!c.verificationAgent,"Verification agents cannot exit plan mode");
        require(!args.contains("plan")||!c.approvedToolPreview["plan"].toObject().isEmpty(),"Only the host's permission response may edit a reviewed plan");
    };
    leave.prepare=[state=d,definition=leave.definition](const QJsonObject& args,const ToolContext& c) {
        const auto id=owner(c);auto reviewed=state->status(id,c.cancellation);
        require(reviewed["phase"]=="planning","Session is not in plan mode; enter planning before requesting review");
        const auto approved=c.approvedToolPreview["plan"].toObject();
        if(!approved.isEmpty())state->unchanged(reviewed,approved);
        auto preview=definition;preview.metadata["plan"]=reviewed;preview.metadata["allowedPrompts"]=args.value("allowedPrompts").toArray();
        const auto edited=args.contains("plan")?std::optional<QString>(args["plan"].toString()):std::nullopt;
        if(edited)require(edited->toUtf8().size()<=state->maxBytes&&!edited->contains(QChar::Null),"Edited plan exceeds limit or contains NUL");
        return PreparedTool{preview,[state,id,reviewed,edited,token=c.cancellation] {
            const auto guard=state->lock(id,token,false);const auto current=state->snapshot(id);state->unchanged(current,reviewed);
            token.throwIfCancelled();if(edited)state->write(id,"plan.md",edited->toUtf8());
            const auto accepted=state->snapshot(id);auto value=state->state(id);
            value["phase"]="approved";value["approved_sha256"]=accepted["plan_sha256"];value["approved_file_exists"]=accepted["file_exists"];
            state->save(id,value);auto output=state->snapshot(id);output["plan_was_edited"]=bool(edited);
            return ToolResult{"The host approved the plan. Planning restrictions are lifted; existing host tool rules still apply.\n"+output["plan"].toString(),output};
        }};
    };result.append(std::move(leave));return result;
}
void PlanMode::fork(const QString& from,const QString& to,const CancellationToken& token) const {
    require(from!=to,"Plan fork requires a new owner");const auto original=status(from,token);if(original["phase"]=="inactive")return;
    const auto guard=d->lock(to,token,true);require(d->state(to)["phase"]=="inactive","Target already has a plan");
    if(original["file_exists"].toBool())d->write(to,"plan.md",original["plan"].toString().toUtf8());
    d->save(to,{{"schema","iisacc.plan/1"},{"session_id",to},{"phase","planning"},{"revision",1},{"base_mode",original["base_mode"]}});
}
}
