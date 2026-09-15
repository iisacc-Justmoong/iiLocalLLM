#include "McpTools.h"
#include "McpResult.h"
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
        Tool tool; tool.isMcp = true; tool.definition.name = toolName(options.serverName, remoteName);
        if (names.contains(tool.definition.name)) throw Error(ErrorCode::AlreadyExists, "MCP tool name collision");
        names.insert(tool.definition.name);
        tool.definition.description = remote["description"].toString();
        tool.definition.inputSchema = remote["inputSchema"].toObject();
        tool.definition.outputSchema = remote["outputSchema"].toObject();
        tool.definition.readOnly = options.trustAnnotations && remote["annotations"].toObject()["readOnlyHint"] == true;
        tool.definition.concurrencySafe = tool.definition.readOnly;
        tool.definition.metadata = {{"source", "mcp"}, {"server_name", options.serverName}, {"remote_name", remoteName},
            {"remote_definition", remote}, {"annotations_trusted", options.trustAnnotations}};
        // This hint only restricts unattended agents; it never grants permission.
        if (remote["_meta"].toObject()["iisacc/userInteraction"] == true)
            tool.definition.metadata["requires_user_interaction"] = true;
        if (!options.appId.isEmpty()) tool.definition.metadata["app_id"] = options.appId;
        tool.execute = [client, remoteName, generation](const QJsonObject& args, const ToolContext& context) {
            if (generation != client->connectionGeneration())
                throw Error(ErrorCode::RuntimeFailure, "MCP session changed; refresh tool definitions before executing");
            const auto wire = client->callTool(remoteName, args, context.cancellation, context.progress, generation);
            ToolResult result; result.content = wire["content"].toArray();
            result.data = wire["structuredContent"].toObject(); result.metadata = wire["_meta"].toObject();
            result.isError = wire["isError"].toBool();
            result.text = detail::mcpTextContent(detail::withStructuredText(result.content, result.data));
            return result;
        };
        validation.add(tool); tools.append(std::move(tool));
    }
    return tools;
}
}
