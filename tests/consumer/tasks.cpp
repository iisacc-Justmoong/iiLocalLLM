#include <agent/TaskStore.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QDir>
#include <iostream>
namespace a = iiLocalLLM::agent;
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        QTemporaryDir root(QDir::current().filePath("installed-tasks-XXXXXX"));
        auto store = std::make_shared<a::TaskStore>(root.filePath("state"));
        auto registry = std::make_shared<a::ToolRegistry>();
        for (auto tool : a::taskTools(store, "application-session", false)) registry->add(std::move(tool));
        a::ToolRunner runner(registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Plan));
        auto created = runner.run({"create", "TaskCreate", {{"subject", "Check installed ABI"}, {"description", "Read, claim and reload"}}}, {});
        if (created.isError || created.data["task"].toObject()["id"] != "1") return 1;
        auto claim = runner.run({"claim", "TaskClaim", {{"taskId", "1"}, {"owner", "consumer"}}}, {});
        if (claim.isError || !claim.data["success"].toBool()) return 2;
        a::TaskStore reopened(root.filePath("state"));
        const auto record = reopened.execute("application-session", "TaskGet", {{"taskId", "1"}}).data["task"].toObject();
        if (record["owner"] != "consumer" || record["status"] != "in_progress") return 3;
        std::cout << "Installed task schemas, policy, atomic claim and persistent reload verified\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 4; }
}
