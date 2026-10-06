#include "agent/Api.h"
#include "agent/Procedures.h"
#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <QtCore/QJsonDocument>
#include <QtTest/QtTest>
#include <atomic>
#include <chrono>
#include <thread>
using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
using namespace std::chrono_literals;
namespace {
class Model final : public a::Model {
public:
    std::function<a::ModelReply(const a::ModelRequest&,const CancellationToken&)> next;
    a::ModelReply generate(const a::ModelRequest& r,const CancellationToken& t,const TextCallback&) override { return next(r,t); }
};
QJsonObject schema() {return {{"type","object"},{"properties",QJsonObject{{"value",QJsonObject{{"type","integer"}}}}},{"required",QJsonArray{"value"}},{"additionalProperties",false}};}
struct Host {
    QTemporaryDir root;
    QString workspace=root.filePath("workspace");
    std::shared_ptr<Model> model=std::make_shared<Model>();
    std::shared_ptr<a::ToolRegistry> tools=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::PermissionPolicy> policy=std::make_shared<a::RulePolicy>();
    a::EngineOptions options{.decision={.enabled=false}};
    Host(){QDir().mkpath(workspace);options.sessionsDirectory=root.filePath("sessions");options.projectContext.enabled=false;options.compaction.automatic=false;}
};
}
class ProcedureTests final : public QObject {
    Q_OBJECT
private slots:
    void hostChangesModelReturnBeforeDispatch() {
        Host h;int executed=0;
        a::Tool tool;tool.definition={"echo","echo",schema(),schema(),true,true};
        tool.execute=[&](const auto& args,const auto&){++executed;return a::ToolResult{"observed",args};};h.tools->add(tool);
        h.options.procedures=std::make_shared<a::Procedures>(a::ProcedureOptions{},[](const QJsonObject& step,const auto&){
            a::ProcedureResponse response;
            if(step["kind"]=="model"&&!step["output"].toObject()["tool_calls"].toArray().isEmpty()) {
                response.action=a::ProcedureAction::Replace;
                response.output={{"text",""},{"tool_calls",QJsonArray{QJsonObject{{"id","host-call"},{"name","echo"},{"arguments",QJsonObject{{"value",7}}}}}}};
            }
            return response;
        });
        h.model->next=[](const auto& r,const auto&){if(r.messages.last().role==a::MessageRole::Tool)return a::ModelReply{QString::number(r.messages.last().data["value"].toInt())};return a::ModelReply{{},{{"model-call","echo",{{"value",1}}}}};};
        a::Engine engine(h.model,h.tools,h.policy,h.options);auto session=engine.createSession("local",h.workspace);
        QList<QJsonObject> events;auto result=engine.run({session.id,"work"},[&](const auto& event){if(event.kind==a::EventKind::Procedure)events.append(event.data);}).result.get();
        QCOMPARE(result.status,a::RunStatus::Completed);QCOMPARE(result.text,"7");QCOMPARE(executed,1);
        QVERIFY(!events.isEmpty());QCOMPARE(events.first()["kind"],"run");QCOMPARE(events.first()["phase"],"started");
        QCOMPARE(events.first()["controllable"],false);
        QVERIFY_THROWS_EXCEPTION(Error,engine.respondProcedure(events.first()["procedure_id"].toString(),{{"action","cancel"}}));
        bool replaced=false;for(const auto& step:events)if(step["kind"]=="model"&&step["phase"]=="completed"&&step["action"]=="replace")replaced=true;
        QVERIFY(replaced);QVERIFY(h.options.procedures->list(session.id)["procedures"].toArray().size()>=6);
    }
    void remoteReturnControlWorksWhileApiQueueIsFull() {
        Host h;a::ApiOptions options{.engine={.decision={.enabled=false}}};options.workingDirectory=h.workspace;options.stateDirectory=h.root.filePath("api");
        const QString token(48,'a'),other(48,'b');options.clientTokens={{"first",token},{"second",other}};
        options.maxConcurrentRequests=1;options.maxQueuedRequests=0;options.engine=h.options;options.engine.sessionsDirectory.clear();options.procedures.intercept={"model"};
        h.model->next=[](const auto&,const auto&){return a::ModelReply{"original"};};
        a::Api api(h.model,h.tools,h.policy,options);
        auto call=[&](QString method,QJsonObject input,QString auth){return api.dispatch(method,input,auth).result.get().toObject();};
        QCOMPARE(call("agent.info",{},token)["procedures"].toObject()["intercept"].toArray(),QJsonArray{"model"});
        const auto sid=call("agent.sessions.create",{{"model","local"}},token)["session_id"].toString();
        auto run=api.dispatch("agent.run",{{"session_id",sid},{"prompt","work"}},token);
        QJsonObject pending;
        QTRY_VERIFY_WITH_TIMEOUT(([&]{auto list=call("agent.procedures.list",{{"session_id",sid}},token);for(const auto& v:list["procedures"].toArray())if(v.toObject()["phase"]=="waiting"){pending=v.toObject();return true;}return false;})(),3000);
        QVERIFY(api.isControlMethod("agent.procedures.respond"));
        QVERIFY_THROWS_EXCEPTION(Error,call("agent.procedures.list",{{"session_id",sid}},other));
        const auto pid=pending["procedure_id"].toString();
        QVERIFY_THROWS_EXCEPTION(Error,call("agent.procedures.respond",{{"procedure_id",pid},{"response",QJsonObject{{"action","replace"},{"output",QJsonObject{{"text",4}}}}}},token));
        const QJsonObject response{{"action","replace"},{"output",QJsonObject{{"text","host answer"},{"tool_calls",QJsonArray{}}}}};
        QCOMPARE(call("agent.procedures.respond",{{"procedure_id",pid},{"response",response}},token)["accepted"],true);
        QCOMPARE(call("agent.procedures.respond",{{"procedure_id",pid},{"response",response}},token)["replayed"],true);
        QVERIFY_THROWS_EXCEPTION(Error,call("agent.procedures.respond",{{"procedure_id",pid},{"response",QJsonObject{{"action","cancel"}}}},token));
        QVERIFY(run.result.wait_for(3s)==std::future_status::ready);QCOMPARE(run.result.get().toObject()["text"],"host answer");
    }
    void replacedToolDataMustSatisfyOutputSchema() {
        Host h;a::Tool tool;tool.definition={"echo","echo",schema(),schema(),true,true};tool.execute=[](const auto& args,const auto&){return a::ToolResult{"real",args};};h.tools->add(tool);
        h.options.procedures=std::make_shared<a::Procedures>(a::ProcedureOptions{},[](const QJsonObject& step,const auto&){
            a::ProcedureResponse response;if(step["kind"]=="tool"){response.action=a::ProcedureAction::Replace;response.output={{"text","forged"},{"data",QJsonObject{{"value","wrong type"}}},{"is_error",false}};}return response;
        });
        h.model->next=[](const auto&,const auto&){return a::ModelReply{{},{{"call","echo",{{"value",1}}}}};};
        a::Engine engine(h.model,h.tools,h.policy,h.options);auto session=engine.createSession("local",h.workspace);
        auto result=engine.run({session.id,"work"}).result.get();QCOMPARE(result.status,a::RunStatus::Failed);
        const auto messages=engine.session(session.id).messages;QVERIFY(messages.last().isError);
    }
    void toolReplacementChangesNextModelObservationWithoutRepeatingExecution() {
        Host h;int executions=0;a::Tool tool;tool.definition={"echo","echo",schema(),schema(),true,true};
        tool.execute=[&](const auto& args,const auto&){++executions;a::ToolResult result{"native",args};result.metadata={{"native_marker",true}};return result;};h.tools->add(tool);
        h.options.procedures=std::make_shared<a::Procedures>(a::ProcedureOptions{},[](const QJsonObject& step,const auto&){
            return step["kind"]=="tool"?a::ProcedureResponse{a::ProcedureAction::Replace,{{"text","host result"},{"data",QJsonObject{{"value",22}}},{"is_error",false}}}:a::ProcedureResponse{};
        });
        h.model->next=[](const auto& request,const auto&){return request.messages.last().role==a::MessageRole::Tool?a::ModelReply{QString::number(request.messages.last().data["value"].toInt())}:a::ModelReply{{},{{"call","echo",{{"value",1}}}}};};
        a::Engine engine(h.model,h.tools,h.policy,h.options);const auto sid=engine.createSession("local",h.workspace).id;
        QCOMPARE(engine.run({sid,"work"}).result.get().text,"22");QCOMPARE(executions,1);
        bool marker=false;for(const auto& message:engine.session(sid).messages)if(message.role==a::MessageRole::Tool)marker=message.metadata["native_marker"].toBool();QVERIFY(marker);
    }
    void duplicateChildRequestReusesOneExecutionAndSurvivesRestart() {
        Host h;std::atomic_int calls=0;h.model->next=[&](const auto&,const auto&){++calls;return a::ModelReply{"child result"};};
        a::SessionStore store(h.options.sessionsDirectory);const auto parent=store.create("local","",h.workspace);
        a::ToolContext context{parent.id,"parent-run",h.workspace};context.sessionSnapshot=std::make_shared<a::Session>(parent);
        a::SubagentOptions options;options.workingDirectory=h.workspace;options.stateDirectory=h.root.filePath("children");
        const QJsonObject args{{"prompt","one task"},{"idempotency_key","job-1"}};QString child;
        {a::Subagents agents(h.model,h.tools,h.policy,h.options,options);auto first=agents.run(context,args);auto repeated=agents.run(context,args);
         QCOMPARE(calls.load(),1);child=first.data["agentId"].toString();QCOMPARE(repeated.data["agentId"].toString(),child);QCOMPARE(repeated.data["reused"],true);
         QVERIFY_THROWS_EXCEPTION(Error,agents.run(context,{{"prompt","different task"},{"idempotency_key","job-1"}}));}
        a::Subagents agents(h.model,h.tools,h.policy,h.options,options);auto repeated=agents.run(context,args);QCOMPARE(calls.load(),1);QCOMPARE(repeated.data["agentId"].toString(),child);
        auto automatic=agents.run(context,{{"prompt","automatic task"}});auto duplicate=agents.run(context,{{"prompt","automatic task"}});QCOMPARE(calls.load(),2);QCOMPARE(duplicate.data["agentId"],automatic.data["agentId"]);
        context.runId="next-run";agents.run(context,{{"prompt","automatic task"}});QCOMPARE(calls.load(),3);
    }
    void pendingCancellationTimeoutAndCompletionReplacementAreVisible() {
        for(bool cancel:{false,true}){
            Host h;a::ProcedureOptions options;options.intercept={"model"};options.timeoutMs=50;
            h.options.procedures=std::make_shared<a::Procedures>(options);h.model->next=[](const auto&,const auto&){return a::ModelReply{"candidate"};};
            a::Engine engine(h.model,h.tools,h.policy,h.options);const auto sid=engine.createSession("local",h.workspace).id;
            auto run=engine.run({sid,"work"},[&](const auto& event){if(cancel&&event.kind==a::EventKind::Procedure&&event.data["phase"]=="waiting")engine.respondProcedure(event.data["procedure_id"].toString(),{{"action","cancel"},{"reason","host stop"}});});
            const auto result=run.result.get();QCOMPARE(result.errorCode,cancel?ErrorCode::Cancelled:ErrorCode::Timeout);
            bool found=false;for(const auto& value:engine.procedures(sid)["procedures"].toArray())if(value.toObject()["kind"]=="model")found=value.toObject()["phase"]==(cancel?"cancelled":"expired");QVERIFY(found);
        }
        Host h;h.options.procedures=std::make_shared<a::Procedures>(a::ProcedureOptions{},[](const QJsonObject& step,const auto&){
            return step["kind"]=="completion"?a::ProcedureResponse{a::ProcedureAction::Replace,{{"text","host final"}}}:a::ProcedureResponse{};
        });h.model->next=[](const auto&,const auto&){return a::ModelReply{"candidate"};};
        a::Engine engine(h.model,h.tools,h.policy,h.options);const auto sid=engine.createSession("local",h.workspace).id;
        QCOMPARE(engine.run({sid,"work"}).result.get().text,"host final");QCOMPARE(engine.session(sid).messages.last().text,"host final");
    }
    void parentControlsPendingChildAndDuplicateDoesNotConsumeAnotherSlot() {
        Host h;std::atomic_int calls=0;h.model->next=[&](const auto&,const auto&){++calls;return a::ModelReply{"child candidate"};};
        a::ProcedureOptions config;config.intercept={"model"};h.options.procedures=std::make_shared<a::Procedures>(config);
        a::SessionStore store(h.options.sessionsDirectory);const auto parent=store.create("local","",h.workspace);
        a::ToolContext context{parent.id,"parent-run",h.workspace};context.sessionSnapshot=std::make_shared<a::Session>(parent);context.procedures=h.options.procedures;
        a::ProcedureScope parentStep("run",context);context.procedureId=parentStep.id();
        a::SubagentOptions options;options.workingDirectory=h.workspace;options.stateDirectory=h.root.filePath("children");options.maxConcurrent=1;
        a::Subagents agents(h.model,h.tools,h.policy,h.options,options);
        const QJsonObject args{{"prompt","delegated task"},{"run_in_background",true}};
        const auto first=agents.run(context,args);QJsonObject pending;
        QTRY_VERIFY_WITH_TIMEOUT(([&]{for(const auto& value:h.options.procedures->list(parent.id)["procedures"].toArray())if(value.toObject()["phase"]=="waiting"){pending=value.toObject();return true;}return false;})(),3000);
        QCOMPARE(pending["agent_id"],first.data["agentId"]);QVERIFY(pending["session_id"]!=parent.id);
        QCOMPARE(pending["owner_session_id"],parent.id);
        bool linked=false;for(const auto& value:h.options.procedures->list(parent.id)["procedures"].toArray())if(value.toObject()["kind"]=="run"&&value.toObject()["agent_id"]==first.data["agentId"])linked=value.toObject()["parent_procedure_id"]==parentStep.id();QVERIFY(linked);
        const auto repeated=agents.run(context,args);QCOMPARE(repeated.data["agentId"],first.data["agentId"]);QCOMPARE(repeated.data["reused"],true);QCOMPARE(repeated.data["finished"],false);QCOMPARE(calls.load(),1);
        const auto pid=pending["procedure_id"].toString();
        QVERIFY_THROWS_EXCEPTION(Error,h.options.procedures->respond(pid,{{"action","continue"}},"other-owner"));
        h.options.procedures->respond(pid,{{"action","replace"},{"output",QJsonObject{{"text","parent-controlled child"},{"tool_calls",QJsonArray{}}}}},parent.id);
        const auto result=agents.output(parent.id,first.data["agentId"].toString(),true,3000);QCOMPARE(result["result"].toObject()["text"],"parent-controlled child");
        const auto receipt=agents.run(context,args);QCOMPARE(receipt.data["finished"],true);QCOMPARE(calls.load(),1);
        agents.run(context,{{"prompt","intentional resume"},{"resume",first.data["agentId"]},{"run_in_background",true}});
        QJsonObject resumed;
        QTRY_VERIFY_WITH_TIMEOUT(([&]{for(const auto& value:h.options.procedures->list(parent.id)["procedures"].toArray())if(value.toObject()["phase"]=="waiting"){resumed=value.toObject();return true;}return false;})(),3000);
        const auto originalReceipt=agents.run(context,args);QCOMPARE(originalReceipt.data["finished"],true);QCOMPARE(originalReceipt.data["execution_phase"],"running");
        QCOMPARE(originalReceipt.data["result"].toObject()["text"],"parent-controlled child");
        h.options.procedures->respond(resumed["procedure_id"].toString(),{{"action","cancel"}},parent.id);
        QCOMPARE(agents.output(parent.id,first.data["agentId"].toString(),true,3000)["status"],"cancelled");parentStep.returned({{"finished",true}});
    }
    void initialReceiptRemainsStableAfterIntentionalResume() {
        Host h;h.model->next=[](const auto& request,const auto&){return a::ModelReply{request.messages.last().text};};
        a::SessionStore store(h.options.sessionsDirectory);const auto parent=store.create("local","",h.workspace);
        a::ToolContext context{parent.id,"parent-run",h.workspace};context.sessionSnapshot=std::make_shared<a::Session>(parent);
        a::SubagentOptions options;options.workingDirectory=h.workspace;options.stateDirectory=h.root.filePath("children");
        a::Subagents agents(h.model,h.tools,h.policy,h.options,options);const QJsonObject args{{"prompt","first result"},{"idempotency_key","stable"}};
        const auto first=agents.run(context,args);const auto resumed=agents.run(context,{{"prompt","resumed result"},{"resume",first.data["agentId"]}});
        QCOMPARE(resumed.data["result"].toObject()["text"],"resumed result");
        QCOMPARE(agents.run(context,args).data["result"].toObject()["text"],"first result");
    }
    void completedRecordsAreEvictedWithinPagingAndMemoryLimits() {
        a::ProcedureOptions config;config.maxRecords=8;config.maxRecordBytes=8192;config.maxBytes=8192;
        a::ToolContext context{"owner","run"};context.procedures=std::make_shared<a::Procedures>(config);
        for(int i=0;i<12;++i){a::ProcedureScope step("input",context);step.returned({{"text",QString(512,'a')}});}
        const auto page=context.procedures->list("owner");const auto records=page["procedures"].toArray();QVERIFY(!records.isEmpty());QVERIFY(records.size()<12);
        QVERIFY(QJsonDocument(page).toJson(QJsonDocument::Compact).size()<=config.maxRecordBytes);
        for(const auto& record:records)QCOMPARE(record.toObject()["phase"],"completed");
        QVERIFY(context.procedures->list("owner",page["next_cursor"].toInteger())["procedures"].toArray().isEmpty());
    }
};
QTEST_GUILESS_MAIN(ProcedureTests)
#include "procedure_tests.moc"
