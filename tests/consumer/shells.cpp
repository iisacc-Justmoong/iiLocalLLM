#include <agent/ShellTasks.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <iostream>
namespace a = iiLocalLLM::agent;
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir root(QDir::current().filePath("installed-shells-XXXXXX"));
        const auto workspace = root.filePath("workspace"); QDir().mkpath(workspace);
        auto shells = std::make_shared<a::ShellTasks>(workspace, root.filePath("state"));
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, workspace, shells);
        a::ToolRunner runner(registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass));
        a::ToolContext context{"installed-application", "installed-run", workspace};
        const auto start = runner.run({"start", "Bash", {{"command", "sleep 0.1; printf installed-shell"}, {"run_in_background", true}}}, context);
        const auto id = start.data["backgroundTaskId"].toString(); if (start.isError || id.isEmpty()) return 1;
        const auto output = runner.run({"read", "TaskOutput", {{"task_id", id}, {"timeout", 5000}}}, context);
        if (output.isError || output.data["task"].toObject()["output"] != "installed-shell") return 2;
        const auto active = shells->start(context, "sleep 30"); const auto stopped = shells->stop(context.sessionId, active["task_id"].toString());
        if (stopped["status"] != "killed") return 3;
        shells->close(); a::ShellTasks reopened(workspace, root.filePath("state"));
        if (reopened.output(context.sessionId, id, false, 0, 0, 1024)["task"].toObject()["status"] != "completed") return 4;
        std::cout << "Installed shell ABI, native process, output, stop and persistent history verified\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 5; }
}
