#include <chat.h>
#include <llama-grammar.h>
#include <QtCore/QFile>
#include <QtTest/QTest>
#include <memory>

namespace {
bool accepts(const std::string& source, const std::string& text)
{
    std::unique_ptr<llama_grammar, decltype(&llama_grammar_free_impl)> grammar(
        llama_grammar_init_impl(nullptr, source.c_str(), "root", false, nullptr, 0, nullptr, 0),
        llama_grammar_free_impl);
    if (!grammar) throw std::runtime_error("Cannot compile native tool grammar");
    // ASCII fixture: feed each character through the same grammar state machine
    // used by the sampler, without loading weights or drawing random tokens.
    for (const char character : text) {
        try { llama_grammar_accept_token(*grammar, 0, std::string(1, character)); }
        catch (const std::runtime_error&) { return false; }
        if (llama_grammar_get_stacks(grammar.get()).empty()) return false;
    }
    for (const auto& stack : llama_grammar_get_stacks(grammar.get()))
        if (stack.empty()) return true;
    return false;
}
}

class NativeGrammarTests final : public QObject {
    Q_OBJECT
private slots:
    void singleCallLimit_data()
    {
        QTest::addColumn<QString>("filename");
        QTest::addColumn<bool>("required");
        QTest::addColumn<bool>("parallel");
        for (const auto& filename : {QString("Qwen-Qwen3-0.6B.jinja"), QString("Qwen-Qwen2.5-7B-Instruct.jinja")})
            for (const bool required : {false, true})
                for (const bool parallel : {false, true})
                    QTest::newRow(qPrintable(filename + (required ? "-required" : "-auto") + (parallel ? "-parallel" : "-single")))
                        << filename << required << parallel;
    }
    void singleCallLimit()
    {
        QFETCH(QString, filename); QFETCH(bool, required); QFETCH(bool, parallel);
        QFile file(QString::fromUtf8(IILOCALLLM_LLAMA_TEMPLATES) + '/' + filename);
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto templates = common_chat_templates_init(nullptr, file.readAll().toStdString(), "", "<|im_end|>");
        common_chat_templates_inputs input;
        input.messages = common_chat_msgs_parse_oaicompat(common_json::parse(R"([{"role":"user","content":"Read the note."}])"));
        input.tools = common_chat_tools_parse_oaicompat(common_json::parse(R"([{"type":"function","function":{"name":"Read","parameters":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false}}}])"));
        input.tool_choice = required ? COMMON_CHAT_TOOL_CHOICE_REQUIRED : COMMON_CHAT_TOOL_CHOICE_AUTO;
        input.parallel_tool_calls = parallel;
        input.enable_thinking = false;
        const auto prepared = common_chat_templates_apply(templates.get(), input);
        QVERIFY(!prepared.grammar.empty());
        QCOMPARE(prepared.grammar_lazy, !required);
        const std::string call = "<tool_call>\n{\"name\":\"Read\",\"arguments\":{\"path\":\"note.txt\"}}\n</tool_call>\n";
        // Eager grammar includes the generation prefill; the native sampler
        // accepts it before generation. Lazy grammar starts at the tool marker.
        const auto prefix = prepared.grammar_lazy ? std::string() : prepared.generation_prompt;
        QVERIFY2(accepts(prepared.grammar, prefix + call), prepared.grammar.c_str());
        QCOMPARE(accepts(prepared.grammar, prefix + call + call), parallel);
    }
};
QTEST_GUILESS_MAIN(NativeGrammarTests)
#include "native_grammar_tests.moc"
