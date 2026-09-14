#include "agent/Engine.h"
#include "agent/McpTools.h"
#include "agent/McpConnections.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <filesystem>
#include <iostream>
using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const bool catalog = argc == 7 && QString::fromLocal8Bit(argv[1]) == "--catalog"
        && QString::fromLocal8Bit(argv[6]) == "--thinking-control";
    if (!catalog && argc != 2 && argc != 4 && argc != 5) return 2;
    const bool remote = argc >= 4;
    const bool configured = catalog || argc == 5;
    const bool discovery = catalog || (argc == 5 && QString::fromLocal8Bit(argv[4]) == "--discovery");
    if (configured && !discovery && QString::fromLocal8Bit(argv[4]) != "--configured-eager") return 2;
    try {
        QTemporaryDir root(QDir::current().filePath("agent-native-XXXXXX"));
        if (!root.isValid()) throw std::runtime_error("Cannot create native agent fixture");
        ModelManifest model{"agent-fixture", "qwen2", "gguf", "Q4_K_M", 32768, {"text-generation", "chat"}, "model.gguf",
            {{"model.gguf", 491400032, "74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db"}}};
        QString source;
        if (catalog) {
            const auto uri = QString::fromLocal8Bit(argv[3]);
            source = QDir(QString::fromLocal8Bit(argv[2])).filePath(modelId(uri));
            QFile file(QDir(source).filePath("manifest.json"));
            if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) throw std::runtime_error("Cannot read model manifest");
            model = parseModelManifest(QJsonDocument::fromJson(file.readAll()).object());
            if (modelUri(model.id) != uri) throw std::runtime_error("Model identity mismatch");
        }
        const auto modelRoot = root.filePath("Models/" + model.id); QDir().mkpath(modelRoot);
        for (const auto& file : model.files) {
            const auto from = catalog ? QDir(source).filePath(file.path) : QString::fromLocal8Bit(argv[1]);
            const auto to = QDir(modelRoot).filePath(file.path); QDir().mkpath(QFileInfo(to).absolutePath());
            std::error_code error; std::filesystem::create_hard_link(from.toStdString(), to.toStdString(), error);
            if (error && !QFile::copy(from, to)) throw std::runtime_error("Cannot provision fixture weights");
        }
        QFile metadata(QDir(modelRoot).filePath("manifest.json"));
        if (!metadata.open(QIODevice::WriteOnly) || metadata.write(QJsonDocument(manifestObject(model)).toJson()) < 1)
            throw std::runtime_error("Cannot create fixture manifest");
        metadata.close();
        const auto workspace = root.filePath("workspace"); QDir().mkpath(workspace);
        // Keep the observed faithfulness regression as well as a new unpredictable
        // value. Neither value is placed in the prompt or the tool schema.
        const QStringList secrets{"LOCAL_8a6b9c10f8d2", "LOCAL_" + QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(12)};
        QFile input(QDir(workspace).filePath("secret.txt"));
        ServiceOptions serviceOptions; serviceOptions.modelsDirectory = root.filePath("Models");
        Service service(serviceOptions);
        const auto uri = modelUri(model.id);
        ModelLoadRequest load{uri, catalog ? 8192 : 4096};
        if (catalog) load.options = {{"enable_thinking", false}, {"tool_grammar", false}};
        (void)service.loadModel(load).get();
        auto registry = std::make_shared<a::ToolRegistry>();
        std::unique_ptr<a::McpConnections> connections;
        QString toolName = "Read";
        if (configured) {
            const auto path = root.filePath("mcp.json"); QFile config(path);
            if (!config.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot write MCP fixture config");
            config.write(QJsonDocument(QJsonObject{{"mcpServers", QJsonObject{{"fixture", QJsonObject{
                {"command", QString::fromLocal8Bit(argv[catalog ? 4 : 2])}, {"args", QJsonArray{"-B", QString::fromLocal8Bit(argv[catalog ? 5 : 3]), input.fileName()}},
                {"appId", "com.iisacc.fixture"}}}}}}).toJson()); config.close();
            a::McpConnectionOptions o; o.workingDirectory = workspace; o.configFiles = {path};
            o.deferTools = discovery;
            connections = std::make_unique<a::McpConnections>(registry, o);
            const auto connectionState = connections->status();
            if (connectionState.isEmpty() || connectionState.first().toObject().value("state") != "ready")
                throw std::runtime_error("Configured MCP peer did not connect: " + QJsonDocument(connectionState).toJson(QJsonDocument::Compact).toStdString());
            toolName = "mcp__fixture__read_secret";
        } else if (remote) {
            mcp::StdioOptions transport; transport.program = QString::fromLocal8Bit(argv[2]);
            transport.arguments = {"-B", QString::fromLocal8Bit(argv[3]), input.fileName()};
            auto client = std::make_shared<mcp::StdioClient>(transport);
            toolName = "mcp__fixture__read_secret";
            for (auto tool : a::mcpTools(client, {"fixture", "com.iisacc.fixture", true}))
                if (tool.definition.name == toolName) registry->add(std::move(tool));
        } else {
            a::registerWorkspaceTools(*registry, workspace);
            for (const auto& tool : registry->definitions()) if (tool.name != toolName) registry->remove(tool.name);
        }
        a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions");
        a::Engine engine(std::make_shared<a::ServiceModel>(service), registry, std::make_shared<a::RulePolicy>(
            a::PermissionMode::DontAsk, QList<a::PermissionRule>{{toolName, a::PermissionBehavior::Allow}}), options);
        for (const auto& secret : secrets) {
            if (!input.open(QIODevice::WriteOnly | QIODevice::Truncate) || input.write(secret.toUtf8()) < 1)
                throw std::runtime_error("Cannot write secret");
            input.close();
            auto session = engine.createSession(uri, workspace);
            a::RunRequest request{session.id, discovery
                ? "First call ToolSearch with query select:mcp__fixture__read_secret. Then call mcp__fixture__read_secret to read the secret. Then return its exact value as your final answer. Do not guess."
                : remote
                ? "Use the mcp__fixture__read_secret tool to read the secret. Then return its exact value as your final answer. Do not guess."
                : "Use the Read tool to read secret.txt. Then return the exact file contents as your final answer. Do not guess."};
            request.generation.temperature = 0; request.generation.maxTokens = 512; request.maxTurns = discovery ? 6 : 4;
            if (catalog) {
                request.prompt += " /no_think";
                request.generation.temperature = 0.7; request.generation.topP = 0.8;
                request.generation.topK = 20; request.generation.maxTokens = 2048;
            }
            bool read = false; int progress = 0;
            auto result = engine.run(request, [&](const a::Event& event) {
                if (event.kind == a::EventKind::ToolStarted && event.data["name"] == toolName) read = true;
                if (event.kind == a::EventKind::ToolProgress) ++progress;
                if (event.kind != a::EventKind::ModelDelta) std::cout << QJsonDocument(a::toJson(event)).toJson(QJsonDocument::Compact).constData() << '\n';
            }).result.get();
            if (result.status != a::RunStatus::Completed || !read || !result.text.contains(secret))
                throw std::runtime_error(("Local model/tool/final answer acceptance failed: " + result.errorMessage + " / " + result.text).toStdString());
            if (!a::pendingToolCalls(engine.session(session.id).messages).isEmpty()) throw std::runtime_error("Unpaired tool calls");
            if (result.usage.generatedTokens < 1 || result.turns < 2) throw std::runtime_error("Missing inference evidence");
            if (remote && progress < 2) throw std::runtime_error("Missing MCP progress evidence");
            if (discovery) {
                const auto messages = engine.session(session.id).messages;
                bool selected = false;
                for (const auto& message : messages) if (message.role == a::MessageRole::Tool && !message.isError
                    && !message.metadata["iilocal.tool_search"].toObject()["entries"].toArray().isEmpty()) selected = true;
                if (!selected || result.turns < 3) throw std::runtime_error("Missing successful native ToolSearch evidence");
            }
            std::cout << "Native local model selected " << toolName.toStdString() << ", consumed the actual file value, and completed the agent turn.\n";
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
