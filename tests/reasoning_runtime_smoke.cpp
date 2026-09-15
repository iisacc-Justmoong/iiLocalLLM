#include <Runtime.h>
#include <QtCore/QCoreApplication>
#include <iostream>
using namespace iiLocalLLM;
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    try {
        const auto runtime = createLlamaRuntime();
        // A controlled native Jinja fixture makes the expected template switch
        // observable without relying on a stochastic model answer or special token IDs.
        const QString chatTemplate =
            "{% for message in messages %}{{ '<|im_start|>' + message['role'] + '\n' }}"
            "{% if message['role'] == 'assistant' and message['reasoning_content'] is defined %}{{ '<think>\n' + message['reasoning_content'] + '\n</think>\n\n' }}{% endif %}"
            "{{ message['content'] + '<|im_end|>\n' }}{% endfor %}"
            "{% if add_generation_prompt %}{{ '<|im_start|>assistant\n' }}"
            "{% if enable_thinking is defined and not enable_thinking %}{{ '<think>\n\n</think>\n\n' }}{% endif %}{% endif %}";
        ModelSpec spec{"reasoning-fixture", QString::fromLocal8Bit(argv[1]), 1024,
            {{"chat_template", chatTemplate}, {"enable_thinking", "false"}}, "gguf"};
        try { (void)runtime->load(spec, {ComputeBackend::Cpu, {}}, {}); throw std::runtime_error("Invalid thinking option was accepted"); }
        catch (const Error& error) {
            if (error.code() != ErrorCode::InvalidArgument || !QString::fromUtf8(error.what()).contains("enable_thinking must be a boolean")) throw;
        }
        ConversationRequest request; request.messages = {QJsonObject{{"role", "user"}, {"content", "Hello"}}};
        spec.options["enable_thinking"] = false;
        auto disabled = runtime->load(spec, {ComputeBackend::Cpu, {}}, {});
        const auto noThinking = disabled->prepareConversation(request, {});
        if (disabled->tokenize({{Role::User, "Hello"}}, {}) != noThinking.tokens)
            throw std::runtime_error("Text and structured conversation ignored the same thinking control");
        for(const auto& response:{QString("</think>\n\nMISSING"),QString(" \n</think>\n\nMISSING")}) {
            const auto parsed=disabled->parseConversation(noThinking,response);
            if(parsed.text.trimmed()!="MISSING"||!parsed.reasoning.trimmed().isEmpty()||!parsed.toolCalls.isEmpty())
                throw std::runtime_error(("Repeated closing reasoning marker was exposed: "+parsed.text).toStdString());
        }
        for(const auto& literal:{QString("Literal </think> content"),QString("`</think>`"),QString("{\"tag\":\"</think>\"}")})
            if(disabled->parseConversation(noThinking,literal).text.trimmed()!=literal)
                throw std::runtime_error("Literal reasoning tag was rewritten");
        spec.options["enable_thinking"] = true;
        auto enabled = runtime->load(spec, {ComputeBackend::Cpu, {}}, {});
        const auto thinking = enabled->prepareConversation(request, {});
        if(enabled->parseConversation(thinking,"</think>\n\nMISSING").text.trimmed()!="</think>\n\nMISSING")
            throw std::runtime_error("An unprefilled closing tag was rewritten");
        if (noThinking.tokens == thinking.tokens || noThinking.tokens.size() <= thinking.tokens.size())
            throw std::runtime_error("enable_thinking did not change the native prepared prompt");
        if (enabled->tokenize({{Role::User, "Hello"}}, {}) != thinking.tokens)
            throw std::runtime_error("Text conversation ignored enable_thinking=true");
        request.enableThinking=false;
        if(enabled->prepareConversation(request,{}).tokens!=noThinking.tokens)
            throw std::runtime_error("Per-request thinking override was ignored");
        request.enableThinking=true;
        if(disabled->prepareConversation(request,{}).tokens!=thinking.tokens)
            throw std::runtime_error("Per-request thinking override could not enable thinking");
        request.enableThinking.reset();
        if(enabled->prepareConversation(request,{}).tokens!=thinking.tokens||disabled->prepareConversation(request,{}).tokens!=noThinking.tokens)
            throw std::runtime_error("Per-request override changed the loaded model default");
        spec.options.remove("enable_thinking");
        auto defaults = runtime->load(spec, {ComputeBackend::Cpu, {}}, {});
        if (defaults->prepareConversation(request, {}).tokens != thinking.tokens)
            throw std::runtime_error("The default thinking template contract changed");
        std::cout << "Native thinking option type, explicit true/false, default, request overrides and both prompt paths verified\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
