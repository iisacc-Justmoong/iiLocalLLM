#include <agent/ObjectTools.h>
#include <agent/McpConnections.h>
#include <agent/McpServer.h>
#include <mcp/LocalApplications.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QThread>
#include <QElapsedTimer>
#include <future>
#include <iostream>

namespace a = iiLocalLLM::agent;
namespace m = iiLocalLLM::mcp;
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir root(QDir::current().filePath("installed-app-XXXXXX"));
        QObject owner; owner.setProperty("answer", 42);
        auto registry = std::make_shared<a::ToolRegistry>();
        registry->add(a::objectTool(&owner, {"status", "Read installed consumer state", {{"type", "object"}}, {}, true, true},
            [](QObject& object, const QJsonObject&, const a::ToolContext&) {
                if (object.thread() != QThread::currentThread()) throw std::runtime_error("Wrong thread");
                return a::ToolResult{"state", {{"answer", QJsonValue::fromVariant(object.property("answer"))}}};
            }, 3000));
        a::McpServerOptions bridge; bridge.workingDirectory = root.path();
        m::LocalApplicationServer server({"com.iisacc.consumer", "Installed consumer", "1"},
            a::mcpServerOptions(registry, std::make_shared<a::RulePolicy>(), bridge), {root.filePath("apps")});
        if (!server.listen()) throw std::runtime_error(server.errorString().toStdString());
        const auto found = m::discoverLocalApplications(root.filePath("apps"));
        if (found.error != iiLocalLLM::ErrorCode::None || found.applications.size() != 1) return 1;
        auto imported = std::make_shared<a::ToolRegistry>();
        a::McpConnectionOptions options; options.workingDirectory = root.path();
        options.localApplicationsDirectory = root.filePath("apps"); options.refreshIntervalMs = 0;
        a::McpConnections connections(imported, options);
        if (imported->definitions().size() != 2) return 2;
        QString statusName,inspectionName;
        for(const auto& definition:imported->definitions()) {
            if(definition.metadata["remote_name"]=="status")statusName=definition.name;
            if(definition.metadata["remote_name"]=="iiLocalLLM.agent.permissions.get")inspectionName=definition.name;
        }
        if(statusName.isEmpty()||inspectionName.isEmpty())return 2;
        const auto inspection=imported->get(inspectionName).execute({},{});
        if(inspection.isError||inspection.data["provider"]!="rules")return 6;
        auto tool = imported->get(statusName);
        auto future = std::async(std::launch::async, [&] { return tool.execute({}, {}); });
        QElapsedTimer timer; timer.start();
        while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready && timer.elapsed() < 5000) {
            QCoreApplication::processEvents(); QThread::msleep(1);
        }
        const auto result = future.get();
        if (result.isError || result.data["answer"].toInt() != 42) return 3;
        server.close(); connections.refresh();
        if (!imported->definitions().isEmpty() || !m::discoverLocalApplications(root.filePath("apps")).applications.isEmpty()) return 4;
        std::cout << "Installed local app registration, authenticated discovery, QObject dispatch and removal verified\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 5; }
}
