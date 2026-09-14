#include <agent/McpConnections.h>
#include <agent/ToolSearch.h>
#include <agent/Engine.h>
#include <mcp/HttpClient.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QDir>
#include <QtNetwork/QTcpServer>
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
        // A listening loopback endpoint accepts TCP but never serves HTTP. This
        // exercises the installed deadline exception without an external server.
        QTcpServer silent;
        if (!silent.listen(QHostAddress::LocalHost, 0)) return 6;
        iiLocalLLM::mcp::HttpOptions http;
        http.endpoint = QUrl(QString("http://127.0.0.1:%1/mcp").arg(silent.serverPort()));
        http.initializeTimeoutMs = 150; http.shutdownTimeoutMs = 0;
        try { iiLocalLLM::mcp::HttpClient client(http); return 7; }
        catch (const iiLocalLLM::mcp::RequestTimeoutError& error) {
            if (error.code() != iiLocalLLM::ErrorCode::Timeout || error.method() != "initialize"
                || error.timeoutMs() != 150 || error.elapsedMs() < 150 || !error.submitted()) return 8;
        }
        if (!config.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 9;
        config.write(QJsonDocument(QJsonObject{{"mcpServers", QJsonObject{{"silent", QJsonObject{
            {"type", "http"}, {"url", http.endpoint.toString()}, {"initializeTimeoutMs", 300}}}}}}).toJson()); config.close();
        options.limits.initializeTimeoutMs = 150; options.limits.shutdownTimeoutMs = 0;
        a::McpConnections failed(registry, options);
        const auto status = failed.status().first().toObject();
        const auto timeout = status.value("request_timeout").toObject();
        if (status["state"] != "failed" || status["error_phase"] != "connect"
            || status["error_elapsed_ms"].toInteger() < 300 || timeout["method"] != "initialize"
            || timeout["timeout_ms"].toInt() != 300 || !timeout["submitted"].toBool()) return 10;
        std::cout << "Installed tool search, MCP deadline exception and connection diagnostics verified\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 5; }
}
