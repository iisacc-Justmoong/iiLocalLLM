#include "Tools.h"
#include "PlanMode.h"
#include "McpResult.h"
#include "PermissionResponses.h"
#include "PermissionRequests.h"
#include "PermissionRulesInternal.h"
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QUuid>
#include <map>
#include <shared_mutex>
#include <mutex>
#include <future>

namespace iiLocalLLM::agent {
namespace {
using Schema = jsoncons::jsonschema::json_schema<jsoncons::json>;
jsoncons::json convert(const QJsonObject& o) {
    return jsoncons::json::parse(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString());
}
std::shared_ptr<Schema> compile(QJsonObject o) {
    if (!o.contains("$schema")) o.insert("$schema", "https://json-schema.org/draft/2020-12/schema");
    try { return std::make_shared<Schema>(jsoncons::jsonschema::make_json_schema(convert(o))); }
    catch (const std::exception& e) { throw Error(ErrorCode::InvalidArgument, "Invalid tool schema: " + QString::fromUtf8(e.what())); }
}
void event(const EventCallback& cb, EventKind kind, const ToolContext& c, const ToolCall& call, QString text = {}, QJsonObject data = {}) {
    if (!cb) return;
    try { cb({kind, c.runId, c.sessionId, call.id, std::move(text), std::move(data)}); }
    catch (...) { throw Error(ErrorCode::ConsumerFailure, "Agent event consumer threw an exception"); }
}
}
struct ToolRegistry::Entry {
    Tool tool;
    std::shared_ptr<Schema> input, output;
    void validateInput(const QJsonObject& value) const {
        if (!input->is_valid(convert(value))) throw Error(ErrorCode::InvalidArgument, "Tool arguments do not satisfy inputSchema: " + tool.definition.name);
    }
    void validateOutput(const QJsonObject& value) const {
        if (output && !output->is_valid(convert(value))) throw Error(ErrorCode::ProtocolError, "Tool result does not satisfy outputSchema: " + tool.definition.name);
    }
};
class ToolRegistry::Impl {
public:
    mutable std::shared_mutex mutex;
    std::map<QString, std::shared_ptr<const Entry>> tools;
};
ToolRegistry::ToolRegistry() : d(std::make_unique<Impl>()) {}
ToolRegistry::~ToolRegistry() = default;
void ToolRegistry::add(Tool tool) {
    static const QRegularExpression name("^[A-Za-z0-9_.:-]{1,128}$");
    if (!name.match(tool.definition.name).hasMatch() || (!tool.execute && !tool.prepare))
        throw Error(ErrorCode::InvalidArgument, "Tool name and implementation are required");
    if (tool.definition.inputSchema.isEmpty()) tool.definition.inputSchema = {{"type", "object"}};
    if (QJsonDocument(tool.definition.inputSchema).toJson().size() > 1024 * 1024
        || QJsonDocument(tool.definition.outputSchema).toJson().size() > 1024 * 1024)
        throw Error(ErrorCode::ResourceLimit, "Tool schema exceeds 1 MiB");
    auto entry = std::make_shared<Entry>(Entry{tool, compile(tool.definition.inputSchema), {}});
    if (!tool.definition.outputSchema.isEmpty()) entry->output = compile(tool.definition.outputSchema);
    std::unique_lock lock(d->mutex);
    if (!d->tools.emplace(tool.definition.name, std::move(entry)).second)
        throw Error(ErrorCode::AlreadyExists, "Tool already registered: " + tool.definition.name);
}
void ToolRegistry::remove(const QString& name) { std::unique_lock lock(d->mutex); d->tools.erase(name); }
void ToolRegistry::replace(const QStringList& names, QList<Tool> tools) {
    ToolRegistry additions;
    for (auto& tool : tools) additions.add(std::move(tool));
    std::map<QString, std::shared_ptr<const Entry>> replacement;
    {
        std::unique_lock lock(d->mutex);
        replacement = d->tools;
        for (const auto& name : names) replacement.erase(name);
        for (const auto& [name, entry] : additions.d->tools)
            if (!replacement.emplace(name, entry).second)
                throw Error(ErrorCode::AlreadyExists, "Tool already registered: " + name);
        d->tools.swap(replacement);
    } // Release removed handlers outside the registry lock.
}
QList<ToolDefinition> ToolRegistry::definitions(bool all) const {
    std::shared_lock lock(d->mutex); QList<ToolDefinition> out;
    for (const auto& [name, entry] : d->tools) if (all || !entry->tool.definition.deferred) out.append(entry->tool.definition);
    return out;
}
std::shared_ptr<const ToolRegistry::Entry> ToolRegistry::resolve(const QString& name) const {
    std::shared_lock lock(d->mutex);
    const auto it = d->tools.find(name);
    if (it == d->tools.end()) throw Error(ErrorCode::NotFound, "Unknown tool: " + name);
    return it->second;
}
Tool ToolRegistry::get(const QString& name) const { return resolve(name)->tool; }
std::shared_ptr<ToolRegistry> ToolRegistry::snapshot() const {
    auto result = std::make_shared<ToolRegistry>();
    std::shared_lock lock(d->mutex); result->d->tools = d->tools;
    return result;
}
void ToolRegistry::validateInput(const QString& name, const QJsonObject& input) const {
    resolve(name)->validateInput(input);
}
void ToolRegistry::validateOutput(const QString& name, const QJsonObject& output) const {
    resolve(name)->validateOutput(output);
}
QStringList PermissionPolicy::workingDirectories(const ToolContext& context) const {
    context.cancellation.throwIfCancelled();QStringList paths;
    if(!context.workingDirectory.isEmpty()) {
        paths.append(QDir::cleanPath(QFileInfo(context.workingDirectory).absoluteFilePath()));
        const auto canonical=QFileInfo(context.workingDirectory).canonicalFilePath();
        if(!canonical.isEmpty())paths.append(canonical);
    }
    paths.removeDuplicates();return paths;
}
RulePolicy::RulePolicy(PermissionMode mode, QList<PermissionRule> rules) : mode_(mode) {
    QStringList all;
    for (const auto& rule : rules) { for (const auto& pattern : parsePermissionRules({rule.toolPattern})) { auto item=rule;item.toolPattern=pattern;rules_.append(item); all.append(pattern); } }
    parsePermissionRules(all);
}
QJsonObject RulePolicy::describe(const ToolContext& context) const {
    context.cancellation.throwIfCancelled();
    const auto effective=context.permissionMode.value_or(mode_);
    const QString mode=effective==PermissionMode::AcceptEdits?"acceptEdits":effective==PermissionMode::DontAsk?"dontAsk"
        :effective==PermissionMode::Bypass?"bypassPermissions":effective==PermissionMode::Plan?"plan":"default";
    QJsonArray rules;
    for(const auto& rule:rules_)rules.append(QJsonObject{{"rule",rule.toolPattern},{"behavior",rule.behavior==PermissionBehavior::Allow?"allow":rule.behavior==PermissionBehavior::Deny?"deny":"ask"},
        {"source",rule.source.isEmpty()?QString("host"):rule.source},{"root_directory",rule.rootDirectory},{"settings_syntax",rule.settingsSyntax}});
    return {{"provider","rules"},{"mode",mode},{"rules",rules},{"inspection_supported",true},
        {"working_directories",QJsonArray::fromStringList(workingDirectories(context))}};
}
PermissionDecision RulePolicy::decide(const ToolDefinition& tool, const QJsonObject& args, const ToolContext& context) const {
    const auto mode=context.permissionMode.value_or(mode_);
    std::optional<PermissionBehavior> matched;
    QList<PermissionRule> denies, asks, allows;
    for(const auto& rule:parsePermissionRules(context.allowedTools)) allows.append({rule,PermissionBehavior::Allow});
    for (const auto& rule : rules_) {
        if (rule.behavior == PermissionBehavior::Deny) denies.append(rule);
        else if (rule.behavior == PermissionBehavior::Ask) asks.append(rule);
        else allows.append(rule);
    }
    if (detail::permissionRulesMatch(denies, tool, args, context, false)) return {PermissionBehavior::Deny, "Explicit tool deny rule"};
    const bool taskState = tool.metadata["source"] == "builtin.task"
        && QStringList{"TaskCreate", "TaskGet", "TaskList", "TaskUpdate", "TaskClaim", "TodoWrite", "TodoRead"}.contains(tool.name);
    const bool stopOwnShell = tool.name == "TaskStop" && tool.metadata["source"] == "builtin.shell.control";
    const bool memoryControl=tool.metadata["source"]=="builtin.memory.control"&&QStringList{
        "iiLocalLLM.agent.memory.extract","iiLocalLLM.agent.memory.extraction.status","iiLocalLLM.agent.memory.extraction.cancel",
        "iiLocalLLM.agent.memory.dream","iiLocalLLM.agent.memory.dream.status","iiLocalLLM.agent.memory.dream.cancel"}.contains(tool.name);
    const bool planControl=tool.metadata["source"]=="builtin.plan"&&QStringList{"EnterPlanMode","ExitPlanMode"}.contains(tool.name);
    const bool engineControl=(tool.metadata["source"]=="builtin.agent.control"&&QStringList{"iiLocalLLM.agent.run","iiLocalLLM.agent.compact","iiLocalLLM.agent.inputs.run"}.contains(tool.name))
        ||(tool.metadata["source"]=="builtin.session.control"&&tool.name=="iiLocalLLM.agent.clear")
        ||(tool.metadata["source"]=="builtin.input.control"&&QStringList{"iiLocalLLM.agent.inputs.enqueue","iiLocalLLM.agent.inputs.remove"}.contains(tool.name));
    const bool ownPlan=!context.planFilePath.isEmpty()&&context.planModeActive&&tool.metadata["source"]=="builtin.workspace"
        &&QStringList{"Write","Edit"}.contains(tool.name)&&tool.metadata["canonical_path"]==context.planFilePath;
    if (mode == PermissionMode::Plan && !tool.readOnly && !taskState && !stopOwnShell && !ownPlan && !planControl && !engineControl && !memoryControl)
        return {PermissionBehavior::Deny, "Plan mode allows read-only tools, the owned plan file, internal task state and stopping owned executions"};
    if (detail::permissionRulesMatch(asks, tool, args, context, false)) matched = PermissionBehavior::Ask;
    else if (detail::permissionRulesMatch(allows, tool, args, context, true)) matched = PermissionBehavior::Allow;
    auto decision = matched.value_or(mode == PermissionMode::Bypass || (tool.readOnly && tool.name != "WebFetch") || taskState || stopOwnShell || ownPlan || memoryControl
        || (mode == PermissionMode::AcceptEdits && tool.editsFiles) ? PermissionBehavior::Allow : PermissionBehavior::Ask);
    if (decision == PermissionBehavior::Ask && mode == PermissionMode::DontAsk) decision = PermissionBehavior::Deny;
    return {decision, matched ? "Tool permission rule (host or current invocation)" : "Session permission mode"};
}
ToolRunner::ToolRunner(std::shared_ptr<ToolRegistry> registry, std::shared_ptr<const PermissionPolicy> policy, ToolRunnerOptions options)
    : registry_(std::move(registry)), policy_(std::move(policy)), options_(std::move(options)) {
    if (!registry_ || !policy_ || options_.maxResultCharacters < 1) throw Error(ErrorCode::InvalidArgument, "Invalid tool runner configuration");
}
void PermissionPolicy::applyUpdates(const QJsonArray&,const ToolContext&) const {
    throw Error(ErrorCode::RuntimeUnavailable,"Permission updates require a trusted host handler or mutable policy");
}
bool ToolRunner::concurrencySafe(const ToolCall& call) const {
    try { const auto entry = registry_->resolve(call.name); entry->validateInput(call.arguments); const auto& tool = entry->tool;
        // Input-changing hooks can change the scheduling classification. Serialize those runs.
        return !tool.completesRun && options_.hooks.isEmpty() && !options_.permissionResponse && !options_.permissionRequests
            && (tool.canRunConcurrently ? tool.canRunConcurrently(call.arguments) : tool.definition.concurrencySafe);
    } catch (...) { return false; }
}
ToolResult ToolRunner::run(ToolCall call, const ToolContext& suppliedContext, const EventCallback& callback) const {
    auto context=suppliedContext;
    ToolResult result;
    QJsonObject hookContext;
    std::shared_ptr<ModelHookContext> modelContext;
    if(!options_.hooks.isEmpty()) {
        modelContext=std::make_shared<ModelHookContext>();modelContext->model=options_.hookModel;
        modelContext->modelName=options_.hookModelName;modelContext->session=context.sessionSnapshot;
        modelContext->registry=registry_->snapshot();modelContext->tools=modelContext->registry->definitions();modelContext->policy=policy_;
        modelContext->executionContext=context;modelContext->executionContext.progress={};modelContext->executionContext.permissionRequests={};
        modelContext->agentExecutor=options_.hookAgent;
    }
    auto hookEvents=[&](const HookResult& value) {
        for(const auto& diagnostic:value.diagnostics)event(callback,EventKind::Hook,context,call,{},diagnostic.toObject());
        if(value.stop)throw Error(ErrorCode::Cancelled,value.stopReason.isEmpty()?QString("Stopped by hook"):value.stopReason);
    };
    QString beforeFeedback;
    bool mcpOutputEligible=false;
    try {
        context.cancellation.throwIfCancelled();
        if(options_.planning)context=options_.planning->scope(std::move(context),*policy_);
        if(modelContext){modelContext->executionContext=context;modelContext->executionContext.progress={};modelContext->executionContext.permissionRequests={};}
        const auto entry = registry_->resolve(call.name);
        const auto& tool = entry->tool;
        entry->validateInput(call.arguments);
        if (tool.validate) tool.validate(call.arguments, context);
        if(!options_.hooks.isEmpty()) {
            hookContext={{"cwd",context.workingDirectory},{"permission_mode",policy_->describe(context)["mode"].toString("unknown")}};
            hookContext["transcript_path"]=context.transcriptPath;
        }
        std::optional<PermissionDecision> hookPermission;
        for (const auto& hook : options_.hooks) {
            context.cancellation.throwIfCancelled();
            const auto r = hook({HookKind::BeforeTool, context.sessionId, context.runId, call, {}, {},hookContext,modelContext}, context.cancellation);
            hookEvents(r);
            if (r.block) throw Error(ErrorCode::InvalidArgument, "Pre-tool hook blocked execution: " + r.feedback);
            if (r.updatedArguments) call.arguments = *r.updatedArguments;
            if(r.permission) {
                auto priority=[](PermissionBehavior b){return b==PermissionBehavior::Deny?3:b==PermissionBehavior::Ask?2:1;};
                if(!hookPermission||priority(r.permission->behavior)>=priority(hookPermission->behavior))hookPermission=r.permission;
            }
            if(!r.feedback.isEmpty()) {if(!beforeFeedback.isEmpty())beforeFeedback+='\n';beforeFeedback+=r.feedback;}
        }
        entry->validateInput(call.arguments);
        if (tool.validate) tool.validate(call.arguments, context);
        context.workingDirectories=policy_->workingDirectories(context);
        if(hookPermission&&hookPermission->behavior==PermissionBehavior::Allow)context.allowedTools.append(call.name);
        auto prepare=[&] {
            auto value=tool.prepare?tool.prepare(call.arguments,context):PreparedTool{tool.definition,[&]{return tool.execute(call.arguments,context);}};
            if(!value.execute||value.definition.name!=tool.definition.name||value.definition.inputSchema!=tool.definition.inputSchema
                ||value.definition.outputSchema!=tool.definition.outputSchema)
                throw Error(ErrorCode::InvalidArgument,"Prepared tool changed identity/schema or omitted execution");
            return value;
        };
        auto prepared=prepare();
        auto decide=[&] {
            if(context.planModeActive) {
                auto planContext=context;planContext.permissionMode=PermissionMode::Plan;
                const auto boundary=RulePolicy(PermissionMode::Plan).decide(prepared.definition,call.arguments,planContext);
                if(boundary.behavior==PermissionBehavior::Deny)return boundary;
            }
            auto permissionContext=context;
            // Native conversation controls delegate their actual tools into
            // the Engine. Preserve the host's base permission on the wrapper;
            // its inner model tools still receive the current plan scope.
            if(context.planModeActive&&QStringList{"builtin.agent.control","builtin.session.control","builtin.input.control"}
                .contains(prepared.definition.metadata["source"].toString()))permissionContext.permissionMode=suppliedContext.permissionMode;
            return policy_->decide(prepared.definition,call.arguments,permissionContext);
        };
        auto decision = decide();
        if(hookPermission&&(hookPermission->behavior==PermissionBehavior::Deny
            ||(hookPermission->behavior==PermissionBehavior::Ask&&decision.behavior!=PermissionBehavior::Deny)))decision=*hookPermission;
        if(tool.requiresPermission&&decision.behavior==PermissionBehavior::Allow) {
            const bool dontAsk=context.permissionMode==PermissionMode::DontAsk||policy_->describe(context)["mode"]=="dontAsk";
            decision={dontAsk?PermissionBehavior::Deny:PermissionBehavior::Ask,"This tool requires a reviewed host decision"};
        }
        if (tool.prepare) decision.reason += "\n" + prepared.definition.description + "\n"
            + QString::fromUtf8(QJsonDocument(prepared.definition.metadata).toJson(QJsonDocument::Compact));
        bool allowed = decision.behavior == PermissionBehavior::Allow;
        if (decision.behavior == PermissionBehavior::Ask) {
            auto data = toJson(call);
            if (tool.prepare) data["permission_preview"] = toJson(prepared.definition);
            if(!decision.suggestions.isEmpty())data["permission_suggestions"]=decision.suggestions;
            auto requestContext=hookContext;requestContext["permission_reason"]=decision.reason;
            requestContext["permission_mode"]=policy_->describe(context)["mode"].toString("unknown");
            if(tool.prepare)requestContext["permission_preview"]=toJson(prepared.definition);
            if(!decision.suggestions.isEmpty())requestContext["permission_suggestions"]=decision.suggestions;
            std::optional<PermissionResponse> response;
            const auto channel=context.permissionRequests?context.permissionRequests:options_.permissionRequests;
            if(channel) {
                const auto ticket=channel->begin(call,decision,context,tool.prepare?toJson(prepared.definition):QJsonObject{});
                const auto localToken=CancellationToken::linkedTo(context.cancellation);
                struct LocalResult {QJsonArray diagnostics;QString feedback;bool won=false;};
                std::future<LocalResult> local;
                QJsonObject resolution;
                try {
                    if(!options_.hooks.isEmpty()||options_.permissionResponse||options_.permission) {
                        local=std::async(std::launch::async,[&] {
                            LocalResult result;
                            try {
                                std::optional<PermissionResponse> answer;QString source="hook";
                                for(const auto& hook:options_.hooks) {
                                    localToken.throwIfCancelled();
                                    const auto value=hook({HookKind::PermissionRequest,context.sessionId,context.runId,call,{},decision.reason,requestContext,modelContext},localToken);
                                    for(const auto& diagnostic:value.diagnostics)result.diagnostics.append(diagnostic);
                                    if(!value.feedback.isEmpty()){if(!result.feedback.isEmpty())result.feedback+='\n';result.feedback+=value.feedback;}
                                    if(value.stop)answer=PermissionResponse{PermissionBehavior::Deny,value.stopReason,{}, {},true};
                                    else if(value.block)answer=PermissionResponse{PermissionBehavior::Deny,value.feedback};
                                    else if(value.permissionResponse)answer=value.permissionResponse;
                                    if(answer)break;
                                }
                                localToken.throwIfCancelled();auto localContext=context;localContext.cancellation=localToken;
                                if(!answer&&options_.permissionResponse){source="host_callback";answer=options_.permissionResponse(call,decision,localContext);}
                                if(!answer&&options_.permission){source="host_callback";answer=PermissionResponse{options_.permission(call,decision,localContext)?PermissionBehavior::Allow:PermissionBehavior::Deny};}
                                if(answer)result.won=channel->settle(ticket,*answer,source);
                            }catch(const std::exception& error) {
                                if(!localToken.isCancelled()) {
                                    result.diagnostics.append(QJsonObject{{"hook_event_name","PermissionRequest"},{"outcome","handler_error"},{"message",QString::fromUtf8(error.what())}});
                                    result.won=channel->settle(ticket,{PermissionBehavior::Deny,"Permission handler failed"},"handler_error");
                                }
                            }catch(...) {
                                if(!localToken.isCancelled())result.won=channel->settle(ticket,{PermissionBehavior::Deny,"Unknown permission handler failure"},"handler_error");
                            }
                            return result;
                        });
                    }
                    event(callback,EventKind::PermissionRequested,context,call,decision.reason,ticket.request);
                    response=channel->wait(ticket,context.cancellation,&resolution);localToken.cancel();
                    if(local.valid()) {
                        const auto result=local.get();
                        for(const auto& diagnostic:result.diagnostics)event(callback,EventKind::Hook,context,call,{},diagnostic.toObject());
                        if(result.won&&!result.feedback.isEmpty()){if(!beforeFeedback.isEmpty())beforeFeedback+='\n';beforeFeedback+=result.feedback;}
                    }
                    event(callback,EventKind::PermissionResolved,context,call,{},resolution);
                }catch(...) {
                    localToken.cancel();channel->dismiss(ticket);
                    if(local.valid())try{(void)local.get();}catch(...){}
                    // Preserve the original failure even if the observer also throws.
                    try {if(resolution.isEmpty())(void)channel->wait(ticket,{},&resolution);
                        event(callback,EventKind::PermissionResolved,context,call,{},resolution);}catch(...){}
                    throw;
                }
            } else {
                event(callback, EventKind::PermissionRequested, context, call, decision.reason, data);
                for(const auto& hook:options_.hooks) {
                    context.cancellation.throwIfCancelled();
                    const auto r=hook({HookKind::PermissionRequest,context.sessionId,context.runId,call,{},decision.reason,requestContext,modelContext},context.cancellation);
                    hookEvents(r);
                    if(!r.feedback.isEmpty()){if(!beforeFeedback.isEmpty())beforeFeedback+='\n';beforeFeedback+=r.feedback;}
                    if(r.block)response=PermissionResponse{PermissionBehavior::Deny,r.feedback};
                    else if(r.permissionResponse)response=r.permissionResponse;
                    if(response)break;
                }
                context.cancellation.throwIfCancelled();
                if(!response&&options_.permissionResponse)response=options_.permissionResponse(call,decision,context);
                if(!response)response=PermissionResponse{options_.permission&&options_.permission(call,decision,context)?PermissionBehavior::Allow:PermissionBehavior::Deny};
            }
            context.cancellation.throwIfCancelled();detail::validatePermissionResponse(*response);
            allowed=response->behavior==PermissionBehavior::Allow;
            if(!response->message.isEmpty())decision.reason=response->message;
            if(response->interrupt) {
                context.cancellation.cancel();throw Error(ErrorCode::Cancelled,response->message.isEmpty()?QString("Permission denied with interrupt"):response->message);
            }
            if(allowed) {
                context.approvedToolPreview=prepared.definition.metadata;
                auto reprepare=[&] {
                    context.workingDirectories=policy_->workingDirectories(context);
                    entry->validateInput(call.arguments);if(tool.validate)tool.validate(call.arguments,context);prepared=prepare();
                };
                auto checkDeny=[&] {
                    const auto current=decide();
                    if(current.behavior==PermissionBehavior::Deny)throw Error(ErrorCode::InvalidArgument,"Tool permission denied: "+current.reason);
                };
                if(response->updatedArguments){call.arguments=*response->updatedArguments;entry->validateInput(call.arguments);}
                if(!response->updatedPermissions.isEmpty()) {
                    if(options_.permissionUpdates)options_.permissionUpdates(response->updatedPermissions,context);
                    else policy_->applyUpdates(response->updatedPermissions,context);
                    context.cancellation.throwIfCancelled();
                    reprepare();
                } else if(response->updatedArguments)reprepare();
                checkDeny();
            }
        }
        if (!allowed) throw Error(ErrorCode::InvalidArgument, "Tool permission denied: " + decision.reason);
        context.cancellation.throwIfCancelled();
        event(callback, EventKind::ToolStarted, context, call, {}, toJson(call));
        result = options_.planning?options_.planning->execute(context,prepared.definition,prepared.execute):prepared.execute();
        context.cancellation.throwIfCancelled();
        if (!result.isError) entry->validateOutput(result.data);
        mcpOutputEligible=tool.isMcp&&!result.isError;
    } catch (const Error& e) {
        if (e.code() == ErrorCode::Cancelled || e.code() == ErrorCode::ConsumerFailure) throw;
        result = {QString::fromUtf8(e.what()), {{"error_code", iiLocalLLM::enumName(e.code())}}, true};
    } catch (const std::exception& e) { result = {QString::fromUtf8(e.what()), {}, true}; }
    catch (...) { result = {"Tool failed with an unknown exception", {}, true}; }
    if(!beforeFeedback.isEmpty())result.text+='\n'+beforeFeedback;
    auto accumulatedFeedback=beforeFeedback;
    for (const auto& hook : options_.hooks) {
        context.cancellation.throwIfCancelled();
        auto r = hook({HookKind::AfterTool, context.sessionId, context.runId, call, result, {},hookContext,modelContext}, context.cancellation);
        hookEvents(r);
        if(mcpOutputEligible&&!result.isError&&r.updatedMCPToolOutput) {
            try {
                if(const auto content=detail::mcpOutputContent(*r.updatedMCPToolOutput)) {
                    auto text=detail::mcpTextContent(*content);
                    if(!accumulatedFeedback.isEmpty())text+='\n'+accumulatedFeedback;
                    result.text=std::move(text);result.content=*content;result.data={};
                }
            } catch(const Error& error) {
                event(callback,EventKind::Hook,context,call,{},{{"hook_event_name","PostToolUse"},{"outcome","non_blocking_error"},
                    {"error_code",enumName(error.code())},{"error",QString::fromUtf8(error.what())}});
            }
        }
        if (!r.feedback.isEmpty()) {
            result.text += "\n" + r.feedback;
            if(!accumulatedFeedback.isEmpty())accumulatedFeedback+='\n';accumulatedFeedback+=r.feedback;
        }
        if (r.block) result.isError = true;
    }
    if (result.text.size() > options_.maxResultCharacters && !context.artifactsDirectory.isEmpty()) {
        if (!QDir().mkpath(context.artifactsDirectory)) throw Error(ErrorCode::StorageFailure, "Cannot create tool artifact directory");
        const auto path = QDir(context.artifactsDirectory).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".txt");
        QSaveFile file(path); const auto bytes = result.text.toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            throw Error(ErrorCode::StorageFailure, "Cannot persist large tool output");
        result.text = result.text.left(options_.maxResultCharacters) + "\nFull output: " + path;
    }
    event(callback, EventKind::ToolFinished, context, call, result.text, {{"is_error", result.isError}, {"result", result.data},
        {"content", result.content}, {"metadata", result.metadata}});
    return result;
}
}
