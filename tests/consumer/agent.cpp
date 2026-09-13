#include <agent/Engine.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QDir>
#include <iostream>
namespace a = iiLocalLLM::agent;
class AppModel final : public a::Model {
public:
    a::ModelReply generate(const a::ModelRequest& input, const iiLocalLLM::CancellationToken& cancellation,
                          const std::function<bool(const QString&)>&) override {
        cancellation.throwIfCancelled();
        if (input.messages.back().role == a::MessageRole::Tool) return {input.messages.back().text, {}};
        return {{}, {{"consumer-call", "iisacc.app.currentDocument", {}}}};
    }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir root(QDir::current().filePath("installed-agent-XXXXXX"));
        auto registry = std::make_shared<a::ToolRegistry>();
        a::Tool tool;
        tool.definition = {"iisacc.app.currentDocument", "Read the application's active document", {{"type", "object"}}, {}, true, true};
        tool.definition.metadata = {{"app_id", "iisacc.installed-consumer"}};
        tool.execute = [](const QJsonObject&, const a::ToolContext&) { return a::ToolResult{"installed document", {}}; };
        registry->add(std::move(tool));
        a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions");
        a::Engine engine(std::make_shared<AppModel>(), registry, std::make_shared<a::RulePolicy>(), options);
        const auto session = engine.createSession("app-test", root.path());
        const auto result = engine.run({session.id, "Read my document"}).result.get();
        if (result.status != a::RunStatus::Completed || result.text != "installed document") return 1;
        if (engine.session(session.id).messages.size() != 4) return 2;
        std::cout << "Installed C++ agent model/tool/session ABI linked and executed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 3; }
}
