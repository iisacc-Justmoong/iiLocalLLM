#include "AgentProfiles.h"
#include "Frontmatter.h"
#include "ContextFile.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDirIterator>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <cmath>
#include <algorithm>
namespace iiLocalLLM::agent {
namespace {
void require(bool ok,const QString& message,ErrorCode code=ErrorCode::InvalidArgument){if(!ok)throw Error(code,message);}
bool inside(const QString& path,const QString& root){return path==root||path.startsWith(root+'/');}
QString name(const QJsonValue& value){
    const auto s=value.toString();static const QRegularExpression valid("\\A[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}\\z");
    require(value.isString()&&valid.match(s).hasMatch(),"Invalid agent profile name");return s;
}
QString text(const QJsonObject& object,const QString& key,int limit,bool required=false){
    if(!object.contains(key)){require(!required,"Missing agent profile field: "+key);return {};}
    const auto value=object[key];require(value.isString()&&value.toString().size()<=limit&&!value.toString().contains(QChar::Null)
        &&(!required||!value.toString().trimmed().isEmpty()),"Invalid agent profile field: "+key);return value.toString();
}
bool flag(const QJsonObject& o,const QString& key){
    if(!o.contains(key))return false;const auto v=o[key];if(v.isBool())return v.toBool();
    require(v=="true"||v=="false","Agent profile flag must be boolean: "+key);return v=="true";
}
QStringList list(const QJsonValue& value,bool tools){
    QStringList result;
    if(value.isUndefined())return tools?QStringList{"*"}:QStringList{};
    if(value.isNull())return {};
    if(value.isString())result=value.toString().split(QRegularExpression("[,\\s]+"),Qt::SkipEmptyParts);
    else {require(value.isArray(),"Agent profile list must be a string or array");for(const auto& item:value.toArray()){
        require(item.isString(),"Agent profile list must contain strings");const auto s=item.toString().trimmed();if(!s.isEmpty())result.append(s);
    }}
    require(result.size()<=256,"Agent profile list exceeds limit",ErrorCode::ResourceLimit);
    for(const auto& item:result)require(item.size()<=256&&!item.contains(QChar::Null),"Invalid agent profile list item");
    if(tools&&result.contains("*"))return {"*"};result.removeDuplicates();return result;
}
SubagentDefinition parse(QString agentName,const QJsonObject& fields,const QString& body,bool json){
    SubagentDefinition p;p.name=name(agentName);p.description=text(fields,"description",4096,true);
    p.systemPrompt=json?text(fields,"prompt",1024*1024,true):body.trimmed();require(!p.systemPrompt.isEmpty(),"Agent profile prompt is empty");
    p.model=text(fields,"model",256).trimmed();if(p.model.compare("inherit",Qt::CaseInsensitive)==0)p.model.clear();
    p.tools=list(fields.value("tools"),true);p.disallowedTools=fields.contains("disallowedTools")?list(fields["disallowedTools"],true):QStringList{};
    p.skills=list(fields.value("skills"),false);p.initialPrompt=text(fields,"initialPrompt",65536);p.background=flag(fields,"background");
    p.readOnly=flag(fields,"readOnly");p.permissionMode=text(fields,"permissionMode",64);
    require(p.permissionMode.isEmpty()||QStringList{"default","inherit","acceptEdits","dontAsk","bypassPermissions","plan","auto"}.contains(p.permissionMode),"Unknown agent permission mode");
    if(p.permissionMode=="auto")p.unsupportedFeatures.append("permissionMode:auto");
    if(fields.contains("maxTurns")){
        const auto v=fields["maxTurns"];bool ok=false;double n=v.isString()?v.toString().toDouble(&ok):v.toDouble(-1);ok|=v.isDouble();
        require(ok&&std::isfinite(n)&&std::floor(n)==n&&n>=1&&n<=10000,"Invalid agent maxTurns");p.maxTurns=int(n);
    }
    const QSet<QString> supported{"name","description","prompt","tools","disallowedTools","model","skills","initialPrompt","background","maxTurns","permissionMode","readOnly","color"};
    for(auto i=fields.begin();i!=fields.end();++i)if(!supported.contains(i.key())){p.metadata[i.key()]=i.value();p.unsupportedFeatures.append(i.key());}
    if(fields.contains("color"))p.metadata["color"]=text(fields,"color",64);
    for(const auto& pattern:p.tools+p.disallowedTools)if(pattern.contains('(')||pattern.contains(')'))p.unsupportedFeatures.append("tool-argument-rules");
    p.unsupportedFeatures.removeDuplicates();return p;
}
QList<SubagentDefinition> builtins(){
    SubagentDefinition general;general.source="built-in";
    general.systemPrompt="Carry out the delegated task with the available tools. Report observed results and unresolved limitations.";
    auto explore=general;explore.name="Explore";explore.description="Find and inspect files, symbols and available application data without modifying them.";
    explore.systemPrompt="Investigate the requested workspace or application information using read-only tools. Ground findings in observed paths and results.";explore.readOnly=true;
    auto plan=explore;plan.name="Plan";plan.description="Inspect the current implementation and propose an actionable implementation plan.";
    plan.systemPrompt="Read relevant source and constraints, then propose concrete changes and validation steps. Do not modify files or application state.";plan.permissionMode="plan";
    return {general,explore,plan};
}
QString fileIdentity(const QString& path){
#ifdef Q_OS_UNIX
    struct stat info{};if(::stat(QFile::encodeName(path).constData(),&info)==0)return QString::number(qulonglong(info.st_dev))+':'+QString::number(qulonglong(info.st_ino));
#endif
    return path;
}
}
QJsonObject SubagentDefinition::toJson(bool includePrompt) const {
    QJsonObject result{{"name",name},{"description",description},{"model",model},{"tools",QJsonArray::fromStringList(tools)},
        {"disallowed_tools",QJsonArray::fromStringList(disallowedTools)},{"read_only",readOnly},{"max_turns",maxTurns},
        {"skills",QJsonArray::fromStringList(skills)},{"background",background},{"permission_mode",permissionMode},
        {"source",source},{"path",path},{"directory",directory},{"sha256",sha256},{"unsupported_features",QJsonArray::fromStringList(unsupportedFeatures)}};
    if(metadata.contains("color"))result["color"]=metadata["color"];
    // Unimplemented hooks/MCP fields may contain private host values. Only the
    // private persisted execution record includes them, never the model catalog.
    if(includePrompt){result["system_prompt"]=systemPrompt;result["initial_prompt"]=initialPrompt;result["metadata"]=metadata;}
    return result;
}
const SubagentDefinition& AgentProfileCatalog::find(const QString& name) const {
    for(const auto& profile:profiles)if(profile.name==name)return profile;
    throw Error(ErrorCode::NotFound,"Unknown or invalid agent profile: "+name);
}
QJsonObject AgentProfileCatalog::toJson() const {
    QJsonArray entries;for(const auto& p:profiles)entries.append(p.toJson());return {{"profiles",entries},{"shadowed",shadowed},{"failed_files",failedFiles}};
}
AgentProfileCatalog discoverAgentProfiles(const QString& workspace,const AgentProfileOptions& options,const QList<SubagentDefinition>& host,const CancellationToken& token){
    token.throwIfCancelled();AgentProfileCatalog catalog;
    if(!options.enabled){catalog.profiles=host;if(catalog.profiles.isEmpty())catalog.profiles.append(SubagentDefinition{});return catalog;}
    require(options.maxFileBytes>0&&options.maxFileBytes<=1024*1024&&options.maxTotalBytes>0&&options.maxTotalBytes<=16*1024*1024
        &&options.maxProfiles>=1&&options.maxProfiles<=1024&&options.maxScannedEntries>=1&&options.maxScannedEntries<=65536
        &&options.directories.size()+options.pluginDirectories.size()<=64&&host.size()<=128&&options.overrides.size()<=128,"Invalid agent profile discovery limits");
    const auto cwd=QFileInfo(workspace).canonicalFilePath();require(!cwd.isEmpty()&&QFileInfo(cwd).isDir()&&!QDir(cwd).isRoot(),"Invalid agent profile workspace");
    QSet<QString> names,roots,files;int scanned=0; qint64 total=0;
    auto add=[&](SubagentDefinition profile){
        name(profile.name);
        if(names.contains(profile.name)){catalog.shadowed.append(QJsonObject{{"name",profile.name},{"source",profile.source},{"path",profile.path}});return;}
        require(catalog.profiles.size()<options.maxProfiles,"Agent profile count exceeds limit",ErrorCode::ResourceLimit);
        names.insert(profile.name);catalog.profiles.append(std::move(profile));
    };
    auto scan=[&](const QString& input,const QString& source,const QString& confinement=QString()){
        if(input.isEmpty())return;
        require(input.size()<=4096&&!input.contains(QChar::Null)&&!input.startsWith('~')&&!input.contains("://"),"Invalid agent profile directory");
        QFileInfo info(QDir::isAbsolutePath(input)?input:QDir(cwd).filePath(input));if(!info.exists()&&!info.isSymLink())return;
        const auto root=info.canonicalFilePath();require(!root.isEmpty()&&info.isDir()&&!QDir(root).isRoot(),"Agent profiles path must be a directory");
        require(confinement.isEmpty()||inside(root,confinement),"Project agent directory escapes its scope");if(roots.contains(root))return;roots.insert(root);
        QDirIterator it(root,QDir::Files|QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot,QDirIterator::Subdirectories);QStringList candidates;
        while(it.hasNext()){token.throwIfCancelled();require(++scanned<=options.maxScannedEntries,"Agent scan exceeds entry limit",ErrorCode::ResourceLimit);const auto path=it.next();if(path.endsWith(".md")&&!QFileInfo(path).isDir())candidates.append(path);}
        candidates.sort(Qt::CaseSensitive);
        for(const auto& candidate:candidates){
            token.throwIfCancelled();const auto path=QFileInfo(candidate).canonicalFilePath();QByteArray bytes;
            try {
                require(!path.isEmpty()&&inside(path,root),"Agent profile symlink escapes its directory");
                const auto identity=fileIdentity(path);if(files.contains(identity)){catalog.shadowed.append(QJsonObject{{"path",path},{"source",source},{"reason","duplicate_file"}});continue;}
                bytes=detail::readContextFile(root,path,options.maxFileBytes,token);files.insert(identity);
            }catch(const Error& e){if(e.code()==ErrorCode::Cancelled)throw;catalog.failedFiles.append(QJsonObject{{"path",candidate},{"source",source},{"error",QString::fromUtf8(e.what())}});continue;}
            total+=bytes.size();require(total<=options.maxTotalBytes,"Agent profiles exceed total byte limit",ErrorCode::ResourceLimit);QString claimed;
            try {
                const auto document=detail::readFrontmatter(bytes,token);if(!document.fields.contains("name"))continue;
                claimed=name(document.fields["name"]);
                if(names.contains(claimed)){catalog.shadowed.append(QJsonObject{{"name",claimed},{"path",path},{"source",source}});continue;}
                auto profile=parse(claimed,document.fields,document.body,false);profile.path=path;profile.directory=QFileInfo(path).absolutePath();profile.source=source;
                profile.sha256=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());add(std::move(profile));
            }catch(const Error& e){
                if(e.code()==ErrorCode::Cancelled||e.code()==ErrorCode::ResourceLimit)throw;
                if(!claimed.isEmpty())names.insert(claimed); // A broken winning definition must not reveal a weaker fallback.
                catalog.failedFiles.append(QJsonObject{{"name",claimed},{"path",path},{"source",source},{"error",QString::fromUtf8(e.what())}});
            }
        }
    };
    scan(options.managedDirectory,"managed");
    const auto overrideBytes=QJsonDocument(options.overrides).toJson(QJsonDocument::Compact).size();
    total+=options.overrides.isEmpty()?0:overrideBytes;require(total<=options.maxTotalBytes,"Agent JSON overrides exceed total byte limit",ErrorCode::ResourceLimit);
    for(auto it=options.overrides.begin();it!=options.overrides.end();++it){
        token.throwIfCancelled();name(it.key());
        if(names.contains(it.key())){catalog.shadowed.append(QJsonObject{{"name",it.key()},{"source","flag"}});continue;}
        require(it.value().isObject(),"JSON agent definition must be an object");auto p=parse(it.key(),it.value().toObject(),{},true);p.source="flag";
        p.sha256=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(it.value().toObject()).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());add(std::move(p));
    }
    for(auto p:host){p.source="host";add(std::move(p));}
    for(const auto& dir:options.directories)scan(dir,"flag");
    if(options.includeProject){
        QString boundary;if(!options.projectBoundary.isEmpty()){boundary=QFileInfo(options.projectBoundary).canonicalFilePath();require(!boundary.isEmpty()&&inside(cwd,boundary),"Agent project boundary must contain the workspace");}
        QString current=cwd;int depth=0;
        for(;;){
            require(++depth<=128,"Agent project hierarchy exceeds limit",ErrorCode::ResourceLimit);scan(QDir(current).filePath(".claude/agents"),"project",current);
            if((!boundary.isEmpty()&&current==boundary)||(boundary.isEmpty()&&(QFileInfo::exists(QDir(current).filePath(".git"))||current==QDir::homePath()))||QDir(current).isRoot())break;
            current=QFileInfo(QDir(current).filePath("..")).canonicalFilePath();require(!current.isEmpty(),"Cannot resolve agent project ancestor");
        }
    }
    scan(options.userDirectory,"user");for(const auto& dir:options.pluginDirectories)scan(dir,"plugin");
    if(options.includeBuiltins)for(auto p:builtins())add(std::move(p));
    return catalog;
}
}
