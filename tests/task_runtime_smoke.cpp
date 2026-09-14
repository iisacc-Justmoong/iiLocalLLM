#include <agent/Engine.h>
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
    QCoreApplication application(argc, argv);
    const auto args = application.arguments();
    const bool catalog = args.size() >= 4 && args.size() <= 8 && args[1] == "--catalog";
    bool deferred = false, noThink = false, noToolGrammar = false, disableThinking = false;
    if (!catalog && argc != 2) return 2;
    if (catalog) for (int n = 4; n < args.size(); ++n) {
        if (args[n] == "--deferred" && !deferred) deferred = true;
        else if (args[n] == "--no-think" && !noThink) noThink = true;
        else if (args[n] == "--no-tool-grammar" && !noToolGrammar) noToolGrammar = true;
        else if (args[n] == "--disable-thinking" && !disableThinking) disableThinking = true;
        else return 2;
    }
    try {
        QTemporaryDir root(QDir::current().filePath("task-native-XXXXXX"));
        if (!root.isValid()) throw std::runtime_error("Cannot create task inference fixture");
        ModelManifest model{"task-fixture", "qwen2", "gguf", "Q4_K_M", 32768, {"text-generation", "chat"}, "model.gguf",
            {{"model.gguf", 491400032, "74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db"}}};
        QString source;
        if (catalog) {
            source = QDir(args[2]).filePath(modelId(args[3])); QFile file(QDir(source).filePath("manifest.json"));
            if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) throw std::runtime_error("Cannot read agent model manifest");
            model = parseModelManifest(QJsonDocument::fromJson(file.readAll()).object());
            if (modelUri(model.id) != args[3]) throw std::runtime_error("Agent model identity mismatch");
        }
        const auto directory = root.filePath("models/" + model.id); QDir().mkpath(directory);
        for (const auto& file : model.files) {
            const auto from = catalog ? QDir(source).filePath(file.path) : QString::fromLocal8Bit(argv[1]);
            const auto to = QDir(directory).filePath(file.path); QDir().mkpath(QFileInfo(to).absolutePath());
            std::error_code error; std::filesystem::create_hard_link(from.toStdString(), to.toStdString(), error);
            if (error && !QFile::copy(from, to)) throw std::runtime_error("Cannot provision model");
        }
        QFile manifest(QDir(directory).filePath("manifest.json"));
        if (!manifest.open(QIODevice::WriteOnly) || manifest.write(QJsonDocument(manifestObject(model)).toJson()) < 1)
            throw std::runtime_error("Cannot write model manifest");
        manifest.close();
        const auto workspace = root.filePath("workspace"); QDir().mkpath(workspace);
        ServiceOptions serviceOptions; serviceOptions.modelsDirectory = root.filePath("models");
        Service service(serviceOptions); const auto uri = modelUri(model.id);
        ModelLoadRequest load{uri, 8192};
        if (noToolGrammar) load.options["tool_grammar"] = false;
        if (disableThinking) load.options["enable_thinking"] = false;
        // ModelManager::load verifies every manifest file before loading. Do not
        // hash a multi-GB model a second time solely for this acceptance fixture.
        const auto loaded = service.loadModel(load).get();
        std::cout << QJsonDocument(QJsonObject{{"model", uri}, {"loaded", loaded.model.loaded},
            {"load_options", loaded.options}}).toJson(QJsonDocument::Compact).constData() << '\n';
        a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions"); options.taskToolsEnabled = true;
        options.taskToolsDeferred = deferred; options.projectContext.enabled = false; options.compaction.automatic = false;
        a::Engine engine(std::make_shared<a::ServiceModel>(service), std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), options);
        auto run = [&](const QString& session, const QString& prompt, const QString& expectedTool) {
            auto instruction = deferred ? "First call ToolSearch with query select:" + expectedTool + ". Then " + prompt : prompt;
            if (noThink) instruction += " /no_think";
            a::RunRequest request{session, instruction};
            request.generation.temperature = catalog ? (noThink ? 0.7 : 0.6) : 0;
            request.generation.maxTokens = catalog ? 2048 : 512; request.maxTurns = 6;
            if (catalog) { request.generation.topK = 20; request.generation.topP = noThink ? 0.8 : 0.95; }
            int calls = 0, searches = 0;
            const auto result = engine.run(request, [&](const a::Event& e) {
                if (e.kind == a::EventKind::ToolStarted && e.data["name"] == expectedTool) ++calls;
                if (e.kind == a::EventKind::ToolStarted && e.data["name"] == "ToolSearch") ++searches;
                if (e.kind != a::EventKind::ModelDelta)
                    std::cout << QJsonDocument(a::toJson(e)).toJson(QJsonDocument::Compact).constData() << '\n';
            }).result.get();
            if (result.status != a::RunStatus::Completed || calls < 1 || (expectedTool == "TaskCreate" && calls != 1)
                || result.turns < 2 || result.usage.generatedTokens < 1
                || (deferred && (searches < 1 || result.turns < 3))
                || !a::pendingToolCalls(engine.session(session).messages).isEmpty())
                throw std::runtime_error(("Native task call failed: " + result.errorMessage + " / " + result.text).toStdString());
            return result;
        };
        const auto readSession = engine.createSession(uri, workspace).id;
        const auto secret = "TASK_" + QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(12);
        engine.runTaskTool(readSession, "TaskCreate", {{"subject", "Inspect artifact"}, {"description", secret}});
        // Description is absent from the live task preview and the prompt/schema.
        const auto answer = run(readSession, "Use TaskGet with taskId 1. Return only the exact description from its actual result.", "TaskGet");
        if (!answer.text.contains(secret)) throw std::runtime_error("Native model did not consume the actual task description");
        const auto session = engine.createSession(uri, workspace).id;
        run(session, "Call TaskCreate once with subject \"Verify export\" and description \"Run installed tests\". Then report the new task ID.", "TaskCreate");
        auto listed = engine.runTaskTool(session, "TaskList").data;
        if (listed["total"] != 1 || listed["tasks"].toArray()[0].toObject()["subject"] != "Verify export")
            throw std::runtime_error("Native TaskCreate did not persist the requested task");
        run(session, "Use TaskUpdate for taskId \"1\" to set status \"in_progress\" and owner \"builder\". Change only these two fields, then confirm the update.", "TaskUpdate");
        const auto task = engine.runTaskTool(session, "TaskGet", {{"taskId", "1"}}).data["task"].toObject();
        if (task["status"] != "in_progress" || task["owner"] != "builder" || task["description"] != "Run installed tests" || task["subject"] != "Verify export")
            throw std::runtime_error("Native TaskUpdate did not persist the requested state");
        std::cout << "Native " << uri.toStdString() << (deferred ? " deferred" : " eager")
            << (noThink ? " /no_think" : " default thinking")
            << (noToolGrammar ? " unconstrained generation" : " schema grammar")
            << (disableThinking ? " thinking template disabled" : " default thinking template")
            << " read an unknown task description, created one task, and persisted an owner/status update.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
