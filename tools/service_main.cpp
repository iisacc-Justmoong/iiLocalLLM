#include <iiLocalLLM.h>
#include "IpcEndpoint.h"
#include "PrivateFile.h"
#include "AgentProfileConfig.h"
#include "PermissionSettingsConfig.h"
#include "PermissionRequestsConfig.h"
#include "CommandHookConfig.h"
#include <agent/Api.h>
#include <agent/McpConnections.h>
#include <agent/ShellTasks.h>
#include <mcp/LocalApplications.h>
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTimer>
#include <csignal>
#include <iostream>
#include <optional>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
QString required(const QJsonObject& object, const QString& key)
{
    const auto value = object.value(key);
    if (!value.isString() || value.toString().trimmed().isEmpty())
        throw iiLocalLLM::Error(iiLocalLLM::ErrorCode::InvalidArgument, key + QStringLiteral(" is required"));
    return value.toString();
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("iiLocalLLMD"));
    app.setApplicationVersion(QStringLiteral(IILOCALLLM_APP_VERSION));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("iiLocalLLM local JSON IPC service"));
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOptions({{{QStringLiteral("s"), QStringLiteral("socket")}, QStringLiteral("Local socket path/name"), QStringLiteral("path")},
        {{QStringLiteral("c"), QStringLiteral("config")}, QStringLiteral("JSON model catalog (optional)"), QStringLiteral("file")},
        {QStringLiteral("mlx-python"), QStringLiteral("Python with mlx-lm installed"), QStringLiteral("executable"), QStringLiteral("python3")},
        {QStringLiteral("mlx-worker"), QStringLiteral("Path to mlx_worker.py"), QStringLiteral("path")},
        {QStringLiteral("models-root"), QStringLiteral("Service-owned Models directory"), QStringLiteral("directory"), QStringLiteral("Models")},
        {QStringLiteral("registry"), QStringLiteral("Download registry JSON (aliases, pinned manifests and URLs)"), QStringLiteral("file")},
        {QStringLiteral("memory-budget-mib"), QStringLiteral("Model residency budget; 0 uses physical RAM policy"), QStringLiteral("MiB"), QStringLiteral("0")},
        {QStringLiteral("memory-reserve-mib"), QStringLiteral("Live available RAM headroom"), QStringLiteral("MiB"), QStringLiteral("256")},
        {QStringLiteral("max-models"), QStringLiteral("Maximum resident models"), QStringLiteral("count"), QStringLiteral("4")},
        {QStringLiteral("context-tokens"), QStringLiteral("Default model context capacity"), QStringLiteral("count"), QStringLiteral("2048")},
        {QStringLiteral("keep-alive"), QStringLiteral("Idle model lifetime, e.g. 5m or 0; default is hardware-dependent"), QStringLiteral("duration")},
        {QStringLiteral("http-port"), QStringLiteral("Enable HTTP on 127.0.0.1; 0 selects an available port"), QStringLiteral("port")},
        {"http-workers", "Maximum ordinary HTTP responses in flight (1..64).", "count", "8"},
        {"http-control-requests", "Separate HTTP control responses in flight (1..16).", "count", "2"},
        {"agent-workspace", "Enable the agent API for this existing workspace.", "directory"},
        {"agent-state", "Private agent state directory outside the workspace.", "directory"},
        {"agent-credentials", "Private JSON object mapping client IDs to distinct random tokens (32..256 URL-safe characters).", "file"},
        {"agent-allow", "Allow a tool permission rule, e.g. Write(src/**), Bash(git status:*) or Skill(review); repeat for more rules. Read-only tools except WebFetch are allowed by default.", "pattern"},
        {"agent-permission-settings", "Private host configuration outside the workspace for layered permission settings.", "file"},
        {"agent-permission-requests", "Private host JSON outside the workspace enabling app permission requests.", "file"},
        {"agent-add-dir", "Additional file working directory; repeat. Does not enable disk settings without --agent-permission-settings.", "directory"},
        {"agent-hooks", "Private command/HTTP/prompt/agent hook JSON configuration outside the workspace.", "file"},
        {"agent-mcp-config", "Host-authorized MCP configuration file; repeat in increasing priority.", "file"},
        {"agent-mcp-project", "Load workspace/.mcp.json after explicit MCP configuration files."},
        {"agent-mcp-eager", "Publish all configured MCP tools to the model without ToolSearch."},
        {"agent-apps-dir", "Private registry of running local application MCP endpoints.", "directory"},
        {"agent-no-apps", "Disable discovery of running local applications."},
        {"agent-no-plan-mode", "Disable session planning and review tools."},
        {"agent-no-user-questions", "Disable AskUserQuestion."},
        {"agent-no-memory", "Disable persistent project memory context and file access."},
        {"agent-no-memory-recall", "Disable model-ranked project memory recall."},
        {"agent-no-memory-extraction", "Disable automatic project memory extraction after main-agent responses."},
        {"agent-no-session-history", "Disable owned session transcript search."},
        {"agent-no-file-checkpoints", "Disable native file checkpoints and rewind controls."},
        {"agent-no-worktrees", "Disable owned worktree lifecycle tools."},
        {"agent-worktree-config", "Private host JSON for worktree storage, base ref and sparse paths.", "file"},
        {"agent-lsp-config", "Private host JSON configuring local language servers.", "file"},
        {"agent-no-web-fetch", "Disable anonymous web page fetching and local extraction."},
        {"agent-web-model", "Local WebFetch extraction model (default: session model).", "model"},
        {"agent-web-private-origin", "Trusted exact private web origin; repeat. Still requires WebFetch domain permission.", "origin"},
        {"agent-auto-dream", "Enable automatic project memory consolidation after eligible main-agent responses."},
        {"agent-memory-recall-model", "Local selector model (default: conversation model).", "model"},
        {"agent-question-preview", "User question preview format (markdown or html).", "format", "markdown"},
        {"agent-no-tasks", "Disable persistent task and todo tools for the agent API."},
        {"agent-no-skills", "Disable local skill discovery and invocation."},
        {"agent-no-subagents", "Disable delegated local agent execution."},
        {"agent-no-teams", "Disable persistent local teammates and team messaging."},
        {"agent-profiles", "Private JSON host configuration for profile directories, overrides and model grants.", "file"},
        {"no-agent-profiles", "Disable agent profile file discovery; retain general-purpose."},
        {"agent-subagent-options", "Private JSON file of host-owned child GenerationOptions (temperature, max_tokens, etc.).", "file"},
        {"agent-skills-dir", "Additional host-authorized skills directory; repeat in highest-priority-first order.", "directory"},
        {"agent-no-background", "Disable background shell execution and its control tools."},
        {"agent-no-auto-compact", "Disable automatic agent conversation compaction; explicit compact requests remain available."},
        {"agent-no-project-context", "Disable automatic project instruction loading for the agent API."},
        {"agent-context-exclude", "Exclude a workspace-relative instruction glob; repeat for more patterns.", "pattern"},
        {QStringLiteral("install"), QStringLiteral("Install a local manifest bundle; repeat for multiple bundles"), QStringLiteral("directory")},
        {QStringLiteral("hardware"), QStringLiteral("Inspect startup hardware as JSON and exit")}});
    parser.process(app);
    try {
        auto worker = parser.value(QStringLiteral("mlx-worker"));
        iiLocalLLM::ServiceOptions options;
        options.modelsDirectory = parser.value(QStringLiteral("models-root"));
        options.registryFile = parser.value(QStringLiteral("registry"));
        auto integer = [&](const char* key, quint64 maximum) {
            bool valid = false;
            const auto value = parser.value(QLatin1String(key)).toULongLong(&valid);
            if (!valid || value > maximum) throw std::runtime_error(std::string(key) + " has an invalid integer value");
            return value;
        };
        options.memoryBudgetBytes = integer("memory-budget-mib", 1048576) * 1024 * 1024;
        options.memoryReserveBytes = integer("memory-reserve-mib", 1048576) * 1024 * 1024;
        options.maxModels = int(integer("max-models", 1024));
        options.defaultContextTokens = int(integer("context-tokens", 1048576));
        iiLocalLLM::HttpOptions httpOptions;
        httpOptions.workerThreads = int(integer("http-workers", 64));
        httpOptions.maxControlRequests = int(integer("http-control-requests", 16));
        if (!httpOptions.workerThreads || !httpOptions.maxControlRequests)
            throw std::runtime_error("HTTP capacity must be positive");
        if (!parser.isSet("http-port") && (parser.isSet("http-workers") || parser.isSet("http-control-requests")))
            throw std::runtime_error("HTTP capacity options require --http-port");
        if (parser.isSet("keep-alive")) options.keepAliveMs = iiLocalLLM::parseKeepAlive(parser.value("keep-alive"));
        // Validate private credentials before hardware/driver initialization.
        namespace a = iiLocalLLM::agent;
        std::optional<a::ApiOptions> agentConfig;
        std::shared_ptr<const a::PermissionPolicy> agentPolicy;
        if (parser.isSet("agent-workspace") || parser.isSet("agent-state") || parser.isSet("agent-credentials") || parser.isSet("agent-allow")
            || parser.isSet("agent-no-auto-compact") || parser.isSet("agent-no-project-context") || parser.isSet("agent-context-exclude") || parser.isSet("agent-no-memory")
            || parser.isSet("agent-no-memory-recall") || parser.isSet("agent-memory-recall-model") || parser.isSet("agent-no-memory-extraction") || parser.isSet("agent-no-session-history") || parser.isSet("agent-no-file-checkpoints") || parser.isSet("agent-auto-dream") || parser.isSet("agent-no-web-fetch") || parser.isSet("agent-web-model") || parser.isSet("agent-web-private-origin") || parser.isSet("agent-lsp-config") || parser.isSet("agent-worktree-config") || parser.isSet("agent-no-worktrees")
            || parser.isSet("agent-mcp-config") || parser.isSet("agent-mcp-project") || parser.isSet("agent-mcp-eager")
            || parser.isSet("agent-apps-dir") || parser.isSet("agent-no-apps") || parser.isSet("agent-no-plan-mode") || parser.isSet("agent-no-user-questions") || parser.isSet("agent-question-preview") || parser.isSet("agent-no-tasks") || parser.isSet("agent-no-background")
            || parser.isSet("agent-no-skills") || parser.isSet("agent-skills-dir") || parser.isSet("agent-no-subagents") || parser.isSet("agent-no-teams") || parser.isSet("agent-subagent-options")
            || parser.isSet("agent-profiles") || parser.isSet("no-agent-profiles") || parser.isSet("agent-permission-settings") || parser.isSet("agent-permission-requests") || parser.isSet("agent-add-dir") || parser.isSet("agent-hooks")) {
            if (parser.isSet("agent-apps-dir") && parser.isSet("agent-no-apps"))
                throw std::runtime_error("--agent-apps-dir and --agent-no-apps cannot be combined");
            if (parser.isSet("agent-apps-dir") && parser.value("agent-apps-dir").isEmpty())
                throw std::runtime_error("--agent-apps-dir requires a nonempty directory");
            if (!parser.isSet("agent-workspace") || !parser.isSet("agent-state") || !parser.isSet("agent-credentials"))
                throw std::runtime_error("Agent API requires --agent-workspace, --agent-state and --agent-credentials together");
            a::ApiOptions config; config.workingDirectory = QFileInfo(parser.value("agent-workspace")).canonicalFilePath();
            const auto credentials = QFileInfo(parser.value("agent-credentials")).canonicalFilePath();
            const auto prefix = config.workingDirectory.endsWith('/') ? config.workingDirectory : config.workingDirectory + '/';
            if (config.workingDirectory.isEmpty() || credentials == config.workingDirectory || credentials.startsWith(prefix))
                throw std::runtime_error("Agent workspace must exist and the credential file must be outside it");
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(iiLocalLLMClient::readPrivateFile(parser.value("agent-credentials")), &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) throw std::runtime_error("Agent credentials must be a JSON object");
            const auto entries = document.object();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (!it.value().isString()) throw std::runtime_error("Agent credentials must map client IDs to strings");
                config.clientTokens.insert(it.key(), it.value().toString());
            }
            config.stateDirectory = parser.value("agent-state");
            config.engine.compaction.automatic = !parser.isSet("agent-no-auto-compact");
            config.engine.taskToolsEnabled = !parser.isSet("agent-no-tasks");
            config.engine.planToolsEnabled = !parser.isSet("agent-no-plan-mode");
            config.engine.userQuestionsEnabled = !parser.isSet("agent-no-user-questions");
            config.engine.projectMemoryEnabled = !parser.isSet("agent-no-memory");
            config.engine.memoryRecall.enabled = !parser.isSet("agent-no-memory-recall");
            config.engine.memoryExtraction.enabled = !parser.isSet("agent-no-memory-extraction");
            config.engine.sessionHistoryEnabled = !parser.isSet("agent-no-session-history");
            config.engine.fileCheckpointsEnabled = !parser.isSet("agent-no-file-checkpoints");
            config.engine.worktrees.enabled=!parser.isSet("agent-no-worktrees");
            if(parser.isSet("agent-worktree-config")) {
                if(parser.isSet("agent-no-worktrees"))throw std::runtime_error("Worktree configuration conflicts with disabled worktrees");
                QJsonParseError parse;const auto json=QJsonDocument::fromJson(iiLocalLLMClient::readPrivateFile(parser.value("agent-worktree-config")),&parse);
                if(parse.error!=QJsonParseError::NoError||!json.isObject())throw std::runtime_error("Invalid worktree host configuration");
                config.engine.worktrees=a::worktreeOptionsFromJson(json.object());
            }
            if(parser.isSet("agent-lsp-config")) {
                QJsonParseError parse;const auto json=QJsonDocument::fromJson(iiLocalLLMClient::readPrivateFile(parser.value("agent-lsp-config")),&parse);
                if(parse.error!=QJsonParseError::NoError||!json.isObject())throw std::runtime_error("Invalid LSP host configuration");
                config.engine.lsp=a::lspOptionsFromJson(json.object());
            }
            config.engine.webFetchEnabled = !parser.isSet("agent-no-web-fetch");
            config.engine.webFetch.model = parser.value("agent-web-model");
            config.engine.webFetch.privateOrigins = parser.values("agent-web-private-origin");
            config.engine.memoryDream.automatic = parser.isSet("agent-auto-dream");
            config.engine.memoryRecall.model = parser.value("agent-memory-recall-model");
            config.engine.userQuestions.previewFormat = parser.value("agent-question-preview");
            config.engine.skills.enabled = !parser.isSet("agent-no-skills");
            config.engine.skills.directories = parser.values("agent-skills-dir");
            config.subagentsEnabled = !parser.isSet("agent-no-subagents");
            config.subagents = iiLocalLLMClient::profileConfig(parser.value("agent-profiles"),parser.isSet("no-agent-profiles"));
            a::discoverAgentProfiles(config.workingDirectory,config.subagents.profiles);
            if (parser.isSet("agent-subagent-options")) {
                QJsonParseError parse;
                const auto generation = QJsonDocument::fromJson(iiLocalLLMClient::readPrivateFile(parser.value("agent-subagent-options")), &parse);
                if (parse.error != QJsonParseError::NoError || !generation.isObject())
                    throw std::runtime_error("--agent-subagent-options must contain a JSON GenerationOptions object");
                config.subagents.generation = iiLocalLLM::generationOptionsFromJson(generation.object());
            }
            config.teamsEnabled=!parser.isSet("agent-no-teams");
            config.teams.profiles=config.subagents.profiles;config.teams.definitions=config.subagents.definitions;
            config.teams.allowedModels=config.subagents.allowedModels;config.teams.modelAliases=config.subagents.modelAliases;config.teams.generation=config.subagents.generation;
            config.engine.projectContext.enabled = !parser.isSet("agent-no-project-context");
            config.engine.projectContext.excludes = parser.values("agent-context-exclude");
            // Bound agent dispatch separately from HTTP response admission.
            config.maxConcurrentRequests = 6; config.maxQueuedRequests = 0;
            if(parser.isSet("agent-permission-settings")&&parser.value("agent-permission-settings").isEmpty())
                throw std::runtime_error("--agent-permission-settings requires a file");
            QList<a::PermissionRule> rules;
            for(const auto& value:parser.values("agent-allow"))rules.append({value,a::PermissionBehavior::Allow});
            if(parser.isSet("agent-permission-requests")&&parser.value("agent-permission-requests").isEmpty())throw std::runtime_error("--agent-permission-requests requires a file");
            config.permissionRequests=iiLocalLLMClient::permissionRequestsConfig(parser.value("agent-permission-requests"),config.workingDirectory);
            agentPolicy=iiLocalLLMClient::permissionConfig(parser.value("agent-permission-settings"),config.workingDirectory,rules,{},parser.values("agent-add-dir"),config.permissionRequests?a::PermissionMode::Default:a::PermissionMode::DontAsk);
            if(parser.isSet("agent-hooks")&&parser.value("agent-hooks").isEmpty())throw std::runtime_error("--agent-hooks requires a file");
            config.engine.hooks=iiLocalLLMClient::commandHookConfig(parser.value("agent-hooks"),config.workingDirectory);
            agentConfig = std::move(config);
        }
        iiLocalLLM::Service service(options, {parser.value(QStringLiteral("mlx-python")), worker});
        if (parser.isSet(QStringLiteral("hardware"))) {
            std::cout << QJsonDocument(iiLocalLLM::hardwareObject(service.hardware())).toJson().constData();
            return 0;
        }
        for (const auto& directory : parser.values(QStringLiteral("install"))) {
            const auto model = service.installModel(directory).get();
            std::cout << QJsonDocument(iiLocalLLM::modelRecordObject(model)).toJson(QJsonDocument::Compact).constData() << std::endl;
        }
        if (parser.isSet("install") && !parser.isSet("socket") && !parser.isSet("http-port")) return 0;
        if (parser.isSet(QStringLiteral("config"))) {
            const auto configPath = parser.value(QStringLiteral("config"));
            QFile config(configPath);
            if (!config.open(QIODevice::ReadOnly)) throw std::runtime_error(config.errorString().toStdString());
            if (config.size() > 1024 * 1024) throw std::runtime_error("Config exceeds 1 MiB");
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(config.readAll(), &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject()
                || !document.object().value(QStringLiteral("models")).isArray())
                throw std::runtime_error("Config must contain a models array");
            for (const auto& value : document.object().value(QStringLiteral("models")).toArray()) {
                if (!value.isObject()) throw std::runtime_error("Model must be an object");
                const auto o = value.toObject();
                for (auto it = o.begin(); it != o.end(); ++it) {
                    if (it.key() != QStringLiteral("model")
                        && it.key() != QStringLiteral("context_tokens") && it.key() != QStringLiteral("options") && it.key() != QStringLiteral("keep_alive"))
                        throw iiLocalLLM::Error(iiLocalLLM::ErrorCode::InvalidArgument,
                            QStringLiteral("Unknown model field; execution is selected by the service: ") + it.key());
                }
                const auto ctx = o.value(QStringLiteral("context_tokens"));
                if (!ctx.isUndefined() && (!ctx.isDouble() || ctx.toDouble() != ctx.toInt() || ctx.toInt() < 0 || ctx.toInt() > 1024 * 1024))
                    throw std::runtime_error("context_tokens must be an integer in [0, 1048576]");
                if (o.contains(QStringLiteral("options")) && !o.value(QStringLiteral("options")).isObject())
                    throw std::runtime_error("options must be an object");
                const auto model = service.loadModel({required(o, QStringLiteral("model")),
                    ctx.toInt(0), o.value(QStringLiteral("options")).toObject(), iiLocalLLM::parseKeepAlive(o.value("keep_alive"))}).get();
                std::cout << "Model " << model.model.uri.toStdString() << ": "
                    << QJsonDocument(iiLocalLLM::executionObject(model.execution)).toJson(QJsonDocument::Compact).constData() << std::endl;
            }
        }
        std::shared_ptr<iiLocalLLM::agent::Api> agent;
        if (agentConfig) {
            std::shared_ptr<a::ShellTasks> shells;
#if defined(Q_OS_UNIX) && !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
            if (!parser.isSet("agent-no-background")) shells = std::make_shared<a::ShellTasks>(agentConfig->workingDirectory,
                QDir(agentConfig->stateDirectory).filePath("shells"));
#endif
            a::McpConnectionOptions connections; connections.workingDirectory = agentConfig->workingDirectory;
            for (const auto& path : parser.values("agent-mcp-config")) connections.configFiles.append(QFileInfo(path).absoluteFilePath());
            if (parser.isSet("agent-mcp-project")) connections.configFiles.append(".mcp.json");
            connections.deferTools = !parser.isSet("agent-mcp-eager");
#if defined(Q_OS_UNIX) && !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
            if (!parser.isSet("agent-no-apps")) connections.localApplicationsDirectory = iiLocalLLM::mcp::localApplicationsDirectory();
#endif
            if (parser.isSet("agent-apps-dir")) connections.localApplicationsDirectory = QFileInfo(parser.value("agent-apps-dir")).absoluteFilePath();
            QStringList privatePaths{agentConfig->stateDirectory};
            for(const auto& key:{"agent-credentials","agent-permission-settings","agent-permission-requests","agent-profiles","agent-subagent-options","agent-hooks","agent-lsp-config","agent-worktree-config"})
                if(parser.isSet(key))privatePaths.append(parser.value(key));
            for(const auto& file:connections.configFiles)privatePaths.append(QDir::isAbsolutePath(file)?file:QDir(agentConfig->workingDirectory).filePath(file));
            if(!connections.localApplicationsDirectory.isEmpty())privatePaths.append(connections.localApplicationsDirectory);
            agentConfig->engine.lsp.protectedPaths=privatePaths;
            auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, agentConfig->workingDirectory, shells,privatePaths);
            if (!connections.configFiles.isEmpty() || !connections.localApplicationsDirectory.isEmpty())
                agentConfig->mcp = std::make_shared<a::McpConnections>(registry, std::move(connections));
            agent = std::make_shared<a::Api>(std::make_shared<a::ServiceModel>(service), registry, agentPolicy, std::move(*agentConfig));
        }
        iiLocalLLM::LocalIpcServer server(service); server.setRpcHandler(agent);
        if (parser.isSet(QStringLiteral("socket")) || !parser.isSet(QStringLiteral("http-port"))) {
            if (!server.listen(parser.isSet("socket") ? parser.value("socket") : iiLocalLLMClient::defaultEndpoint())) throw std::runtime_error(server.errorString().toStdString());
            std::cout << "iiLocalLLM listening: " << server.serverName().toStdString() << std::endl;
        }
        iiLocalLLM::HttpApiServer http(service, httpOptions); http.setRpcHandler(agent);
        if (parser.isSet(QStringLiteral("http-port"))) {
            bool valid = false;
            const auto port = parser.value(QStringLiteral("http-port")).toUInt(&valid);
            if (!valid || port > 65535) throw std::runtime_error("http-port must be an integer in [0, 65535]");
            if (!http.listen(quint16(port))) throw std::runtime_error(http.errorString().toStdString());
            std::cout << "iiLocalLLM HTTP: http://127.0.0.1:" << http.port() << std::endl;
        }
        std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt);
        QTimer shutdown;
        QObject::connect(&shutdown, &QTimer::timeout, &app, [&] { if (interrupted) app.quit(); });
        shutdown.start(100);
        return app.exec();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
