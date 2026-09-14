#include "agent/Subagents.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <QtTest/QtTest>
using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
class Model final : public a::Model {
public:
    std::function<a::ModelReply(const a::ModelRequest&)> next;
    bool budget=false;
    std::optional<ContextBudget> measure(const a::ModelRequest& r,const CancellationToken&) override {
        if(!budget)return {};qint64 size=100;for(const auto& m:r.messages)size+=m.text.size()+10;return ContextBudget{size,4096};
    }
    a::ModelReply generate(const a::ModelRequest& r, const CancellationToken&, const TextCallback&) override {
        auto reply = next(r); for (auto& c : reply.toolCalls) c.id = QUuid::createUuid().toString(QUuid::WithoutBraces); return reply;
    }
};
struct Host {
    QTemporaryDir root;
    QString workspace = root.filePath("workspace");
    std::shared_ptr<Model> model = std::make_shared<Model>();
    std::shared_ptr<a::ToolRegistry> registry = std::make_shared<a::ToolRegistry>();
    a::EngineOptions options;
    int writes = 0;
    Host() {
        QDir().mkpath(workspace);
        workspace = QFileInfo(workspace).canonicalFilePath();
        options.sessionsDirectory = root.filePath("sessions"); options.compaction.automatic = false; options.projectContext.enabled = false;
        registry->add({{"Write", "Write evidence", {{"type", "object"}}}, [this](const auto&, const auto&) { ++writes; return a::ToolResult{"WRITTEN"}; }});
        model->next = [](const auto& r) -> a::ModelReply {
            if (r.messages.last().role == a::MessageRole::Tool) return {r.messages.last().isError ? "DENIED" : "ALLOWED", {}};
            if (r.messages.last().text == "invoke") return {{}, {{"skill", "Skill", {{"skill", "writer"}}}, {"write", "Write", {{"path", "out.txt"}}}}};
            if (r.messages.last().metadata.contains("iilocal.skill_parent")) return {"DONE", {}};
            return {{}, {{"write", "Write", {{"path", "out.txt"}}}}};
        };
    }
    void skill(QByteArray fields = "allowed-tools: [Write]\n", QByteArray body = "ORIGINAL") {
        const auto path = workspace + "/.claude/skills/writer/SKILL.md"; QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path); check(f.open(QIODevice::WriteOnly), "open skill"); f.write("---\ndescription: Write a file\n" + fields + "---\n" + body);
    }
};
}
class SkillPermissionTests final : public QObject {
    Q_OBJECT
private slots:
    void directGrantExpiresAcrossRunsForksAndRestart() {
        Host h; h.skill(); auto policy = std::make_shared<a::RulePolicy>();
        QString session, fork;
        {
            a::Engine engine(h.model,h.registry,policy,h.options); session=engine.createSession("fixture",h.workspace).id;
            a::RunRequest r{session}; r.skill="writer";
            QCOMPARE(engine.run(r).result.get().text,"ALLOWED"); QCOMPARE(h.writes,1);
            fork=engine.forkSession(session).id;
            QCOMPARE(engine.run({session,"again"}).result.get().text,"DENIED");
            QCOMPARE(engine.run({fork,"again"}).result.get().text,"DENIED"); QCOMPARE(h.writes,1);
        }
        a::Engine reopened(h.model,h.registry,policy,h.options);
        QCOMPARE(reopened.run({session,"after restart"}).result.get().text,"DENIED"); QCOMPARE(h.writes,1);
    }
    void nativeInvocationNeedsPermissionAndAppliesInSameBatch() {
        for (bool allow : {false,true}) {
            Host h; h.skill(); int asks=0; QJsonObject preview;
            h.options.permission=[&](const auto& call,const auto& decision,const auto& context){
                ++asks; check(context.allowedTools.isEmpty(),"proposed grant already active");
                if(call.name=="Skill") { check(decision.reason.contains("Write"),"no permission detail"); return allow; }
                return false;
            };
            a::Engine engine(h.model,h.registry,std::make_shared<a::RulePolicy>(),h.options);
            const auto id=engine.createSession("fixture",h.workspace).id;
            const auto result=engine.run({id,"invoke"},[&](const a::Event& e){if(e.kind==a::EventKind::PermissionRequested&&e.data["name"]=="Skill")preview=e.data;}).result.get();
            QCOMPARE(result.status,a::RunStatus::Completed); QCOMPARE(h.writes,allow?1:0); QCOMPARE(asks,allow?1:2);
            QVERIFY(preview["permission_preview"].toObject()["_meta"].toObject()["skill"].toObject()["allowed_tools"].toArray().contains("Write"));
        }
    }
    void approvedSnapshotSurvivesFileChangeAndHookRetargeting() {
        Host h; h.skill(); int asks=0;
        const auto originalNext=h.model->next;h.model->next=[originalNext](const auto& r){auto reply=originalNext(r);
            for(auto& call:reply.toolCalls)if(call.name=="Skill")call.arguments["skill"]="absent-before-hook";return reply;};
        h.options.hooks.append([](const a::HookInput& i,const auto&){a::HookResult r;
            if(i.kind==a::HookKind::BeforeTool&&i.call.name=="Skill")r.updatedArguments=QJsonObject{{"skill","writer"}};return r;});
        h.options.permission=[&](const auto& call,const auto& decision,const auto&){
            if(call.name!="Skill") return false; ++asks; check(call.arguments["skill"]=="writer","permission used original hook arguments");
            check(decision.reason.contains("Write"),"missing exact grant"); h.skill("allowed-tools: [Bash]\n","CHANGED"); return true;
        };
        a::Engine engine(h.model,h.registry,std::make_shared<a::RulePolicy>(),h.options);const auto id=engine.createSession("fixture",h.workspace).id;
        QCOMPARE(engine.run({id,"invoke"}).result.get().status,a::RunStatus::Completed); QCOMPARE(asks,1);QCOMPARE(h.writes,1);
        bool found=false;
        for(const auto& m:engine.session(id).messages)if(m.metadata.contains("iilocal.skill_parent")){
            found=true; QVERIFY(m.text.contains("ORIGINAL")); QVERIFY(!m.text.contains("CHANGED"));
            QCOMPARE(m.metadata["iilocal.skill"].toObject()["allowed_tools"].toArray(),QJsonArray{"Write"});
        }
        QVERIFY(found);
    }
    void denyAskPlanAndPostHookVetoRemainAuthoritative() {
        for(int scenario=0;scenario<4;++scenario){
            Host h;h.skill();QList<a::PermissionRule> rules{{"Skill(writer)",a::PermissionBehavior::Allow}};
            if(scenario<2)rules.append({"Write",scenario==0?a::PermissionBehavior::Deny:a::PermissionBehavior::Ask});
            if(scenario==3)h.options.hooks.append([](const a::HookInput& i,const auto&){a::HookResult r;r.block=i.kind==a::HookKind::AfterTool&&i.call.name=="Skill";return r;});
            auto mode=scenario==2?a::PermissionMode::Plan:a::PermissionMode::Default;
            a::Engine engine(h.model,h.registry,std::make_shared<a::RulePolicy>(mode,rules),h.options);const auto id=engine.createSession("fixture",h.workspace).id;
            QCOMPARE(engine.run({id,"invoke"}).result.get().status,a::RunStatus::Completed);QCOMPARE(h.writes,0);
        }
    }
    void forkGrantIsChildOnlyAndResumeUsesCurrentCallerScope() {
        Host h;h.skill("context: fork\nagent: writer\nallowed-tools: [Write]\n");
        a::SubagentOptions sub;sub.stateDirectory=h.root.filePath("children");sub.workingDirectory=h.workspace;
        a::SubagentDefinition profile;profile.name="writer";profile.description="Writer";profile.tools={"Write","Skill"};sub.definitions={profile};
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{{"Skill(writer)",a::PermissionBehavior::Allow}});
        auto agents=std::make_shared<a::Subagents>(h.model,h.registry,policy,h.options,sub);a::Subagents::attach(h.options,agents);
        a::Engine engine(h.model,h.registry,policy,h.options);const auto id=engine.createSession("fixture",h.workspace).id;
        QCOMPARE(engine.run({id,"invoke"}).result.get().status,a::RunStatus::Completed);QCOMPARE(h.writes,1);
        auto jobs=agents->list(id);QCOMPARE(jobs.size(),1);
        a::ToolContext context{id,"resume",h.workspace};context.sessionSnapshot=std::make_shared<a::Session>(engine.session(id));
        const auto resumed=agents->run(context,{{"resume",jobs[0].toObject()["agentId"]},{"prompt","again"}});
        QVERIFY(!resumed.isError);QCOMPARE(h.writes,1);QCOMPARE(resumed.data["result"].toObject()["text"],"DENIED");
    }
    void metadataSyntaxAndUnsupportedArgumentsFailClosed() {
        Host h;h.skill("allowed-tools: 'Read, Bash(git status:*) Write(src/**)'\n");
        const auto info=a::discoverSkills(h.workspace).skills[0];QVERIFY(info.unsupportedFeatures.isEmpty());
        QCOMPARE(info.toJson()["allowed_tools"].toArray(),QJsonArray({"Read","Bash(git status:*)","Write(src/**)"}));
        h.skill("allowed-tools: 'Bash(unclosed'\n");QVERIFY_THROWS_EXCEPTION(Error,a::discoverSkills(h.workspace));
    }
    void queuedPromptsResetSkillGrantsButNotificationsDoNot() {
        for(bool notification:{false,true}) {
            Host h;h.skill();a::Engine engine(h.model,h.registry,std::make_shared<a::RulePolicy>(),h.options);
            const auto id=engine.createSession("fixture",h.workspace).id;bool sent=false;a::RunRequest request{id};request.skill="writer";
            const auto result=engine.run(request,[&](const a::Event& event){
                if(!sent&&event.kind==a::EventKind::ToolFinished){sent=true;engine.enqueueInput(id,{{"kind",notification?"notification":"prompt"},{"priority","next"},{"text","again"}});}
            }).result.get();
            QCOMPARE(result.status,a::RunStatus::Completed);QCOMPARE(h.writes,notification?2:1);
            QCOMPARE(result.text,notification?"ALLOWED":"DENIED");
        }
    }
    void recoveredPromptDoesNotRestoreAuthority() {
        Host h;h.skill();auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{{"Skill(writer)",a::PermissionBehavior::Allow}});
        QString id;
        {
            a::Engine engine(h.model,h.registry,policy,h.options);id=engine.createSession("fixture",h.workspace).id;
            const auto result=engine.run({id,"invoke"},[](const a::Event& e){
                if(e.kind==a::EventKind::Message&&e.data["metadata"].toObject().contains("iilocal.skill_result"))throw std::runtime_error("observer stopped");
            }).result.get();
            QCOMPARE(result.errorCode,ErrorCode::ConsumerFailure);QCOMPARE(h.writes,0);
        }
        a::Engine reopened(h.model,h.registry,policy,h.options);
        QCOMPARE(reopened.run({id,"continue"}).result.get().text,"DENIED");QCOMPARE(h.writes,0);
        int injected=0;for(const auto& m:reopened.session(id).messages)injected+=m.metadata.contains("iilocal.skill_parent");QCOMPARE(injected,1);
    }
    void ordinaryChildrenInheritCurrentScopeButPreloadsAndResumeDoNotGrant() {
        Host h;h.skill();auto policy=std::make_shared<a::RulePolicy>();
        a::SubagentOptions sub;sub.workingDirectory=h.workspace;sub.stateDirectory=h.root.filePath("children");
        a::SubagentDefinition p;p.name="writer";p.description="Writer";p.tools={"Write","Skill"};p.skills={"writer"};sub.definitions={p};
        auto agents=std::make_shared<a::Subagents>(h.model,h.registry,policy,h.options,sub);a::Subagents::attach(h.options,agents);
        a::Engine engine(h.model,h.registry,policy,h.options);const auto session=engine.createSession("fixture",h.workspace);
        a::ToolContext context{session.id,"launch",h.workspace};context.sessionSnapshot=std::make_shared<a::Session>(session);
        const QJsonObject args{{"subagent_type","writer"},{"prompt","write once"}};
        auto first=agents->run(context,args);QVERIFY(!first.isError);QCOMPARE(h.writes,0); // Preloaded body alone grants nothing.
        context.allowedTools={"Write"};auto background=args;background["run_in_background"]=true;
        auto second=agents->run(context,background);QVERIFY(!second.isError);context.allowedTools.clear();
        const auto done=agents->output(session.id,second.data["agentId"].toString(),true,10000);
        QCOMPARE(done["status"],"completed");QCOMPARE(h.writes,1); // Accepted background scope was frozen.
        const auto resumed=agents->run(context,{{"resume",second.data["agentId"]},{"prompt","write again"}});
        QVERIFY(!resumed.isError);QCOMPARE(h.writes,1);QCOMPARE(resumed.data["result"].toObject()["text"],"DENIED");
    }
    void automaticCompactionPreservesOnlyCurrentRunGrants() {
        Host h;h.skill();h.model->budget=true;h.options.compaction.automatic=true;h.options.compaction.keepRecentGroups=1;
        a::Engine engine(h.model,h.registry,std::make_shared<a::RulePolicy>(),h.options);const auto id=engine.createSession("fixture",h.workspace).id;
        {a::SessionStore store(h.options.sessionsDirectory);auto lease=store.acquire(id);
            lease->append({{},a::MessageRole::User,"old request"});lease->append({{},a::MessageRole::Assistant,{},{{"old-call","Read",{}}}});
            lease->append({{},a::MessageRole::Tool,QString(3000,'x'),{},"old-call"});lease->append({{},a::MessageRole::Assistant,"old result"});}
        a::RunRequest request{id};request.skill="writer";request.generation.maxTokens=256;
        const auto result=engine.run(request).result.get();QVERIFY2(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage));
        QCOMPARE(result.usage.compactions,1);QCOMPARE(h.writes,1);
        QCOMPARE(engine.run({id,"after compaction"}).result.get().text,"DENIED");QCOMPARE(h.writes,1);
    }
};
QTEST_GUILESS_MAIN(SkillPermissionTests)
#include "skill_permission_tests.moc"
