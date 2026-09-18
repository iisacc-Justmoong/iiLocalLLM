#include "PluginRuntime.h"
#include "Engine.h"
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSet>

namespace iiLocalLLM::agent {
namespace {
void require(bool ok,const QString& text){if(!ok)throw Error(ErrorCode::InvalidArgument,text);}
bool inside(const QString& p,const QString& root){return p==root||p.startsWith(root+'/');}
}
PluginRuntime::PluginRuntime(PluginSnapshot s,CommandHookOptions options):snapshot(std::move(s)){
    workspace=QFileInfo(options.workingDirectory).canonicalFilePath();
    require(!workspace.isEmpty()&&QFileInfo(workspace).isDir(),"Plugin runtime workspace must exist");
    const auto store=QFileInfo(snapshot.storeDirectory).canonicalFilePath();
    require(!store.isEmpty()&&!inside(store,workspace)&&!inside(workspace,store),"Plugin store and tool workspace must be disjoint");
    for(const auto& hook:snapshot.hooks){
        auto config=options;config.workingDirectory=workspace;
        config.environment.insert("CLAUDE_PLUGIN_ROOT",hook.root);config.environment.insert("CLAUDE_PLUGIN_DATA",hook.dataDirectory);
        callbacks.append(CommandHooks(hook.settings,std::move(config)).callback());
    }
}
void PluginRuntime::attach(EngineOptions& engine,AgentProfileOptions& profiles,McpConnectionOptions& mcp,std::shared_ptr<PluginRuntime> runtime){
    require(bool(runtime)&&!engine.pluginSnapshot,"Plugins must be attached once before constructing the engines");
    require(QFileInfo(mcp.workingDirectory).canonicalFilePath()==runtime->workspace,"Plugin MCP and engine workspace must agree");
    auto e=engine;auto p=profiles;auto m=mcp;
    e.skills.sources.append(runtime->snapshot.skills);p.pluginSources.append(runtime->snapshot.agents);
    e.hooks.append(runtime->callbacks);e.lsp.servers.append(runtime->snapshot.lsp.servers);
    e.lsp.protectedPaths.append(runtime->snapshot.storeDirectory);
    // Validate limits, schemas, collisions and content before changing any caller options.
    (void)discoverSkills(runtime->workspace,e.skills);
    const auto agents=discoverAgentProfiles(runtime->workspace,p);
    for(const auto& failed:agents.failedFiles)
        require(failed.toObject()["source"]!="plugin","A plugin agent profile failed validation during activation");
    Lsp validate(e.lsp);
    for(auto it=runtime->snapshot.mcpServers.begin();it!=runtime->snapshot.mcpServers.end();++it){
        require(!m.inlineServers.contains(it.key()),"Plugin MCP server conflicts with a host definition");
        m.inlineServers[it.key()]=it.value();
        require(!m.serverVariables.contains(it.key()),"Plugin MCP variables conflict with host variables");
        m.serverVariables[it.key()]=runtime->snapshot.mcpVariables.value(it.key());
        m.normalizedNameServers.insert(it.key());
    }
    require(m.inlineServers.size()<=m.maxServers,"Plugin MCP server count exceeds the host limit");
    e.pluginSnapshot=std::make_shared<const PluginSnapshot>(runtime->snapshot);
    engine=std::move(e);profiles=std::move(p);mcp=std::move(m);
}
QJsonObject PluginRuntime::status() const{return snapshot.toJson();}
}
