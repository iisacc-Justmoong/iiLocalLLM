#include <iiLocalLLM.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <iostream>
#include <thread>

using namespace iiLocalLLM;
void copyPackage(const QString& source, const QString& destination)
{
    if (!QDir().mkpath(destination)) throw std::runtime_error("Cannot create fixture directory");
    for (const auto& entry : QDir(source).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const auto path = QDir(destination).filePath(entry.fileName());
        if (entry.isDir()) copyPackage(entry.absoluteFilePath(), path);
        else if (!QFile::copy(entry.absoluteFilePath(), path)) throw std::runtime_error("Cannot copy fixture asset");
    }
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() < 3) return 2;
    try {
        const auto runtime = args[1];
        MlxRuntimeOptions mlx;
        if (runtime == QStringLiteral("mlx")) {
            if (args.size() < 5) return 2;
            mlx = {args[3], args[4], 120000};
        }
        QTemporaryDir root(QDir::current().filePath(QStringLiteral("inference-XXXXXX")));
        if (!root.isValid()) throw std::runtime_error("Cannot create fixture catalog");
        const auto package = root.filePath(QStringLiteral("package"));
        QDir().mkpath(package);
        const bool gguf = runtime == QStringLiteral("llama.cpp");
        if (gguf) {
            if (!QFile::copy(args[2], QDir(package).filePath(QStringLiteral("model.gguf")))) throw std::runtime_error("Cannot copy GGUF fixture");
        } else copyPackage(args[2], package);
        ModelManifest manifest{"smoke", "llama", gguf ? "gguf" : "mlx", gguf ? "Q4_0" : "4bit", 512,
            {"text-generation", "chat"}, gguf ? "model.gguf" : "."};
        QFile metadata(QDir(package).filePath(QStringLiteral("manifest.json")));
        if (!metadata.open(QIODevice::WriteOnly) || metadata.write(QJsonDocument(manifestObject(manifest)).toJson()) < 1)
            throw std::runtime_error("Cannot create inference manifest");
        metadata.close();
        ServiceOptions options;
        options.modelsDirectory = root.filePath(QStringLiteral("Models"));
        Service service(options, mlx);
        const auto installed = service.installModel(package).get();
        if (!service.verifyModel(installed.uri).get().valid) throw std::runtime_error("Installed fixture failed integrity verification");
        ModelLoadRequest model{installed.uri, 512};
        if (runtime == QStringLiteral("llama.cpp") && args.size() > 3) model.options.insert(QStringLiteral("chat_template"), args[3]);
        const auto loaded = service.loadModel(model).get();
        if (loaded.execution.runtime != runtime) throw std::runtime_error("Wrong automatic model runtime");
        const auto hardware = service.hardware();
        if (hardware.appleSilicon && hardware.metalAvailable && loaded.execution.device.backend != ComputeBackend::Metal)
            throw std::runtime_error("Expected automatic Metal selection on this Apple Silicon host");
        const auto session = service.createSession(model.model).get();
        ChatRequest request;
        request.sessionId = session;
        request.prompt = QStringLiteral("Write a short sentence about the sky.");
        request.options.maxTokens = 24;
        request.options.temperature = 0;
        QString streamed;
        auto first = service.chat(request, [&](const StreamEvent& e) {
            if (e.kind == StreamEventKind::Delta) streamed += e.text;
        }).result.get();
        if (first.errorCode != ErrorCode::None || first.text.isEmpty() || first.text != streamed)
            throw std::runtime_error((first.errorMessage + QStringLiteral(" Empty/mismatched inference output")).toStdString());
        request.prompt = QStringLiteral("Now write one more sentence.");
        const auto second = service.chat(request).result.get();
        if (second.errorCode != ErrorCode::None || second.text.isEmpty() || second.usage.cachedTokens < 1)
            throw std::runtime_error((second.errorMessage + QStringLiteral(" Failed second turn or no KV reuse")).toStdString());
        if (service.session(session).get().messages.size() != 4) return 3;
        service.resetSession(session).get();
        const auto cold = service.chat(request).result.get();
        if (cold.errorCode != ErrorCode::None || cold.usage.cachedTokens != 0) return 4;
        const auto historyBeforeCancel = service.session(session).get().messages;
        std::promise<void> deltaSeen;
        auto deltaFuture = deltaSeen.get_future();
        std::atomic_bool release{false};
        bool signalled = false;
        auto interrupted = service.chat(request, [&](const StreamEvent& event) {
            if (event.kind == StreamEventKind::Delta && !signalled) {
                signalled = true;
                deltaSeen.set_value();
                while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        const auto ready = deltaFuture.wait_for(std::chrono::seconds(30));
        interrupted.cancel();
        release = true;
        if (ready != std::future_status::ready || interrupted.result.get().finishReason != FinishReason::Cancelled) return 5;
        if (service.session(session).get().messages != historyBeforeCancel || service.stats().get().cachedContexts != 0) return 6;
        const auto resumed = service.chat(request).result.get();
        if (resumed.errorCode != ErrorCode::None || resumed.text.isEmpty() || resumed.usage.cachedTokens != 0) return 7;
        // A strong positive bias must change the selected token through each real adapter.
        request.options.maxTokens = 1;
        request.options.repetitionPenalty = 1.1;
        request.options.presencePenalty = .1;
        request.options.frequencyPenalty = .1;
        request.options.repetitionContextSize = -1;
        QStringList biased;
        for (const auto* token : {"42", "43"}) {
            request.options.logitBias = {{token,100}};
            const auto result = service.chat(request).result.get();
            if (result.errorCode != ErrorCode::None || result.text.isEmpty())
                throw std::runtime_error(("Biased generation failed: " + result.errorMessage).toStdString());
            biased.append(result.text);
        }
        if (biased[0] == biased[1]) throw std::runtime_error("logit_bias did not affect real inference");
        request.options.logitBias = {};
        request.options.maxTokens = 3; request.options.temperature = .7;
        request.options.minP = .05; request.options.minKeep = 2;
        request.options.xtcProbability = .1;
        request.options.typicalP = gguf ? .95 : 1;
        const auto sampled = service.chat(request).result.get();
        if (sampled.errorCode != ErrorCode::None) throw std::runtime_error(sampled.errorMessage.toStdString());
        std::cout << QJsonDocument(QJsonObject{{QStringLiteral("execution"), executionObject(loaded.execution)}, {QStringLiteral("text"), first.text},
            {QStringLiteral("prompt_tokens"), first.usage.promptTokens}, {QStringLiteral("generated_tokens"), first.usage.generatedTokens},
            {QStringLiteral("second_turn_cached_tokens"), second.usage.cachedTokens}}).toJson().constData();
        service.closeSession(session).get();
        service.unloadModel(model.model).get();
        service.removeModel(model.model).get();
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
