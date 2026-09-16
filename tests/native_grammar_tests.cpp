#include <chat.h>
#include <llama-grammar.h>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtTest/QTest>
#include <memory>
#include <algorithm>

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
    void conditionalArguments_data()
    {
        QTest::addColumn<QString>("filename");QTest::addColumn<bool>("required");
        for(const auto& filename:{QString("Qwen3.5-4B.jinja"),QString("Qwen-Qwen3-0.6B.jinja")})
            for(bool required:{false,true})
                QTest::newRow(qPrintable(filename+(required?"-required":"-auto")))<<filename<<required;
    }
    void conditionalArguments()
    {
        QFETCH(QString,filename);QFETCH(bool,required);using json=common_json;
        QFile file(QString::fromUtf8(IILOCALLLM_LLAMA_TEMPLATES)+'/'+filename);QVERIFY(file.open(QIODevice::ReadOnly));
        auto templates=common_chat_templates_init(nullptr,file.readAll().toStdString(),"","<|im_end|>");
        auto base=json::parse(R"({"type":"object","properties":{"to":{"type":"string"},"summary":{"type":"string"},"message":{"anyOf":[{"type":"string"},{"type":"object","properties":{"type":{"const":"shutdown_request"}},"required":["type"],"additionalProperties":false}]}},"required":["to","message"],"additionalProperties":false})");
        auto plain=base,control=base;
        plain["properties"]["message"]=base["properties"]["message"]["anyOf"][0];
        plain["required"].push_back("summary");
        control["properties"]["message"]=base["properties"]["message"]["anyOf"][1];
        base["anyOf"]=json::array({plain,control});
        common_chat_templates_inputs input;
        input.messages=common_chat_msgs_parse_oaicompat(json::parse(R"([{"role":"user","content":"Send the observed value."}])"));
        input.tools=common_chat_tools_parse_oaicompat(json::array({{{"type","function"},{"function",{{"name","Notify"},{"parameters",base}}}}}));
        input.tool_choice=required?COMMON_CHAT_TOOL_CHOICE_REQUIRED:COMMON_CHAT_TOOL_CHOICE_AUTO;
        input.parallel_tool_calls=false;input.enable_thinking=false;
        const auto prepared=common_chat_templates_apply(templates.get(),input);QVERIFY(!prepared.grammar.empty());
        const auto prefix=prepared.grammar_lazy?std::string():prepared.generation_prompt;
        common_chat_parser_params parser(prepared);if(!prepared.parser.empty())parser.parser.load(prepared.parser);
        const bool xml=filename.startsWith("Qwen3.5");
        auto call=[&](const json& args,const std::vector<std::string>& order){
            if(!xml){
                json ordered=json::object();
                const std::vector<std::string> keys=args["message"].is_object()?std::vector<std::string>{"to","message","summary"}:std::vector<std::string>{"to","summary","message"};
                for(const auto& key:keys)if(args.contains(key))ordered[key]=args[key];
                return std::string("<tool_call>\n")+json{{"name","Notify"},{"arguments",ordered}}.dump()+"\n</tool_call>\n";
            }
            std::string text="<tool_call>\n<function=Notify>\n";
            for(const auto& key:order)if(args.contains(key)){
                // Mixed string/object arguments use JSON values to preserve their
                // type, including a string whose contents happen to be JSON.
                const auto value=key=="message"?args[key].dump():args[key].get<std::string>();
                text+="<parameter="+key+">\n"+value+"\n</parameter>\n";
            }
            return text+"</function>\n</tool_call>\n";
        };
        const json missing{{"to","worker"},{"message","OBSERVED"}};
        QVERIFY(!accepts(prepared.grammar,prefix+call(missing,{"to","message"})));
        for(const auto& message:{json("OBSERVED"),json("{\"type\":\"shutdown_request\"}"),json{{"type","shutdown_request"}}}){
            json args{{"to","worker"},{"message",message}};
            if(message.is_string())args["summary"]="Observed";
            std::vector<std::string> order={"message","summary","to"};
            do{
                const auto text=call(args,order);
                QVERIFY2(accepts(prepared.grammar,prefix+text),text.c_str());
                const auto parsed=common_chat_parse(text,false,parser);QCOMPARE(parsed.tool_calls.size(),size_t(1));
                QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(parsed.tool_calls[0].arguments)).object(),QJsonDocument::fromJson(QByteArray::fromStdString(args.dump())).object());
                if(!xml)break;
            }while(std::next_permutation(order.begin(),order.end()));
        }
        const json controlWithSummary{{"to","worker"},{"message",{{"type","shutdown_request"}}},{"summary","Shutdown"}};
        std::vector<std::string> order={"message","summary","to"};
        do{
            const auto text=call(controlWithSummary,order);
            QVERIFY(accepts(prepared.grammar,prefix+text));
            const auto parsed=common_chat_parse(text,false,parser);QCOMPARE(parsed.tool_calls.size(),size_t(1));
            QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(parsed.tool_calls[0].arguments)).object(),QJsonDocument::fromJson(QByteArray::fromStdString(controlWithSummary.dump())).object());
            if(!xml)break;
        }while(std::next_permutation(order.begin(),order.end()));
        if(xml){
            QVERIFY(!accepts(prepared.grammar,prefix+call(controlWithSummary,{"to","message","summary","summary"})));
            QVERIFY(!accepts(prepared.grammar,prefix+call(controlWithSummary,{"to","message","to"})));
            const json historicalArgs{{"to","worker"},{"summary","Observed"},{"message","{\"type\":\"shutdown_request\"}"}};
            input.messages=common_chat_msgs_parse_oaicompat(json::array({
                {{"role","user"},{"content","Send this text."}},
                {{"role","assistant"},{"content",""},{"tool_calls",json::array({{{"id","previous"},{"type","function"},{"function",{{"name","Notify"},{"arguments",historicalArgs}}}}})}},
                {{"role","tool"},{"tool_call_id","previous"},{"content","Delivered"}},
                {{"role","user"},{"content","Send the next observation."}}
            }));
            const auto continued=common_chat_templates_apply(templates.get(),input);
            const auto encoded="<parameter=message>\n"+historicalArgs["message"].dump()+"\n</parameter>";
            QVERIFY2(continued.prompt.find(encoded)!=std::string::npos,"History must use the same typed XML encoding as generation");
            QVERIFY(continued.prompt.find("JSON-encoded")!=std::string::npos);
        }
    }
    void taggedRootAndLargeSignatures()
    {
        using json=common_json;
        QFile file(QString::fromUtf8(IILOCALLLM_LLAMA_TEMPLATES)+"/Qwen3.5-4B.jinja");QVERIFY(file.open(QIODevice::ReadOnly));
        auto templates=common_chat_templates_init(nullptr,file.readAll().toStdString(),"","<|im_end|>");
        // Partial alternatives must retain the root signature. The host still
        // validates constraints that this tagged grammar does not implement.
        const auto partial=json::parse(R"({"type":"object","properties":{"mode":{"type":"string"},"prompt":{"type":"string"},"skill":{"type":"string"}},"required":["mode"],"anyOf":[{"required":["prompt"]},{"required":["skill"]}]})");
        auto large=json::parse(R"({"type":"object","properties":{"mode":{"type":"string"},"text":{"type":"string"},"a":{"type":"string"},"b":{"type":"string"},"c":{"type":"string"},"d":{"type":"string"},"e":{"type":"string"}},"required":["mode","text"]})");
        for(const auto& schema:{partial,large}){
            common_chat_templates_inputs input;
            input.messages=common_chat_msgs_parse_oaicompat(json::parse(R"([{"role":"user","content":"Run the selected action."}])"));
            input.tools=common_chat_tools_parse_oaicompat(json::array({{{"type","function"},{"function",{{"name","Action"},{"parameters",schema}}}}}));
            input.tool_choice=COMMON_CHAT_TOOL_CHOICE_REQUIRED;input.parallel_tool_calls=false;input.enable_thinking=false;
            const auto prepared=common_chat_templates_apply(templates.get(),input);
            common_chat_parser_params parser(prepared);parser.parser.load(prepared.parser);
            const bool isPartial=schema.contains("anyOf");
            const json args=isPartial?json{{"prompt","Plain raw text"},{"mode","work"}}:json{{"text","Plain raw text"},{"mode","work"},{"e","Tail"}};
            auto call=[&](bool includeMode){
                std::string text="<tool_call>\n<function=Action>\n";
                for(const auto& item:args.items())if(includeMode||item.key()!="mode")text+="<parameter="+item.key()+">\n"+item.value().get<std::string>()+"\n</parameter>\n";
                return text+"</function>\n</tool_call>\n";
            };
            const auto text=call(true);
            QVERIFY2(accepts(prepared.grammar,prepared.generation_prompt+text),text.c_str());
            QVERIFY(!accepts(prepared.grammar,prepared.generation_prompt+call(false)));
            const auto parsed=common_chat_parse(text,false,parser);QCOMPARE(parsed.tool_calls.size(),size_t(1));
            QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(parsed.tool_calls[0].arguments)).object(),QJsonDocument::fromJson(QByteArray::fromStdString(args.dump())).object());
        }
    }
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
