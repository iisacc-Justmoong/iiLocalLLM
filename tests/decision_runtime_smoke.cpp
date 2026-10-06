#include "agent/Engine.h"
#include "agent/DecisionProtocol.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <cmath>
#include <filesystem>
#include <iostream>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);if(argc!=2)return 2;
    try{
        QTemporaryDir root(QDir::current().filePath("decision-native-XXXXXX"));if(!root.isValid())throw std::runtime_error("Cannot create native decision fixture");
        ModelManifest manifest{"decision-fixture","qwen2","gguf","Q4_K_M",32768,{"text-generation","chat"},"model.gguf",{{"model.gguf",491400032,"74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db"}}};
        const auto models=root.filePath("Models/"+manifest.id);QDir().mkpath(models);std::error_code error;
        std::filesystem::create_hard_link(argv[1],QDir(models).filePath("model.gguf").toStdString(),error);
        if(error&&!QFile::copy(QString::fromLocal8Bit(argv[1]),QDir(models).filePath("model.gguf")))throw std::runtime_error("Cannot provision model");
        QFile file(QDir(models).filePath("manifest.json"));if(!file.open(QIODevice::WriteOnly)||file.write(QJsonDocument(manifestObject(manifest)).toJson())<1)throw std::runtime_error("Cannot save model manifest");file.close();
        ServiceOptions so;so.modelsDirectory=root.filePath("Models");Service service(so);auto uri=modelUri(manifest.id);(void)service.loadModel({uri,4096}).get();
        auto model=std::make_shared<a::ServiceModel>(service);auto registry=std::make_shared<a::ToolRegistry>();const auto workspace=root.filePath("work");QDir().mkpath(workspace);
        int likely=0,valuable=0,modelReturns=0;QJsonArray modelEvidence,decisions;
        for(const auto& name:{QString("likely"),QString("valuable")}){a::Tool tool;tool.definition={name,name,{{"type","object"}},{},false,false};
            tool.execute=[&,name](const auto&,const auto&){if(name=="likely")++likely;else ++valuable;QFile receipt(QDir(workspace).filePath(name+".txt"));if(!receipt.open(QIODevice::WriteOnly)||receipt.write("executed")!=8)throw std::runtime_error("Cannot write execution receipt");return a::ToolResult{"executed",{{"name",name}}};};registry->add(tool);}
        a::EngineOptions options;options.sessionsDirectory=root.filePath("sessions");options.projectContext.enabled=false;options.skills.enabled=false;options.compaction.automatic=false;options.toolSearch.enabled=false;
        options.decision.valueUnit="USD";options.decision.inputs=[](const a::DecisionRequest& r){a::DecisionInput input{"USD"};input.evidence.observations={{"verified",r.goal=="low"?"no":"yes"}};
            for(const auto& c:r.candidates){const bool small=c.name=="likely";const auto yes=small?0.99L:c.name=="valuable"?0.8L:0.9L;
                a::DecisionEstimate e{c.id,small?10.0L:100.0L,small?1.0L:10.0L,small?2.0L:5.0L};
                e.states={{"success",{{"verified","yes"}},std::log(yes)},{"failure",{{"verified","yes"}},std::log1p(-yes)},{"success",{{"verified","no"}},std::log(0.1L)},{"failure",{{"verified","no"}},std::log(0.9L)}};input.estimates.push_back(std::move(e));}return input;};
        options.procedures=std::make_shared<a::Procedures>(a::ProcedureOptions{},[&](const QJsonObject& step,const auto&){a::ProcedureResponse response;
            if(step["kind"]=="decision")decisions.append(step["output"]);
            if(step["kind"]=="model"){modelEvidence.append(step["output"]);response.action=a::ProcedureAction::Replace;
                if(++modelReturns==1)response.output={{"text",""},{"tool_calls",QJsonArray{a::toJson(a::ToolCall{"l","likely",{}}),a::toJson(a::ToolCall{"v","valuable",{}})}}};
                else response.output={{"text","VALUE_CONFIRMED"},{"tool_calls",QJsonArray{}}};}
            return response;});
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);a::Engine engine(model,registry,policy,options);
        GenerationOptions generation;generation.maxTokens=64;generation.temperature=0;
        auto low=engine.createSession(uri,workspace);auto lowResult=engine.run({low.id,"low",generation,3}).result.get();
        if(lowResult.status!=a::RunStatus::Deferred||modelReturns!=0||likely||valuable)throw std::runtime_error("Low probability ran native inference or a tool");
        auto high=engine.createSession(uri,workspace,"Reply with exactly OK. Do not call tools.");auto highResult=engine.run({high.id,"Reply with exactly OK.",generation,3}).result.get();
        if(highResult.status!=a::RunStatus::Completed||highResult.text!="VALUE_CONFIRMED"||modelReturns!=2||valuable!=1||likely!=0||highResult.usage.generatedTokens<=0
            ||!QFile::exists(QDir(workspace).filePath("valuable.txt"))||QFile::exists(QDir(workspace).filePath("likely.txt")))throw std::runtime_error("Value-ranked native execution failed: "+QJsonDocument(a::toJson(highResult)).toJson().toStdString());
        std::cout<<QJsonDocument(QJsonObject{{"low",a::toJson(lowResult)},{"high",a::toJson(highResult)},{"native_model_returns",modelReturns},{"native_model_outputs",modelEvidence},{"decisions",decisions},{"valuable_executions",valuable},{"likely_executions",likely},{"file_receipt_verified",true}}).toJson(QJsonDocument::Compact).toStdString()<<'\n';
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
