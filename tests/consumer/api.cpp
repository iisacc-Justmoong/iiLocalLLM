#include <agent/Api.h>
#include <HttpApiServer.h>
#include <LocalIpcServer.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <iostream>
namespace a = iiLocalLLM::agent;
static_assert(!std::is_copy_constructible_v<a::Api>);
class Model final : public a::Model {
public:
    a::ModelReply generate(const a::ModelRequest&, const iiLocalLLM::CancellationToken& token, const iiLocalLLM::TextCallback&) override {
        token.throwIfCancelled(); return {"installed API model", {}, {1, 1, 0, 0}};
    }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir root(QDir::current().filePath("api-XXXXXX"));
        iiLocalLLM::ServiceOptions serviceOptions; serviceOptions.modelsDirectory = root.filePath("Models");
        iiLocalLLM::Service service(serviceOptions);
        a::ApiOptions options; options.workingDirectory = root.filePath("work"); QDir().mkpath(options.workingDirectory);
        options.stateDirectory = root.filePath("private"); const QString token(48, 'a'); options.clientTokens = {{"consumer", token}};
        auto api = std::make_shared<a::Api>(std::make_shared<Model>(), std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), options);
        auto call = [&](const QString& method, QJsonObject params = {}) { return api->dispatch(method, params, token).result.get().toObject(); };
        const auto session = call("agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        if (call("agent.run", {{"session_id", session}, {"prompt", "hello"}})["text"] != "installed API model") return 1;
        if (call("agent.sessions.fork", {{"session_id", session}}).value("session_id") == session) return 2;
        iiLocalLLM::LocalIpcServer ipc(service); ipc.setRpcHandler(api); if (!ipc.listen(root.filePath("s"))) return 3;
        iiLocalLLM::HttpApiServer http(service); http.setRpcHandler(api); if (!http.listen()) return 4;
        std::cout << "Installed public C++ RPC/agent API and both transport setters linked and executed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 5; }
}
