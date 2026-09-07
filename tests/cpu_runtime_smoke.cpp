#include <iiLocalLLM.h>
#include <QtCore/QCoreApplication>
#include <iostream>

using namespace iiLocalLLM;
// Adapter conformance test: bypasses service policy to exercise the actual CPU fallback implementation.
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() < 3) return 2;
    try {
        std::shared_ptr<Runtime> runtime;
        ModelSpec spec{"cpu-test", args[2], 256};
        if (args[1] == QStringLiteral("llama.cpp")) {
            runtime = createLlamaRuntime();
            spec.options.insert(QStringLiteral("chat_template"), QStringLiteral("chatml"));
        } else {
            if (args.size() != 5) return 2;
            runtime = createMlxRuntime({args[3], args[4]});
        }
        const auto model = runtime->load(spec, {ComputeBackend::Cpu, {}}, {});
        QList<ChatMessage> messages{{Role::User, QStringLiteral("Tell a short story.")}};
        const auto tokens = model->tokenize(messages, {});
        auto context = model->createContext({});
        GenerationOptions options;
        options.maxTokens = 8;
        options.temperature = 0;
        QString text;
        const auto result = context->generate(tokens, options, {}, [&](const auto& delta) { text += delta; return true; });
        if (text.isEmpty() || result.generatedTokens < 1) return 3;
        messages.append({Role::Assistant, text});
        messages.append({Role::User, QStringLiteral("Continue.")});
        QString secondText;
        const auto second = context->generate(model->tokenize(messages, {}), options, {},
            [&](const auto& delta) { secondText += delta; return true; });
        if (secondText.isEmpty() || second.cachedTokens < 1) return 4;
        std::cout << runtime->id().toStdString() << " CPU inference: " << text.toStdString()
                  << " (second turn cached tokens: " << second.cachedTokens << ")\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
