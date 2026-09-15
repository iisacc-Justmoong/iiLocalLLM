#include <Runtime.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <iostream>
using namespace iiLocalLLM;
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);if(argc!=2)return 2;
    try {
        const auto runtime=createLlamaRuntime();
        const auto model=runtime->load({"schema-fixture",QString::fromLocal8Bit(argv[1]),2048,
            {{"enable_thinking",true},{"tool_grammar",false}},"gguf"},{ComputeBackend::Cpu,{}},{});
        ConversationRequest request;request.enableThinking=false;request.toolChoice="none";
        request.messages={QJsonObject{{"role","user"},{"content","Output only NOT_JSON. Never output braces or the word ok."}}};
        request.responseSchema={{"type","object"},{"properties",QJsonObject{{"ok",QJsonObject{{"type","boolean"},{"const",true}}}}},
            {"required",QJsonArray{"ok"}},{"additionalProperties",false}};
        const auto prepared=model->prepareConversation(request,{});auto context=model->createContext({});
        GenerationOptions options;options.maxTokens=64;options.temperature=0;QString output;
        const auto result=context->generateConversation(prepared,options,{},[&](const QString& text){output+=text;return true;});
        const auto parsed=model->parseConversation(prepared,output);QJsonParseError error;
        const auto json=QJsonDocument::fromJson(parsed.text.trimmed().toUtf8(),&error);
        if(result.generatedTokens<=0||!parsed.toolCalls.isEmpty()||error.error!=QJsonParseError::NoError
            ||json.object()!=QJsonObject{{"ok",true}})
            throw std::runtime_error(("Native schema constraint failed: "+output).toStdString());
        std::cout<<QJsonDocument(QJsonObject{{"schema_enforced",true},{"tool_grammar",false},{"generated_tokens",result.generatedTokens},
            {"result",json.object()}}).toJson(QJsonDocument::Compact).constData()<<'\n';return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
