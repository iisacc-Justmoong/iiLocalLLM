#include "FileCheckpoints.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QUuid>
#include <QtCore/QSet>
#include <algorithm>

namespace iiLocalLLM::agent {
namespace {
constexpr qint64 fileLimit=1024*1024, stateLimit=16*1024*1024, blobLimit=64*1024*1024;
constexpr int snapshotLimit=100, trackedLimit=256;
void check(bool ok,const QString& text,ErrorCode code=ErrorCode::StorageFailure){if(!ok)throw Error(code,text);}
QString hash(const QByteArray& bytes){return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
QString uuid(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
bool inside(const QString& path,const QString& root){return !root.isEmpty()&&(path==root||path.startsWith(root.endsWith('/')?root:root+'/'));}
bool digest(const QString& value){static const QRegularExpression re("^[0-9a-f]{64}$");return re.match(value).hasMatch();}
void validId(const QString& value){check(!QUuid(value).isNull()&&QUuid(value).toString(QUuid::WithoutBraces)==value,"Invalid checkpoint/session ID",ErrorCode::InvalidArgument);}
void plainPath(const QString& path){
    auto info=QFileInfo(path);
    for(;;){check(!info.isSymLink(),"Checkpoint paths must not contain symlinks: "+path);const auto parent=info.dir().absolutePath();if(parent==info.absoluteFilePath())break;info=QFileInfo(parent);}
}
QByteArray read(const QString& path,qint64 limit){
    plainPath(path);check(QFileInfo(path).isFile(),"Checkpoint target is not a regular file: "+path);
    QFile file(path);check(file.open(QIODevice::ReadOnly),"Cannot read checkpoint file: "+path);
    const auto bytes=file.read(limit+1);check(file.error()==QFileDevice::NoError,"Cannot read complete checkpoint file: "+path);
    check(bytes.size()<=limit,"Checkpoint file exceeds byte limit",ErrorCode::ResourceLimit);return bytes;
}
void write(const QString& path,const QByteArray& bytes,std::optional<QFileDevice::Permissions> permissions={}){
    plainPath(path);QSaveFile file(path);check(file.open(QIODevice::WriteOnly),"Cannot open checkpoint destination: "+path);
    if(permissions)check(file.setPermissions(*permissions),"Cannot restore file permissions");
    check(file.write(bytes)==bytes.size()&&file.commit(),"Cannot commit checkpoint file: "+path);
}
QString scope(const ToolContext& c){return hash((c.workingDirectory+'\n'+QString::number(c.workspaceRevision)).toUtf8());}
void target(const QString& path,const ToolContext& c,const QString& privateRoot){
    check(QDir::isAbsolutePath(path)&&path.size()<=4096&&QDir::cleanPath(path)==path&&!path.contains(QChar::Null),"Invalid checkpoint target",ErrorCode::ProtocolError);
    plainPath(path);
    auto covered=[&](const QString& root){return path!=root&&inside(path,root);};
    check(covered(c.workingDirectory)||std::any_of(c.workingDirectories.cbegin(),c.workingDirectories.cend(),covered),"Checkpoint target is outside current working directories",ErrorCode::Unauthorized);
    check(!inside(path,privateRoot)&&!inside(path,c.artifactsDirectory)&&!inside(path,c.plansDirectory),"Checkpoint target is host-private",ErrorCode::Unauthorized);
    for(const auto& denied:c.protectedPaths)check(!inside(path,denied)&&!inside(path,QFileInfo(denied).canonicalFilePath()),"Checkpoint target is protected",ErrorCode::Unauthorized);
    check(!QFileInfo::exists(path)||QFileInfo(path).isFile(),"Checkpoint target is not a regular file: "+path);
}
QJsonObject current(const QString& path){
    plainPath(path);if(!QFileInfo::exists(path))return {{"exists",false}};
    const auto bytes=read(path,fileLimit);return {{"exists",true},{"sha256",hash(bytes)},{"bytes",bytes.size()},{"permissions",int(QFileInfo(path).permissions())}};
}
void validateBackup(const QJsonValue& value){
    check(value.isObject(),"Invalid checkpoint backup",ErrorCode::ProtocolError);const auto o=value.toObject();
    check(o["exists"].isBool(),"Invalid checkpoint existence",ErrorCode::ProtocolError);
    if(!o["exists"].toBool()){check(o.size()==1,"Invalid absent checkpoint",ErrorCode::ProtocolError);return;}
    check(o.size()==4&&digest(o["sha256"].toString())&&o["bytes"].isDouble()&&o["bytes"].toDouble()==o["bytes"].toInteger()
        &&o["bytes"].toInteger()>=0&&o["bytes"].toInteger()<=fileLimit&&o["permissions"].isDouble()
        &&o["permissions"].toDouble()==o["permissions"].toInteger()&&o["permissions"].toInteger()>=0&&o["permissions"].toInteger()<=65535,"Invalid checkpoint backup fields",ErrorCode::ProtocolError);
}
struct Journal {
    QString root,dir;QLockFile lock;QJsonObject state;
    Journal(const QString& root,const ToolContext& c):root(root),dir(QDir(root).filePath(c.sessionId)),lock(dir+"/history.lock"){
        check(!c.workingDirectory.isEmpty()&&QFileInfo(c.workingDirectory).canonicalFilePath()==c.workingDirectory&&QFileInfo(c.workingDirectory).isDir(),"Checkpoint workspace must be canonical",ErrorCode::InvalidArgument);
        c.cancellation.throwIfCancelled();validId(c.sessionId);plainPath(dir);check(QDir().mkpath(dir),"Cannot create checkpoint directory");
        lock.setStaleLockTime(0);check(lock.tryLock(0),"File checkpoints are in use",ErrorCode::ModelInUse);
        const auto path=dir+"/state.json";plainPath(path);
        if(!QFileInfo::exists(path))state={{"version",1},{"session_id",c.sessionId},{"sequence",0},{"snapshots",QJsonArray{}},{"scopes",QJsonObject{}}};
        else {
            QJsonParseError error;const auto doc=QJsonDocument::fromJson(read(path,stateLimit),&error);
            check(error.error==QJsonParseError::NoError&&doc.isObject(),"Corrupt file checkpoint journal",ErrorCode::ProtocolError);state=doc.object();
        }
        validate(c);
    }
    void validate(const ToolContext& c){
        check(state["version"]==1&&state["session_id"]==c.sessionId&&state["sequence"].isDouble()&&state["sequence"].toDouble()==state["sequence"].toInteger()
            &&state["sequence"].toInteger()>=0&&state["sequence"].toInteger()<9007199254740991LL
            &&state["snapshots"].isArray()&&state["scopes"].isObject(),"Invalid file checkpoint journal",ErrorCode::ProtocolError);
        const auto scopes=state["scopes"].toObject();const auto snaps=state["snapshots"].toArray();
        check(snaps.size()<=snapshotLimit&&scopes.size()<=snapshotLimit,"Checkpoint history exceeds limits",ErrorCode::ResourceLimit);
        int tracked=0;
        for(auto it=scopes.begin();it!=scopes.end();++it){
            const auto o=it.value().toObject();const auto workspace=o["workspace"].toString(),revision=o["revision"].toString();bool ok=false;const auto n=revision.toULongLong(&ok);
            check(it.value().isObject()&&QDir::isAbsolutePath(workspace)&&QDir::cleanPath(workspace)==workspace&&ok&&QString::number(n)==revision
                &&it.key()==hash((workspace+'\n'+revision).toUtf8())&&o["initial"].isObject(),"Invalid checkpoint workspace",ErrorCode::ProtocolError);
            const auto files=o["initial"].toObject();tracked+=files.size();for(auto f=files.begin();f!=files.end();++f){check(QDir::isAbsolutePath(f.key())&&QDir::cleanPath(f.key())==f.key(),"Invalid tracked path",ErrorCode::ProtocolError);validateBackup(f.value());}
        }
        check(tracked<=trackedLimit,"Too many tracked files",ErrorCode::ResourceLimit);QSet<QString> seen;
        for(const auto& value:snaps){const auto o=value.toObject();validId(o["message_id"].toString());const auto key=o["scope"].toString();
            check(value.isObject()&&scopes.contains(key)&&o["files"].isObject()&&o["timestamp"].isString(),"Invalid file checkpoint",ErrorCode::ProtocolError);
            const auto id=key+o["message_id"].toString();check(!seen.contains(id),"Duplicate file checkpoint",ErrorCode::ProtocolError);seen.insert(id);
            const auto files=o["files"].toObject(),initial=scopes[key].toObject()["initial"].toObject();
            for(auto f=files.begin();f!=files.end();++f){check(initial.contains(f.key()),"Untracked checkpoint file",ErrorCode::ProtocolError);validateBackup(f.value());}
        }
    }
    QJsonObject capture(const QString& path,const ToolContext& c){
        target(path,c,root);const auto result=current(path);if(!result["exists"].toBool())return result;
        const auto bytes=read(path,fileLimit);check(hash(bytes)==result["sha256"].toString(),"File changed while checkpointing");
        const auto blob=dir+'/'+result["sha256"].toString()+".blob";
        if(QFileInfo::exists(blob))check(read(blob,fileLimit)==bytes,"Corrupt existing checkpoint blob",ErrorCode::ProtocolError);
        else {qint64 total=0;for(const auto& info:QDir(dir).entryInfoList({"*.blob"},QDir::Files|QDir::Hidden))total+=info.size();
            check(total<=blobLimit-bytes.size(),"Checkpoint blob storage exceeds 64 MiB",ErrorCode::ResourceLimit);write(blob,bytes);}
        return result;
    }
    int find(const QString& id,const QString& key)const{
        const auto snaps=state["snapshots"].toArray();for(int i=snaps.size()-1;i>=0;--i){const auto o=snaps[i].toObject();if(o["message_id"]==id&&o["scope"]==key)return i;}return -1;
    }
    QJsonObject add(const QString& id,const ToolContext& c){
        validId(id);const auto key=scope(c);const int found=find(id,key);if(found>=0)return state["snapshots"].toArray()[found].toObject();
        auto scopes=state["scopes"].toObject();auto owner=scopes[key].toObject();
        if(owner.isEmpty())owner={{"workspace",c.workingDirectory},{"revision",QString::number(c.workspaceRevision)},{"initial",QJsonObject{}}};
        QJsonObject files;const auto initial=owner["initial"].toObject();
        for(auto it=initial.begin();it!=initial.end();++it){c.cancellation.throwIfCancelled();files[it.key()]=capture(it.key(),c);}
        scopes[key]=owner;state["scopes"]=scopes;
        QJsonObject snapshot{{"message_id",id},{"scope",key},{"timestamp",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},{"files",files}};
        auto snaps=state["snapshots"].toArray();snaps.append(snapshot);while(snaps.size()>snapshotLimit)snaps.removeFirst();state["snapshots"]=snaps;
        state["sequence"]=state["sequence"].toInteger()+1;return snapshot;
    }
    void save(const ToolContext& c){
        auto scopes=state["scopes"].toObject();QSet<QString> retained;
        for(const auto& v:state["snapshots"].toArray())retained.insert(v.toObject()["scope"].toString());
        for(auto it=scopes.begin();it!=scopes.end();)if(!retained.contains(it.key()))it=scopes.erase(it);else ++it;
        state["scopes"]=scopes;validate(c);const auto bytes=QJsonDocument(state).toJson(QJsonDocument::Compact);
        check(bytes.size()<=stateLimit,"Checkpoint journal exceeds 16 MiB",ErrorCode::ResourceLimit);c.cancellation.throwIfCancelled();write(dir+"/state.json",bytes);
        // Only delete unreachable content-addressed blobs after the new manifest
        // is durable. First versions remain reachable for older checkpoints.
        QSet<QString> live;auto collect=[&](const QJsonObject& files){for(const auto& v:files)if(v.toObject()["exists"].toBool())live.insert(v.toObject()["sha256"].toString()+".blob");};
        for(const auto& o:scopes)collect(o.toObject()["initial"].toObject());for(const auto& o:state["snapshots"].toArray())collect(o.toObject()["files"].toObject());
        for(const auto& name:QDir(dir).entryList({"*.blob"},QDir::Files|QDir::NoSymLinks))if(!live.contains(name)&&digest(name.chopped(5)))QFile::remove(dir+'/'+name);
    }
};
}
FileCheckpoints::FileCheckpoints(QString directory):directory_(QDir::cleanPath(QFileInfo(directory).absoluteFilePath())){
    check(!directory.trimmed().isEmpty(),"Checkpoint directory is required",ErrorCode::InvalidArgument);plainPath(directory_);
}
QJsonObject FileCheckpoints::checkpoint(const QString& id,const ToolContext& c)const{
    Journal journal(directory_,c);const auto value=journal.add(id,c);journal.save(c);return {{"message_id",value["message_id"]},{"timestamp",value["timestamp"]},{"tracked_files",value["files"].toObject().size()}};
}
void FileCheckpoints::track(const QString& path,const std::optional<QByteArray>& before,const ToolContext& c)const{
    Journal j(directory_,c);target(path,c,directory_);check(before?read(path,fileLimit)==*before:!QFileInfo::exists(path),"File changed before checkpointing");
    const auto id=c.fileCheckpointId.isEmpty()?uuid():c.fileCheckpointId,key=scope(c);j.add(id,c);
    auto scopes=j.state["scopes"].toObject();auto owner=scopes[key].toObject(),initial=owner["initial"].toObject();
    auto snaps=j.state["snapshots"].toArray();const int index=j.find(id,key);auto snap=snaps[index].toObject(),files=snap["files"].toObject();
    if(!files.contains(path)){const auto backup=j.capture(path,c);if(!initial.contains(path))initial[path]=backup;files[path]=backup;}
    owner["initial"]=initial;scopes[key]=owner;j.state["scopes"]=scopes;snap["files"]=files;snaps[index]=snap;j.state["snapshots"]=snaps;j.save(c);
}
QJsonObject FileCheckpoints::list(const ToolContext& c)const{
    Journal j(directory_,c);QJsonArray result;const auto key=scope(c);
    int tracked=0;for(const auto& value:j.state["scopes"].toObject())tracked+=value.toObject()["initial"].toObject().size();
    for(const auto& v:j.state["snapshots"].toArray()){const auto o=v.toObject();if(o["scope"]==key)result.append(QJsonObject{{"message_id",o["message_id"]},{"timestamp",o["timestamp"]},{"tracked_files",o["files"].toObject().size()}});}
    return {{"enabled",true},{"snapshots",result},{"sequence",j.state["sequence"]},{"total_tracked_files",tracked},{"max_snapshots",snapshotLimit},{"workspace",c.workingDirectory},{"workspace_revision",QString::number(c.workspaceRevision)}};
}
QJsonObject FileCheckpoints::rewind(const QString& id,bool dryRun,const ToolContext& c,const QString& expected)const{
    validId(id);Journal j(directory_,c);const auto key=scope(c);const int index=j.find(id,key);
    check(index>=0,"Checkpoint not found in the current workspace",ErrorCode::NotFound);
    const auto snap=j.state["snapshots"].toArray()[index].toObject(),initial=j.state["scopes"].toObject()[key].toObject()["initial"].toObject(),files=snap["files"].toObject();
    struct Change {QString path;QJsonObject before,after;QByteArray bytes;};QList<Change> pending;QJsonArray changes,names,identities;
    for(auto it=initial.begin();it!=initial.end();++it){
        c.cancellation.throwIfCancelled();const auto path=it.key();target(path,c,directory_);const auto after=files.contains(path)?files[path].toObject():it.value().toObject();
        const auto before=current(path);QByteArray bytes;
        if(after["exists"].toBool()){bytes=read(j.dir+'/'+after["sha256"].toString()+".blob",fileLimit);check(bytes.size()==after["bytes"].toInteger()&&hash(bytes)==after["sha256"].toString(),"Checkpoint blob digest mismatch",ErrorCode::ProtocolError);}
        identities.append(QJsonObject{{"path",path},{"before",before},{"after",after}});
        if(before==after)continue;
        pending.append({path,before,after,bytes});names.append(path);
        changes.append(QJsonObject{{"path",path},{"operation",!after["exists"].toBool()?"delete":!before["exists"].toBool()?"create":"modify"},{"before_bytes",before["bytes"].toInteger()},{"after_bytes",after["bytes"].toInteger()}});
    }
    const auto fingerprint=hash(QJsonDocument(QJsonObject{{"checkpoint",id},{"scope",key},{"files",identities}}).toJson(QJsonDocument::Compact));
    check(expected.isEmpty()||expected==fingerprint,"Files changed after rewind preview",ErrorCode::InvalidArgument);
    QJsonObject result{{"canRewind",true},{"message_id",id},{"dryRun",dryRun},{"complete",true},{"filesChanged",names},{"changes",changes},{"fingerprint",fingerprint},{"filesRestored",QJsonArray{}}};
    check(QJsonDocument(result).toJson(QJsonDocument::Compact).size()<=512*1024,"Checkpoint preview exceeds 512 KiB",ErrorCode::ResourceLimit);
    if(dryRun)return result;
    QJsonArray restored,errors;
    for(const auto& item:pending){try{
        c.cancellation.throwIfCancelled();target(item.path,c,directory_);check(current(item.path)==item.before,"File changed while rewinding");
        if(item.after["exists"].toBool()){
            check(QDir().mkpath(QFileInfo(item.path).absolutePath()),"Cannot create restore directory");target(item.path,c,directory_);
            write(item.path,item.bytes,QFileDevice::Permissions(item.after["permissions"].toInt()));
        }else check(QFile::remove(item.path),"Cannot remove file created after checkpoint");
        restored.append(item.path);
    }catch(const Error& e){errors.append(QJsonObject{{"path",item.path},{"error",QString::fromUtf8(e.what())},{"cancelled",e.code()==ErrorCode::Cancelled}});break;}}
    result["filesRestored"]=restored;result["complete"]=errors.isEmpty();result["errors"]=errors;return result;
}
}
