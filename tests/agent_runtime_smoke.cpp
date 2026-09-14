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
    if (argc != 2 && argc != 4 && argc != 5) return 2;
    const bool remote = argc >= 4;
    const bool configured = argc == 5;
    const bool discovery = argc == 5 && QString::fromLocal8Bit(argv[4]) == "--discovery";
    if (configured && !discovery && QString::fromLocal8Bit(argv[4]) != "--configured-eager") return 2;
    try {
        QTemporaryDir root(QDir::current().filePath("agent-native-XXXXXX"));
        if (!root.isValid()) throw std::runtime_error("Cannot create native agent fixture");
        const auto modelRoot = root.filePath("Models/agent-fixture"); QDir().mkpath(modelRoot);
        const auto weights = QDir(modelRoot).filePath("model.gguf");
        std::error_code error;
        std::filesystem::create_hard_link(argv[1], weights.toStdString(), error);
        if (error && !QFile::copy(QString::fromLocal8Bit(argv[1]), weights)) throw std::runtime_error("Cannot provision fixture weights");
        const QJsonObject manifest{{"schema_version", 1}, {"id", "agent-fixture"}, {"architecture", "qwen2"},
            {"format", "gguf"}, {"quantization", "Q4_K_M"}, {"context_length", 32768}, {"entry_point", "model.gguf"},
            {"capabilities", QJsonArray{"text-generation", "chat"}},
            {"files", QJsonArray{QJsonObject{{"path", "model.gguf"}, {"size", 491400032},
                {"sha256", "74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db"}}}}};
        QFile metadata(QDir(modelRoot).filePath("manifest.json"));
        if (!metadata.open(QIODevice::WriteOnly) || metadata.write(QJsonDocument(manifest).toJson()) < 1)
            throw std::runtime_error("Cannot create fixture manifest");
        metadata.close();
        const auto workspace = root.filePath("workspace"); QDir().mkpath(workspace);
        // Keep the observed faithfulness regression as well as a new unpredictable
        // value. Neither value is placed in the prompt or the tool schema.
        const QStringList secrets{"LOCAL_8a6b9c10f8d2", "LOCAL_" + QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(12)};
        QFile input(QDir(workspace).filePath("secret.txt"));
        ServiceOptions serviceOptions; serviceOptions.modelsDirectory = root.filePath("Models");
        Service service(serviceOptions);
        (void)service.loadModel({"model://agent-fixture", 4096}).get();
        auto registry = std::make_shared<a::ToolRegistry>();
        std::unique_ptr<a::McpConnections> connections;
        QString toolName = "Read";
        if (configured) {
            const auto path = root.filePath("mcp.json"); QFile config(path);
            if (!config.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot write MCP fixture config");
            config.write(QJsonDocument(QJsonObject{{"mcpServers", QJsonObject{{"fixture", QJsonObject{
                {"command", QString::fromLocal8Bit(argv[2])}, {"args", QJsonArray{"-B", QString::fromLocal8Bit(argv[3]), input.fileName()}},
                {"appId", "com.iisacc.fixture"}}}}}}).toJson()); config.close();
            a::McpConnectionOptions o; o.workingDirectory = workspace; o.configFiles = {path};
            o.deferTools = discovery;
            connections = std::make_unique<a::McpConnections>(registry, o);
            if (connections->status().first().toObject()["state"] != "ready") throw std::runtime_error("Configured MCP peer did not connect");
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
            auto session = engine.createSession("model://agent-fixture", workspace);
            a::RunRequest request{session.id, discovery
                ? "First call ToolSearch with query select:mcp__fixture__read_secret. Then call mcp__fixture__read_secret to read the secret. Then return its exact value as your final answer. Do not guess."
                : remote
                ? "Use the mcp__fixture__read_secret tool to read the secret. Then return its exact value as your final answer. Do not guess."
                : "Use the Read tool to read secret.txt. Then return the exact file contents as your final answer. Do not guess."};
            request.generation.temperature = 0; request.generation.maxTokens = 512; request.maxTurns = discovery ? 6 : 4;
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
