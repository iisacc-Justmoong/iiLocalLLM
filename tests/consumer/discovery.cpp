#include <agent/McpConnections.h>
#include <agent/ToolSearch.h>
#include <agent/Engine.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QDir>
#include <iostream>
namespace a = iiLocalLLM::agent;
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        QTemporaryDir root(QDir::current().filePath("installed-discovery-XXXXXX"));
        auto registry = std::make_shared<a::ToolRegistry>();
        a::Tool tool; tool.definition = {"app.document", "Read the active document", {{"type", "object"}}, {}, true, true, false, true};
        tool.execute = [](const QJsonObject&, const a::ToolContext&) { return a::ToolResult{"document"}; };
        registry->replace({}, {tool});
        if (a::searchTools(registry->definitions(), "select:app.document").data["matches"].toArray() != QJsonArray{"app.document"}) return 1;
        QFile config(root.filePath("mcp.json"));
        if (!config.open(QIODevice::WriteOnly)) return 2;
        config.write("{\"mcpServers\":{\"disabled\":{\"disabled\":true}}}"); config.close();
        a::McpConnectionOptions options; options.workingDirectory = root.path(); options.configFiles = {config.fileName()}; options.refreshIntervalMs = 0;
        a::McpConnections connections(registry, options);
        if (connections.status().first().toObject()["state"] != "disabled") return 3;
        connections.refresh(); connections.reload(); connections.close();
        if (registry->definitions().size() != 1) return 4;
        std::cout << "Installed tool search, atomic registry and MCP connection ABI verified\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 5; }
}
