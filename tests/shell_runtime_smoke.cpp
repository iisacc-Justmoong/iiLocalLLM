#include <agent/Engine.h>
#include <agent/ShellTasks.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <filesystem>
#include <iostream>
using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); const auto args = app.arguments();
    if (args.size() < 4 || args.size() > 6 || args[1] != "--catalog") return 2;
    bool noThink = false, noGrammar = false;
    for (int n = 4; n < args.size(); ++n) {
        if (args[n] == "--no-think" && !noThink) noThink = true;
        else if (args[n] == "--no-tool-grammar" && !noGrammar) noGrammar = true;
        else return 2;
    }
    try {
        QTemporaryDir root(QDir::current().filePath("shell-native-XXXXXX"));
        if (!root.isValid()) throw std::runtime_error("Cannot create native shell fixture");
        const auto source = QDir(args[2]).filePath(modelId(args[3])); QFile manifest(QDir(source).filePath("manifest.json"));
        if (!manifest.open(QIODevice::ReadOnly) || manifest.size() > 1024 * 1024) throw std::runtime_error("Cannot read model manifest");
        const auto model = parseModelManifest(QJsonDocument::fromJson(manifest.readAll()).object());
        if (modelUri(model.id) != args[3]) throw std::runtime_error("Model identity mismatch");
        const auto directory = root.filePath("models/" + model.id); QDir().mkpath(directory);
        for (const auto& file : model.files) {
            const auto from = QDir(source).filePath(file.path), to = QDir(directory).filePath(file.path);
            QDir().mkpath(QFileInfo(to).absolutePath()); std::error_code error;
            std::filesystem::create_hard_link(from.toStdString(), to.toStdString(), error);
            if (error && !QFile::copy(from, to)) throw std::runtime_error("Cannot provision model fixture");
        }
        QFile copied(QDir(directory).filePath("manifest.json"));
        if (!copied.open(QIODevice::WriteOnly) || copied.write(QJsonDocument(manifestObject(model)).toJson()) < 1) return 3;
        copied.close();
        const auto workspace = root.filePath("workspace"); QDir().mkpath(workspace);
        const auto secret = "SHELL_" + QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(12);
        QFile observation(QDir(workspace).filePath("observation.txt"));
        if (!observation.open(QIODevice::WriteOnly) || observation.write(secret.toUtf8()) < 1) return 4;
        observation.close();
        ServiceOptions so; so.modelsDirectory = root.filePath("models"); Service service(so);
        ModelLoadRequest load{args[3], 8192}; if (noGrammar) load.options["tool_grammar"] = false;
        service.loadModel(load).get(); // Verifies every pinned manifest file before loading.
        auto shells = std::make_shared<a::ShellTasks>(workspace, root.filePath("shells"));
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, workspace, shells);
        for (const auto* name : {"TaskOutput", "TaskStop", "ShellTaskList"}) {
            auto tool = registry->get(name); tool.definition.deferred = false; registry->remove(name); registry->add(std::move(tool));
        }
        a::EngineOptions eo; eo.sessionsDirectory = root.filePath("sessions"); eo.projectContext.enabled = false; eo.compaction.automatic = false;
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Default, QList<a::PermissionRule>{{"Bash", a::PermissionBehavior::Allow}});
        a::Engine engine(std::make_shared<a::ServiceModel>(service), registry, policy, eo);
        const auto id = engine.createSession(args[3], workspace).id;
        a::RunRequest request{id, "Call Bash once with command \"sleep 0.1; cat observation.txt\" and run_in_background true. "
            "Then call TaskOutput with the returned backgroundTaskId as task_id and block true. "
            "Return only the exact output from TaskOutput after the task completes. Do not use Read or any other tool."};
        if (noThink) request.prompt += " /no_think";
        request.maxTurns = 6; request.generation.maxTokens = 2048;
        request.generation.temperature = noThink ? 0.7 : 0.6; request.generation.topP = noThink ? 0.8 : 0.95;
        request.generation.topK = 20; request.generation.seed = 0;
        const auto result = engine.run(request, [&](const a::Event& event) {
            if (event.kind != a::EventKind::ModelDelta) std::cout << QJsonDocument(a::toJson(event)).toJson(QJsonDocument::Compact).constData() << '\n';
        }).result.get();
        const auto jobs = shells->list(id); int starts = 0, reads = 0; bool wrong = false;
        const auto jobId = jobs.isEmpty() ? QString() : jobs[0].toObject()["task_id"].toString();
        for (const auto& message : engine.session(id).messages) for (const auto& call : message.toolCalls) {
            if (call.name == "Bash" && call.arguments["run_in_background"] == true
                && call.arguments["command"] == "sleep 0.1; cat observation.txt") ++starts;
            else if (call.name == "TaskOutput" && call.arguments["task_id"] == jobId) ++reads;
            else wrong = true;
        }
        if (result.status != a::RunStatus::Completed || !result.text.contains(secret) || result.turns < 3 || starts != 1 || reads < 1 || wrong
            || jobs.size() != 1 || jobs[0].toObject()["status"] != "completed" || !a::pendingToolCalls(engine.session(id).messages).isEmpty())
            throw std::runtime_error(("Native background execution was not verified: " + result.text + " / " + result.errorMessage).toStdString());
        std::cout << "Native model started one real background shell, retrieved its output by actual task ID, and answered with an unknown file value.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
