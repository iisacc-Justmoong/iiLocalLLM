#include <agent/McpTools.h>
#include <mcp/HttpClient.h>
#include <QtCore/QProcess>
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (argc != 3 && argc != 4) return 2;
    try {
        QTemporaryDir directory;
        const auto secret = "MCP_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QFile file(directory.filePath("secret.txt"));
        if (!file.open(QIODevice::WriteOnly) || file.write(secret.toUtf8()) < 1) throw std::runtime_error("Cannot create fixture");
        file.close();
        iiLocalLLM::mcp::StdioOptions options;
        options.program = QString::fromLocal8Bit(argv[1]); options.arguments = {"-B", QString::fromLocal8Bit(argv[2]), file.fileName()};
        // This oracle checks interoperability, not cold Python import speed.
        // Keep production defaults and deterministic timeout tests unchanged.
        constexpr int peerStartupMs = 30000;
        options.initializeTimeoutMs = peerStartupMs;
        QElapsedTimer startup;startup.start();
        iiLocalLLM::mcp::ClientOptions config;
        config.roots = {QJsonObject{{"uri", QUrl::fromLocalFile(directory.path()).toString()}}};
        struct ServerProcess {
            QProcess process;
            ~ServerProcess() { if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(3000); } }
        } server;
        std::shared_ptr<iiLocalLLM::mcp::Client> client;
        if (argc == 4) {
            options.arguments.append("streamable-http");
            server.process.start(options.program, options.arguments);
            if (!server.process.waitForStarted(peerStartupMs)
                || startup.elapsed() >= peerStartupMs
                || !server.process.waitForReadyRead(peerStartupMs - int(startup.elapsed())))
                throw std::runtime_error(("Official HTTP peer failed to start: " + server.process.readAllStandardError()).toStdString());
            iiLocalLLM::mcp::HttpOptions http;
            http.endpoint = QUrl(QString::fromUtf8(server.process.readLine()).trimmed());
            client = std::make_shared<iiLocalLLM::mcp::HttpClient>(http, config);
        } else client = std::make_shared<iiLocalLLM::mcp::StdioClient>(options, config);
        std::cout << "Official MCP peer initialized in " << startup.elapsed() << " ms.\n";
        auto check = [](bool ok, const char* message) { if (!ok) throw std::runtime_error(message); };
        check(client->protocolVersion() == "2025-11-25", "Wrong protocol negotiation");
        check(client->listTools().size() == 2, "Official tools not discovered");
        check(client->listResources().size() == 1, "Official resources not discovered");
        check(client->listPrompts().size() == 1, "Official prompts not discovered");
        check(client->readResource("fixture://secret")["contents"].toArray()[0].toObject()["text"] == secret, "Resource bytes changed");
        check(client->getPrompt("summarize", {{"topic", "MCP"}})["messages"].toArray()[0].toObject()["content"].toObject()["text"] == "Summarize MCP", "Prompt changed");
        int progress = 0;
        const auto result = client->callTool("read_secret", {}, {}, [&](const QJsonObject&) { ++progress; });
        check(progress == 2 && result["structuredContent"].toObject()["value"] == secret, "Structured tool result or progress lost");
        check(client->callTool("inspect_roots", {})["structuredContent"].toObject()["count"].toInt() == 1, "Reverse roots request failed");
        auto tools = iiLocalLLM::agent::mcpTools(client, {"oracle", "com.iisacc.fixture", true});
        iiLocalLLM::agent::ToolRegistry registry;
        for (auto& tool : tools) registry.add(std::move(tool));
        check(registry.definitions().size() == 2, "MCP agent adapter failed");
        client->close();
        std::cout << (argc == 4 ? "Streamable HTTP: " : "stdio: ") << "Official MCP Python SDK 1.26.0: initialize, tools, structured output, progress, resources, prompts and reverse roots passed.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
