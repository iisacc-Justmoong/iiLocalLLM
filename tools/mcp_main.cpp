#include "agent/McpServer.h"
#include "agent/McpConnections.h"
#include "mcp/LocalApplications.h"
#include "mcp/HttpServer.h"
#include "McpCredentials.h"
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QLockFile>
#include <QtCore/QTimer>
#include <cmath>
#include <csignal>
#include <iostream>
namespace { volatile std::sig_atomic_t interrupted = 0; void interrupt(int) { interrupted = 1; } }

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); app.setApplicationName("iillm-mcp"); app.setApplicationVersion("0.11.0");
    QCommandLineParser parser; parser.setApplicationDescription("iiLocalLLM C++ MCP stdio or authenticated local HTTP server");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOptions({{{"w", "workspace"}, "Existing workspace to expose.", "path"},
        {"allow", "Allow a tool name or wildcard; repeat for more rules. Read-only tools are allowed by default.", "pattern"},
        {"mcp-config", "Host-authorized MCP configuration file; repeat in increasing priority.", "file"},
        {"mcp-project", "Load workspace/.mcp.json after explicit MCP configuration files."},
        {"mcp-eager", "Publish all configured MCP tools to the agent without ToolSearch."},
        {"apps-dir", "Private registry of running local application MCP endpoints.", "directory"},
        {"no-apps", "Disable discovery of running local applications."},
        {"no-tasks", "Disable persistent task and todo tools."},
        {"artifacts", "Directory for large tool results.", "path"},
        {"model", "Enable the local agent using an installed model:// URI.", "uri"},
        {"models", "Installed model catalog directory; required with --model.", "path"},
        {"sessions", "Agent transcript directory (default: workspace/.iilocal-llm/sessions).", "path"},
        {"http-port", "Serve Streamable HTTP on 127.0.0.1/mcp; 0 selects an available port.", "port"},
        {"credentials", "Private JSON client-ID/token file outside the workspace; required with --http-port.", "path"},
        {"state", "Private directory disjoint from the workspace; required with --http-port.", "path"},
        {"origin", "Additional exact browser origin allowed by HTTP; repeat for more origins.", "origin"},
        {"context", "Local model context tokens.", "tokens", "4096"},
        {"max-tokens", "Generated tokens per agent turn.", "tokens", "512"},
        {"temperature", "Agent sampling temperature.", "number", "0.7"},
        {"request-timeout", "Maximum MCP request duration in milliseconds.", "milliseconds", "60000"}});
    parser.process(app);
    try {
        if (parser.isSet("apps-dir") && parser.isSet("no-apps"))
            throw std::runtime_error("--apps-dir and --no-apps cannot be combined");
        if (parser.isSet("apps-dir") && parser.value("apps-dir").isEmpty())
            throw std::runtime_error("--apps-dir requires a nonempty directory");
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
        const bool http = parser.isSet("http-port");
        const auto statePath = parser.value("state");
        iiLocalLLM::mcp::HttpServerOptions transport;
        quint16 port = 0; QString privateState; std::unique_ptr<QLockFile> stateLock;
        if (http || parser.isSet("credentials") || parser.isSet("state") || parser.isSet("origin")) {
            if (!http || !parser.isSet("credentials") || !parser.isSet("state") || parser.isSet("sessions") || parser.isSet("artifacts"))
                throw std::runtime_error("HTTP requires --http-port, --credentials and --state; use --state instead of --sessions or --artifacts");
            const auto value = parser.value("http-port").toUInt(&ok);
            if (!ok || value > 65535) throw std::runtime_error("--http-port must be an integer in [0, 65535]");
            port = quint16(value);
            // Complete credential validation before model/driver initialization.
            transport.authenticate = iiLocalLLMClient::mcpCredentials(parser.value("credentials"), workspace);
            transport.allowedOrigins = parser.values("origin");
            if (statePath.trimmed().isEmpty() || !QDir().mkpath(statePath)) throw std::runtime_error("Cannot create MCP HTTP state directory");
            privateState = QFileInfo(statePath).canonicalFilePath();
            if (privateState.isEmpty() || iiLocalLLMClient::containsPath(workspace, privateState)
                || iiLocalLLMClient::containsPath(privateState, workspace))
                throw std::runtime_error("MCP HTTP state and workspace must be disjoint");
            if (!QFile::setPermissions(privateState, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
                throw std::runtime_error("Cannot make MCP HTTP state private");
            stateLock = std::make_unique<QLockFile>(QDir(privateState).filePath("mcp.lock")); stateLock->setStaleLockTime(0);
            if (!stateLock->tryLock(0)) throw std::runtime_error("MCP HTTP state is already owned or inaccessible");
        }
        namespace a = iiLocalLLM::agent;
        QList<a::PermissionRule> rules;
        for (const auto& value : parser.values("allow")) rules.append({value, a::PermissionBehavior::Allow});
        const bool agent = parser.isSet("model");
        if (agent) rules.append({"iiLocalLLM.agent.run", a::PermissionBehavior::Allow});
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::DontAsk, rules);
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, workspace);
        a::McpConnectionOptions connectionOptions; connectionOptions.workingDirectory = workspace;
        for (const auto& path : parser.values("mcp-config")) connectionOptions.configFiles.append(QFileInfo(path).absoluteFilePath());
        if (parser.isSet("mcp-project")) connectionOptions.configFiles.append(".mcp.json");
        connectionOptions.deferTools = !parser.isSet("mcp-eager");
#if defined(Q_OS_UNIX) && !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
        if (!parser.isSet("no-apps")) connectionOptions.localApplicationsDirectory = iiLocalLLM::mcp::localApplicationsDirectory();
#endif
        if (parser.isSet("apps-dir")) connectionOptions.localApplicationsDirectory = QFileInfo(parser.value("apps-dir")).absoluteFilePath();
        std::unique_ptr<a::McpConnections> connections;
        if (!connectionOptions.configFiles.isEmpty() || !connectionOptions.localApplicationsDirectory.isEmpty())
            connections = std::make_unique<a::McpConnections>(registry, std::move(connectionOptions));
        std::unique_ptr<iiLocalLLM::Service> service;
        a::McpServerOptions options; options.workingDirectory = workspace; options.appId = "com.iisacc.iiLocalLLM";
        options.artifactsDirectory = http ? QDir(privateState).filePath("artifacts")
            : parser.isSet("artifacts") ? parser.value("artifacts") : QDir(workspace).filePath(".iilocal-llm/artifacts");
        if (agent) {
            if (!parser.isSet("models") || !parser.value("model").startsWith("model://")) throw std::runtime_error("--model requires a model:// URI and --models catalog");
            iiLocalLLM::ServiceOptions serviceOptions; serviceOptions.modelsDirectory = parser.value("models"); serviceOptions.defaultContextTokens = contextTokens;
            service = std::make_unique<iiLocalLLM::Service>(serviceOptions);
            a::EngineOptions engineOptions;
            engineOptions.taskToolsEnabled = !parser.isSet("no-tasks");
            engineOptions.sessionsDirectory = http ? QDir(privateState).filePath("sessions")
                : parser.isSet("sessions") ? parser.value("sessions") : QDir(workspace).filePath(".iilocal-llm/sessions");
            options.engine = std::make_shared<a::Engine>(std::make_shared<a::ServiceModel>(*service), registry, policy, engineOptions);
            options.model = parser.value("model"); options.generation.maxTokens = maxTokens; options.generation.temperature = temperature;
        }
        if (!agent && !parser.isSet("no-tasks"))
            options.taskStore = std::make_shared<a::TaskStore>(http ? QDir(privateState).filePath("tasks")
                : QDir(workspace).filePath(".iilocal-llm/tasks"));
        auto server = a::mcpServerOptions(registry, policy, std::move(options)); server.requestTimeoutMs = timeout;
        if (http) {
            // All configured clients share this explicitly selected workspace
            // and policy. Reuse the bridge's cross-connection tool locks.
            iiLocalLLM::mcp::HttpServer listener([server](const QString&) { return server; }, std::move(transport));
            if (!listener.listen(port)) throw std::runtime_error(listener.errorString().toStdString());
            std::cout << QJsonDocument(QJsonObject{{"endpoint", listener.endpoint().toString()}}).toJson(QJsonDocument::Compact).constData() << std::endl;
            std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt);
            QTimer shutdown;
            QObject::connect(&shutdown, &QTimer::timeout, &app, [&] { if (interrupted) app.quit(); });
            shutdown.start(100); return app.exec();
        }
        iiLocalLLM::mcp::serveStdio(std::move(server));
        return 0;
    } catch (const std::exception& e) { std::cerr << "iillm-mcp: " << e.what() << '\n'; return 1; }
}
