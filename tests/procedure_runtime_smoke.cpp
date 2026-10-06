#include "agent/Engine.h"
#include "agent/Subagents.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <filesystem>
#include <iostream>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);if(argc!=2)return 2;
    try {
        QTemporaryDir root(QDir::current().filePath("procedures-native-XXXXXX"));
        if(!root.isValid())throw std::runtime_error("Cannot create native procedure fixture");
        ModelManifest manifest{"procedure-fixture","qwen2","gguf","Q4_K_M",32768,{"text-generation","chat"},"model.gguf",
            {{"model.gguf",491400032,"74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db"}}};
        const auto modelRoot=root.filePath("Models/"+manifest.id);QDir().mkpath(modelRoot);
        std::error_code linkError;const auto target=QDir(modelRoot).filePath("model.gguf");
        std::filesystem::create_hard_link(argv[1],target.toStdString(),linkError);
        if(linkError&&!QFile::copy(QString::fromLocal8Bit(argv[1]),target))throw std::runtime_error("Cannot provision local model");
        QFile metadata(QDir(modelRoot).filePath("manifest.json"));
        if(!metadata.open(QIODevice::WriteOnly)||metadata.write(QJsonDocument(manifestObject(manifest)).toJson())<1)throw std::runtime_error("Cannot write manifest");metadata.close();
        ServiceOptions so;so.modelsDirectory=root.filePath("Models");Service service(so);
        const auto uri=modelUri(manifest.id);(void)service.loadModel({uri,4096}).get();
        auto model=std::make_shared<a::ServiceModel>(service);auto tools=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        const auto workspace=root.filePath("workspace");QDir().mkpath(workspace);
        a::EngineOptions eo{.decision={.enabled=false}};eo.sessionsDirectory=root.filePath("sessions");eo.projectContext.enabled=false;eo.compaction.automatic=false;eo.toolSearch.enabled=false;eo.skills.enabled=false;
        QJsonArray observedModels;
        eo.procedures=std::make_shared<a::Procedures>(a::ProcedureOptions{},[&](const QJsonObject& step,const CancellationToken&){
            a::ProcedureResponse response;
            if(step["kind"]=="model") {
                observedModels.append(QJsonObject{{"agent_id",step["agent_id"]},{"turn",step["turn"]},{"observed_output",step["output"]}});response.action=a::ProcedureAction::Replace;
                if(!step["agent_id"].toString().isEmpty())response.output={{"text","CHILD_CONFIRMED"},{"tool_calls",QJsonArray{}}};
                else if(step["turn"]==1) {
                    const QJsonObject task{{"prompt","Reply briefly with hello."}};
                    response.output={{"text",""},{"tool_calls",QJsonArray{QJsonObject{{"id","child-first"},{"name","Agent"},{"arguments",task}},QJsonObject{{"id","child-repeat"},{"name","Agent"},{"arguments",task}}}}};
                }else response.output={{"text","PARENT_CONFIRMED"},{"tool_calls",QJsonArray{}}};
            }else if(step["kind"]=="completion"&&step["agent_id"].toString().isEmpty())response={a::ProcedureAction::Replace,{{"text","APP_CONFIRMED"}}};
            return response;
        });
        a::SubagentOptions children;children.workingDirectory=workspace;children.stateDirectory=root.filePath("children");children.generation.maxTokens=16;children.generation.temperature=0;
        auto agents=std::make_shared<a::Subagents>(model,tools,policy,eo,children);a::Subagents::attach(eo,agents);
        a::Engine engine(model,tools,policy,eo);const auto session=engine.createSession(uri,workspace);
        a::RunRequest request{session.id,"Reply briefly with hello."};request.maxTurns=4;request.generation.maxTokens=128;request.generation.temperature=0;
        const auto result=engine.run(request).result.get();
        if(result.status!=a::RunStatus::Completed||result.text!="APP_CONFIRMED"||result.usage.generatedTokens==0)throw std::runtime_error("Native parent return control failed: "+QJsonDocument(a::toJson(result)).toJson(QJsonDocument::Compact).toStdString());
        const auto jobs=agents->list(session.id);if(jobs.size()!=1||observedModels.size()!=3)throw std::runtime_error("Duplicate child caused extra native inference");
        const auto child=jobs.first().toObject();const auto output=agents->output(session.id,child["agentId"].toString());
        if(output["result"].toObject()["text"]!="CHILD_CONFIRMED"||output["result"].toObject()["usage"].toObject()["generated_tokens"].toInt()==0)throw std::runtime_error("Native child return control failed");
        int reused=0;QJsonArray receipts;
        // Concurrency-safe Agent calls may be admitted in either order. Both
        // calls must identify the same child, and exactly one reuses admission.
        for(const auto& message:engine.session(session.id).messages)if(message.role==a::MessageRole::Tool&&QStringList{"child-first","child-repeat"}.contains(message.toolCallId)) {
            receipts.append(QJsonObject{{"tool_call_id",message.toolCallId},{"data",message.data},{"is_error",message.isError}});
            if(message.isError||message.data["agentId"]!=child["agentId"])throw std::runtime_error("Native child receipt identity failed");
            reused+=message.data["reused"].toBool();
        }
        if(reused!=1||receipts.size()!=2)throw std::runtime_error("Duplicate child receipt was not visible to parent");
        std::cout<<QJsonDocument(QJsonObject{{"schema","iisacc.procedures-native-test/1"},{"result",a::toJson(result)},{"native_model_returns",observedModels},{"child_count",jobs.size()},{"duplicate_reused",true},{"child_receipts",receipts},{"child_result",output["result"]}}).toJson(QJsonDocument::Compact).constData()<<'\n';
        engine.close();agents->close();return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
