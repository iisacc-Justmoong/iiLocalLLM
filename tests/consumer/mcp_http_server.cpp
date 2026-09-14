#include <mcp/HttpServer.h>
#include <mcp/HttpClient.h>
#include <agent/McpServer.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); QTemporaryDir workspace;
    try {
        namespace m = iiLocalLLM::mcp; namespace a = iiLocalLLM::agent;
        std::map<QString, m::ServerOptions> exports;
        for (const auto& principal : {QString("app-alpha"), QString("app-beta")}) {
            auto tools = std::make_shared<a::ToolRegistry>(); a::Tool tool;
            const int value = principal == "app-alpha" ? 37 : 91;
            tool.definition = {"app_value", "Read authenticated app state", {{"type", "object"}}, {}, true, true};
            tool.execute = [value](const auto&, const auto&) { return a::ToolResult{"installed HTTP app", {{"number", value}}}; };
            tools->add(tool);
            a::McpServerOptions options; options.workingDirectory = workspace.path(); options.appId = principal;
            exports.emplace(principal, a::mcpServerOptions(tools, std::make_shared<a::RulePolicy>(), options));
        }
        m::HttpServerOptions transport;
        transport.authenticate = [](const QByteArray& token) {
            return token == "consumer-alpha-fixture" ? QString("app-alpha") : token == "consumer-beta-fixture" ? QString("app-beta") : QString{};
        };
        m::HttpServer server([exports](const QString& principal) { return exports.at(principal); }, transport);
        if (!server.listen()) throw std::runtime_error(server.errorString().toStdString());
        for (const auto& principal : {QString("alpha"), QString("beta")}) {
            m::HttpOptions options; options.endpoint = server.endpoint();
            options.bearerToken = [principal] { return "consumer-" + principal.toUtf8() + "-fixture"; };
            m::HttpClient client(options);
            const auto definitions = client.listTools();
            if (definitions.size() != 2) return 2;
            for(const auto& definition:definitions)if(definition.toObject().value("_meta").toObject().value("iisacc/appId") != "app-" + principal)return 2;
            const auto inspection=client.callTool("iiLocalLLM.agent.permissions.get",{});
            if(inspection["isError"].toBool()||inspection["structuredContent"].toObject()["provider"]!="rules")return 4;
            const auto result = client.callTool("app_value", {});
            if (result["isError"].toBool() || result["structuredContent"].toObject()["number"] != (principal == "alpha" ? 37 : 91)) return 3;
        }
        server.close();
        std::cout << "Installed authenticated C++ HTTP MCP server and per-app ToolRegistry factories passed.\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
