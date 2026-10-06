#include "agent/Api.h"
#include "agent/DecisionProtocol.h"
#include "agent/Subagents.h"
#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <atomic>
#include <chrono>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
using namespace std::chrono_literals;
namespace {
class Model final:public a::Model {
public:
    std::atomic_int calls=0;
    std::function<a::ModelReply(const a::ModelRequest&)> next=[](const auto&){return a::ModelReply{"done"};};
    a::ModelReply generate(const a::ModelRequest& r,const CancellationToken&,const TextCallback&)override{++calls;return next(r);}
};
struct Host {
    QTemporaryDir root;QString workspace=root.filePath("work");
    std::shared_ptr<Model> model=std::make_shared<Model>();std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::PermissionPolicy> policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    a::EngineOptions options;
    Host(){QDir().mkpath(workspace);options.sessionsDirectory=root.filePath("sessions");options.skills.enabled=false;options.projectContext.enabled=false;options.compaction.automatic=false;options.decision.valueUnit="USD";}
    void tool(QString name,int& executions){a::Tool t;t.definition={name,name,{{"type","object"}},{},true,true};t.execute=[&executions](const auto& args,const auto&){++executions;return a::ToolResult{"ran",args};};registry->add(t);}
    void profitableRun(){options.decision.profiles={{"run",100,10,1,0.9L}};}
};
}
class DecisionEngineTests final:public QObject {
    Q_OBJECT
private slots:
    void defaultMissingEconomicsDefersBeforeModelOrHooks(){
        Host h;int hooks=0;h.options.hooks={[&](const auto&,const auto&){++hooks;return a::HookResult{};}};
        a::Engine engine(h.model,h.registry,h.policy,h.options);const auto s=engine.createSession("local",h.workspace);
        const auto r=engine.run({s.id,"work"}).result.get();QCOMPARE(r.status,a::RunStatus::Deferred);QCOMPARE(h.model->calls.load(),0);QCOMPARE(hooks,0);QCOMPARE(r.errorCode,ErrorCode::None);
        QCOMPARE(r.decision["reason"],"missing_host_estimate");QVERIFY(engine.session(s.id).messages.isEmpty());
        const auto records=engine.procedures(s.id)["procedures"].toArray();bool decision=false;
        for(const auto& entry:records)if(entry.toObject()["kind"]=="decision"&&entry.toObject()["phase"]=="completed")decision=true;QVERIFY(decision);
    }
    void lowRunProbabilityAndLowRunValueDoNotGenerate(){
        for(const auto& e:std::vector<a::DecisionEstimate>{{"run",100000,1,0,0.1L},{"run",1,0,1,0.9L}}){Host h;h.options.decision.profiles={e};a::Engine engine(h.model,h.registry,h.policy,h.options);auto s=engine.createSession("local",h.workspace);QCOMPARE(engine.run({s.id,"work"}).result.get().status,a::RunStatus::Deferred);QCOMPARE(h.model->calls.load(),0);}
    }
    void mostValuableToolExecutesOnceAndOtherCallsAreClosed(){
        Host h;h.profitableRun();int likely=0,valuable=0;h.tool("likely",likely);h.tool("valuable",valuable);
        h.options.decision.profiles.push_back({"likely",10,1,2,0.99L});h.options.decision.profiles.push_back({"valuable",100,10,5,0.8L});
        h.model->next=[](const auto& r){return r.messages.last().role==a::MessageRole::Tool?a::ModelReply{"done"}:a::ModelReply{{},{{"l","likely",{}},{"v","valuable",{}}}};};
        a::Engine engine(h.model,h.registry,h.policy,h.options);auto s=engine.createSession("local",h.workspace);auto r=engine.run({s.id,"work"}).result.get();QCOMPARE(r.status,a::RunStatus::Completed);QCOMPARE(likely,0);QCOMPARE(valuable,1);QCOMPARE(h.model->calls.load(),2);
        bool skipped=false;for(const auto& m:engine.session(s.id).messages)if(m.toolCallId=="l")skipped=m.data["not_executed"].toBool();QVERIFY(skipped);QVERIFY(a::pendingToolCalls(engine.session(s.id).messages).isEmpty());
    }
    void lowToolValueEndsWithDeferredWithoutRepeatingInference(){
        Host h;h.profitableRun();int executions=0;h.tool("costly",executions);h.options.decision.profiles.push_back({"costly",1,10,5,0.9L});
        h.model->next=[](const auto&){return a::ModelReply{{},{{"c","costly",{}}}};};a::Engine engine(h.model,h.registry,h.policy,h.options);auto s=engine.createSession("local",h.workspace);auto r=engine.run({s.id,"work"}).result.get();QCOMPARE(r.status,a::RunStatus::Deferred);QCOMPARE(executions,0);QCOMPARE(h.model->calls.load(),1);QVERIFY(a::pendingToolCalls(engine.session(s.id).messages).isEmpty());
    }
    void standaloneToolDefersBeforeAnyToolHook(){
        Host h;int executions=0,hooks=0;h.tool("work",executions);
        a::Engine engine(h.model,h.registry,h.policy,h.options);auto s=engine.createSession("local",h.workspace);
        a::ToolContext context{s.id,"direct",h.workspace};engine.bindDecisionContext(context);
        a::ToolRunnerOptions options;options.hooks={[&](const auto&,const auto&){++hooks;return a::HookResult{};}};
        auto result=a::ToolRunner(h.registry,h.policy,options).run({"c","work",{}},context);QVERIFY(result.metadata["iilocal.decision_deferred"].toBool());QCOMPARE(executions,0);QCOMPARE(hooks,0);
    }
    void changedHookArgumentsInvalidateTheDecisionReceipt(){
        Host h;h.profitableRun();int executions=0;h.tool("work",executions);
        h.options.decision.inputs=[](const a::DecisionRequest& r){a::DecisionInput in{"USD"};for(const auto& c:r.candidates)in.estimates.push_back({c.id,100,10,1,c.arguments.find("expensive")!=std::string::npos?0.1L:0.9L});return in;};
        h.options.hooks={[](const a::HookInput& input,const auto&){a::HookResult r;if(input.kind==a::HookKind::BeforeTool)r.updatedArguments=QJsonObject{{"expensive",true}};return r;}};
        h.model->next=[](const auto&){return a::ModelReply{{},{{"c","work",{}}}};};a::Engine engine(h.model,h.registry,h.policy,h.options);auto s=engine.createSession("local",h.workspace);auto r=engine.run({s.id,"work"}).result.get();QCOMPARE(r.status,a::RunStatus::Deferred);QCOMPARE(executions,0);QCOMPARE(r.decision["assessments"].toArray().first().toObject()["reason"],"low_probability");
    }
    void authenticatedHostSuppliesInputWhileTheExecutionQueueIsFull(){
        Host h;a::ApiOptions options;options.workingDirectory=h.workspace;options.stateDirectory=h.root.filePath("api");options.engine=h.options;options.engine.sessionsDirectory.clear();options.maxConcurrentRequests=1;options.maxQueuedRequests=0;options.procedures.intercept={"decision_input","decision"};
        QString auth(48,'a'),other(48,'b');options.clientTokens={{"app",auth},{"other",other}};a::Api api(h.model,h.registry,h.policy,options);
        auto rpc=[&](QString method,QJsonObject p,QString credential){return api.dispatch(method,p,credential).result.get().toObject();};
        auto sid=rpc("agent.sessions.create",{{"model","local"}},auth)["session_id"].toString();auto handle=api.dispatch("agent.run",{{"session_id",sid},{"prompt","work"}},auth);
        auto waiting=[&](QString kind){QJsonObject found;for(const auto& v:rpc("agent.procedures.list",{{"session_id",sid}},auth)["procedures"].toArray())if(v.toObject()["kind"]==kind&&v.toObject()["phase"]=="waiting")found=v.toObject();return found;};
        QJsonObject pending;QTRY_VERIFY_WITH_TIMEOUT(!(pending=waiting("decision_input")).isEmpty(),3000);
        QVERIFY_THROWS_EXCEPTION(Error,rpc("agent.procedures.respond",{{"procedure_id",pending["procedure_id"]},{"response",QJsonObject{{"action","continue"}}}},other));
        auto respond=[&](QJsonObject step,QJsonObject response){return rpc("agent.procedures.respond",{{"procedure_id",step["procedure_id"]},{"response",response}},auth);};
        auto output=QJsonObject{{"value_unit","USD"},{"estimates",QJsonArray{QJsonObject{{"candidate_id","run"},{"success_gain","100"},{"failure_loss","10"},{"cost","1"},{"success_probability","0.9"}}}},{"evidence",QJsonArray{}}};
        QCOMPARE(respond(pending,{{"action","replace"},{"output",output}})["accepted"],true);
        QTRY_VERIFY_WITH_TIMEOUT(!(pending=waiting("decision")).isEmpty(),3000);
        QVERIFY_THROWS_EXCEPTION(Error,respond(pending,{{"action","replace"},{"output",QJsonObject{{"action","execute"}}}}));respond(pending,{{"action","continue"}});
        // The next-turn run assessment receives the same trusted profile again.
        QTRY_VERIFY_WITH_TIMEOUT(!(pending=waiting("decision_input")).isEmpty(),3000);respond(pending,{{"action","replace"},{"output",output}});
        QTRY_VERIFY_WITH_TIMEOUT(!(pending=waiting("decision")).isEmpty(),3000);respond(pending,{{"action","continue"}});
        QVERIFY(handle.result.wait_for(3s)==std::future_status::ready);QCOMPARE(handle.result.get().toObject()["status"],"completed");QCOMPARE(h.model->calls.load(),1);
    }
    void childInheritsTheHostValuePolicyAndReturnsDeferred(){
        Host h;h.profitableRun();h.options.decision.profiles.push_back({"Agent",100,1,1,0.9L});
        h.options.decision.inputs=[](const a::DecisionRequest& r){a::DecisionInput in{"USD"};for(const auto& c:r.candidates)in.estimates.push_back({c.id,100,1,1,r.agentId.empty()?0.9L:0.1L});return in;};
        a::SubagentOptions config;config.stateDirectory=h.root.filePath("agents");config.workingDirectory=h.workspace;auto children=std::make_shared<a::Subagents>(h.model,h.registry,h.policy,h.options,config);a::Subagents::attach(h.options,children);
        a::Engine engine(h.model,h.registry,h.policy,h.options);auto s=engine.createSession("local",h.workspace);auto output=engine.runSubagentTool(s.id,"Agent",{{"prompt","child work"}});
        QCOMPARE(output.data["status"],"deferred");QCOMPARE(h.model->calls.load(),0);QCOMPARE(children->list(s.id).size(),1);
    }
};
QTEST_GUILESS_MAIN(DecisionEngineTests)
#include "decision_engine_tests.moc"
