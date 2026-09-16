#include "Worktrees.h"
#include "ShellProcess.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QUuid>

namespace iiLocalLLM::agent {
namespace {
void require(bool value,const QString& message,ErrorCode code=ErrorCode::InvalidArgument){if(!value)throw Error(code,message);}
QString digest(const QByteArray& bytes){return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
QString canonical(const QString& path){return QFileInfo(path).canonicalFilePath();}
void keys(const QJsonObject& value,const QStringList& allowed){for(auto it=value.begin();it!=value.end();++it)require(allowed.contains(it.key()),"Unknown worktree argument: "+it.key());}
void slug(const QString& name){
    require(!name.isEmpty()&&name.size()<=64,"Worktree name must contain 1 to 64 characters");
    static const QRegularExpression segment("\\A[A-Za-z0-9._-]+\\z");
    for(const auto& part:name.split('/'))require(part!="."&&part!=".."&&segment.match(part).hasMatch(),"Invalid worktree name segment");
}
QString flat(QString name){slug(name);return name.replace('/','+');}
void input(const QJsonObject& args,bool entering){
    keys(args,entering?QStringList{"name"}:QStringList{"action","discard_changes"});
    if(entering){if(args.contains("name")){require(args["name"].isString(),"Worktree name must be a string");slug(args["name"].toString());}}
    else {require(args["action"].isString()&&(args["action"]=="keep"||args["action"]=="remove"),"Worktree action must be keep or remove");
        require(!args.contains("discard_changes")||args["discard_changes"].isBool(),"discard_changes must be boolean");}
}
void directory(const QString& path,bool create){
    require(QDir::isAbsolutePath(path)&&!QDir(path).isRoot(),"Worktree directory must be an absolute non-root path");
    QString current=QDir(path).rootPath();
    for(const auto& part:QDir::cleanPath(path).split('/',Qt::SkipEmptyParts)){
        current=QDir(current).filePath(part);QFileInfo info(current);
        require(!info.isSymLink(),"Worktree directory contains a symbolic link",ErrorCode::Unauthorized);
        if(!info.exists()){require(create&&QDir().mkdir(current),"Cannot create worktree directory",ErrorCode::StorageFailure);info.refresh();}
        require(info.isDir(),"Worktree directory is not a directory",ErrorCode::StorageFailure);
    }
    require(canonical(path)==QDir::cleanPath(path),"Worktree directory changed identity",ErrorCode::Unauthorized);
}
ToolDefinition definition(bool entering,bool deferred){
    ToolDefinition result;result.name=entering?"EnterWorktree":"ExitWorktree";
    result.description=entering?"Create or resume an owned isolated worktree and switch this session into it. Use only when the user asks for a worktree. Name is optional, up to 64 ASCII characters with slash-separated segments.":
        "Leave this session's worktree and return to the original directory. keep preserves all work. remove deletes the owned worktree and branch; changed files or new commits require discard_changes=true. Use only when the user asks to leave.";
    result.inputSchema={{"type","object"},{"additionalProperties",false},{"properties",entering?
        QJsonObject{{"name",QJsonObject{{"type","string"},{"minLength",1},{"maxLength",64}}}}:
        QJsonObject{{"action",QJsonObject{{"type","string"},{"enum",QJsonArray{"keep","remove"}}}},{"discard_changes",QJsonObject{{"type","boolean"}}}}}};
    if(!entering)result.inputSchema["required"]=QJsonArray{"action"};
    result.outputSchema={{"type","object"}};result.deferred=deferred;result.metadata={{"source","builtin.worktree"}};
    return result;
}
}
class Worktrees::Impl {
public:
    QString stateDirectory;WorktreeOptions options;
    Impl(QString state,WorktreeOptions config):options(std::move(config)){
        require(!state.isEmpty()&&options.commandTimeoutMs>=100&&options.commandTimeoutMs<=600000&&options.maxOutputBytes>=1024&&options.maxOutputBytes<=16*1024*1024,"Invalid worktree configuration");
        require(!options.gitProgram.isEmpty()&&!options.gitProgram.contains(QChar::Null),"Invalid Git program");
        require(!options.baseRef.contains(QChar::Null)&&options.baseRef.size()<=4096,"Invalid worktree base ref");
        if(!options.directory.isEmpty())require(QDir::isAbsolutePath(options.directory),"Worktree storage must be absolute");
        for(const auto& path:options.sparsePaths)require(!path.isEmpty()&&!QDir::isAbsolutePath(path)&&!path.split('/').contains("..")&&!path.contains(QChar::Null)&&!path.startsWith('-'),"Invalid sparse checkout path");
        stateDirectory=QFileInfo(state).absoluteFilePath();directory(stateDirectory,true);
        require(QFile::setPermissions(stateDirectory,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner),"Cannot protect worktree state",ErrorCode::StorageFailure);
    }
    QString statePath(const QString& owner)const{
        static const QRegularExpression id("\\A[A-Za-z0-9_-]{1,128}\\z");require(id.match(owner).hasMatch(),"Invalid worktree owner");
        return QDir(stateDirectory).filePath(owner+".json");
    }
    std::unique_ptr<QLockFile> lock(const QString& owner)const{
        directory(stateDirectory,false);auto result=std::make_unique<QLockFile>(statePath(owner)+".lock");result->setStaleLockTime(0);
        require(result->tryLock(0),"Worktree owner is busy",ErrorCode::ModelInUse);return result;
    }
    QJsonObject load(const QString& owner)const{
        directory(stateDirectory,false);const auto path=statePath(owner);const QFileInfo info(path);
        require(!info.isSymLink(),"Worktree state must not be a symbolic link",ErrorCode::Unauthorized);
        if(!info.exists())return {{"schema","iisacc.worktrees/1"},{"owner",owner},{"revision",0},{"active",""},{"owned",QJsonObject{}}};
        QFile file(path);require(info.isFile()&&info.size()<=1024*1024&&file.open(QIODevice::ReadOnly),"Cannot read worktree state",ErrorCode::StorageFailure);
        QJsonParseError error;auto value=QJsonDocument::fromJson(file.readAll(),&error);
        require(error.error==QJsonParseError::NoError&&value.isObject(),"Invalid worktree state",ErrorCode::ProtocolError);const auto object=value.object();
        require(object["schema"]=="iisacc.worktrees/1"&&object["owner"]==owner&&object["revision"].isDouble()&&object["revision"].toInteger()>=0&&object["revision"].toDouble()==object["revision"].toInteger()&&object["revision"].toInteger()<9007199254740991LL
            &&object["active"].isString()&&object["owned"].isObject()&&object["owned"].toObject().size()<=64,"Invalid worktree state identity",ErrorCode::ProtocolError);
        const auto owned=object["owned"].toObject();
        require(object["active"].toString().isEmpty()||owned.contains(object["active"].toString()),"Active worktree record is missing",ErrorCode::ProtocolError);
        for(auto it=owned.begin();it!=owned.end();++it) {
            slug(it.key());require(it.value().isObject(),"Invalid owned worktree record",ErrorCode::ProtocolError);const auto record=it.value().toObject();
            require(record["sessionId"]==owner&&record["worktreeName"]==it.key()&&record["hookBased"].isBool()
                &&QStringList{"creating","active","kept","failed","branch_retained"}.contains(record["phase"].toString()),"Invalid worktree record identity",ErrorCode::ProtocolError);
            for(const auto& key:{"originalCwd","worktreePath","repoRoot","commonDirectory","gitDirectory"})if(record.contains(key))
                require(record[key].isString()&&QDir::isAbsolutePath(record[key].toString())&&!record[key].toString().contains(QChar::Null)
                    &&QDir::cleanPath(record[key].toString())==record[key].toString()&&!QDir(record[key].toString()).isRoot(),"Invalid worktree record path",ErrorCode::ProtocolError);
            require(record.contains("originalCwd"),"Missing original worktree directory",ErrorCode::ProtocolError);
            if(record["phase"]=="active"||record["phase"]=="kept")require(record.contains("worktreePath")&&(record["hookBased"].toBool()
                ||(record["worktreeBranch"]=="worktree-"+flat(it.key())&&record.contains("gitDirectory")&&record.contains("commonDirectory")&&record.contains("repoRoot"))),"Incomplete owned worktree",ErrorCode::ProtocolError);
        }
        return object;
    }
    void save(QJsonObject& value)const{
        const auto path=statePath(value["owner"].toString());require(!QFileInfo(path).isSymLink(),"Worktree state changed path",ErrorCode::Unauthorized);
        const auto revision=value["revision"].toInteger();require(revision<9007199254740990LL,"Worktree revision exhausted",ErrorCode::ResourceLimit);value["revision"]=revision+1;
        const auto bytes=QJsonDocument(value).toJson(QJsonDocument::Compact);require(bytes.size()<=1024*1024,"Worktree state exceeds limit",ErrorCode::ResourceLimit);
        QSaveFile file(path);require(file.open(QIODevice::WriteOnly)&&file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner)
            &&file.write(bytes)==bytes.size()&&file.commit(),"Cannot persist worktree state",ErrorCode::StorageFailure);
    }
    struct Command {QByteArray out,error;int code;};
    Command git(const QString& cwd,const QStringList& args,const CancellationToken& token,bool mustSucceed=true)const{
        auto env=QProcessEnvironment::systemEnvironment();
        for(const auto& key:env.keys())if(QStringList{"GIT_DIR","GIT_WORK_TREE","GIT_COMMON_DIR","GIT_INDEX_FILE","GIT_OBJECT_DIRECTORY","GIT_ALTERNATE_OBJECT_DIRECTORIES","GIT_PREFIX","GIT_CONFIG_COUNT"}.contains(key)
            ||key.startsWith("GIT_CONFIG_KEY_")||key.startsWith("GIT_CONFIG_VALUE_"))env.remove(key);
        env.insert("GIT_TERMINAL_PROMPT","0");env.insert("GIT_ASKPASS","");env.insert("SSH_ASKPASS_REQUIRE","never");env.insert("GIT_PAGER","cat");env.insert("LC_ALL","C");
        Command result;const auto exit=detail::shellProcess(cwd,{},options.commandTimeoutMs,token,{},[&](const QByteArray& bytes,bool error){
            auto& output=error?result.error:result.out;require(output.size()+bytes.size()<=options.maxOutputBytes,"Git output exceeds worktree limit",ErrorCode::ResourceLimit);output+=bytes;
        },{},&env,options.gitProgram,args);result.code=exit.crashed?-1:exit.code;
        if(mustSucceed)require(result.code==0,"Git worktree operation failed: "+QString::fromUtf8(result.error).left(4096),ErrorCode::RuntimeFailure);
        return result;
    }
    QString gitText(const QString& cwd,const QStringList& args,const CancellationToken& token,bool required=true)const{
        const auto value=git(cwd,args,token,required);if(value.code!=0)return {};auto bytes=value.out;if(bytes.endsWith('\n'))bytes.chop(1);return QString::fromUtf8(bytes);
    }
    QString origin(const ToolContext& context)const{
        context.cancellation.throwIfCancelled();const auto result=canonical(context.workingDirectory);
        require(!result.isEmpty()&&QFileInfo(result).isDir(),"Worktree origin must be an existing directory");return result;
    }
    QJsonObject facts(const QString& cwd,const CancellationToken& token)const{
        const auto common=canonical(gitText(cwd,{"rev-parse","--path-format=absolute","--git-common-dir"},token));require(!common.isEmpty(),"Cannot resolve Git common directory",ErrorCode::RuntimeFailure);
        const auto listing=git(cwd,{"worktree","list","--porcelain","-z"},token).out.split('\0');QString main;
        for(const auto& row:listing)if(row.startsWith("worktree ")){main=canonical(QString::fromUtf8(row.mid(9)));break;}
        require(!main.isEmpty(),"Cannot resolve primary Git worktree",ErrorCode::RuntimeFailure);
        return {{"repoRoot",main},{"commonDirectory",common}};
    }
    QString base(const QString& repo,const CancellationToken& token)const{
        static const QRegularExpression sha("\\A[0-9a-f]{40}([0-9a-f]{24})?\\z");
        auto resolve=[&](const QString& ref){const auto value=gitText(repo,{"rev-parse","--verify","--end-of-options",ref+"^{commit}"},token,false);return sha.match(value).hasMatch()?value:QString();};
        if(!options.baseRef.isEmpty()){const auto commit=resolve(options.baseRef);require(!commit.isEmpty(),"Cannot resolve configured worktree base commit");return commit;}
        const auto remoteHead=gitText(repo,{"symbolic-ref","--quiet","refs/remotes/origin/HEAD"},token,false);
        for(const auto& ref:QStringList{remoteHead,"refs/remotes/origin/main","refs/remotes/origin/master"})if(!ref.isEmpty()){
            const auto commit=resolve(ref);if(!commit.isEmpty())return commit;
        }
        if(options.fetchMissingBase&&!gitText(repo,{"remote","get-url","origin"},token,false).isEmpty()){
            auto branch=remoteHead.startsWith("refs/remotes/origin/")?remoteHead.mid(20):gitText(repo,{"symbolic-ref","--quiet","--short","HEAD"},token,false);
            if(branch.isEmpty())branch="HEAD";
            if(git(repo,{"fetch","--no-tags","origin",branch},token,false).code==0){const auto commit=resolve("FETCH_HEAD");if(!commit.isEmpty())return commit;}
        }
        const auto commit=resolve("HEAD");require(!commit.isEmpty(),"Git repository has no committed worktree base");return commit;
    }
    void owns(const QJsonObject& record,const ToolContext& context)const{
        const auto path=record["worktreePath"].toString();const auto cwd=origin(context);
        require(record["sessionId"]==context.sessionId&&(cwd==record["originalCwd"]||cwd==path),"Worktree belongs to another owner or workspace",ErrorCode::Unauthorized);
        require(!path.isEmpty()&&canonical(path)==path&&!QFileInfo(path).isSymLink(),"Owned worktree path changed or is unavailable",ErrorCode::Unauthorized);
        if(record["hookBased"].toBool())return;
        require(gitText(path,{"rev-parse","--show-toplevel"},context.cancellation)==path,"Worktree root changed",ErrorCode::Unauthorized);
        require(canonical(gitText(path,{"rev-parse","--path-format=absolute","--git-dir"},context.cancellation))==record["gitDirectory"].toString()
            &&canonical(gitText(path,{"rev-parse","--path-format=absolute","--git-common-dir"},context.cancellation))==record["commonDirectory"].toString(),"Worktree Git identity changed",ErrorCode::Unauthorized);
        require(gitText(path,{"symbolic-ref","--quiet","--short","HEAD"},context.cancellation)==record["worktreeBranch"].toString(),"Owned worktree branch changed",ErrorCode::Unauthorized);
    }
    QByteArray contentFingerprint(const QString& root,const QStringList& changed,const CancellationToken& token) const {
        QStringList paths;QCryptographicHash hash(QCryptographicHash::Sha256);qint64 bytes=0;
        for(const auto& relative:changed) {
            const auto path=QDir::cleanPath(QDir(root).absoluteFilePath(relative));
            require(path.startsWith(root+'/'),"Changed file escapes worktree",ErrorCode::Unauthorized);paths.append(path);
            if(QFileInfo(path).isDir()&&!QFileInfo(path).isSymLink()) {
                QDirIterator it(path,QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot,QDirIterator::Subdirectories);
                while(it.hasNext()){token.throwIfCancelled();require(paths.size()<10000,"Worktree removal snapshot exceeds 10000 paths",ErrorCode::ResourceLimit);paths.append(it.next());}
            }
        }
        paths.removeDuplicates();paths.sort();
        for(const auto& path:paths) {
            token.throwIfCancelled();QFileInfo info(path);hash.addData(path.toUtf8());hash.addData(QByteArray(1,'\0'));
            if(info.isSymLink()){hash.addData("symlink");hash.addData(info.symLinkTarget().toUtf8());}
            else if(info.isDir())hash.addData("directory");
            else if(!info.exists())hash.addData("missing");
            else {
                require(info.isFile(),"Cannot snapshot special worktree file",ErrorCode::InvalidArgument);
                QFile file(path);require(file.open(QIODevice::ReadOnly),"Cannot snapshot changed worktree file",ErrorCode::StorageFailure);hash.addData("file");hash.addData(QByteArray::number(info.size())+'\0');
                while(!file.atEnd()){token.throwIfCancelled();const auto part=file.read(65536);require(!part.isEmpty()||file.error()==QFileDevice::NoError,"Cannot read worktree snapshot",ErrorCode::StorageFailure);
                    bytes+=part.size();require(bytes<=64*1024*1024,"Worktree removal snapshot exceeds 64 MiB",ErrorCode::ResourceLimit);hash.addData(part);}
            }
            hash.addData(QByteArray(1,'\0'));
        }
        return hash.result();
    }
    QJsonObject changes(const QJsonObject& record,const ToolContext& context)const{
        owns(record,context);if(record["hookBased"].toBool())return {{"changesKnown",false}};
        const auto path=record["worktreePath"].toString();const auto bytes=git(path,{"status","--porcelain=v1","-z","--untracked-files=all","--ignored=matching"},context.cancellation).out;
        const auto entries=bytes.split('\0');int count=0;QJsonArray files;QStringList changed;
        for(qsizetype i=0;i<entries.size();++i){const auto& row=entries[i];if(row.isEmpty())continue;
            require(row.size()>=4&&row[2]==' ',"Invalid Git status record",ErrorCode::ProtocolError);++count;require(count<=10000,"Worktree change count exceeds limit",ErrorCode::ResourceLimit);changed.append(QString::fromUtf8(row.mid(3)));
            if(files.size()<128)files.append(QString::fromUtf8(row.mid(3)));if(row[0]=='R'||row[0]=='C'||row[1]=='R'||row[1]=='C'){require(++i<entries.size()&&!entries[i].isEmpty(),"Missing Git rename source",ErrorCode::ProtocolError);}}
        const auto head=gitText(path,{"rev-parse","--verify","HEAD"},context.cancellation);
        bool valid=false;const auto commits=gitText(path,{"rev-list","--count",record["originalHeadCommit"].toString()+".."+head},context.cancellation).toLongLong(&valid);
        require(valid&&commits>=0,"Cannot count worktree commits",ErrorCode::ProtocolError);
        return {{"changesKnown",true},{"changedFiles",count},{"commits",commits},{"files",files},{"filesTruncated",count>files.size()},
            {"headCommit",head},{"changeFingerprint",digest(bytes+'\0'+head.toUtf8()+contentFingerprint(path,changed,context.cancellation))}};
    }
    QJsonObject active(const QJsonObject& state)const{
        const auto name=state["active"].toString();require(!name.isEmpty(),"No active worktree belongs to this session",ErrorCode::NotFound);
        auto record=state["owned"].toObject()[name].toObject();require(record["phase"]=="active", "Worktree transition needs recovery",ErrorCode::StorageFailure);return record;
    }
    QJsonObject status(const ToolContext& context,bool detailed=true)const{
        const auto state=load(context.sessionId);const auto cwd=origin(context);
        QJsonObject value{{"active",!state["active"].toString().isEmpty()},{"revision",state["revision"]},{"workingDirectory",cwd},{"originalCwd",cwd}};
        QJsonArray kept;const auto owned=state["owned"].toObject();for(auto it=owned.begin();it!=owned.end();++it)if(it.key()!=state["active"])
            kept.append(QJsonObject{{"name",it.key()},{"worktreePath",it.value().toObject()["worktreePath"]},{"worktreeBranch",it.value().toObject()["worktreeBranch"]},{"phase",it.value().toObject()["phase"]}});
        value["retained"]=kept;if(!value["active"].toBool())return value;
        const auto record=owned[state["active"].toString()].toObject();
        require(cwd==record["originalCwd"]||cwd==record["worktreePath"],"Worktree belongs to another workspace",ErrorCode::Unauthorized);
        for(auto it=record.begin();it!=record.end();++it)if(it.key()!="gitDirectory"&&it.key()!="commonDirectory")value[it.key()]=it.value();
        const auto path=record["worktreePath"].toString();
        if(record["phase"]=="active"&&canonical(path)==path&&!QFileInfo(path).isSymLink())value["workingDirectory"]=path;
        else {value["workingDirectory"]=record["originalCwd"];value["recoveryRequired"]=true;value["changesKnown"]=false;}
        if(detailed)try {const auto summary=changes(record,context);for(auto it=summary.begin();it!=summary.end();++it)value[it.key()]=it.value();}
        catch(const Error& error){if(error.code()==ErrorCode::Cancelled)throw;value["changesKnown"]=false;value["recoveryRequired"]=true;value["diagnostic"]=QString::fromUtf8(error.what());}
        return value;
    }
    ToolResult enter(const QJsonObject& args,const ToolContext& context,qint64 expected=-1,const QString& expectedOrigin={})const{
        input(args,true);const auto cwd=origin(context);require(expectedOrigin.isEmpty()||cwd==expectedOrigin,"Worktree origin changed after permission preview",ErrorCode::Unauthorized);
        auto guard=lock(context.sessionId);auto state=load(context.sessionId);require(expected<0||state["revision"].toInteger()==expected,"Worktree state changed after permission preview",ErrorCode::ModelInUse);
        require(state["active"].toString().isEmpty(),"Already in an owned worktree session",ErrorCode::ModelInUse);
        const auto name=args.contains("name")?args["name"].toString():"session-"+QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);const auto leaf=flat(name);
        auto owned=state["owned"].toObject();auto record=owned[name].toObject();
        if(!record.isEmpty()){
            require(record["phase"]=="kept","Owned worktree requires recovery",ErrorCode::StorageFailure);owns(record,context);record["phase"]="active";
        }else{
            require(owned.size()<64,"Owned worktree limit reached",ErrorCode::ResourceLimit);
            record={{"sessionId",context.sessionId},{"originalCwd",cwd},{"worktreeName",name},{"phase","creating"},{"hookBased",bool(options.create)}};
            try {
            if(options.create){
                owned[name]=record;state["owned"]=owned;save(state);
                const auto path=canonical(options.create(name,context));require(!path.isEmpty()&&QFileInfo(path).isDir()&&path!=cwd&&!cwd.startsWith(path+'/')&&path!=stateDirectory&&!stateDirectory.startsWith(path+'/')&&!path.startsWith(stateDirectory+'/'),"Worktree hook returned an invalid directory",ErrorCode::RuntimeFailure);record["worktreePath"]=path;
            }else{
                const auto repository=facts(cwd,context.cancellation);for(auto it=repository.begin();it!=repository.end();++it)record[it.key()]=it.value();
                const auto repo=record["repoRoot"].toString();const auto root=options.directory.isEmpty()?QDir(QFileInfo(repo).absolutePath()).filePath(".iilocal-worktrees/"+digest(repo.toUtf8()).left(16)):QDir::cleanPath(options.directory);
                directory(root,true);const auto path=QDir(root).filePath(leaf);const auto branch="worktree-"+leaf;
                require(git(repo,{"check-ref-format","--branch",branch},context.cancellation,false).code==0,"Invalid Git worktree branch name");
                require(!QFileInfo::exists(path)&&!QFileInfo(path).isSymLink(),"Worktree path already exists",ErrorCode::ModelInUse);
                require(git(repo,{"show-ref","--verify","--quiet","refs/heads/"+branch},context.cancellation,false).code==1,"Worktree branch already exists or cannot be inspected",ErrorCode::ModelInUse);
                const auto commit=base(repo,context.cancellation);record["worktreePath"]=path;record["worktreeBranch"]=branch;record["originalHeadCommit"]=commit;
                record["originalBranch"]=gitText(repo,{"symbolic-ref","--quiet","--short","HEAD"},context.cancellation,false);
                owned[name]=record;state["owned"]=owned;save(state);
                QStringList arguments{"worktree","add"};if(!options.sparsePaths.isEmpty())arguments.append("--no-checkout");arguments.append({"-b",branch,path,commit});
                git(repo,arguments,context.cancellation);
                record["gitDirectory"]=canonical(gitText(path,{"rev-parse","--path-format=absolute","--git-dir"},context.cancellation));
                owned[name]=record;state["owned"]=owned;save(state);
                if(!options.sparsePaths.isEmpty()){
                    QStringList sparse{"sparse-checkout","set","--cone","--"};sparse.append(options.sparsePaths);git(path,sparse,context.cancellation);git(path,{"checkout","HEAD"},context.cancellation);record["usedSparsePaths"]=true;
                }
            }
            } catch(...) {
                // Partial Git or host-hook work is retained for inspection; never
                // guess that it is safe to delete files after a failed operation.
                record["phase"]="failed";owned[name]=record;state["owned"]=owned;state["active"]="";save(state);throw;
            }
            record["phase"]="active";
        }
        owned[name]=record;state["owned"]=owned;state["active"]=name;save(state);
        QJsonObject data{{"worktreePath",record["worktreePath"]},{"originalCwd",record["originalCwd"]},{"revision",state["revision"]},{"message","Session entered its owned worktree."}};
        for(const auto& key:{"worktreeBranch","originalHeadCommit","hookBased","usedSparsePaths"})if(record.contains(key))data[key]=record[key];
        return {data["message"].toString(),data};
    }
    ToolResult exit(const QJsonObject& args,const ToolContext& context,qint64 expected=-1,const QString& fingerprint={})const{
        input(args,false);context.cancellation.throwIfCancelled();auto guard=lock(context.sessionId);auto state=load(context.sessionId);
        require(expected<0||state["revision"].toInteger()==expected,"Worktree state changed after permission preview",ErrorCode::ModelInUse);
        const auto name=state["active"].toString();require(!name.isEmpty(),"No active worktree belongs to this session",ErrorCode::NotFound);
        auto owned=state["owned"].toObject();auto record=owned[name].toObject();
        const auto cwd=origin(context);require(record["sessionId"]==context.sessionId&&(cwd==record["originalCwd"]||cwd==record["worktreePath"]),"Worktree belongs to another owner or workspace",ErrorCode::Unauthorized);
        const auto original=record["originalCwd"].toString();require(canonical(original)==original,"Original working directory is unavailable",ErrorCode::StorageFailure);
        const bool remove=args["action"]=="remove",discard=args["discard_changes"].toBool();
        const auto summary=remove?changes(record,context):QJsonObject{};
        if(remove)require(fingerprint.isEmpty()||summary["changeFingerprint"].toString()==fingerprint,"Worktree contents changed after permission preview",ErrorCode::ModelInUse);
        QJsonObject data{{"action",args["action"]},{"originalCwd",original},{"worktreePath",record["worktreePath"]},{"worktreeBranch",record["worktreeBranch"]}};
        if(remove){
            require(discard||(summary["changesKnown"].toBool()&&summary["changedFiles"].toInt()==0&&summary["commits"].toInteger()==0),
                "Worktree has changes, new commits or unknown state; use keep or explicitly discard_changes",ErrorCode::ModelInUse);
            const auto path=record["worktreePath"].toString();
            if(record["hookBased"].toBool()){require(bool(options.remove),"No host WorktreeRemove adapter is configured",ErrorCode::RuntimeUnavailable);options.remove(path,context);require(!QFileInfo::exists(path),"WorktreeRemove adapter did not remove its directory",ErrorCode::StorageFailure);}
            else{
                QStringList arguments{"worktree","remove"};if(discard)arguments.append("--force");arguments.append(path);git(record["repoRoot"].toString(),arguments,context.cancellation);
                record["phase"]="branch_retained";owned[name]=record;state["owned"]=owned;state["active"]="";save(state);
                git(record["repoRoot"].toString(),{"update-ref","--no-deref","-d","refs/heads/"+record["worktreeBranch"].toString(),summary["headCommit"].toString()},context.cancellation);
            }
            data["discardedFiles"]=summary["changedFiles"];data["discardedCommits"]=summary["commits"];owned.remove(name);
        }else{record["phase"]="kept";owned[name]=record;}
        state["owned"]=owned;state["active"]="";save(state);data["revision"]=state["revision"];
        data["message"]=remove?"Removed the owned worktree and returned to the original directory.":"Returned to the original directory; the worktree and branch are preserved.";
        return {data["message"].toString(),data};
    }
};
Worktrees::Worktrees(QString directory,WorktreeOptions options):d(std::make_shared<Impl>(std::move(directory),std::move(options))){}
Worktrees::~Worktrees()=default;
WorktreeView Worktrees::view(const ToolContext& context)const{
    const auto value=d->status(context,false);return {value["originalCwd"].toString(d->origin(context)),value["workingDirectory"].toString(),quint64(value["revision"].toInteger()),value};
}
QJsonObject Worktrees::status(const ToolContext& context)const{return d->status(context);}
ToolResult Worktrees::enter(const QJsonObject& args,const ToolContext& context)const{return d->enter(args,context);}
ToolResult Worktrees::exit(const QJsonObject& args,const ToolContext& context)const{return d->exit(args,context);}
Tool Worktrees::enterTool(bool deferred)const{
    Tool tool;tool.definition=definition(true,deferred);auto state=d;tool.validate=[](const QJsonObject& args,const ToolContext&){input(args,true);};
    tool.execute=[state](const QJsonObject& args,const ToolContext& context){return state->enter(args,context);};
    tool.prepare=[state,base=tool.definition](const QJsonObject& args,const ToolContext& context){input(args,true);auto chosen=args;if(!chosen.contains("name"))chosen["name"]="session-"+QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        const auto data=state->load(context.sessionId);const auto cwd=state->origin(context);auto prepared=base;prepared.metadata["worktree_name"]=chosen["name"];
        prepared.metadata["worktree_revision"]=data["revision"];prepared.metadata["original_directory"]=cwd;
        return PreparedTool{prepared,[state,chosen,context,revision=data["revision"].toInteger(),cwd]{return state->enter(chosen,context,revision,cwd);}};};return tool;
}
Tool Worktrees::exitTool(bool deferred)const{
    Tool tool;tool.definition=definition(false,deferred);auto state=d;tool.validate=[](const QJsonObject& args,const ToolContext&){input(args,false);};
    tool.execute=[state](const QJsonObject& args,const ToolContext& context){return state->exit(args,context);};
    tool.prepare=[state,base=tool.definition](const QJsonObject& args,const ToolContext& context){input(args,false);const auto data=state->status(context);require(data["active"].toBool(),"No active worktree belongs to this session",ErrorCode::NotFound);
        if(args["action"]=="remove"&&!data["hookBased"].toBool())require(data["changesKnown"].toBool()&&!data["changeFingerprint"].toString().isEmpty(),"Cannot prepare worktree removal without a complete Git change snapshot",ErrorCode::ModelInUse);
        auto prepared=base;
        prepared.metadata["worktree_preview"]=data;prepared.metadata["destructive"]=args["action"]=="remove";
        return PreparedTool{prepared,[state,args,context,revision=data["revision"].toInteger(),fingerprint=data["changeFingerprint"].toString()]{return state->exit(args,context,revision,fingerprint);}};};return tool;
}
WorktreeOptions worktreeOptionsFromJson(const QJsonObject& config) {
    keys(config,{"directory","base_ref","fetch_missing_base","sparse_paths","command_timeout_ms"});WorktreeOptions result;result.enabled=true;
    for(const auto& key:{"directory","base_ref"})if(config.contains(key))require(config[key].isString()&&!config[key].toString().contains(QChar::Null),"Invalid worktree configuration string");
    result.directory=config["directory"].toString();result.baseRef=config["base_ref"].toString();
    require(result.directory.isEmpty()||QDir::isAbsolutePath(result.directory),"Worktree directory must be absolute");
    if(config.contains("fetch_missing_base")){require(config["fetch_missing_base"].isBool(),"fetch_missing_base must be boolean");result.fetchMissingBase=config["fetch_missing_base"].toBool();}
    if(config.contains("sparse_paths")){require(config["sparse_paths"].isArray()&&config["sparse_paths"].toArray().size()<=128,"Invalid sparse_paths");
        for(const auto& path:config["sparse_paths"].toArray()){require(path.isString()&&!path.toString().isEmpty()&&QDir::isRelativePath(path.toString())&&!path.toString().split('/').contains("..")&&!path.toString().contains(QChar::Null),"Invalid sparse path");result.sparsePaths.append(path.toString());}}
    if(config.contains("command_timeout_ms")){const auto value=config["command_timeout_ms"];require(value.isDouble()&&value.toInteger()==value.toDouble()&&value.toInteger()>=100&&value.toInteger()<=600000,"Invalid Git timeout");result.commandTimeoutMs=value.toInt();}
    return result;
}

}
