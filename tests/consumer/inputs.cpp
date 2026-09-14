#include <agent/InputQueue.h>
#include <agent/Engine.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QDir>
#include <iostream>
namespace a = iiLocalLLM::agent;
class Echo final : public a::Model {
    a::ModelReply generate(const a::ModelRequest& r, const iiLocalLLM::CancellationToken& c,
        const std::function<bool(const QString&)>&) override { c.throwIfCancelled(); return {r.messages.last().text}; }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir root(QDir::current().filePath("installed-inputs-XXXXXX"));
        a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions");
        a::Engine engine(std::make_shared<Echo>(), std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), options);
        const auto id = engine.createSession("consumer", root.path()).id;
        const auto published = engine.enqueueInput(id, {{"text", "installed queue ABI"}})["input"].toObject();
        a::InputQueue reopened(root.filePath("sessions/inputs"));
        if (reopened.snapshot(id)["inputs"].toArray().first().toObject() != published) return 1;
        bool delivered = false;
        const auto result = engine.runQueued({id, {}}, [&](const a::Event& event) { delivered |= event.kind == a::EventKind::InputDelivered; }).result.get();
        if (!delivered || result.status != a::RunStatus::Completed || result.text != "installed queue ABI"
            || reopened.snapshot(id)["count"].toInt() != 0 || engine.session(id).messages.first().id != published["id"].toString()) return 2;
        iiLocalLLM::CancellationToken parent; auto operation = iiLocalLLM::CancellationToken::linkedTo(parent);
        operation.cancel(); if (parent.isCancelled()) return 3;
        std::cout << "Installed InputQueue ABI, durable reopen, runQueued, event and linked cancellation verified\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 4; }
}
