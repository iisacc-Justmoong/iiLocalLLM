#include "Tools.h"
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QDir>
#include <QtCore/QUuid>
#include <map>
#include <shared_mutex>
#include <mutex>

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
    if (!name.match(tool.definition.name).hasMatch() || !tool.execute)
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
RulePolicy::RulePolicy(PermissionMode mode, QList<PermissionRule> rules) : mode_(mode), rules_(std::move(rules)) {}
PermissionDecision RulePolicy::decide(const ToolDefinition& tool, const QJsonObject&, const ToolContext&) const {
    std::optional<PermissionBehavior> matched;
    for (const auto& rule : rules_) {
        auto regex = QRegularExpression(QRegularExpression::wildcardToRegularExpression(rule.toolPattern));
        if (!regex.match(tool.name).hasMatch()) continue;
        if (rule.behavior == PermissionBehavior::Deny) return {PermissionBehavior::Deny, "Explicit tool deny rule"};
        if (!matched || rule.behavior == PermissionBehavior::Ask) matched = rule.behavior;
    }
    if (mode_ == PermissionMode::Plan && !tool.readOnly) return {PermissionBehavior::Deny, "Plan mode allows only read-only tools"};
    auto decision = matched.value_or(mode_ == PermissionMode::Bypass || tool.readOnly
        || (mode_ == PermissionMode::AcceptEdits && tool.editsFiles) ? PermissionBehavior::Allow : PermissionBehavior::Ask);
    if (decision == PermissionBehavior::Ask && mode_ == PermissionMode::DontAsk) decision = PermissionBehavior::Deny;
    return {decision, matched ? "Explicit tool rule" : "Session permission mode"};
}
ToolRunner::ToolRunner(std::shared_ptr<ToolRegistry> registry, std::shared_ptr<const PermissionPolicy> policy, ToolRunnerOptions options)
    : registry_(std::move(registry)), policy_(std::move(policy)), options_(std::move(options)) {
    if (!registry_ || !policy_ || options_.maxResultCharacters < 1) throw Error(ErrorCode::InvalidArgument, "Invalid tool runner configuration");
}
bool ToolRunner::concurrencySafe(const ToolCall& call) const {
    try { const auto entry = registry_->resolve(call.name); entry->validateInput(call.arguments); const auto& tool = entry->tool;
        // Input-changing hooks can change the scheduling classification. Serialize those runs.
        return options_.hooks.isEmpty() && (tool.canRunConcurrently ? tool.canRunConcurrently(call.arguments) : tool.definition.concurrencySafe);
    } catch (...) { return false; }
}
ToolResult ToolRunner::run(ToolCall call, const ToolContext& context, const EventCallback& callback) const {
    ToolResult result;
    try {
        context.cancellation.throwIfCancelled();
        const auto entry = registry_->resolve(call.name);
        const auto& tool = entry->tool;
        entry->validateInput(call.arguments);
        if (tool.validate) tool.validate(call.arguments, context);
        for (const auto& hook : options_.hooks) {
            context.cancellation.throwIfCancelled();
            const auto r = hook({HookKind::BeforeTool, context.sessionId, context.runId, call, {}, {}}, context.cancellation);
            if (r.block) throw Error(ErrorCode::InvalidArgument, "Pre-tool hook blocked execution: " + r.feedback);
            if (r.updatedArguments) call.arguments = *r.updatedArguments;
        }
        entry->validateInput(call.arguments);
        if (tool.validate) tool.validate(call.arguments, context);
        const auto decision = policy_->decide(tool.definition, call.arguments, context);
        bool allowed = decision.behavior == PermissionBehavior::Allow;
        if (decision.behavior == PermissionBehavior::Ask) {
            event(callback, EventKind::PermissionRequested, context, call, decision.reason, toJson(call));
            allowed = options_.permission && options_.permission(call, decision, context);
        }
        if (!allowed) throw Error(ErrorCode::InvalidArgument, "Tool permission denied: " + decision.reason);
        context.cancellation.throwIfCancelled();
        event(callback, EventKind::ToolStarted, context, call, {}, toJson(call));
        result = tool.execute(call.arguments, context);
        context.cancellation.throwIfCancelled();
        if (!result.isError) entry->validateOutput(result.data);
    } catch (const Error& e) {
        if (e.code() == ErrorCode::Cancelled || e.code() == ErrorCode::ConsumerFailure) throw;
        result = {QString::fromUtf8(e.what()), {{"error_code", iiLocalLLM::enumName(e.code())}}, true};
    } catch (const std::exception& e) { result = {QString::fromUtf8(e.what()), {}, true}; }
    catch (...) { result = {"Tool failed with an unknown exception", {}, true}; }
    for (const auto& hook : options_.hooks) {
        context.cancellation.throwIfCancelled();
        auto r = hook({HookKind::AfterTool, context.sessionId, context.runId, call, result, {}}, context.cancellation);
        if (!r.feedback.isEmpty()) result.text += "\n" + r.feedback;
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
