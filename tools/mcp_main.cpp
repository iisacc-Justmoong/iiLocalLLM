#include "agent/McpServer.h"
#include "agent/McpConnections.h"
#include "agent/ShellTasks.h"
#include "agent/Subagents.h"
#include "mcp/LocalApplications.h"
#include "mcp/HttpServer.h"
#include "McpCredentials.h"
#include "AgentProfileConfig.h"
#include "PermissionSettingsConfig.h"
#include "PermissionRequestsConfig.h"
#include "CommandHookConfig.h"
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QLockFile>
#include <QtCore/QTimer>
#include <cmath>
#include <csignal>
#include <iostream>
#include <atomic>
#include <thread>
namespace {
std::atomic_bool interrupted=false;
static_assert(std::atomic_bool::is_always_lock_free);
void interrupt(int) { interrupted.store(true,std::memory_order_relaxed); }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); app.setApplicationName("iillm-mcp"); app.setApplicationVersion("0.27.0");
    QCommandLineParser parser; parser.setApplicationDescription("iiLocalLLM C++ MCP stdio or authenticated local HTTP server");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOptions({{{"w", "workspace"}, "Existing workspace to expose.", "path"},
        {"allow", "Allow a tool permission rule, e.g. Write(src/**), Bash(git status:*) or Skill(review); repeat for more rules. Read-only tools are allowed by default.", "pattern"},
        {"permission-settings", "Private host configuration outside the workspace for layered permission settings.", "file"},
        {"permission-requests", "Private host JSON outside the workspace enabling app permission requests.", "file"},
        {"add-dir", "Additional file working directory; repeat. Does not enable disk settings without --permission-settings.", "directory"},
        {"hooks", "Private command-hook JSON configuration outside the workspace.", "file"},
        {"mcp-config", "Host-authorized MCP configuration file; repeat in increasing priority.", "file"},
        {"mcp-project", "Load workspace/.mcp.json after explicit MCP configuration files."},
        {"mcp-eager", "Publish all configured MCP tools to the agent without ToolSearch."},
        {"apps-dir", "Private registry of running local application MCP endpoints.", "directory"},
        {"no-apps", "Disable discovery of running local applications."},
        {"no-tasks", "Disable persistent task and todo tools."},
        {"no-skills", "Disable local skill discovery and invocation in the agent."},
        {"no-subagents", "Disable delegated local agent execution."},
        {"agent-profiles", "Private JSON host configuration for profile directories, overrides and model grants.", "file"},
        {"no-agent-profiles", "Disable agent profile file discovery; retain general-purpose."},
        {"skills-dir", "Additional host-authorized skills directory; repeat in highest-priority-first order.", "directory"},
        {"no-background", "Disable background shell execution and its control tools."},
        {"artifacts", "Directory for large tool results.", "path"},
        {"model", "Enable the local agent using an installed model:// URI.", "uri"},
        {"models", "Installed model catalog directory; required with --model.", "path"},
        {"model-options", "Private JSON model load options outside the workspace; preload --model before serving.", "file"},
        {"sessions", "Agent transcript directory (default: workspace/.iilocal-llm/sessions).", "path"},
        {"http-port", "Serve Streamable HTTP on 127.0.0.1/mcp; 0 selects an available port.", "port"},
        {"credentials", "Private JSON client-ID/token file outside the workspace; required with --http-port.", "path"},
        {"state", "Private directory disjoint from the workspace; required for HTTP and subagents.", "path"},
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
        QJsonObject modelOptions;
        if (parser.isSet("model-options")) {
            if (!parser.isSet("model") || !parser.isSet("models"))
                throw std::runtime_error("--model-options requires --model and --models");
            // Validate the host-owned file before creating Service or its catalog.
            // Report only the option name, never backend values or file contents.
            try {
                const auto path = parser.value("model-options");
                const auto canonical = QFileInfo(path).canonicalFilePath();
                if (canonical.isEmpty() || iiLocalLLMClient::containsPath(workspace, canonical))
                    throw std::runtime_error("Model options must be outside the workspace");
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(iiLocalLLMClient::readPrivateFile(path), &error);
                if (error.error != QJsonParseError::NoError || !document.isObject())
                    throw std::runtime_error("Model options must be a JSON object");
                modelOptions = document.object();
            } catch (const std::exception&) {
                throw std::runtime_error("Invalid --model-options file: require a private regular JSON object file outside the workspace, at most 65536 bytes");
            }
        }
        const auto profiles=iiLocalLLMClient::profileConfig(parser.value("agent-profiles"),parser.isSet("no-agent-profiles"));
        iiLocalLLM::agent::discoverAgentProfiles(workspace,profiles.profiles);
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
        if (http) {
            if (!parser.isSet("credentials") || !parser.isSet("state"))
                throw std::runtime_error("HTTP requires --credentials and --state");
            const auto value = parser.value("http-port").toUInt(&ok);
            if (!ok || value > 65535) throw std::runtime_error("--http-port must be an integer in [0, 65535]");
            port = quint16(value);
            transport.authenticate = iiLocalLLMClient::mcpCredentials(parser.value("credentials"), workspace);
            transport.allowedOrigins = parser.values("origin");
        } else if (parser.isSet("credentials") || parser.isSet("origin")) {
            throw std::runtime_error("Credentials and origins require --http-port");
        }
        if (parser.isSet("state")) {
            if (parser.isSet("sessions") || parser.isSet("artifacts"))
                throw std::runtime_error("Use --state instead of --sessions or --artifacts");
            if (statePath.trimmed().isEmpty() || !QDir().mkpath(statePath)) throw std::runtime_error("Cannot create MCP state directory");
            privateState = QFileInfo(statePath).canonicalFilePath();
            if (privateState.isEmpty() || iiLocalLLMClient::containsPath(workspace, privateState)
                || iiLocalLLMClient::containsPath(privateState, workspace))
                throw std::runtime_error("MCP state and workspace must be disjoint");
            if (!QFile::setPermissions(privateState, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
                throw std::runtime_error("Cannot make MCP state private");
            stateLock = std::make_unique<QLockFile>(QDir(privateState).filePath("mcp.lock")); stateLock->setStaleLockTime(0);
            if (!stateLock->tryLock(0)) throw std::runtime_error("MCP state is already owned or inaccessible");
        }
        namespace a = iiLocalLLM::agent;
        QList<a::PermissionRule> rules,hostRules;
        for (const auto& value : parser.values("allow")) rules.append({value, a::PermissionBehavior::Allow});
        const bool agent = parser.isSet("model");
        if (agent) {
            hostRules.append({"iiLocalLLM.agent.run", a::PermissionBehavior::Allow});
            hostRules.append({"iiLocalLLM.agent.clear", a::PermissionBehavior::Allow});
            // The inner native Agent/AgentStop call still evaluates host policy.
            hostRules.append({"iiLocalLLM.agent.agents.run", a::PermissionBehavior::Allow});
            hostRules.append({"iiLocalLLM.agent.agents.stop", a::PermissionBehavior::Allow});
        }
        if(parser.isSet("permission-settings")&&parser.value("permission-settings").isEmpty())throw std::runtime_error("--permission-settings requires a file");
        if(parser.isSet("permission-requests")&&parser.value("permission-requests").isEmpty())throw std::runtime_error("--permission-requests requires a file");
        const auto permissionRequests=iiLocalLLMClient::permissionRequestsConfig(parser.value("permission-requests"),workspace);
        auto policy=iiLocalLLMClient::permissionConfig(parser.value("permission-settings"),workspace,rules,hostRules,parser.values("add-dir"),permissionRequests?a::PermissionMode::Default:a::PermissionMode::DontAsk);
        if(parser.isSet("hooks")&&parser.value("hooks").isEmpty())throw std::runtime_error("--hooks requires a file");
        const auto hooks=iiLocalLLMClient::commandHookConfig(parser.value("hooks"),workspace);
        std::shared_ptr<a::ShellTasks> shells;
#if defined(Q_OS_UNIX) && !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
        if (!parser.isSet("no-background")) shells = std::make_shared<a::ShellTasks>(workspace,
            !privateState.isEmpty() ? QDir(privateState).filePath("shells") : QDir(workspace).filePath(".iilocal-llm/shells"));
#endif
        a::McpConnectionOptions connectionOptions; connectionOptions.workingDirectory = workspace;
        for (const auto& path : parser.values("mcp-config")) connectionOptions.configFiles.append(QFileInfo(path).absoluteFilePath());
        if (parser.isSet("mcp-project")) connectionOptions.configFiles.append(".mcp.json");
        connectionOptions.deferTools = !parser.isSet("mcp-eager");
#if defined(Q_OS_UNIX) && !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
        if (!parser.isSet("no-apps")) connectionOptions.localApplicationsDirectory = iiLocalLLM::mcp::localApplicationsDirectory();
#endif
        if (parser.isSet("apps-dir")) connectionOptions.localApplicationsDirectory = QFileInfo(parser.value("apps-dir")).absoluteFilePath();
        QStringList privatePaths{privateState.isEmpty()?QDir(workspace).filePath(".iilocal-llm"):privateState};
        for(const auto& key:{"credentials","permission-settings","permission-requests","agent-profiles","model-options","sessions","artifacts","hooks"})
            if(parser.isSet(key))privatePaths.append(parser.value(key));
        for(const auto& file:connectionOptions.configFiles)privatePaths.append(QDir::isAbsolutePath(file)?file:QDir(workspace).filePath(file));
        if(!connectionOptions.localApplicationsDirectory.isEmpty())privatePaths.append(connectionOptions.localApplicationsDirectory);
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, workspace, shells,privatePaths);
        std::unique_ptr<a::McpConnections> connections;
        if (!connectionOptions.configFiles.isEmpty() || !connectionOptions.localApplicationsDirectory.isEmpty())
            connections = std::make_unique<a::McpConnections>(registry, std::move(connectionOptions));
        std::unique_ptr<iiLocalLLM::Service> service;
        a::McpServerOptions options; options.workingDirectory = workspace; options.appId = "com.iisacc.iiLocalLLM";
        options.tools.hooks=hooks;
        options.permissionRequests=permissionRequests;
        options.artifactsDirectory = !privateState.isEmpty() ? QDir(privateState).filePath("artifacts")
            : parser.isSet("artifacts") ? parser.value("artifacts") : QDir(workspace).filePath(".iilocal-llm/artifacts");
        if (agent) {
            if (!parser.isSet("models") || !parser.value("model").startsWith("model://")) throw std::runtime_error("--model requires a model:// URI and --models catalog");
            iiLocalLLM::ServiceOptions serviceOptions; serviceOptions.modelsDirectory = parser.value("models"); serviceOptions.defaultContextTokens = contextTokens;
            service = std::make_unique<iiLocalLLM::Service>(serviceOptions);
            if (parser.isSet("model-options"))
                (void)service->loadModel({parser.value("model"), contextTokens, modelOptions}).get();
            a::EngineOptions engineOptions;
            engineOptions.hooks=hooks;
            engineOptions.taskToolsEnabled = !parser.isSet("no-tasks");
            engineOptions.skills.enabled = !parser.isSet("no-skills");
            engineOptions.skills.directories = parser.values("skills-dir");
            engineOptions.sessionsDirectory = !privateState.isEmpty() ? QDir(privateState).filePath("sessions")
                : parser.isSet("sessions") ? parser.value("sessions") : QDir(workspace).filePath(".iilocal-llm/sessions");
            auto model = std::make_shared<a::ServiceModel>(*service);
            if (!privateState.isEmpty() && !parser.isSet("no-subagents")) {
                auto subagents=profiles; subagents.workingDirectory = workspace;
                subagents.stateDirectory = QDir(privateState).filePath("subagents");
                subagents.maxRuntimeMs = timeout; subagents.generation.maxTokens = maxTokens; subagents.generation.temperature = temperature;
                a::Subagents::attach(engineOptions,std::make_shared<a::Subagents>(model, registry, policy, engineOptions, subagents));
            }
            options.engine = std::make_shared<a::Engine>(model, registry, policy, engineOptions);
            options.model = parser.value("model"); options.generation.maxTokens = maxTokens; options.generation.temperature = temperature;
        }
        if (!agent && !parser.isSet("no-tasks"))
            options.taskStore = std::make_shared<a::TaskStore>(!privateState.isEmpty() ? QDir(privateState).filePath("tasks")
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
        std::signal(SIGINT, interrupt);std::signal(SIGTERM, interrupt);
        iiLocalLLM::CancellationToken cancellation;
        std::jthread shutdown([&](std::stop_token stop) {
            while(!stop.stop_requested()&&!interrupted.load(std::memory_order_relaxed))std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if(interrupted.load(std::memory_order_relaxed))cancellation.cancel();
        });
        iiLocalLLM::mcp::serveStdio(std::move(server),cancellation);
        return 0;
    } catch (const std::exception& e) { std::cerr << "iillm-mcp: " << e.what() << '\n'; return 1; }
}
