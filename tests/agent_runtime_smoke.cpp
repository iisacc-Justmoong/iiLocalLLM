#include "agent/Engine.h"
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
    if (argc != 2) return 2;
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
        const auto secret = "LOCAL_" + QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(12);
        QFile input(QDir(workspace).filePath("secret.txt"));
        if (!input.open(QIODevice::WriteOnly) || input.write(secret.toUtf8()) < 1) throw std::runtime_error("Cannot write secret");
        input.close();
        ServiceOptions serviceOptions; serviceOptions.modelsDirectory = root.filePath("Models");
        Service service(serviceOptions);
        (void)service.loadModel({"model://agent-fixture", 4096}).get();
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, workspace);
        for (const auto& tool : registry->definitions()) if (tool.name != "Read") registry->remove(tool.name);
        a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions");
        a::Engine engine(std::make_shared<a::ServiceModel>(service), registry, std::make_shared<a::RulePolicy>(), options);
        auto session = engine.createSession("model://agent-fixture", workspace);
        a::RunRequest request{session.id, "Use the Read tool to read secret.txt. Then return the exact file contents as your final answer. Do not guess."};
        request.generation.temperature = 0; request.generation.maxTokens = 512; request.maxTurns = 4;
        bool read = false;
        auto result = engine.run(request, [&](const a::Event& event) {
            if (event.kind == a::EventKind::ToolStarted && event.data["name"] == "Read") read = true;
            if (event.kind != a::EventKind::ModelDelta) std::cout << QJsonDocument(a::toJson(event)).toJson(QJsonDocument::Compact).constData() << '\n';
        }).result.get();
        if (result.status != a::RunStatus::Completed || !read || !result.text.contains(secret))
            throw std::runtime_error(("Local model/tool/final answer acceptance failed: " + result.errorMessage + " / " + result.text).toStdString());
        if (!a::pendingToolCalls(engine.session(session.id).messages).isEmpty()) throw std::runtime_error("Unpaired tool calls");
        if (result.usage.generatedTokens < 1 || result.turns < 2) throw std::runtime_error("Missing inference evidence");
        std::cout << "Native local model selected Read, consumed an unpredictable file value, and completed the agent turn.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
