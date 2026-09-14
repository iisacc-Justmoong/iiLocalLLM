#include <agent/Skills.h>
#include <agent/Engine.h>
#include <agent/Subagents.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <iostream>
namespace a = iiLocalLLM::agent;
class Model final : public a::Model {
    a::ModelReply generate(const a::ModelRequest& r, const iiLocalLLM::CancellationToken&, const iiLocalLLM::TextCallback&) override {
        return {r.messages.last().text, {}};
    }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir root; const auto dir = root.filePath(".claude/skills/inspect");
        if (!root.isValid() || !QDir().mkpath(dir)) return 1;
        QFile file(dir + "/SKILL.md"); if (!file.open(QIODevice::WriteOnly)) return 1;
        file.write("---\ndescription: Installed skill\n---\nINSTALLED_SKILL $0"); file.close();
        if (a::discoverSkills(root.path()).skills.size() != 1) return 1;
        a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions");
        a::Engine engine(std::make_shared<Model>(), std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), options);
        const auto session = engine.createSession("fixture", root.path());
        a::RunRequest request{session.id}; request.skill = "inspect"; request.skillArguments = "'consumer value'";
        const auto result = engine.run(request).result.get();
        if (result.status != a::RunStatus::Completed || !result.text.contains("INSTALLED_SKILL consumer value")) return 1;
        if (!engine.session(session.id).messages.first().metadata.contains("iilocal.skill")) return 1;
        QTemporaryDir children; auto model=std::make_shared<Model>(); auto tools=std::make_shared<a::ToolRegistry>();
        auto policy=std::make_shared<a::RulePolicy>(); a::SubagentOptions childOptions;
        childOptions.workingDirectory=root.path();childOptions.stateDirectory=children.filePath("state");
        auto agents=std::make_shared<a::Subagents>(model,tools,policy,options,childOptions);a::Subagents::attach(options,agents);
        a::Engine forkEngine(model,tools,policy,options);
        if(!file.open(QIODevice::WriteOnly|QIODevice::Truncate))return 1;
        file.write("---\ndescription: Installed fork\ncontext: fork\n---\nFORK_CONSUMER $0");file.close();
        if(forkEngine.skills(session.id).skills.first().executionContext!="fork")return 1;
        const auto forked=forkEngine.run(request).result.get();
        if(forked.status!=a::RunStatus::Completed || !forked.text.contains("FORK_CONSUMER consumer value") || forked.sessionId!=session.id)return 1;
        const auto saved=forkEngine.session(session.id);
        if(saved.messages.size()!=4 || !saved.messages.last().metadata.contains("iilocal.skill_fork") || agents->list(session.id).size()!=1)return 1;
        std::cout << "installed skill discovery, ABI, inline and isolated child execution passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
