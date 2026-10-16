#include "MediaContent.h"
#include "agent/Api.h"
#include "agent/DecisionProtocol.h"
#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <atomic>
using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
namespace {
QJsonObject image() { return {{"type", "image"}, {"mimeType", "image/png"}, {"data", "aW1hZ2U="}}; }
QJsonObject audio() { return {{"type", "audio"}, {"mimeType", "audio/wav"}, {"data", "YXVkaW8="}}; }
QJsonObject video() {
    auto first=image(),second=image();first["timestamp_ms"]=0;second["timestamp_ms"]=1000;
    return {{"type", "video"}, {"frames", QJsonArray{first,second}}};
}
class ProbeModel final : public a::Model {
public:
    QList<a::ModelRequest> seen;
    std::function<a::ModelReply(const a::ModelRequest&)> next=[](const auto&){return a::ModelReply{"done"};};
    std::function<std::optional<ContextBudget>(const a::ModelRequest&)> budget;
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken&,const TextCallback&)override {
        seen.append(request);return next(request);
    }
    std::optional<ContextBudget> measure(const a::ModelRequest& r,const CancellationToken&)override {
        return budget?budget(r):std::nullopt;
    }
};
struct Host {
    QTemporaryDir root;QString workspace=root.filePath("work");
    std::shared_ptr<ProbeModel> model=std::make_shared<ProbeModel>();
    std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::RulePolicy> policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    a::EngineOptions options;
    Host() {
        QDir().mkpath(workspace);options.sessionsDirectory=root.filePath("sessions");options.skills.enabled=false;
        options.projectContext.enabled=false;options.compaction.automatic=false;options.decision.valueUnit="value";
        options.decision.modelRoutes={{"text","model://text",1},{"vision","model://vision",11},{"omni","model://omni",15}};
        options.decision.profiles={{"run",100,10,1,0.9L},{"model:text",100,10,1,0.9L},
            {"model:vision",100,10,2,0.9L},{"model:omni",100,10,20,0.9L}};
    }
};
}
class MultimodalTests final : public QObject {
    Q_OBJECT
private slots:
    void validatesMediaAndTemporalOrder() {
        QCOMPARE(contentModalities({image(),audio(),video()}),Modalities(15));
        auto bad=image();bad["data"]="not base64";QVERIFY_THROWS_EXCEPTION(Error,nativeContent({bad}));
        bad=image();bad["mimeType"]="image/svg+xml";QVERIFY_THROWS_EXCEPTION(Error,nativeContent({bad}));
        auto clip=video();auto frames=clip["frames"].toArray();auto f=frames[1].toObject();f["timestamp_ms"]=0;frames[1]=f;clip["frames"]=frames;
        QVERIFY_THROWS_EXCEPTION(Error,nativeContent({clip}));
        QVERIFY_THROWS_EXCEPTION(Error,nativeContent({image()},10));
        auto resource=QJsonObject{{"type","resource"},{"resource",QJsonObject{{"uri","memory://frame"},{"mimeType","image/png"},{"blob","aW1hZ2U="}}}};
        QCOMPARE(contentModalities({resource}),Modalities(3));
        QCOMPARE(contentModalities({QJsonObject{{"type","image_url"},{"image_url",QJsonObject{{"url","data:image/png;base64,aW1hZ2U="}}}}}),Modalities(3));
        QVERIFY_THROWS_EXCEPTION(Error,nativeContent({QJsonObject{{"type","image_url"},{"image_url",QJsonObject{{"url","https://example.com/image.png"}}}}}));
        QCOMPARE(contentModalities({QJsonObject{{"type","input_audio"},{"input_audio",QJsonObject{{"format","wav"},{"data","YXVkaW8="}}}}}),Modalities(5));
    }
    void filtersCapabilitiesBeforeExpectedValueSelection() {
        Host h;a::DecisionGate gate(h.options.decision);
        const auto candidates=gate.modelCandidates(3);QCOMPARE(candidates.size(),size_t(2));
        QCOMPARE(gate.evaluate(candidates,gate.inputs({"model_route",{},{},{},{},{},candidates,3})).selectedCandidateId,std::string("vision"));
        QCOMPARE(gate.modelCandidates(7).size(),size_t(1));
        auto options=h.options.decision;options.modelRoutes.push_back({"bad","model://bad",9});
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,(void)a::DecisionGate{options});
    }
    void hostConfigurationExposesRoutesButRejectsPathOverrides() {
        QJsonObject config{{"enabled",false},{"model_routes",QJsonArray{QJsonObject{{"id","vision"},{"model","model://vision"},{"modalities",QJsonArray{"text","image","video"}}}}}};
        QCOMPARE(a::decisionOptionsFromJson(config).modelRoutes[0].inputModalities,Modalities(11));
        auto routes=config["model_routes"].toArray();auto route=routes[0].toObject();route["model"]="/tmp/model.gguf";routes[0]=route;config["model_routes"]=routes;
        QVERIFY_THROWS_EXCEPTION(Error,a::decisionOptionsFromJson(config));
    }
    void contentOnlyRunSurvivesRecoveryAndFork() {
        Host h;QString id;
        {
            a::Engine engine(h.model,h.registry,h.policy,h.options);auto session=engine.createSession("model://text",h.workspace);id=session.id;
            a::RunRequest request;request.sessionId=id;request.content={image()};
            const auto result=engine.run(request).result.get();QCOMPARE(result.status,a::RunStatus::Completed);
            QCOMPARE(result.selectedModel,"model://vision");QCOMPARE(h.model->seen.last().messages.last().content,request.content);
            auto fork=engine.forkSession(id);QCOMPARE(fork.messages.first().content,request.content);
        }
        a::Engine restored(h.model,h.registry,h.policy,h.options);QCOMPARE(restored.session(id).messages.first().content,QJsonArray{image()});
    }
    void queuedAudioAndHistoryRequireBothModalities() {
        Host h;a::Engine engine(h.model,h.registry,h.policy,h.options);auto s=engine.createSession("model://text",h.workspace);
        a::RunRequest request;request.sessionId=s.id;request.content={image()};QCOMPARE(engine.run(request).result.get().status,a::RunStatus::Completed);
        engine.enqueueInput(s.id,{{"content",QJsonArray{audio()}}});
        const auto r=engine.runQueued({s.id}).result.get();QCOMPARE(r.status,a::RunStatus::Completed);QCOMPARE(r.selectedModel,"model://omni");
        QCOMPARE(h.model->seen.last().messages.last().content,QJsonArray{audio()});
    }
    void toolMediaChangesTheNextModelRoute() {
        Host h;h.options.decision.profiles.push_back({"Observe",100,10,1,0.9L});
        a::Tool tool;tool.definition={"Observe","Image observation",{{"type","object"}},{},true,true};
        tool.execute=[](const auto&,const auto&){a::ToolResult r{"observation",{{"verified",true}}};r.content={image()};return r;};h.registry->add(tool);
        h.model->next=[](const auto& r){return r.messages.last().role==a::MessageRole::Tool?a::ModelReply{"done"}:a::ModelReply{{},{{"call","Observe",{}}}};};
        a::Engine engine(h.model,h.registry,h.policy,h.options);auto s=engine.createSession("model://text",h.workspace);
        const auto r=engine.run({s.id,"inspect"}).result.get();QCOMPARE(r.status,a::RunStatus::Completed);QCOMPARE(h.model->seen.size(),2);
        QCOMPARE(h.model->seen[0].model,"model://text");QCOMPARE(h.model->seen[1].model,"model://vision");
        QCOMPARE(h.model->seen[1].messages.last().content,QJsonArray{image()});QVERIFY(a::pendingToolCalls(engine.session(s.id).messages).isEmpty());
    }
    void missingEconomicsAndUnsupportedModalitiesDoNotGenerate() {
        Host h;h.options.decision.profiles.resize(1);
        a::Engine engine(h.model,h.registry,h.policy,h.options);auto s=engine.createSession("model://text",h.workspace);
        a::RunRequest request;request.sessionId=s.id;request.content={image()};
        auto r=engine.run(request).result.get();QCOMPARE(r.status,a::RunStatus::Deferred);QCOMPARE(r.routing["reason"],"missing_host_estimate");QVERIFY(h.model->seen.isEmpty());
        Host other;other.options.decision.modelRoutes.resize(1);a::Engine second(other.model,other.registry,other.policy,other.options);
        s=second.createSession("model://text",other.workspace);request.sessionId=s.id;r=second.run(request).result.get();
        QCOMPARE(r.status,a::RunStatus::Deferred);QCOMPARE(r.routing["reason"],"unsupported_modalities");QVERIFY(other.model->seen.isEmpty());
    }
    void videoUsesVisionPathAndPreservesTimestamps() {
        Host h;a::Engine engine(h.model,h.registry,h.policy,h.options);auto s=engine.createSession("model://text",h.workspace);
        a::RunRequest request;request.sessionId=s.id;request.prompt="compare frames";request.content={video()};
        auto r=engine.run(request).result.get();QCOMPARE(r.status,a::RunStatus::Completed);QCOMPARE(r.selectedModel,"model://vision");
        QCOMPARE(h.model->seen.last().messages.last().content,request.content);QCOMPARE(r.routing["required_modalities"],11);
    }
    void currentGoalAndMediaDigestsReachTrustedDecisionInputs() {
        Host h;std::string goal,arguments;Modalities requirements=0;
        h.options.decision.inputs=[&](const a::DecisionRequest& r) {
            if(r.stage=="model_route"){goal=r.goal;arguments=r.candidates[0].arguments;requirements=r.requiredModalities;}
            a::DecisionInput input{"value"};for(const auto& c:r.candidates)input.estimates.push_back({c.id,100,10,1,0.9L});return input;
        };
        a::Engine engine(h.model,h.registry,h.policy,h.options);auto s=engine.createSession("model://text",h.workspace);
        engine.enqueueInput(s.id,{{"text","current queued goal"},{"content",QJsonArray{image()}}});
        QCOMPARE(engine.runQueued({s.id}).result.get().status,a::RunStatus::Completed);
        QCOMPARE(goal,std::string("current queued goal"));QCOMPARE(requirements,Modalities(3));
        QVERIFY(arguments.find("sha256")!=std::string::npos);QVERIFY(arguments.find("aW1hZ2U=")==std::string::npos);
    }
    void semanticCompactionReceivesTypedMediaAndRetainsOriginalRecords() {
        Host h;h.model->budget=[](const a::ModelRequest& r)->std::optional<ContextBudget> {
            qint64 tokens=10;for(const auto& m:r.messages)tokens+=m.text.size()/2+1+(contentModalities(m.content)!=1?1000:0);
            return ContextBudget{tokens,4096};
        };
        h.model->next=[](const auto& r){return a::ModelReply{r.summarizing?"Earlier image observation was summarized.":"done"};};
        a::Engine engine(h.model,h.registry,h.policy,h.options);auto s=engine.createSession("model://text",h.workspace);
        engine.enqueueInput(s.id,{{"text","original image task"},{"content",QJsonArray{image()}}});
        QCOMPARE(engine.runQueued({s.id}).result.get().status,a::RunStatus::Completed);
        QCOMPARE(engine.run({s.id,"second task"}).result.get().status,a::RunStatus::Completed);
        QCOMPARE(engine.run({s.id,"latest exact request"}).result.get().status,a::RunStatus::Completed);
        const auto compacted=engine.compact({s.id}).result.get();QCOMPARE(compacted.status,a::RunStatus::Completed);
        bool typed=false;
        for(const auto& request:h.model->seen)if(request.summarizing) {
            for(const auto& message:request.messages)if(contentModalities(message.content)==3)typed=true;
            QVERIFY(!request.messages.first().text.contains("aW1hZ2U="));
        }
        QVERIFY(typed);const auto stored=engine.session(s.id);QCOMPARE(stored.messages.first().content,QJsonArray{image()});
        bool latest=false;for(const auto& message:a::modelMessages(stored))if(message.text=="latest exact request")latest=true;QVERIFY(latest);
        QCOMPARE(engine.run({s.id,"continue"}).result.get().selectedModel,"model://text");
    }
    void authenticatedApiAcceptsContentOnlyRun() {
        Host h;a::ApiOptions options;options.workingDirectory=h.workspace;options.stateDirectory=h.root.filePath("api");
        options.engine=h.options;options.engine.sessionsDirectory.clear();QString credential(48,'a');options.clientTokens={{"test",credential}};
        a::Api api(h.model,h.registry,h.policy,options);
        const auto session=api.dispatch("agent.sessions.create",{{"model","model://text"}},credential).result.get().toObject();
        const auto result=api.dispatch("agent.run",{{"session_id",session["session_id"]},{"content",QJsonArray{image()}}},credential).result.get().toObject();
        QCOMPARE(result["status"],"completed");QCOMPARE(result["selected_model"],"model://vision");
    }
};
QTEST_GUILESS_MAIN(MultimodalTests)
#include "multimodal_tests.moc"
