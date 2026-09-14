#include "agent/Engine.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <filesystem>
#include <iostream>
using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
namespace {
void put(const QString& path, const QByteArray& value) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) throw std::runtime_error("mkdir failed");
    QFile file(path); if (!file.open(QIODevice::WriteOnly) || file.write(value) != value.size()) throw std::runtime_error("write failed");
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const bool catalog = argc == 4 && QString::fromLocal8Bit(argv[1]) == "--catalog";
    if (argc != 2 && !catalog) return 2;
    try {
        QTemporaryDir root(QDir::current().filePath("skills-native-XXXXXX")); if (!root.isValid()) return 1;
        const auto models = root.filePath("Models"), package = models + "/skill-fixture";
        QDir().mkpath(package);
        ModelManifest manifest{"skill-fixture", "qwen2", "gguf", "Q4_K_M", 32768, {"text-generation", "chat"}, "model.gguf",
            {{"model.gguf", 491400032, "74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db"}}};
        QString source;
        if (catalog) {
            const auto uri = QString::fromLocal8Bit(argv[3]); source = QDir(QString::fromLocal8Bit(argv[2])).filePath(modelId(uri));
            QFile file(QDir(source).filePath("manifest.json"));
            if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) throw std::runtime_error("Cannot read model manifest");
            manifest = parseModelManifest(QJsonDocument::fromJson(file.readAll()).object());
            if (modelUri(manifest.id) != uri) throw std::runtime_error("Model identity mismatch");
            manifest.id = "skill-fixture";
        }
        for (const auto& file : manifest.files) {
            const auto from = catalog ? QDir(source).filePath(file.path) : QString::fromLocal8Bit(argv[1]);
            const auto to = QDir(package).filePath(file.path); QDir().mkpath(QFileInfo(to).absolutePath());
            std::filesystem::create_hard_link(from.toStdString(), to.toStdString());
        }
        put(package + "/manifest.json", QJsonDocument(manifestObject(manifest)).toJson());
        const auto workspace = root.filePath("workspace");
        put(workspace + "/.claude/skills/inspect/SKILL.md", "---\ndescription: Read a file and return its exact contents.\nargument-hint: '[filename]'\n---\nUse the Read tool to read $0. Then return the exact file contents as your final answer. Do not guess.\n");
        ServiceOptions so; so.modelsDirectory = models;
        if (catalog) so.maxCachedContexts = 1; // Sequential qualification needs only one KV context.
        Service service(so);
        const auto uri = modelUri(manifest.id); ModelLoadRequest load{uri, 8192};
        if (catalog) load.options = {{"enable_thinking", false}, {"tool_grammar", false}};
        std::cout << QJsonDocument(QJsonObject{{"qualification", catalog ? "catalog" : "qwen2.5-0.5b"},
            {"max_cached_contexts", so.maxCachedContexts}, {"context_tokens", 8192}, {"load_options", load.options}}).toJson(QJsonDocument::Compact).constData() << std::endl;
        (void)service.loadModel(load).get();
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, workspace);
        for (const auto& tool : registry->definitions()) if (tool.name != "Read") registry->remove(tool.name);
        a::EngineOptions eo; eo.sessionsDirectory = root.filePath("sessions"); eo.projectContext.enabled = false; eo.compaction.automatic = false;
        a::Engine engine(std::make_shared<a::ServiceModel>(service), registry, std::make_shared<a::RulePolicy>(), eo);
        for (bool modelInvocation : {false, true}) {
            const auto secret = "SKILL_" + QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(16);
            put(workspace + "/secret.txt", secret.toUtf8());
            auto session = engine.createSession(uri, workspace);
            a::RunRequest request{session.id}; request.generation.temperature = 0; request.generation.maxTokens = catalog ? 2048 : 512; request.maxTurns = 6;
            if (modelInvocation) request.prompt = "First call the Skill tool with skill inspect and args secret.txt. Then follow the loaded skill instructions. Return the exact file contents as your final answer.";
            else { request.skill = "inspect"; request.skillArguments = "secret.txt"; }
            bool read = false, skill = false;
            const auto result = engine.run(request, [&](const a::Event& event) {
                if (event.kind == a::EventKind::ToolStarted) { read |= event.data["name"] == "Read"; skill |= event.data["name"] == "Skill"; }
                if (event.kind != a::EventKind::ModelDelta) std::cout << QJsonDocument(a::toJson(event)).toJson(QJsonDocument::Compact).constData() << '\n';
            }).result.get();
            const auto transcript = engine.session(session.id); bool injected = false, observed = false;
            for (const auto& message : transcript.messages) {
                injected |= message.role == a::MessageRole::User && message.metadata.contains("iilocal.skill") && message.text.contains("Read tool to read secret.txt");
                observed |= message.role == a::MessageRole::Tool && !message.isError && message.text.contains(secret);
            }
            const bool passed = result.status == a::RunStatus::Completed && read && injected && observed && result.text.contains(secret)
                && (!modelInvocation || skill) && a::pendingToolCalls(transcript.messages).isEmpty();
            std::cout << QJsonDocument(QJsonObject{{"mode", modelInvocation ? "model" : "user"}, {"passed", passed},
                {"read_called", read}, {"skill_called", skill}, {"injected", injected}, {"observed", observed}, {"result", a::toJson(result)}}).toJson(QJsonDocument::Compact).constData() << std::endl;
            if (!passed) return 1;
        }
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
