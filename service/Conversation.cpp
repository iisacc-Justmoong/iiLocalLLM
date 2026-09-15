#include "Conversation.h"
#include "../Parameters.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QUuid>

namespace iiLocalLLM::detail {
namespace {
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, QString::fromUtf8(message));
}
void fields(const QJsonObject& object, const QSet<QString>& allowed) {
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.contains(it.key())) throw Error(ErrorCode::InvalidArgument, "Unsupported structured conversation field: " + it.key());
}
bool nameValid(const QString& value) {
    static const QRegularExpression pattern("^[A-Za-z0-9_.:-]{1,128}$");
    return pattern.match(value).hasMatch();
}
QString checkCall(const QJsonValue& value, ErrorCode code) {
    const auto call = value.toObject();
    const auto function = call["function"].toObject();
    require(value.isObject() && call["type"] == "function" && nameValid(function["name"].toString()), code, "Invalid function call");
    require(call["id"].isString() && !call["id"].toString().isEmpty() && call["id"].toString().size() <= 128, code, "Invalid function call ID");
    QJsonParseError error;
    const auto arguments = QJsonDocument::fromJson(function["arguments"].toString().toUtf8(), &error);
    require(function["arguments"].isString() && error.error == QJsonParseError::NoError && arguments.isObject(), code,
        "Function arguments must be a JSON object encoded as a string");
    return call["id"].toString();
}
}
void validateConversationRequest(const ConversationRequest& r, int maxCharacters) {
    constexpr auto code = ErrorCode::InvalidArgument;
    validateGenerationOptions(r.options);
    require(r.contextId.size() <= 128 && r.keepAliveMs >= -1 && r.keepAliveMs <= 7LL * 86400000, code, "Invalid conversation context or lifetime");
    require(r.toolChoice == "auto" || r.toolChoice == "none" || r.toolChoice == "required", code, "Invalid tool choice");
    require(r.toolChoice != "required" || !r.tools.isEmpty(), code, "Required tool choice needs tools");
    require(!r.messages.isEmpty() && r.messages.size() <= 4096 && r.tools.size() <= 256, code, "Invalid conversation size");
    const QJsonObject input{{"messages", r.messages}, {"tools", r.tools},{"response_schema",r.responseSchema}};
    require(QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact)).size() <= maxCharacters, code,
        "Conversation exceeds service input limit");
    QSet<QString> names;
    for (const auto& value : r.tools) {
        const auto tool = value.toObject(); const auto f = tool["function"].toObject(); const auto name = f["name"].toString();
        fields(tool, {"type", "function"}); fields(f, {"name", "description", "parameters"});
        require(tool["type"] == "function" && nameValid(name) && !names.contains(name) && f["parameters"].isObject(), code,
            "Invalid or duplicate function definition");
        require(!f.contains("description") || f["description"].isString(), code, "Invalid function description");
        names.insert(name);
    }
    QSet<QString> ids, pending;
    for (qsizetype i = 0; i < r.messages.size(); ++i) {
        const auto value = r.messages[i]; const auto message = value.toObject(); const auto role = message["role"].toString();
        fields(message, {"role", "content", "tool_calls", "tool_call_id", "reasoning_content", "name"});
        require(value.isObject() && (role == "system" || role == "user" || role == "assistant" || role == "tool"), code,
            "Unsupported conversation role");
        require(!message.contains("content") || message["content"].isNull() || message["content"].isString(), code,
            "Only text conversation content is supported");
        require(!message.contains("reasoning_content") || (role == "assistant" && message["reasoning_content"].isString()), code,
            "Only assistant messages may contain string reasoning_content");
        require(!message.contains("name") || (role == "tool" && message["name"].isString()), code, "Invalid tool result name");
        require(role != "system" || i == 0, code, "System message must be first");
        require(role == "tool" || pending.isEmpty(), code, "Tool results must precede another conversation message");
        require(role == "assistant" || !message.contains("tool_calls"), code, "Only assistant messages may call tools");
        require(role == "tool" || !message.contains("tool_call_id"), code, "Only tool messages may contain a result ID");
        if (role == "tool") {
            const auto id = message["tool_call_id"].toString();
            require(pending.remove(id) && message["content"].isString(), code, "Orphan or duplicate tool result");
        } else if (role == "assistant" && message.contains("tool_calls")) {
            require(message["tool_calls"].isArray(), code, "tool_calls must be an array");
            const auto calls = message["tool_calls"].toArray();
            require(!calls.isEmpty() && calls.size() <= 64, code, "Invalid tool call count");
            for (const auto& call : calls) {
                const auto id = checkCall(call, code);
                require(!ids.contains(id), code, "Duplicate tool call ID");
                ids.insert(id); pending.insert(id);
            }
        } else require(!message["content"].toString().trimmed().isEmpty(), code, "Empty conversation message");
    }
    const auto lastRole = r.messages.last().toObject()["role"].toString();
    require(pending.isEmpty() && (lastRole == "user" || lastRole == "tool"), code,
        "Conversation must end in user input or complete tool results");
}
void validateConversationReply(RuntimeConversationReply& reply, const ConversationRequest& request) {
    constexpr auto code = ErrorCode::ProtocolError;
    require(reply.toolCalls.size() <= 64, code, "Runtime returned too many tool calls");
    require(request.parallelToolCalls || reply.toolCalls.size() <= 1, code, "Runtime ignored parallel_tool_calls false");
    require(request.toolChoice != "none" || reply.toolCalls.isEmpty(), code, "Runtime ignored tool_choice none");
    require(request.toolChoice != "required" || !reply.toolCalls.isEmpty(), code, "Runtime ignored required tool choice");
    QSet<QString> names, ids;
    for (const auto& tool : request.tools) names.insert(tool.toObject()["function"].toObject()["name"].toString());
    for (const auto& message : request.messages)
        for (const auto& call : message.toObject()["tool_calls"].toArray()) ids.insert(call.toObject()["id"].toString());
    for (qsizetype i = 0; i < reply.toolCalls.size(); ++i) {
        auto call = reply.toolCalls[i].toObject();
        if (!call.contains("id") || (call["id"].isString() && call["id"].toString().isEmpty()))
            call["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto id = checkCall(call, code);
        require(!ids.contains(id) && names.contains(call["function"].toObject()["name"].toString()), code,
            "Runtime returned an unknown tool or duplicate ID");
        ids.insert(id); reply.toolCalls[i] = call;
    }
}
}
