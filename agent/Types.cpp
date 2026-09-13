#include "Types.h"
#include "ProtocolState.h"
#include <QtCore/QSet>
#include <QtCore/QUuid>
#include <QtCore/QRegularExpression>

namespace iiLocalLLM::agent {
namespace {
void require(bool value, const QString& message) { if (!value) throw Error(ErrorCode::ProtocolError, message); }
QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
ToolCall callFromJson(const QJsonObject& o) {
    for (auto it = o.begin(); it != o.end(); ++it)
        require(it.key() == "id" || it.key() == "name" || it.key() == "arguments", "Unknown tool call field: " + it.key());
    static const QRegularExpression name("^[A-Za-z0-9_.:-]{1,128}$");
    require(o["name"].isString() && name.match(o["name"].toString()).hasMatch(), "Invalid tool name");
    require(o["arguments"].isObject(), "Tool arguments must be an object");
    require(!o.contains("id") || (o["id"].isString() && !o["id"].toString().isEmpty() && o["id"].toString().size() <= 128), "Invalid tool call ID");
    return {o.contains("id") ? o["id"].toString() : id(), o["name"].toString(), o["arguments"].toObject()};
}
}
QString enumName(MessageRole v) {
    switch(v) { case MessageRole::User: return "user"; case MessageRole::Assistant: return "assistant"; case MessageRole::Tool: return "tool"; }
    return "unknown";
}
QString enumName(RunStatus v) {
    switch(v) { case RunStatus::Completed: return "completed"; case RunStatus::Cancelled: return "cancelled";
        case RunStatus::TurnLimit: return "turn_limit"; case RunStatus::Failed: return "failed"; }
    return "unknown";
}
QString enumName(EventKind v) {
    switch(v) { case EventKind::Started: return "started"; case EventKind::ModelDelta: return "model_delta";
        case EventKind::Message: return "message"; case EventKind::ToolStarted: return "tool_started";
        case EventKind::ToolProgress: return "tool_progress"; case EventKind::ToolFinished: return "tool_finished";
        case EventKind::PermissionRequested: return "permission_requested"; case EventKind::Hook: return "hook";
        case EventKind::Finished: return "finished"; }
    return "unknown";
}
QJsonObject toJson(const ToolCall& c) { return {{"id", c.id}, {"name", c.name}, {"arguments", c.arguments}}; }
QJsonObject toJson(const ToolDefinition& t) {
    QJsonObject o{{"name", t.name}, {"description", t.description}, {"inputSchema", t.inputSchema},
        {"annotations", QJsonObject{{"readOnlyHint", t.readOnly}, {"concurrencySafe", t.concurrencySafe}}}};
    if (!t.outputSchema.isEmpty()) o.insert("outputSchema", t.outputSchema);
    if (!t.metadata.isEmpty()) o.insert("_meta", t.metadata);
    return o;
}
QJsonObject toJson(const Message& m) {
    QJsonArray calls; for (const auto& c : m.toolCalls) calls.append(toJson(c));
    return {{"id", m.id}, {"role", enumName(m.role)}, {"text", m.text}, {"tool_calls", calls},
        {"tool_call_id", m.toolCallId}, {"is_error", m.isError}, {"data", m.data}};
}
QJsonObject toJson(const RunResult& r) {
    return {{"run_id", r.runId}, {"session_id", r.sessionId}, {"text", r.text}, {"status", enumName(r.status)},
        {"turns", r.turns}, {"usage", QJsonObject{{"prompt_tokens", r.usage.promptTokens}, {"generated_tokens", r.usage.generatedTokens},
            {"cached_tokens", r.usage.cachedTokens}, {"dropped_messages", r.usage.droppedMessages}}},
        {"error_code", iiLocalLLM::enumName(r.errorCode)}, {"error_message", r.errorMessage}};
}
QJsonObject toJson(const Event& e) {
    return {{"event", enumName(e.kind)}, {"run_id", e.runId}, {"session_id", e.sessionId},
        {"tool_call_id", e.toolCallId}, {"text", e.text}, {"data", e.data}};
}
ModelReply replyFromJson(const QJsonObject& o) {
    for (auto it = o.begin(); it != o.end(); ++it) require(it.key() == "text" || it.key() == "tool_calls", "Unknown model reply field: " + it.key());
    require(!o.contains("text") || o["text"].isString(), "Reply text must be a string");
    require(!o.contains("tool_calls") || o["tool_calls"].isArray(), "tool_calls must be an array");
    ModelReply r; r.text = o["text"].toString();
    QSet<QString> ids;
    require(o["tool_calls"].toArray().size() <= 64, "Too many tool calls");
    for (const auto& v : o["tool_calls"].toArray()) {
        require(v.isObject(), "Tool call must be an object");
        auto call = callFromJson(v.toObject());
        require(!ids.contains(call.id), "Duplicate tool call ID"); ids.insert(call.id);
        r.toolCalls.append(std::move(call));
    }
    require(!r.text.trimmed().isEmpty() || !r.toolCalls.isEmpty(), "Empty model reply");
    return r;
}
Message messageFromJson(const QJsonObject& o) {
    require(o["id"].isString() && !o["id"].toString().isEmpty(), "Missing message ID");
    require(o["text"].isString() && o["tool_calls"].isArray() && o["tool_call_id"].isString()
        && o["is_error"].isBool() && o["data"].isObject(), "Invalid transcript message fields");
    const auto role = o["role"].toString();
    require(role == "user" || role == "assistant" || role == "tool", "Invalid transcript role");
    Message m{o["id"].toString(), role == "user" ? MessageRole::User : role == "assistant" ? MessageRole::Assistant : MessageRole::Tool,
        o["text"].toString(), {}, o["tool_call_id"].toString(), o["is_error"].toBool(), o["data"].toObject()};
    for (const auto& v : o["tool_calls"].toArray()) {
        require(v.isObject() && v.toObject().contains("id"), "Invalid transcript tool call");
        m.toolCalls.append(callFromJson(v.toObject()));
    }
    return m;
}
QList<ToolCall> pendingToolCalls(const QList<Message>& messages) {
    detail::ProtocolState state;
    for (const auto& message : messages) state.accept(message);
    return state.pending;
}
}
