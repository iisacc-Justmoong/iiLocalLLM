#include "agent/Subagents.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <atomic>
#include <chrono>
#include <thread>
using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
using namespace std::chrono_literals;
namespace {
void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
class Model final : public a::Model {
public:
    std::function<a::ModelReply(const a::ModelRequest&, const CancellationToken&)> next;
    a::ModelReply generate(const a::ModelRequest& r, const CancellationToken& t, const TextCallback&) override { return next(r,t); }
};
struct Host {
    QTemporaryDir root;
    QString workspace = root.filePath("workspace");
    std::shared_ptr<Model> model = std::make_shared<Model>();
    std::shared_ptr<a::ToolRegistry> registry = std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::PermissionPolicy> policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    a::EngineOptions eo;
    a::SubagentOptions so;
    std::shared_ptr<a::Subagents> agents;
    Host() {
        QDir().mkpath(workspace); eo.sessionsDirectory = root.filePath("parents");
        eo.projectContext.enabled=false; eo.compaction.automatic=false;
        so.workingDirectory=workspace; so.stateDirectory=root.filePath("children");
        a::SubagentDefinition p; p.name="reader"; p.description="Reader";
        p.systemPrompt="READER_SYSTEM"; p.tools={"Read", "Skill"}; p.background=true;
        so.definitions={p}; so.modelAliases={{"reader-model","child-model"}};
    }
    void skill(QByteArray fields = "context: fork\nagent: reader\nmodel: reader-model\n") {
        const auto path=workspace+"/.claude/skills/inspect/SKILL.md"; QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path); check(f.open(QIODevice::WriteOnly),"open skill");
        f.write("---\ndescription: Inspect evidence\n"+fields+"---\nPRIVATE_BODY $0 parent=${CLAUDE_SESSION_ID}\n");
    }
    std::unique_ptr<a::Engine> start(bool attached=true) {
        if(attached) { agents=std::make_shared<a::Subagents>(model,registry,policy,eo,so); a::Subagents::attach(eo,agents); }
        return std::make_unique<a::Engine>(model,registry,policy,eo);
    }
};
}
class SkillForkTests final : public QObject {
    Q_OBJECT
private slots:
    void fallbackModelAuthorizationAndParentPolicyRemainEnforced() {
        Host h; h.skill("context: fork\nagent: absent\nmodel: inherit\n"); int reads=0;
        h.policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,
            QList<a::PermissionRule>{{"Read",a::PermissionBehavior::Deny},{"Skill",a::PermissionBehavior::Deny}});
        h.registry->add({{"Read","Read file",{{"type","object"}}, {},true},[&](const auto&,const auto&){++reads;return a::ToolResult{"BAD"};}});
        h.model->next=[](const a::ModelRequest& r,const auto&){
            check(r.model=="parent","inherit model lost");
            if(r.messages.last().role!=a::MessageRole::Tool)return a::ModelReply{{},{{"read","Read",{}}}};
            check(r.messages.last().isError,"parent denial bypassed");return a::ModelReply{"DENIED_AS_EXPECTED",{}};
        };
        auto engine=h.start();const auto s=engine->createSession("parent",h.workspace);
        a::RunRequest r{s.id};r.skill="inspect";const auto result=engine->run(r).result.get();
        QVERIFY2(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage)); QCOMPARE(reads,0);
        QCOMPARE(h.agents->list(s.id)[0].toObject()["agent_type"],"reader");
        h.skill("context: fork\nagent: reader\nmodel: unauthorized\n");
        QCOMPARE(engine->run(r).result.get().errorCode,ErrorCode::InvalidArgument); QCOMPARE(h.agents->list(s.id).size(),1);
        h.skill("context: fork\nuser-invocable: false\n");
        QCOMPARE(engine->run(r).result.get().errorCode,ErrorCode::InvalidArgument); QCOMPARE(h.agents->list(s.id).size(),1);
    }
    void directForkLeavesQueuedInputPendingAndHonorsStopVeto() {
        Host h; h.skill(); int stops=0; QString parentId; std::unique_ptr<a::Engine> engine;
        h.eo.hooks.append([&](const a::HookInput& i,const auto&){
            if(i.kind==a::HookKind::SubagentStop) return a::HookResult{++stops==1,"TRY_AGAIN"};
            return a::HookResult{};
        });
        h.model->next=[&](const a::ModelRequest& r,const auto&){
            if(!stops) engine->enqueueInput(parentId,{{"text","QUEUED_PARENT"},{"priority","now"}});
            for(const auto& m:r.messages)check(!m.text.contains("QUEUED_PARENT"),"queued parent input leaked");
            return a::ModelReply{stops?"AFTER_VETO":"BEFORE_VETO",{}};
        };
        engine=h.start(); parentId=engine->createSession("parent",h.workspace).id;
        a::RunRequest r{parentId};r.skill="inspect";const auto result=engine->run(r).result.get();
        QVERIFY2(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage)); QCOMPARE(result.text,"AFTER_VETO");
        QCOMPARE(stops,2); QCOMPARE(result.turns,2); QCOMPARE(engine->queuedInputs(parentId)["count"],1);
    }
    void observerFailureJoinsChildAndDoesNotReplayOnReopen() {
        Host h;h.skill();int calls=0;h.model->next=[&](const auto&,const auto&){++calls;return a::ModelReply{"answer",{}};};
        auto engine=h.start();const auto s=engine->createSession("parent",h.workspace);a::RunRequest r{s.id};r.skill="inspect";
        const auto result=engine->run(r,[](const a::Event& e){if(e.kind==a::EventKind::ToolProgress)throw std::runtime_error("observer");}).result.get();
        QCOMPARE(result.status,a::RunStatus::Failed); QCOMPARE(result.errorCode,ErrorCode::ConsumerFailure); QCOMPARE(calls,0);
        const auto jobs=h.agents->list(s.id);QCOMPARE(jobs.size(),1);QVERIFY(jobs[0].toObject()["finished"].toBool());
        engine.reset();engine=std::make_unique<a::Engine>(h.model,h.registry,h.policy,h.eo);
        QCOMPARE(engine->run({s.id,"continue"}).result.get().status,a::RunStatus::Completed);QCOMPARE(calls,1);QCOMPARE(h.agents->list(s.id).size(),1);
    }
    void catalogRequiresAnExecutorAndKeepsInlineModelUnsupported() {
        Host h; h.skill(); auto catalog=a::discoverSkills(h.workspace);
        QCOMPARE(catalog.skills[0].executionContext,"fork"); QCOMPARE(catalog.skills[0].agent,"reader");
        QCOMPARE(catalog.skills[0].model,"reader-model"); QVERIFY(catalog.skills[0].unsupportedFeatures.isEmpty());
        auto engine=h.start(false); const auto s=engine->createSession("parent",h.workspace);
        QVERIFY(engine->skills(s.id).message().text.isEmpty());
        QVERIFY(engine->skills(s.id).skills[0].unsupportedFeatures.contains("fork-executor-unavailable"));
        a::RunRequest request{s.id}; request.skill="inspect";
        QCOMPARE(engine->run(request).result.get().errorCode,ErrorCode::RuntimeUnavailable);
        QVERIFY(engine->session(s.id).messages.isEmpty());
        h.skill("model: reader-model\n"); QVERIFY(!a::discoverSkills(h.workspace).skills[0].unsupportedFeatures.isEmpty());
    }
    void directInvocationIsIsolatedForegroundAndPreservesParentIdentity() {
        Host h; h.skill(); int calls=0, starts=0, stops=0; QString parentId;
        h.eo.hooks.append([&](const a::HookInput& i,const auto&){
            starts+=i.kind==a::HookKind::SubagentStart; stops+=i.kind==a::HookKind::SubagentStop; return a::HookResult{};
        });
        h.model->next=[&](const a::ModelRequest& r,const auto&){
            ++calls; check(r.contextId!=parentId,"parent model queried"); check(r.model=="child-model","alias not resolved");
            check(r.systemPrompt=="READER_SYSTEM","wrong child system"); check(r.generation.temperature==0.13,"generation lost");
            for(const auto& m:r.messages) check(!m.text.contains("PARENT_SECRET"),"parent history leaked");
            check(r.messages.last().text.contains("PRIVATE_BODY two words parent="+parentId),"wrong expansion context");
            check(r.messages.last().text.contains("ADDITIONAL"),"additional input lost");
            check(r.messages.last().metadata["iilocal.skill"].toObject()["invocation"]=="user","snapshot lost");
            for(const auto& m:r.messages) check(!m.metadata.contains("iilocal.skill_catalog"),"recursive fork advertised");
            a::ModelReply reply{"CHILD_RESULT",{}}; reply.usage.promptTokens=17; reply.usage.generatedTokens=3; return reply;
        };
        auto engine=h.start(); const auto s=engine->createSession("parent",h.workspace,"PARENT_SECRET"); parentId=s.id;
        { a::SessionStore store(h.eo.sessionsDirectory); auto lease=store.acquire(s.id); lease->append({{},a::MessageRole::User,"PARENT_SECRET"}); }
        a::RunRequest request{s.id,"ADDITIONAL"}; request.skill="inspect"; request.skillArguments="'two words'"; request.generation.temperature=0.13;
        const auto handle=engine->run(request); const auto result=handle.result.get();
        QVERIFY2(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage)); QCOMPARE(result.text,"CHILD_RESULT");
        QCOMPARE(result.sessionId,s.id); QCOMPARE(result.runId,handle.runId); QCOMPARE(result.turns,1);
        QCOMPARE(result.usage.promptTokens,17); QCOMPARE(result.usage.generatedTokens,3); QCOMPARE(calls,1); QCOMPARE(starts,1); QCOMPARE(stops,1);
        const auto jobs=h.agents->list(s.id); QCOMPARE(jobs.size(),1);
        const auto child=h.agents->output(s.id,jobs[0].toObject()["agentId"].toString());
        QCOMPARE(child["background"],false); QCOMPARE(child["fork_context"],false); QVERIFY(!child.contains("notification_id"));
        const auto saved=engine->session(s.id); QCOMPARE(saved.messages.size(),3);
        for(const auto& m:saved.messages) QVERIFY(!m.text.contains("PRIVATE_BODY"));
        QCOMPARE(saved.messages.last().metadata["iilocal.skill_fork"].toObject()["agentId"],child["agentId"]);
    }
    void nativeToolReturnsResultWithoutInjectingInstructionsOrReplaying() {
        Host h; h.skill(); QString parentId; int childCalls=0,parentCalls=0;
        h.model->next=[&](const a::ModelRequest& r,const auto&){
            if(r.contextId!=parentId) { ++childCalls; return a::ModelReply{"NATIVE_CHILD",{}}; }
            ++parentCalls;
            for(const auto& m:r.messages) check(!m.text.contains("PRIVATE_BODY"),"inline injection");
            if(r.messages.last().text=="invoke") return a::ModelReply{{},{{"skill-1","Skill",{{"skill","inspect"},{"args","evidence"}}}}};
            if(r.messages.last().role==a::MessageRole::Tool) {
                const auto& m=r.messages.last(); check(!m.isError,"fork failed"); check(m.data["status"]=="forked","wrong result status");
                check(m.data["result"]=="NATIVE_CHILD","missing actual child result");
                check(!m.metadata.contains("iilocal.skill_result"),"inline recovery marker");
            }
            return a::ModelReply{"PARENT_RESULT",{}};
        };
        auto engine=h.start(); const auto s=engine->createSession("parent",h.workspace); parentId=s.id;
        const auto result=engine->run({s.id,"invoke"}).result.get(); QVERIFY2(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage));
        QCOMPARE(result.text,"PARENT_RESULT"); QCOMPARE(childCalls,1); QCOMPARE(parentCalls,2); QCOMPARE(engine->session(s.id).messages.size(),4);
        engine.reset(); h.skill("context: fork\nagent: reader\n"); engine=std::make_unique<a::Engine>(h.model,h.registry,h.policy,h.eo);
        QCOMPARE(engine->run({s.id,"continue"}).result.get().status,a::RunStatus::Completed); QCOMPARE(childCalls,1);
        QVERIFY(a::pendingToolCalls(engine->session(s.id).messages).isEmpty());
    }
    void childFailureAndTurnLimitAreNotReportedCompleted() {
        for(bool limited:{false,true}) {
            Host h; h.skill(); h.model->next=[limited](const auto&,const auto&)->a::ModelReply{
                if(!limited) throw Error(ErrorCode::RuntimeFailure,"CHILD_FAILED");
                return {{},{{"missing","Read",{}}}};
            };
            auto engine=h.start(); const auto s=engine->createSession("parent",h.workspace);
            a::RunRequest r{s.id}; r.skill="inspect"; r.maxTurns=1; const auto result=engine->run(r).result.get();
            QCOMPARE(result.status,limited?a::RunStatus::TurnLimit:a::RunStatus::Failed);
            if(!limited) { QCOMPARE(result.errorCode,ErrorCode::RuntimeFailure); QVERIFY(result.errorMessage.contains("CHILD_FAILED")); }
            QCOMPARE(h.agents->list(s.id).size(),1);
        }
    }
    void cancellationAndDeadlineJoinTheChild() {
        for(bool deadline:{false,true}) {
            Host h; h.skill(); std::atomic<bool> entered=false;
            if(deadline) h.so.maxRuntimeMs=50;
            h.model->next=[&](const auto&,const auto& token)->a::ModelReply{
                entered=true; while(true){ token.throwIfCancelled(); std::this_thread::sleep_for(2ms); }
            };
            auto engine=h.start(); const auto s=engine->createSession("parent",h.workspace);
            a::RunRequest r{s.id}; r.skill="inspect"; const auto handle=engine->run(r);
            if(!deadline) { QTRY_VERIFY_WITH_TIMEOUT(entered.load(),2000); handle.cancel(); }
            QVERIFY(handle.result.wait_for(3s)==std::future_status::ready); const auto result=handle.result.get();
            QCOMPARE(result.status,deadline?a::RunStatus::Failed:a::RunStatus::Cancelled);
            QCOMPARE(result.errorCode,deadline?ErrorCode::Timeout:ErrorCode::Cancelled);
            const auto jobs=h.agents->list(s.id); QCOMPARE(jobs.size(),1); QVERIFY(jobs[0].toObject()["finished"].toBool());
        }
    }
};
QTEST_GUILESS_MAIN(SkillForkTests)
#include "skill_fork_tests.moc"
