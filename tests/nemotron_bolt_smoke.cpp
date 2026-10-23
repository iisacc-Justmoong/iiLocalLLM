#include "Runtime.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (argc != 2) return 2;
    using namespace iiLocalLLM;
    try {
        ModelSpec spec{"nemotron-bolt", QString::fromLocal8Bit(argv[1]), 2048,
            {{"enable_thinking", false}, {"tool_grammar", false}, {"threads", 4}}, "gguf", {}};
        auto runtime = createLlamaRuntime();
        auto model = runtime->load(spec, {ComputeBackend::Cpu, {}}, {});
        auto context = model->createContext({});
        ConversationRequest request;
        request.model = "model://nemotron-bolt";
        request.toolChoice = "none";
        request.enableThinking = false;
        request.messages = {QJsonObject{{"role", "system"}, {"content", "Reply briefly in Korean."}},
                            QJsonObject{{"role", "user"}, {"content", "안녕!"}}};
        request.options.temperature = 0;
        request.options.maxTokens = 96;
        const auto prompt = model->prepareConversation(request, {});
        QString raw;
        const auto generated = context->generateConversation(prompt, request.options, {},
            [&](const QString& delta) { raw += delta; return true; });
        const auto reply = model->parseConversation(prompt, raw);
        std::cout << QJsonDocument(QJsonObject{{"text", reply.text},
            {"prompt_tokens", prompt.tokenCount()}, {"generated_tokens", generated.generatedTokens},
            {"finish_reason", enumName(generated.finishReason)}}).toJson(QJsonDocument::Compact).constData() << '\n';
        return generated.finishReason == FinishReason::Stop && generated.generatedTokens > 0
            && !reply.text.trimmed().isEmpty() && reply.toolCalls.isEmpty() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
