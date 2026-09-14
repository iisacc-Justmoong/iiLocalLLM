#include "McpTools.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>

namespace iiLocalLLM::agent {
namespace {
QString toolName(const QString& server, const QString& remote) {
    const QString original = "mcp__" + server + "__" + remote;
    auto name = original; name.replace(QRegularExpression("[^A-Za-z0-9_.:-]"), "_");
    if (name != original || name.size() > 128)
        name = name.left(110) + "__" + QString::fromLatin1(QCryptographicHash::hash(original.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
    return name;
}
QString textContent(const QJsonArray& content) {
    QStringList parts;
    for (const auto& value : content) {
        const auto block = value.toObject(); const auto type = block["type"].toString();
        if (type == "text") parts.append(block["text"].toString());
        else if (type == "resource_link") parts.append("Resource: " + block["name"].toString() + " (" + block["uri"].toString() + ")");
        else if (type == "resource" && block["resource"].toObject()["text"].isString()) {
            const auto resource = block["resource"].toObject();
            parts.append(resource["uri"].toString() + "\n" + resource["text"].toString());
        }
    }
    return parts.join('\n');
}
}
QList<Tool> mcpTools(std::shared_ptr<mcp::Client> client, const McpToolOptions& options, CancellationToken token) {
    if (!client || options.serverName.trimmed().isEmpty()) throw Error(ErrorCode::InvalidArgument, "MCP client and server name are required");
    const auto generation = client->connectionGeneration();
    const auto definitions = client->listTools(token);
    if (generation != client->connectionGeneration())
        throw Error(ErrorCode::RuntimeFailure, "MCP session changed during tool discovery");
    QList<Tool> tools; QSet<QString> names;
    // Compile the complete remote schema set before returning any tool to a host.
    ToolRegistry validation;
    for (const auto& value : definitions) {
        const auto remote = value.toObject(); const auto remoteName = remote["name"].toString();
        Tool tool; tool.definition.name = toolName(options.serverName, remoteName);
        if (names.contains(tool.definition.name)) throw Error(ErrorCode::AlreadyExists, "MCP tool name collision");
        names.insert(tool.definition.name);
        tool.definition.description = remote["description"].toString();
        tool.definition.inputSchema = remote["inputSchema"].toObject();
        tool.definition.outputSchema = remote["outputSchema"].toObject();
        tool.definition.readOnly = options.trustAnnotations && remote["annotations"].toObject()["readOnlyHint"] == true;
        tool.definition.concurrencySafe = tool.definition.readOnly;
        tool.definition.metadata = {{"source", "mcp"}, {"server_name", options.serverName}, {"remote_name", remoteName},
            {"remote_definition", remote}, {"annotations_trusted", options.trustAnnotations}};
        if (!options.appId.isEmpty()) tool.definition.metadata["app_id"] = options.appId;
        tool.execute = [client, remoteName, generation](const QJsonObject& args, const ToolContext& context) {
            if (generation != client->connectionGeneration())
                throw Error(ErrorCode::RuntimeFailure, "MCP session changed; refresh tool definitions before executing");
            const auto wire = client->callTool(remoteName, args, context.cancellation, context.progress, generation);
            ToolResult result; result.content = wire["content"].toArray(); result.text = textContent(result.content);
            result.data = wire["structuredContent"].toObject(); result.metadata = wire["_meta"].toObject();
            result.isError = wire["isError"].toBool();
            if (result.text.isEmpty() && !result.data.isEmpty()) result.text = QString::fromUtf8(QJsonDocument(result.data).toJson(QJsonDocument::Compact));
            return result;
        };
        validation.add(tool); tools.append(std::move(tool));
    }
    return tools;
}
}
