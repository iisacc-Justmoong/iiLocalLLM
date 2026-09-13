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
        QTemporaryDir root(QDir::current().filePath("compact-native-XXXXXX"));
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
        const auto identifier = "COMPACT_" + QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(8);
        ServiceOptions serviceOptions; serviceOptions.modelsDirectory = root.filePath("Models");
        Service service(serviceOptions); (void)service.loadModel({"model://agent-fixture", 4096}).get();
        auto model = std::make_shared<a::ServiceModel>(service);
        auto registry = std::make_shared<a::ToolRegistry>();
        a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions");
        options.compaction.keepRecentGroups = 1;
        a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
        auto session = engine.createSession("model://agent-fixture", workspace);
        {
            a::SessionStore store(options.sessionsDirectory); auto lease = store.acquire(session.id);
            lease->append({"release-request", a::MessageRole::User,
                "Prepare the release. The exact release identifier is " + identifier + ". Keep this identifier in every summary. No deployment has been authorized."});
            for (int i = 0; i < 12; ++i) {
                QString log = "Observed build log " + QString::number(i) + ": compilation succeeded. No deployment occurred.\n";
                for (int n = 0; n < 60; ++n) log += "The checked build log reports successful compilation, but distribution and deployment remain pending.\n";
                lease->append({"build-log-" + QString::number(i), a::MessageRole::Assistant, log});
            }
        }
        session = engine.session(session.id); const auto original = session.messages;
        a::ModelRequest before{session.model, session.systemPrompt, original};
        before.messages.append({{}, a::MessageRole::User, "Continue the current task after compaction."});
        const auto budget = model->measure(before, {}).value();
        if (budget.inputTokens <= budget.contextTokens) throw std::runtime_error("Fixture did not exceed the native context");
        a::CompactRequest request; request.sessionId = session.id;
        request.instructions = "Keep the summary below 120 words. Always retain the exact release identifier from the initial user request and previous summary.";
        int progress = 0;
        const auto compacted = engine.compact(request, [&](const a::Event& e) {
            if (e.kind == a::EventKind::CompactionProgress) ++progress;
        }).result.get();
        if (compacted.status != a::RunStatus::Completed) throw std::runtime_error(("Native compaction failed: " + compacted.errorMessage).toStdString());
        const auto restored = a::SessionStore(options.sessionsDirectory).load(session.id);
        if (restored.messages != original || restored.compactions.size() != 1 || compacted.usage.summaryGeneratedTokens < 1 || progress < 2)
            throw std::runtime_error("Missing durable, multi-pass native compaction evidence");
        const auto& checkpoint = restored.compactions.last();
        const bool summaryRetainedIdentifier = checkpoint.summary.contains(identifier);
        a::RunRequest followUp{session.id, "Return only the exact release identifier specified in the original user request. It starts with COMPACT_."};
        followUp.generation.temperature = 0; followUp.generation.maxTokens = 128; followUp.maxTurns = 2;
        const auto resumed = engine.run(followUp).result.get();
        const bool continued = resumed.status == a::RunStatus::Completed && resumed.text.contains(identifier);
        const QJsonObject report{{"native_input_tokens", budget.inputTokens}, {"context_tokens", budget.contextTokens},
            {"compacted_input_tokens", checkpoint.inputTokensAfter}, {"summary_passes", progress},
            {"summary_generated_tokens", compacted.usage.summaryGeneratedTokens}, {"raw_message_count", original.size()},
            {"summary_retained_identifier", summaryRetainedIdentifier}, {"continued_with_identifier", continued},
            {"summary", checkpoint.summary}, {"answer", resumed.text}, {"error", resumed.errorMessage}};
        std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
        if (!summaryRetainedIdentifier || !continued) throw std::runtime_error("Native model did not preserve the release identifier");
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
