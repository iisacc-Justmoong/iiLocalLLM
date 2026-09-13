#include <agent/Engine.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <iostream>
namespace a = iiLocalLLM::agent;
class AppModel final : public a::Model {
public:
    std::optional<iiLocalLLM::ContextBudget> measure(const a::ModelRequest& r, const iiLocalLLM::CancellationToken& token) override {
        token.throwIfCancelled(); qint64 count = 100 + r.systemPrompt.size() + r.tools.size() * 40;
        for (const auto& message : r.messages) count += message.text.size() + 10;
        return iiLocalLLM::ContextBudget{count, 8192};
    }
    a::ModelReply generate(const a::ModelRequest& r, const iiLocalLLM::CancellationToken& token, const iiLocalLLM::TextCallback&) override {
        token.throwIfCancelled();
        return {r.summarizing ? "Observed earlier records; the current request remains unfinished." : "resumed", {}, {100, 10, 0, 0}};
    }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir root;
        a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions");
        a::Engine engine(std::make_shared<AppModel>(), std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), options);
        const auto session = engine.createSession("app-model", root.path());
        { a::SessionStore store(options.sessionsDirectory); auto lease = store.acquire(session.id);
          lease->append({"u1", a::MessageRole::User, QString(1500, 'x')});
          lease->append({"a1", a::MessageRole::Assistant, "observed"});
          lease->append({"u2", a::MessageRole::User, QString(1500, 'y')});
          lease->append({"a2", a::MessageRole::Assistant, "observed again"});
          lease->append({"u3", a::MessageRole::User, "continue"}); }
        const auto result = engine.compact({session.id}).result.get();
        if (result.status != a::RunStatus::Completed || result.usage.compactions != 1 || result.usage.summaryGeneratedTokens < 1)
            throw std::runtime_error(result.errorMessage.toStdString());
        const auto restored = engine.session(session.id);
        if (restored.messages.size() != 5 || restored.compactions.size() != 1 || a::modelMessages(restored).size() != 3) return 2;
        const auto checkpoint = a::compactionFromJson(a::toJson(restored.compactions.first()));
        if (checkpoint.inputTokensBefore <= checkpoint.inputTokensAfter) return 3;
        const auto fork = engine.forkSession(session.id);
        if (engine.run({fork.id, "resume"}).result.get().text != "resumed") return 4;
        std::cout << "Installed compaction model/session/checkpoint ABI, fork and resume verified\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
