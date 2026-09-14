#include <agent/Engine.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <filesystem>
#include <iostream>
#include <limits>
#include <thread>
#include <cerrno>
#include <signal.h>
using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
using namespace std::chrono_literals;
namespace {
void require(bool value, const QString& message) { if (!value) throw std::runtime_error(message.toStdString()); }
void write(const QString& path, const QString& text) {
    QFile file(path); require(file.open(QIODevice::WriteOnly) && file.write(text.toUtf8()) == text.toUtf8().size(), "Cannot create input fixture");
}
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
QString secret() { return "INPUT_" + QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(12); }
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); const auto args = app.arguments();
    if (args.size() != 4 || args[1] != "--catalog") return 2;
    try {
        QTemporaryDir root(QDir::current().filePath("input-native-XXXXXX"));
        require(root.isValid(), "Cannot create native input fixture");
        const auto source = QDir(args[2]).filePath(modelId(args[3])); QFile manifest(QDir(source).filePath("manifest.json"));
        require(manifest.open(QIODevice::ReadOnly) && manifest.size() <= 1024 * 1024, "Cannot read model manifest");
        const auto model = parseModelManifest(QJsonDocument::fromJson(manifest.readAll()).object());
        require(modelUri(model.id) == args[3], "Model identity mismatch");
        const auto directory = root.filePath("models/" + model.id); QDir().mkpath(directory);
        for (const auto& file : model.files) {
            const auto from = QDir(source).filePath(file.path), to = QDir(directory).filePath(file.path);
            QDir().mkpath(QFileInfo(to).absolutePath()); std::error_code error;
            std::filesystem::create_hard_link(from.toStdString(), to.toStdString(), error);
            require(!error || QFile::copy(from, to), "Cannot provision model fixture");
        }
        write(QDir(directory).filePath("manifest.json"), QString::fromUtf8(QJsonDocument(manifestObject(model)).toJson()));
        const auto workspace = root.filePath("workspace"); QDir().mkpath(workspace);
        const auto first = secret(), next = secret(), urgent = secret();
        write(QDir(workspace).filePath("first.txt"), first); write(QDir(workspace).filePath("next.txt"), next);
        write(QDir(workspace).filePath("urgent.txt"), urgent);
        ServiceOptions so; so.modelsDirectory = root.filePath("models"); Service service(so);
        ModelLoadRequest load{args[3], 8192}; load.options = {{"tool_grammar", false}, {"enable_thinking", false}};
        service.loadModel(load).get();
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, workspace);
        a::EngineOptions eo; eo.sessionsDirectory = root.filePath("sessions"); eo.projectContext.enabled = false; eo.compaction.automatic = false;
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Default, QList<a::PermissionRule>{{"Bash", a::PermissionBehavior::Allow}});
        a::Engine engine(std::make_shared<a::ServiceModel>(service), registry, policy, eo);
        auto request = [&](const QString& id, const QString& prompt) {
            a::RunRequest value{id, prompt}; value.maxTurns = 6; value.generation.maxTokens = 2048;
            value.generation.temperature = 0.7; value.generation.topP = 0.8; value.generation.topK = 20; value.generation.seed = 0; return value;
        };
        const auto log = [](const a::Event& event) {
            if (event.kind != a::EventKind::ModelDelta) std::cout << QJsonDocument(a::toJson(event)).toJson(QJsonDocument::Compact).constData() << std::endl;
        };
        const auto nextId = engine.createSession(args[3], workspace).id; bool published = false;
        const auto nextResult = engine.run(request(nextId, "Use Read to read first.txt and return only its exact contents."), [&](const a::Event& event) {
            log(event);
            if (event.kind == a::EventKind::ToolFinished && !published) {
                published = true; engine.enqueueInput(nextId, {{"text", "Now use Read to read next.txt and return only its exact contents."}, {"priority", "next"}});
            }
        }).result.get();
        const auto nextMessages = engine.session(nextId).messages; int firstReads = 0, nextReads = 0, delivered = 0;
        for (const auto& message : nextMessages) {
            if (message.metadata.contains("iilocal.input")) ++delivered;
            for (const auto& call : message.toolCalls) if (call.name == "Read") {
                if (call.arguments["path"] == "first.txt") ++firstReads;
                if (call.arguments["path"] == "next.txt") ++nextReads;
            }
        }
        const bool nextExact = nextResult.text == next, nextPaired = a::pendingToolCalls(nextMessages).isEmpty();
        const bool nextEmpty = engine.queuedInputs(nextId)["count"].toInt() == 0;
        const bool nextVerified = nextResult.status == a::RunStatus::Completed && nextExact && firstReads == 1 && nextReads == 1
            && delivered == 1 && nextPaired && nextEmpty;
        std::cout << QJsonDocument(QJsonObject{{"phase", "next"}, {"verified", nextVerified}, {"first_reads", firstReads},
            {"value_exact", nextExact}, {"history_paired", nextPaired}, {"queue_empty", nextEmpty},
            {"next_reads", nextReads}, {"delivered", delivered}, {"expected", next}, {"result", a::toJson(nextResult)}}).toJson(QJsonDocument::Compact).constData() << std::endl;
        const auto nowId = engine.createSession(args[3], workspace).id;
        const QString command = "printf '%s' $$ > shell.pid; printf ready > waiting.txt; sleep 30; printf survived > should-not-exist.txt";
        auto run = engine.run(request(nowId, "Call Bash exactly once with command \"" + command
            + "\" and timeout_ms 60000. Wait for the result. Do not read files until instructed."), log);
        const auto ready = QDir(workspace).filePath("waiting.txt"); const auto deadline = std::chrono::steady_clock::now() + 180s;
        while (read(ready) != "ready" && run.result.wait_for(0ms) != std::future_status::ready && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        if (read(ready) != "ready") { run.cancellation.cancel(); const auto failed = run.result.get(); throw std::runtime_error(("Native shell did not become ready: " + failed.text + " / " + failed.errorMessage).toStdString()); }
        bool validPid = false; const auto pid = read(QDir(workspace).filePath("shell.pid")).toLongLong(&validPid);
        try {
            require(validPid && pid > 1 && pid <= std::numeric_limits<pid_t>::max(), "Invalid live shell PID");
            require(::kill(pid_t(pid), 0) == 0, "Shell was not live before urgent input");
            engine.enqueueInput(nowId, {{"text", "Stop waiting. Do not repeat the interrupted command. Use Read to read urgent.txt and return only its exact contents."}, {"priority", "now"}});
        } catch (...) { run.cancellation.cancel(); run.result.wait(); throw; }
        if (run.result.wait_for(180s) != std::future_status::ready) { run.cancellation.cancel(); run.result.wait(); throw std::runtime_error("Urgent continuation timed out"); }
        const auto nowResult = run.result.get(); const auto nowMessages = engine.session(nowId).messages;
        int bashCalls = 0, urgentReads = 0, interrupted = 0;
        for (const auto& message : nowMessages) {
            if (message.role == a::MessageRole::Tool && message.isError && message.data["interrupted"] == true) ++interrupted;
            for (const auto& call : message.toolCalls) {
                if (call.name == "Bash") { ++bashCalls; require(call.arguments["command"] == command, "Model changed the shell command"); }
                if (call.name == "Read" && call.arguments["path"] == "urgent.txt") ++urgentReads;
            }
        }
        const bool shellGone = ::kill(pid_t(pid), 0) == -1 && errno == ESRCH;
        const bool nowExact = nowResult.text == urgent, nowPaired = a::pendingToolCalls(nowMessages).isEmpty();
        const bool nowEmpty = engine.queuedInputs(nowId)["count"].toInt() == 0, rootLive = !run.cancellation.isCancelled();
        const bool noLaterSideEffect = !QFileInfo::exists(QDir(workspace).filePath("should-not-exist.txt"));
        const bool nowVerified = nowResult.status == a::RunStatus::Completed && nowExact && rootLive
            && bashCalls == 1 && urgentReads == 1 && interrupted == 1 && nowPaired && shellGone && noLaterSideEffect && nowEmpty;
        std::cout << QJsonDocument(QJsonObject{{"phase", "now"}, {"verified", nowVerified}, {"bash_calls", bashCalls},
            {"value_exact", nowExact}, {"history_paired", nowPaired}, {"queue_empty", nowEmpty},
            {"root_not_cancelled", rootLive}, {"no_later_side_effect", noLaterSideEffect},
            {"urgent_reads", urgentReads}, {"interrupted", interrupted}, {"shell_gone", shellGone}, {"expected", urgent},
            {"result", a::toJson(nowResult)}}).toJson(QJsonDocument::Compact).constData() << std::endl;
        require(nextVerified && nowVerified, "Native input acceptance failed; see the separate next/now phase records");
        std::cout << "Native input queue verified next delivery, one real interrupted shell, paired tool history, and exact unknown file answers.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
