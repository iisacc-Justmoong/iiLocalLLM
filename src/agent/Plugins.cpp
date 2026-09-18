#include "Plugins.h"
#include "ContextFile.h"
#include "Frontmatter.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDirIterator>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <algorithm>

namespace iiLocalLLM::agent {
namespace {
void require(bool ok, const QString& text, ErrorCode code = ErrorCode::InvalidArgument) { if(!ok) throw Error(code,text); }
bool inside(const QString& path, const QString& root) { return path==root || path.startsWith(root+'/'); }
QString identifier(const QString& s) {
    static const QRegularExpression valid(R"(\A[A-Za-z0-9][A-Za-z0-9_.-]{0,63}\z)");
    require(valid.match(s).hasMatch(),"Invalid local plugin identifier"); return s;
}
QString digest(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()); }
QString field(const QJsonObject& object, const QString& key, int max, bool required = false) {
    if(!object.contains(key)){ require(!required,"Missing plugin field: "+key); return {}; }
    const auto value=object[key];
    require(value.isString() && value.toString().size()<=max && !value.toString().contains(QChar::Null)
        && (!required || !value.toString().trimmed().isEmpty()),"Invalid plugin field: "+key);
    return value.toString();
}
QString boundedPath(const QString& root,const QString& relative,bool required=true) {
    require(!relative.isEmpty() && relative.size()<=4096 && !relative.contains(QChar::Null)
        && !QDir::isAbsolutePath(relative) && !relative.contains(':') && !relative.contains('\\')
        && !relative.split('/').contains(".."),"Plugin paths must stay relative to their package");
    const QFileInfo file(QDir(root).filePath(relative));
    if(!file.exists() && !file.isSymLink()){require(!required,"Plugin component does not exist");return {};}
    const auto path=file.canonicalFilePath();
    require(!file.isSymLink() && !path.isEmpty() && inside(path,root),"Plugin component escapes its package");
    return path;
}
void privateDirectory(const QString& path) {
    require(!QFileInfo(path).isSymLink() && QDir().mkpath(path),"Cannot create plugin directory",ErrorCode::StorageFailure);
    require(QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner),
        "Cannot make plugin directory private",ErrorCode::StorageFailure);
}
QJsonObject jsonFile(const QString& root,const QString& path,const CancellationToken& token={}) {
    const auto bytes=detail::readContextFile(root,path,1024*1024,token);
    QJsonParseError error;const auto doc=QJsonDocument::fromJson(bytes,&error);
    require(error.error==QJsonParseError::NoError && doc.isObject(),"Plugin configuration must be a JSON object");return doc.object();
}
// No symlinks/special files, recursive .git metadata, or inferred dependency downloads.
// Hash paths, entry types, executable bits and bytes with explicit framing.
QString packageTree(const QString& root,const PluginStoreOptions& options,const QString& destination={},const CancellationToken& token={}) {
    QCryptographicHash hash(QCryptographicHash::Sha256); qint64 total=0; int count=0;
    auto frame=[&](const QByteArray& bytes){hash.addData(QByteArray::number(bytes.size())+':');hash.addData(bytes);};
    std::function<void(const QString&)> walk=[&](const QString& relative){
        const QDir dir(QDir(root).filePath(relative));
        for(const auto& file:dir.entryInfoList(QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot,QDir::Name)){
            token.throwIfCancelled(); if(file.fileName()==".git")continue;
            require(++count<=options.maxEntries,"Plugin package entry limit exceeded",ErrorCode::ResourceLimit);
            require(!file.isSymLink() && (file.isFile()||file.isDir()),"Plugin package contains a symlink or special file");
            const auto path=file.canonicalFilePath();require(!path.isEmpty()&&inside(path,root),"Plugin package escapes its source");
            const auto name=relative.isEmpty()?file.fileName():relative+'/'+file.fileName();
            require(name.size()<=4096 && !name.contains(QChar::Null),"Plugin package path exceeds limit");
            frame(name.toUtf8());
            if(file.isDir()){
                frame("directory");
                if(!destination.isEmpty())privateDirectory(QDir(destination).filePath(name));
                walk(name);
            }else{
                const auto bytes=detail::readContextFile(root,path,options.maxFileBytes,token);
                total+=bytes.size();require(total<=options.maxPackageBytes,"Plugin package byte limit exceeded",ErrorCode::ResourceLimit);
                const bool executable=bool(file.permissions()&(QFileDevice::ExeOwner|QFileDevice::ExeGroup|QFileDevice::ExeOther));
                frame(executable?"executable":"file");frame(bytes);
                if(!destination.isEmpty()){
                    QFile output(QDir(destination).filePath(name));
                    require(output.open(QIODevice::WriteOnly|QIODevice::NewOnly)
                        && output.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|(executable?QFileDevice::ExeOwner:QFileDevice::Permissions{}))
                        && output.write(bytes)==bytes.size(),"Cannot cache plugin file",ErrorCode::StorageFailure);
                }
            }
        }
    };
    walk({});return QString::fromLatin1(hash.result().toHex());
}
QJsonObject readState(const PluginStoreOptions& options) {
    const auto path=QDir(options.directory).filePath("installed.json");
    if(!QFileInfo::exists(path)&&!QFileInfo(path).isSymLink())return {};
    const auto state=jsonFile(options.directory,path);
    require(state["schema"]=="iisacc.plugins/1" && state["plugins"].isObject(),"Unsupported plugin state");
    const auto plugins=state["plugins"].toObject();require(plugins.size()<=options.maxPlugins,"Plugin count limit exceeded",ErrorCode::ResourceLimit);
    static const QRegularExpression sha(R"(\A[0-9a-f]{64}\z)");
    for(auto it=plugins.begin();it!=plugins.end();++it){
        identifier(it.key());require(it.value().isObject(),"Invalid installed plugin record");const auto item=it.value().toObject();
        require(item["enabled"].isBool() && item["sha256"].isString() && sha.match(item["sha256"].toString()).hasMatch(),"Invalid installed plugin state");
        field(item,"version",128); // Paths are derived, never read from the state file.
    }
    return plugins;
}
void writeState(const PluginStoreOptions& options,const QJsonObject& plugins) {
    QSaveFile file(QDir(options.directory).filePath("installed.json"));file.setDirectWriteFallback(false);
    const auto bytes=QJsonDocument(QJsonObject{{"schema","iisacc.plugins/1"},{"plugins",plugins}}).toJson();
    require(file.open(QIODevice::WriteOnly)&&file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner)
        &&file.write(bytes)==bytes.size()&&file.commit(),"Cannot commit plugin state",ErrorCode::StorageFailure);
}
struct StoreLock {
    QLockFile lock;
    explicit StoreLock(const QString& root):lock(QDir(root).filePath("plugins.lock")){
        lock.setStaleLockTime(0);require(lock.tryLock(5000),"Plugin store is busy or inaccessible",ErrorCode::AlreadyExists);
    }
};
QString cacheRoot(const PluginStoreOptions& options,const QString& name,const QString& hash){
    return QDir(options.directory).filePath("cache/"+name+'/'+hash);
}
struct Package {
    PluginInfo info;QJsonObject manifest;
};
Package metadata(const QString& root,const QString& data,const CancellationToken& token) {
    Package p;p.info.root=root;p.info.dataDirectory=data;
    const auto manifest=boundedPath(root,".claude-plugin/plugin.json");
    p.manifest=jsonFile(root,manifest,token);p.info.name=identifier(field(p.manifest,"name",64,true));
    p.info.version=field(p.manifest,"version",128);p.info.description=field(p.manifest,"description",4096);
    const QSet<QString> known{"name","version","description","author","homepage","repository","license","keywords",
        "commands","skills","agents","hooks","mcpServers","lspServers","dependencies"};
    for(auto i=p.manifest.begin();i!=p.manifest.end();++i)if(!known.contains(i.key()))p.info.unsupportedFeatures.append(i.key());
    if(p.manifest.contains("dependencies")){
        require(p.manifest["dependencies"].isArray()&&p.manifest["dependencies"].toArray().size()<=64,"Plugin dependencies must be a bounded array");
        for(const auto& v:p.manifest["dependencies"].toArray()){require(v.isString()&&!v.toString().isEmpty()&&v.toString().size()<=256,"Invalid plugin dependency");p.info.dependencies.append(v.toString());}
        p.info.dependencies.removeDuplicates();
    }
    return p;
}
QString expand(const QString& value,const PluginInfo& plugin){
    static const QRegularExpression vars(R"(\$\{CLAUDE_PLUGIN_(ROOT|DATA)\})");
    QString result;qsizetype offset=0;auto matches=vars.globalMatch(value);
    while(matches.hasNext()){const auto m=matches.next();result+=value.mid(offset,m.capturedStart()-offset);
        result+=m.captured(1)=="ROOT"?plugin.root:plugin.dataDirectory;offset=m.capturedEnd();}
    return result+value.mid(offset);
}
QJsonValue expandJson(const QJsonValue& v,const PluginInfo& p,int depth=0){
    require(depth<=32,"Plugin configuration depth exceeds limit",ErrorCode::ResourceLimit);
    if(v.isString())return expand(v.toString(),p);
    if(v.isArray()){QJsonArray a;for(const auto& item:v.toArray())a.append(expandJson(item,p,depth+1));return a;}
    if(v.isObject()){QJsonObject o;const auto input=v.toObject();for(auto i=input.begin();i!=input.end();++i)o[i.key()]=expandJson(i.value(),p,depth+1);return o;}
    return v;
}
QString localName(QString path){path.replace('/',':');for(const auto& part:path.split(':'))identifier(part);return path;}
// Standard component directories are additive with explicit manifest entries.
void compile(Package& p,PluginSnapshot& snapshot,const CancellationToken& token){
    auto& info=p.info;const auto root=info.root;QSet<QString> skills,agents,files;
    QMap<QString,QString> skillAliases,agentAliases;
    QList<SkillSource> skillSources;QList<AgentProfileSource> agentSources;
    int scanned=0;
    auto addDocument=[&](const QString& path,const QString& local,bool agent){
        token.throwIfCancelled();const auto key=(agent?"agent:":"skill:")+path;
        if(files.contains(key))return;files.insert(key);
        const auto bytes=detail::readContextFile(root,path,128*1024,token);
        auto component=local;
        if(agent){
            const auto fields=detail::readFrontmatter(bytes,token).fields;
            if(fields.contains("name")){
                require(fields["name"].isString(),"Plugin agent name must be a string");
                if(!fields["name"].toString().isEmpty())component=local.left(local.lastIndexOf('/')+1)+identifier(fields["name"].toString());
            }
        }
        const auto localId=localName(component),full=info.name+':'+localId;require(full.size()<=128,"Plugin component name exceeds limit");
        auto& names=agent?agents:skills;require(!names.contains(full),"Duplicate plugin component name");names.insert(full);
        if(agent){AgentProfileSource s;s.name=full;s.root=root;s.path=path;s.sha256=digest(bytes);s.pluginRoot=root;s.pluginData=info.dataDirectory;
            agentSources.append(s);agentAliases[localId]=full;
        }else{SkillSource s;s.name=full;s.root=root;s.path=path;s.sha256=digest(bytes);s.pluginRoot=root;s.pluginData=info.dataDirectory;
            skillSources.append(s);skillAliases[localId]=full;}
    };
    auto documents=[&](const QString& path,const QString& kind,const QString& explicitName=QString()){
        const QFileInfo file(path);
        if(file.isFile()){require(path.endsWith(".md"),"Plugin document must be Markdown");
            addDocument(path,explicitName.isEmpty()?file.completeBaseName():explicitName,kind=="agents");return;}
        require(file.isDir(),"Plugin component must be a file or directory");
        if(QFileInfo::exists(QDir(path).filePath("SKILL.md"))&&kind!="agents"){
            addDocument(boundedPath(root,QDir(root).relativeFilePath(QDir(path).filePath("SKILL.md"))),
                explicitName.isEmpty()?file.fileName():explicitName,false);return;
        }
        QDirIterator it(path,QDir::Files|QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot,QDirIterator::Subdirectories);QStringList candidates;
        while(it.hasNext()){token.throwIfCancelled();require(++scanned<=8192,"Plugin component scan exceeds limit",ErrorCode::ResourceLimit);
            const auto candidate=it.next();require(!QFileInfo(candidate).isSymLink(),"Plugin component contains a symlink");
            if(QFileInfo(candidate).isFile()&&candidate.endsWith(".md")&&(kind!="skills"||QFileInfo(candidate).fileName()=="SKILL.md"))candidates.append(candidate);}
        candidates.sort();
        for(const auto& candidate:candidates){
            auto local=QDir(path).relativeFilePath(candidate);
            if(local.endsWith("/SKILL.md"))local.chop(9);else local.chop(3);
            addDocument(candidate,local,kind=="agents");
        }
    };
    for(const QString kind:{"skills","commands","agents"}){
        const auto standard=boundedPath(root,kind,false);if(!standard.isEmpty())documents(standard,kind);
        std::function<void(const QJsonValue&)> add=[&](const QJsonValue& value){
            if(value.isString()){documents(boundedPath(root,value.toString()),kind);return;}
            if(value.isArray()){require(value.toArray().size()<=128,"Too many plugin component paths");for(const auto& v:value.toArray())add(v);return;}
            if(kind=="commands"&&value.isObject()){
                const auto commands=value.toObject();
                for(auto i=commands.begin();i!=commands.end();++i){
                    if(i.value().isString())documents(boundedPath(root,i.value().toString()),kind,identifier(i.key()));
                    else if(i.value().isObject()&&i.value().toObject()["source"].isString()){
                        const auto config=i.value().toObject();documents(boundedPath(root,config["source"].toString()),kind,identifier(i.key()));
                        for(auto c=config.begin();c!=config.end();++c)if(c.key()!="source")info.unsupportedFeatures.append("commands."+i.key()+'.'+c.key());
                    }else info.unsupportedFeatures.append("commands."+i.key());
                }
                return;
            }
            throw Error(ErrorCode::InvalidArgument,"Invalid plugin component paths");
        };
        if(p.manifest.contains(kind))add(p.manifest[kind]);
    }
    for(auto& s:skillSources){s.agentAliases=agentAliases;snapshot.skills.append(s);}
    for(auto& s:agentSources){s.skillAliases=skillAliases;snapshot.agents.append(s);}
    info.contributions={{"skills",skillSources.size()},{"agents",agentSources.size()},{"hooks",0},{"mcp",0},{"lsp",0}};
    QSet<QString> hookFiles;
    std::function<void(const QJsonValue&)> addHooks=[&](const QJsonValue& value){
        if(value.isArray()){require(value.toArray().size()<=128,"Too many plugin hook files");for(const auto& v:value.toArray())addHooks(v);return;}
        QJsonObject settings;
        if(value.isString()){const auto path=boundedPath(root,value.toString());if(hookFiles.contains(path))return;hookFiles.insert(path);settings=jsonFile(root,path,token);}
        else{require(value.isObject(),"Invalid plugin hooks");settings=value.toObject();}
        require(settings["hooks"].isObject(),"Plugin hook configuration requires hooks");
        snapshot.hooks.append({info.name,root,info.dataDirectory,settings});info.contributions["hooks"]=info.contributions["hooks"].toInt()+1;
    };
    if(!boundedPath(root,"hooks/hooks.json",false).isEmpty())addHooks("hooks/hooks.json");
    if(p.manifest.contains("hooks"))addHooks(p.manifest["hooks"]);
    for(const QString kind:{"mcpServers","lspServers"}){
        QJsonObject servers;QSet<QString> sourceFiles;
        std::function<void(const QJsonValue&)> add=[&](const QJsonValue& value){
            if(value.isArray()){require(value.toArray().size()<=128,"Too many plugin server sources");for(const auto& v:value.toArray())add(v);return;}
            QJsonObject input;
            if(value.isString()){
                if(value.toString().contains("://")||value.toString().endsWith(".mcpb")){info.unsupportedFeatures.append(kind+":mcpb-or-url");return;}
                const auto path=boundedPath(root,value.toString());if(sourceFiles.contains(path))return;sourceFiles.insert(path);input=jsonFile(root,path,token);
            }else{require(value.isObject(),"Invalid plugin server configuration");input=value.toObject();}
            if(input.contains(kind)){require(input[kind].isObject(),"Invalid plugin server map");input=input[kind].toObject();}
            for(auto i=input.begin();i!=input.end();++i){identifier(i.key());require(i.value().isObject(),"Invalid plugin server definition");servers[i.key()]=i.value();}
            require(servers.size()<=64,"Too many plugin servers",ErrorCode::ResourceLimit);
        };
        const auto standard=kind=="mcpServers"?QString(".mcp.json"):QString(".lsp.json");
        if(!boundedPath(root,standard,false).isEmpty())add(standard);
        if(p.manifest.contains(kind))add(p.manifest[kind]);
        for(auto i=servers.begin();i!=servers.end();++i){
            auto value=(kind=="mcpServers"?i.value():expandJson(i.value(),info)).toObject();bool supported=true;
            const QSet<QString> allowed=kind=="mcpServers"?QSet<QString>{"command","args","env","cwd","type","url","headers","disabled","alwaysLoad","appId","initializeTimeoutMs","requestTimeoutMs"}
                :QSet<QString>{"command","args","extensionToLanguage","env","initializationOptions","settings","transport"};
            for(auto f=value.begin();f!=value.end();++f)if(!allowed.contains(f.key())){info.unsupportedFeatures.append(kind+'.'+i.key()+'.'+f.key());supported=false;}
            if(kind=="mcpServers"){
                const auto type=value.value("type").toString("stdio");
                if(type!="stdio"&&type!="http"&&type!="streamable-http"){info.unsupportedFeatures.append(kind+'.'+i.key()+":transport");supported=false;}
            }else{
                if(value.contains("transport")&&value["transport"]!="stdio"){info.unsupportedFeatures.append(kind+'.'+i.key()+":transport");supported=false;}
                value.remove("transport");
            }
            if(!supported)continue;
            const auto name=QStringLiteral("plugin:")+info.name+':'+i.key();
            require(name.size()<=128,"Plugin server name exceeds the host limit");
            if(kind=="mcpServers"){
                // Resolve only plugin placeholders here. McpConnections still expands
                // host environment variables without reevaluating replacement values.
                if(!value.contains("type")||value["type"]=="stdio"){
                    require(!value.contains("env")||value["env"].isObject(),"Invalid plugin MCP environment");
                    auto env=value.value("env").toObject();env["CLAUDE_PLUGIN_ROOT"]="${CLAUDE_PLUGIN_ROOT}";env["CLAUDE_PLUGIN_DATA"]="${CLAUDE_PLUGIN_DATA}";value["env"]=env;
                }
                snapshot.mcpVariables[name]={{"CLAUDE_PLUGIN_ROOT",root},{"CLAUDE_PLUGIN_DATA",info.dataDirectory}};
                snapshot.mcpServers[name]=value;info.contributions["mcp"]=info.contributions["mcp"].toInt()+1;
            }else{
                auto env=value.value("env").toObject();env["CLAUDE_PLUGIN_ROOT"]=root;env["CLAUDE_PLUGIN_DATA"]=info.dataDirectory;value["env"]=env;
                const auto lsp=lspOptionsFromJson({{"servers",QJsonObject{{name,value}}}});
                snapshot.lsp.servers.append(lsp.servers);info.contributions["lsp"]=info.contributions["lsp"].toInt()+1;
            }
        }
    }
    info.unsupportedFeatures.removeDuplicates();info.unsupportedFeatures.sort();info.status="configured";
}
}
QJsonObject PluginInfo::toJson(bool paths) const {
    QJsonObject result{{"name",name},{"version",version},{"description",description},{"sha256",sha256},{"enabled",enabled},{"status",status},
        {"dependencies",QJsonArray::fromStringList(dependencies)},{"blocked_by",QJsonArray::fromStringList(blockedBy)},
        {"unsupported_features",QJsonArray::fromStringList(unsupportedFeatures)},{"contributions",contributions}};
    if(paths){result["root"]=root;result["data_directory"]=dataDirectory;}return result;
}
QJsonObject PluginSnapshot::toJson() const {
    QJsonArray items;for(const auto& p:plugins)items.append(p.toJson());
    return {{"schema","iisacc.plugins/1"},{"revision",revision},{"lifecycle","startup_snapshot"},{"reload_requires_restart",true},{"plugins",items}};
}
PluginStore::PluginStore(PluginStoreOptions o):options(std::move(o)){
    require(!options.directory.isEmpty()&&QDir::isAbsolutePath(options.directory)&&!QDir(options.directory).isRoot()
        &&options.maxPlugins>=1&&options.maxPlugins<=256&&options.maxEntries>=1&&options.maxEntries<=65536
        &&options.maxFileBytes>=1&&options.maxFileBytes<=64*1024*1024&&options.maxPackageBytes>=1&&options.maxPackageBytes<=1024LL*1024*1024,"Invalid plugin store options");
    privateDirectory(options.directory);options.directory=QFileInfo(options.directory).canonicalFilePath();
    privateDirectory(QDir(options.directory).filePath("cache"));privateDirectory(QDir(options.directory).filePath("data"));
}
QString PluginStore::directory() const{return options.directory;}
PluginInfo PluginStore::install(const QString& input,bool enabled,const CancellationToken& token){
    token.throwIfCancelled();const QFileInfo source(input);const auto root=source.canonicalFilePath();
    require(!root.isEmpty()&&source.isDir()&&!source.isSymLink()&&!QDir(root).isRoot()
        &&!inside(root,options.directory)&&!inside(options.directory,root),"Plugin source and store must be separate directories");
    StoreLock lock(options.directory);auto state=readState(options);
    QTemporaryDir staging(QDir(options.directory).filePath(".install-XXXXXX"));require(staging.isValid(),"Cannot stage plugin package",ErrorCode::StorageFailure);
    const auto hash=packageTree(root,options,staging.path(),token);
    auto package=metadata(staging.path(),{},token);PluginSnapshot validate;compile(package,validate,token);
    SkillOptions skills;skills.includeProject=false;skills.sources=validate.skills;
    (void)discoverSkills(staging.path(),skills,token);
    AgentProfileOptions agents;agents.includeProject=false;agents.includeBuiltins=false;agents.pluginSources=validate.agents;
    require(discoverAgentProfiles(staging.path(),agents,{},token).failedFiles.isEmpty(),"Invalid plugin agent profile");
    require(state.contains(package.info.name)||state.size()<options.maxPlugins,"Installed plugin count exceeded",ErrorCode::ResourceLimit);
    const auto target=cacheRoot(options,package.info.name,hash);privateDirectory(QFileInfo(target).absolutePath());
    if(QFileInfo::exists(target)){
        require(!QFileInfo(target).isSymLink()&&packageTree(target,options,{},token)==hash,"Existing plugin cache was modified",ErrorCode::StorageFailure);
    }else{
        require(QDir().rename(staging.path(),target),"Cannot publish plugin cache",ErrorCode::StorageFailure);staging.setAutoRemove(false);
    }
    const auto data=QDir(options.directory).filePath("data/"+package.info.name);privateDirectory(data);
    state[package.info.name]=QJsonObject{{"version",package.info.version},{"sha256",hash},{"enabled",enabled}};
    token.throwIfCancelled();writeState(options,state);
    package.info.sha256=hash;package.info.root=target;package.info.dataDirectory=data;package.info.enabled=enabled;package.info.status=enabled?"selected":"disabled";return package.info;
}
void PluginStore::setEnabled(const QString& input,bool enabled){
    const auto name=identifier(input);StoreLock lock(options.directory);auto state=readState(options);
    require(state.contains(name),"Plugin is not installed",ErrorCode::NotFound);auto record=state[name].toObject();record["enabled"]=enabled;state[name]=record;writeState(options,state);
}
void PluginStore::uninstall(const QString& input){
    const auto name=identifier(input);StoreLock lock(options.directory);auto state=readState(options);
    require(state.contains(name),"Plugin is not installed",ErrorCode::NotFound);state.remove(name);writeState(options,state);
}
QJsonArray PluginStore::list() const {
    StoreLock lock(options.directory);const auto state=readState(options);QJsonArray result;
    for(auto i=state.begin();i!=state.end();++i){auto item=i.value().toObject();item["name"]=i.key();result.append(item);}return result;
}
PluginSnapshot PluginStore::snapshot(const CancellationToken& token) const {
    StoreLock lock(options.directory);const auto state=readState(options);PluginSnapshot result;result.storeDirectory=options.directory;
    result.revision=digest(QJsonDocument(state).toJson(QJsonDocument::Compact));QMap<QString,Package> packages;
    for(auto it=state.begin();it!=state.end();++it){
        token.throwIfCancelled();const auto record=it.value().toObject();const auto hash=record["sha256"].toString();const auto root=cacheRoot(options,it.key(),hash);
        Package p;
        if(record["enabled"].toBool()){
            require(!QFileInfo(root).isSymLink()&&QFileInfo(root).isDir()&&packageTree(root,options,{},token)==hash,"Plugin cache integrity check failed",ErrorCode::StorageFailure);
            p=metadata(root,QDir(options.directory).filePath("data/"+it.key()),token);
            require(QFileInfo(p.info.dataDirectory).isDir()&&!QFileInfo(p.info.dataDirectory).isSymLink()
                &&QFileInfo(p.info.dataDirectory).canonicalFilePath()==p.info.dataDirectory,"Plugin data directory was replaced",ErrorCode::StorageFailure);
            require(p.info.name==it.key()&&p.info.version==record["version"].toString(),"Plugin identity does not match installed state",ErrorCode::StorageFailure);
        }else{p.info.name=it.key();p.info.version=record["version"].toString();p.info.status="disabled";}
        p.info.enabled=record["enabled"].toBool();p.info.sha256=hash;packages[it.key()]=p;
    }
    QSet<QString> visiting,finished;
    std::function<bool(const QString&)> load=[&](const QString& name){
        if(!packages.contains(name)||!packages[name].info.enabled)return false;
        auto& p=packages[name];
        if(finished.contains(name))return p.info.status=="configured";
        if(visiting.contains(name))return false;
        visiting.insert(name);
        for(const auto& dependency:p.info.dependencies)if(!load(dependency))p.info.blockedBy.append(dependency);
        visiting.remove(name);finished.insert(name);
        if(!p.info.blockedBy.isEmpty()){p.info.status="blocked";return false;}
        compile(p,result,token);return true;
    };
    for(auto i=packages.begin();i!=packages.end();++i)load(i.key());
    for(auto i=packages.begin();i!=packages.end();++i)result.plugins.append(i.value().info);
    return result;
}
}
