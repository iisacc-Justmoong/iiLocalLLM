#include <agent/McpServer.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); QTemporaryDir workspace;
    try {
        namespace a = iiLocalLLM::agent;
        auto tools = std::make_shared<a::ToolRegistry>(); a::Tool tool;
        tool.definition = {"app_value", "Read app state", {{"type", "object"}}, {}, true, true};
        tool.execute = [](const auto&, const auto&) { return a::ToolResult{"installed app", {{"number", 37}}}; }; tools->add(tool);
        a::McpServerOptions options; options.workingDirectory = workspace.path(); options.appId = "com.iisacc.consumer";
        iiLocalLLM::mcp::ServerSession session(a::mcpServerOptions(tools, std::make_shared<a::RulePolicy>(), options));
        auto take = [&] { auto messages = session.takeMessages(2000); if (messages.size() != 1) throw std::runtime_error("MCP response missing"); return messages[0].toObject(); };
        session.receive({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", QJsonObject{{"protocolVersion", "2025-11-25"},
            {"capabilities", QJsonObject{}}, {"clientInfo", QJsonObject{{"name", "consumer"}, {"version", "1"}}}}}});
        if (!take()["result"].toObject()["capabilities"].toObject().contains("tools")) return 2;
        session.receive({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        session.receive({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "app_value"}}}});
        const auto result = take()["result"].toObject();
        if (result["isError"].toBool() || result["structuredContent"].toObject()["number"] != 37) return 3;
        session.close();
        std::cout << "Installed C++ MCP server and app ToolRegistry bridge passed.\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
