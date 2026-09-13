#include "agent/McpServer.h"
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <cmath>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); app.setApplicationName("iillm-mcp"); app.setApplicationVersion("0.5.0");
    QCommandLineParser parser; parser.setApplicationDescription("iiLocalLLM C++ MCP stdio server");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOptions({{{"w", "workspace"}, "Existing workspace to expose.", "path"},
        {"allow", "Allow a tool name or wildcard; repeat for more rules. Read-only tools are allowed by default.", "pattern"},
        {"artifacts", "Directory for large tool results.", "path"},
        {"model", "Enable the local agent using an installed model:// URI.", "uri"},
        {"models", "Installed model catalog directory; required with --model.", "path"},
        {"sessions", "Agent transcript directory (default: workspace/.iilocal-llm/sessions).", "path"},
        {"context", "Local model context tokens.", "tokens", "4096"},
        {"max-tokens", "Generated tokens per agent turn.", "tokens", "512"},
        {"temperature", "Agent sampling temperature.", "number", "0.7"},
        {"request-timeout", "Maximum MCP request duration in milliseconds.", "milliseconds", "60000"}});
    parser.process(app);
    try {
        const auto workspace = QFileInfo(parser.value("workspace")).canonicalFilePath();
        if (!parser.isSet("workspace") || workspace.isEmpty() || !QFileInfo(workspace).isDir())
            throw iiLocalLLM::Error(iiLocalLLM::ErrorCode::InvalidArgument, "--workspace must name an existing directory");
        auto positive = [&](const char* option, int maximum) { bool ok; const int value = parser.value(option).toInt(&ok);
            if (!ok || value < 1 || value > maximum) throw std::runtime_error(std::string("Invalid --") + option); return value; };
        const int contextTokens = positive("context", 1048576), maxTokens = positive("max-tokens", 1048576);
        const int timeout = positive("request-timeout", 3600000);
        bool ok; const double temperature = parser.value("temperature").toDouble(&ok);
        if (!ok || !std::isfinite(temperature) || temperature < 0 || temperature > 2 || maxTokens >= contextTokens)
            throw std::runtime_error("Invalid sampling or context limits");
        namespace a = iiLocalLLM::agent;
        QList<a::PermissionRule> rules;
        for (const auto& value : parser.values("allow")) rules.append({value, a::PermissionBehavior::Allow});
        const bool agent = parser.isSet("model");
        if (agent) rules.append({"iiLocalLLM.agent.run", a::PermissionBehavior::Allow});
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::DontAsk, rules);
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, workspace);
        std::unique_ptr<iiLocalLLM::Service> service;
        a::McpServerOptions options; options.workingDirectory = workspace; options.appId = "com.iisacc.iiLocalLLM";
        options.artifactsDirectory = parser.isSet("artifacts") ? parser.value("artifacts") : QDir(workspace).filePath(".iilocal-llm/artifacts");
        if (agent) {
            if (!parser.isSet("models") || !parser.value("model").startsWith("model://")) throw std::runtime_error("--model requires a model:// URI and --models catalog");
            iiLocalLLM::ServiceOptions serviceOptions; serviceOptions.modelsDirectory = parser.value("models"); serviceOptions.defaultContextTokens = contextTokens;
            service = std::make_unique<iiLocalLLM::Service>(serviceOptions);
            a::EngineOptions engineOptions;
            engineOptions.sessionsDirectory = parser.isSet("sessions") ? parser.value("sessions") : QDir(workspace).filePath(".iilocal-llm/sessions");
            options.engine = std::make_shared<a::Engine>(std::make_shared<a::ServiceModel>(*service), registry, policy, engineOptions);
            options.model = parser.value("model"); options.generation.maxTokens = maxTokens; options.generation.temperature = temperature;
        }
        auto server = a::mcpServerOptions(registry, policy, std::move(options)); server.requestTimeoutMs = timeout;
        iiLocalLLM::mcp::serveStdio(std::move(server));
        return 0;
    } catch (const std::exception& e) { std::cerr << "iillm-mcp: " << e.what() << '\n'; return 1; }
}
