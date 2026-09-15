#include <agent/CommandHooks.h>
#include <agent/Engine.h>
#include <agent/McpServer.h>
#include <agent/PermissionSettings.h>
#include <agent/Subagents.h>
#include <agent/ShellTasks.h>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtTest/QtTest>
#include <functional>
#include <mutex>
#include <thread>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
class Model final:public a::Model {
public:
    std::function<a::ModelReply(const a::ModelRequest&,const CancellationToken&)> next;
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken& token,const TextCallback&) override {
        token.throwIfCancelled();return next(request,token);
    }
};
a::Hook configured(const QString& work,const QString& event="Stop",double timeout=60,int input=1024*1024,int output=1024*1024) {
    a::CommandHookOptions limits;limits.workingDirectory=work;limits.maxInputBytes=input;limits.maxOutputBytes=output;
    return a::CommandHooks({{"hooks",QJsonObject{{event,QJsonArray{QJsonObject{{"hooks",QJsonArray{
        QJsonObject{{"type","agent"},{"prompt","Verify the host condition. $ARGUMENTS"},{"timeout",timeout}}}}}}}}}},limits).callback();
}
QString read(const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))return {};return QString::fromUtf8(f.readAll());}
void save(const QString& path,const QByteArray& bytes){QDir().mkpath(QFileInfo(path).absolutePath());QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())throw std::runtime_error("fixture write");}
QJsonObject grant(const QString& behavior,const QString& tool,const QString& pattern={}) {
    QJsonObject rule{{"toolName",tool}};if(!pattern.isEmpty())rule["ruleContent"]=pattern;
    return {{"type","addRules"},{"destination","session"},{"behavior",behavior},{"rules",QJsonArray{rule}}};
}
struct Fixture {
    QTemporaryDir root;QString work=root.filePath("work");
    std::shared_ptr<Model> model=std::make_shared<Model>();
    std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    a::EngineOptions options;
    Fixture(){QDir().mkpath(work);options.sessionsDirectory=root.filePath("state");options.skills.enabled=false;options.projectContext.enabled=false;
        a::registerWorkspaceTools(*registry,work,{},QStringList{options.sessionsDirectory});}
    bool clean() const {return QDir(root.filePath("state/hook-agents")).entryList(QDir::Dirs|QDir::NoDotAndDotDot).isEmpty();}
};
}
class AgentHooksTests final:public QObject {
    Q_OBJECT
private slots:
    void freshAgentReadsExactTranscriptAndCompletesWithoutLaterTools() {
        QTemporaryDir root;const auto work=root.filePath("work");QVERIFY(QDir().mkpath(work));
        auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();
        a::registerWorkspaceTools(*registry,work,{},QStringList{root.filePath("state")});
        a::EngineOptions options;options.sessionsDirectory=root.filePath("state");options.skills.enabled=false;options.projectContext.enabled=false;
        options.hooks={configured(work)};QString transcript;int mainCalls=0,verifierCalls=0;bool isolated=false,observed=false;
        model->next=[&](const a::ModelRequest& request,const CancellationToken&) {
            if(!request.verificationAgent){++mainCalls;return a::ModelReply{"PARENT_FINISHED"};}
            ++verifierCalls;
            if(verifierCalls==1) {
                isolated=!QJsonDocument(a::toJson(request.messages.first())).toJson().contains("PARENT_PRIVATE");
                return a::ModelReply{{},{{"read","Read",{{"path",transcript},{"limit",20000}}}}};
            }
            observed=request.messages.last().text.contains("PARENT_PRIVATE")&&!request.messages.last().isError;
            return a::ModelReply{{},{{"write","Write",{{"path","proof.txt"},{"content","VERIFIED"}}},
                {"decision","StructuredOutput",{{"ok",true}}},{"after","Write",{{"path","after.txt"},{"content","MUST_NOT_RUN"}}}}};
        };
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"Write(proof.txt)",a::PermissionBehavior::Allow},{"Write(after.txt)",a::PermissionBehavior::Allow}});
        a::Engine engine(model,registry,policy,options);const auto session=engine.createSession("model://fixture",work);transcript=engine.transcriptPath(session.id);
        const auto result=engine.run({session.id,"PARENT_PRIVATE"}).result.get();
        QCOMPARE(result.status,a::RunStatus::Completed);QCOMPARE(mainCalls,1);QCOMPARE(verifierCalls,2);QVERIFY(isolated&&observed);
        QCOMPARE(read(work+"/proof.txt"),"VERIFIED");QVERIFY(!QFileInfo::exists(work+"/after.txt"));
        QCOMPARE(engine.session(session.id).messages.size(),2);
        const QDir temporary(root.filePath("state/hook-agents"));QVERIFY(temporary.entryList(QDir::Dirs|QDir::NoDotAndDotDot).isEmpty());
    }
    void failedConditionRequestsAnotherParentTurn() {
        QTemporaryDir root;auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();
        a::EngineOptions options;options.sessionsDirectory=root.filePath("state");options.skills.enabled=false;options.projectContext.enabled=false;
        options.hooks={configured(root.path())};int parent=0,verifier=0;bool feedback=false;
        model->next=[&](const a::ModelRequest& request,const CancellationToken&) {
            if(request.verificationAgent)return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",++verifier>1},{"reason","NEEDS_MORE"}}}}};
            ++parent;if(parent==2)feedback=request.messages.last().text.contains("NEEDS_MORE");return a::ModelReply{parent==1?"unfinished":"fixed"};
        };
        a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(),options);const auto session=engine.createSession("model://fixture",root.path());
        const auto result=engine.run({session.id,"Finish the work"}).result.get();QCOMPARE(result.status,a::RunStatus::Completed);
        QCOMPARE(result.text,"fixed");QCOMPARE(parent,2);QCOMPARE(verifier,2);QVERIFY(feedback);
    }
    void assistantMessageLimit_data(){QTest::addColumn<int>("decisionAt");QTest::newRow("49th accepted")<<49;QTest::newRow("50th not executed")<<50;}
    void assistantMessageLimit() {
        QFETCH(int,decisionAt);QTemporaryDir root;auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();
        a::EngineOptions options;options.sessionsDirectory=root.filePath("state");options.skills.enabled=false;options.projectContext.enabled=false;
        options.hooks={configured(root.path())};int calls=0;QJsonObject diagnostic;
        model->next=[&](const a::ModelRequest& request,const CancellationToken&) {
            if(!request.verificationAgent)return a::ModelReply{"main"};
            if(++calls==decisionAt)return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
            return a::ModelReply{"I have not called the result tool yet"};
        };
        a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(),options);const auto session=engine.createSession("model://fixture",root.path());
        const auto result=engine.run({session.id,"Verify"},[&](const a::Event& event){if(event.kind==a::EventKind::Hook&&event.data["hook_type"]=="agent")diagnostic=event.data;}).result.get();
        QCOMPARE(result.status,a::RunStatus::Completed);
        QCOMPARE(calls,decisionAt);QCOMPARE(diagnostic["outcome"],decisionAt==49?"success":"cancelled");
    }
    void taskVerifierCanInspectTheSameBoard() {
        QTemporaryDir root;auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();
        a::EngineOptions options;options.sessionsDirectory=root.filePath("state");options.skills.enabled=false;options.projectContext.enabled=false;
        options.taskToolsEnabled=true;options.taskToolsDeferred=false;options.hooks={configured(root.path(),"TaskCreated")};int turns=0;bool inspected=false;
        model->next=[&](const a::ModelRequest& request,const CancellationToken&) {
            if(++turns==1)return a::ModelReply{{},{{"list","TaskList",{}}}};
            inspected=!request.messages.last().isError&&request.messages.last().data["total"]==0;
            return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(),options);const auto session=engine.createSession("model://fixture",root.path());
        const auto task=engine.runTaskTool(session.id,"TaskCreate",{{"subject","TEST"},{"description","verify"}});
        QVERIFY2(!task.isError,qPrintable(task.text));QVERIFY(inspected);QCOMPARE(turns,2);
        QCOMPARE(engine.runTaskTool(session.id,"TaskList").data["total"].toInt(),1);
    }
    void mcpVerifierUsesEngineOwnerForTasksAndRuntimeGrants() {
        Fixture f;f.options.taskToolsEnabled=true;f.options.taskToolsDeferred=false;
        a::PermissionSettingsOptions settings;settings.workingDirectory=f.work;settings.fallbackMode=a::PermissionMode::Bypass;
        auto policy=std::make_shared<a::SettingsPermissionPolicy>(settings);
        auto engine=std::make_shared<a::Engine>(f.model,f.registry,policy,f.options);
        a::McpServerOptions server;server.workingDirectory=f.work;server.model="model://fixture";server.engine=engine;
        const auto hook=configured(f.work,"PreToolUse");
        server.tools.hooks={ [hook](const a::HookInput& input,const CancellationToken& token){
            return input.kind==a::HookKind::BeforeTool&&input.call.name=="TaskList"?hook(input,token):a::HookResult{};}};
        const auto bridge=a::mcpServerOptions(f.registry,policy,server);
        mcp::ServerRequestContext context;context.sessionId="connection-1";
        auto call=[&](QString name,QJsonObject args=QJsonObject{}){return bridge.handlers.at("tools/call")({{"name",name},{"arguments",args}},context);};
        QVERIFY(!call("TaskCreate",{{"subject","OWNER_TASK"},{"description","parent board"}})["isError"].toBool());
        const auto owner=call("iiLocalLLM.agent.session")["structuredContent"].toObject()["session_id"].toString();
        QVERIFY(!owner.isEmpty()&&owner!=context.sessionId);
        policy->applyUpdates({grant("allow","Write","/granted.txt")},{owner,{},f.work});
        int turns=0;bool correctBoard=false,correctGrant=false;
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){
            if(++turns==1)return a::ModelReply{{},{{"list","TaskList",{}}}};
            if(turns==2){correctBoard=request.messages.last().text.contains("OWNER_TASK");
                return a::ModelReply{{},{{"yes","Write",{{"path","granted.txt"},{"content","GRANTED"}}},{"no","Write",{{"path","implicit.txt"},{"content","DENIED"}}}}};}
            correctGrant=!request.messages[request.messages.size()-2].isError&&request.messages.last().isError;
            return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        const auto result=call("TaskList");QVERIFY2(!result["isError"].toBool(),qPrintable(QJsonDocument(result).toJson()));
        QCOMPARE(turns,3);QVERIFY(correctBoard);QVERIFY(correctGrant);
        QCOMPARE(read(f.work+"/granted.txt"),"GRANTED");QVERIFY(!QFileInfo::exists(f.work+"/implicit.txt"));
        QCOMPARE(policy->describe({owner,{},f.work})["mode"],"bypassPermissions");
        QVERIFY(f.clean());
    }
    void verifierUsesDontAskWithExplicitRules_data() {
        QTest::addColumn<int>("mode");QTest::newRow("bypass")<<int(a::PermissionMode::Bypass);
        QTest::newRow("accept edits")<<int(a::PermissionMode::AcceptEdits);QTest::newRow("default")<<int(a::PermissionMode::Default);
    }
    void verifierUsesDontAskWithExplicitRules() {
        QFETCH(int,mode);Fixture f;f.options.hooks={configured(f.work)};int turns=0,permissions=0;bool checked=false;
        f.options.permission=[&](const auto&,const auto&,const auto&){++permissions;return true;};
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode(mode),QList<a::PermissionRule>{
            {"Write(yes.txt)",a::PermissionBehavior::Allow},{"Write(guard.txt)",a::PermissionBehavior::Allow},
            {"Write(guard.txt)",a::PermissionBehavior::Deny},{"Write(ask.txt)",a::PermissionBehavior::Ask}});
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){
            if(!request.verificationAgent)return a::ModelReply{"main"};
            if(++turns==1){QList<a::ToolCall> calls;for(const auto& name:{"yes.txt","implicit.txt","guard.txt","ask.txt"})
                calls.append({name,"Write",{{"path",name},{"content","DATA"}}});return a::ModelReply{{},calls};}
            const auto n=request.messages.size();checked=!request.messages[n-4].isError&&request.messages[n-3].isError&&request.messages[n-2].isError&&request.messages[n-1].isError;
            return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        a::Engine engine(f.model,f.registry,policy,f.options);const auto session=engine.createSession("fixture",f.work);
        QCOMPARE(engine.run({session.id,"verify"}).result.get().status,a::RunStatus::Completed);
        QCOMPARE(turns,2);QVERIFY(checked);QCOMPARE(permissions,0);QCOMPARE(read(f.work+"/yes.txt"),"DATA");
        for(const auto& path:{"implicit.txt","guard.txt","ask.txt"})QVERIFY(!QFileInfo::exists(f.work+'/'+path));
        QCOMPARE(policy->decide(f.registry->get("Write").definition,{{"path","implicit.txt"}},{session.id,{},f.work}).behavior,
            mode==int(a::PermissionMode::Default)?a::PermissionBehavior::Ask:a::PermissionBehavior::Allow);
        QVERIFY(f.clean());
    }
    void transcriptGrantDoesNotExposeAdjacentStateOrWrites() {
        Fixture f;f.options.hooks={configured(f.work)};QString transcript;int turns=0;bool checked=false;
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"Write",a::PermissionBehavior::Allow}});
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){
            if(!request.verificationAgent)return a::ModelReply{"main"};
            if(++turns==1)return a::ModelReply{{},{{"sibling","Read",{{"path",QFileInfo(transcript).dir().filePath("neighbor.txt")}}},
                {"alias","Read",{{"path","alias.txt"}}},{"write","Write",{{"path",transcript},{"content","CORRUPT"}}}}};
            const auto n=request.messages.size();checked=request.messages[n-3].isError&&request.messages[n-2].isError&&request.messages[n-1].isError;
            return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        a::Engine engine(f.model,f.registry,policy,f.options);const auto session=engine.createSession("fixture",f.work);transcript=engine.transcriptPath(session.id);
        save(QFileInfo(transcript).dir().filePath("neighbor.txt"),"PRIVATE_NEIGHBOR");QVERIFY(QFile::link(transcript,f.work+"/alias.txt"));
        QCOMPARE(engine.run({session.id,"PARENT_PRIVATE"}).result.get().status,a::RunStatus::Completed);
        QCOMPARE(turns,2);QVERIFY(checked);QVERIFY(read(transcript).contains("PARENT_PRIVATE"));QVERIFY(!read(transcript).contains("CORRUPT"));
        QVERIFY(f.clean());
    }
    void explicitTranscriptDenyOverridesTheExactReadGrant() {
        Fixture f;f.options.hooks={configured(f.work)};QString transcript;int turns=0;bool denied=false;
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"Read",a::PermissionBehavior::Deny}});
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){
            if(!request.verificationAgent)return a::ModelReply{"main"};
            if(++turns==1)return a::ModelReply{{},{{"read","Read",{{"path",transcript}}}}};
            denied=request.messages.last().isError;return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        a::Engine engine(f.model,f.registry,policy,f.options);const auto session=engine.createSession("fixture",f.work);transcript=engine.transcriptPath(session.id);
        QCOMPARE(engine.run({session.id,"verify"}).result.get().status,a::RunStatus::Completed);QVERIFY(denied);QVERIFY(f.clean());
    }
    void invalidResultAndForbiddenControlsCannotComplete() {
        Fixture f;f.options.hooks={configured(f.work)};int turns=0,executed=0;bool hidden=true,denied=false;
        const QStringList forbidden{"Agent","AgentOutput","TaskOutput","TaskStop","EnterPlanMode","ExitPlanMode","AskUserQuestion","Workflow","iiLocalLLM.agent.run","mcp__app__question"};
        for(const auto& name:forbidden){a::Tool tool;tool.definition={name,"forbidden",{{"type","object"}},{},true};
            if(name=="mcp__app__question")tool.definition.metadata={{"source","mcp"},{"requires_user_interaction",true}};
            tool.execute=[&](const auto&,const auto&){++executed;return a::ToolResult{};};f.registry->add(tool);}
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){
            if(!request.verificationAgent)return a::ModelReply{"main"};
            for(const auto& tool:request.tools)hidden&=!forbidden.contains(tool.name);
            if(++turns==1){QList<a::ToolCall> calls{{"invalid","StructuredOutput",{{"ok","yes"}}}};
                for(const auto& name:forbidden)calls.append({name,name,{}});return a::ModelReply{{},calls};}
            denied=true;for(auto i=request.messages.crbegin();i!=request.messages.crend()&&i->role==a::MessageRole::Tool;++i)denied&=i->isError;
            return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        a::Engine engine(f.model,f.registry,std::make_shared<a::RulePolicy>(),f.options);const auto session=engine.createSession("fixture",f.work);
        QCOMPARE(engine.run({session.id,"verify"}).result.get().status,a::RunStatus::Completed);
        QCOMPARE(turns,2);QVERIFY(hidden&&denied);QCOMPARE(executed,0);QVERIFY(f.clean());
    }
    void ownDeadlineDoesNotCancelParent() {
        Fixture f;f.options.hooks={configured(f.work,"Stop",0.03)};QJsonObject diagnostic;
        f.model->next=[](const a::ModelRequest& request,const CancellationToken& token)->a::ModelReply {
            if(!request.verificationAgent)return {"main"};
            while(!token.isCancelled())std::this_thread::sleep_for(std::chrono::milliseconds(1));token.throwIfCancelled();return {};
        };
        a::Engine engine(f.model,f.registry,std::make_shared<a::RulePolicy>(),f.options);const auto session=engine.createSession("fixture",f.work);
        const auto result=engine.run({session.id,"verify"},[&](const a::Event& event){if(event.kind==a::EventKind::Hook)diagnostic=event.data;}).result.get();
        QCOMPARE(result.status,a::RunStatus::Completed);QCOMPARE(diagnostic["outcome"],"cancelled");QCOMPARE(diagnostic["error_code"],"timeout");QVERIFY(f.clean());
    }
    void parentCancellationJoinsAndRemovesVerifier() {
        Fixture f;f.options.hooks={configured(f.work)};std::atomic_bool started=false;
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken& token)->a::ModelReply {
            if(!request.verificationAgent)return {"main"};started=true;
            while(!token.isCancelled())std::this_thread::sleep_for(std::chrono::milliseconds(1));token.throwIfCancelled();return {};
        };
        a::Engine engine(f.model,f.registry,std::make_shared<a::RulePolicy>(),f.options);const auto session=engine.createSession("fixture",f.work);
        auto handle=engine.run({session.id,"verify"});QTRY_VERIFY_WITH_TIMEOUT(started.load(),2000);handle.cancel();
        QCOMPARE(handle.result.get().status,a::RunStatus::Cancelled);QVERIFY(f.clean());
    }
    void verifierByteLimitsAreDiagnosedAndCleaned_data() {QTest::addColumn<bool>("inputLimit");QTest::newRow("input")<<true;QTest::newRow("output")<<false;}
    void verifierByteLimitsAreDiagnosedAndCleaned() {
        QFETCH(bool,inputLimit);Fixture f;f.options.hooks={configured(f.work,"Stop",60,inputLimit?2500:1024*1024,inputLimit?1024*1024:512)};
        QJsonObject diagnostic;int calls=0;
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){if(!request.verificationAgent)return a::ModelReply{"main"};++calls;return a::ModelReply{QString(600,'x')};};
        a::Engine engine(f.model,f.registry,std::make_shared<a::RulePolicy>(),f.options);const auto session=engine.createSession("fixture",f.work);
        const auto result=engine.run({session.id,"verify"},[&](const a::Event& event){if(event.kind==a::EventKind::Hook)diagnostic=event.data;}).result.get();
        QCOMPARE(result.status,a::RunStatus::Completed);QCOMPARE(diagnostic["outcome"],"non_blocking_error");QCOMPARE(diagnostic["error_code"],"resource_limit");
        QCOMPARE(calls,inputLimit?0:1);QVERIFY(f.clean());
    }
    void inlineSkillsAndDiscoveryWorkWithoutRecursiveSkillForks() {
        Fixture f;f.options.skills.enabled=true;f.options.hooks={configured(f.work)};int forks=0,turns=0,echoes=0;bool loaded=false,found=false,forkDenied=false;
        f.options.forkedSkill=[&](const auto&,const auto&){++forks;return a::SkillForkResult{};};
        save(f.work+"/.claude/skills/writer/SKILL.md","---\ndescription: Write verification proof\nallowed-tools: [Write]\n---\nINLINE_SKILL_MARKER");
        save(f.work+"/.claude/skills/delegate/SKILL.md","---\ndescription: Nested fork\ncontext: fork\n---\nNESTED_FORK");
        a::Tool echo;echo.definition={"DeferredEcho","Deferred observation",{{"type","object"}},{},true,true,false,true};
        echo.execute=[&](const auto&,const auto&){++echoes;return a::ToolResult{"OBSERVED"};};f.registry->add(echo);
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{{"Skill(writer)",a::PermissionBehavior::Allow},{"Skill(delegate)",a::PermissionBehavior::Allow}});
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){
            if(!request.verificationAgent)return a::ModelReply{"main"};
            if(++turns==1)return a::ModelReply{{},{{"skill","Skill",{{"skill","writer"}}},{"fork","Skill",{{"skill","delegate"}}},
                {"search","ToolSearch",{{"query","select:DeferredEcho"}}}}};
            if(turns==2){for(const auto& m:request.messages){loaded|=m.text.contains("INLINE_SKILL_MARKER");if(m.toolCallId=="fork")forkDenied=m.isError;}
                for(const auto& t:request.tools)found|=t.name=="DeferredEcho";
                return a::ModelReply{{},{{"echo","DeferredEcho",{}},{"write","Write",{{"path","skill.txt"},{"content","SKILL_GRANTED"}}}}};}
            return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        a::Engine engine(f.model,f.registry,policy,f.options);const auto session=engine.createSession("fixture",f.work);
        QCOMPARE(engine.run({session.id,"verify"}).result.get().status,a::RunStatus::Completed);
        QCOMPARE(turns,3);QCOMPARE(echoes,1);QCOMPARE(forks,0);QVERIFY(loaded&&found&&forkDenied);
        QCOMPARE(read(f.work+"/skill.txt"),"SKILL_GRANTED");QVERIFY(f.clean());
        QCOMPARE(policy->decide(f.registry->get("Write").definition,{{"path","skill.txt"}},{session.id,{},f.work}).behavior,a::PermissionBehavior::Ask);
    }
    void scopedSubagentHooksRetainResultToolAndPlanRestrictions() {
        Fixture f;f.options.hooks={configured(f.work,"SubagentStart"),configured(f.work,"SubagentStop")};
        a::Tool probe;probe.definition={"HostProbe","Read host evidence",{{"type","object"}},{},true};
        probe.execute=[](const auto&,const auto&){return a::ToolResult{"HOST_PROBE"};};f.options.additionalTools={probe};
        a::SubagentOptions options;options.workingDirectory=f.work;options.stateDirectory=f.root.filePath("children");
        a::SubagentDefinition profile;profile.name="reviewer";profile.description="Review";profile.tools={"Read","Write","HostProbe"};profile.permissionMode="plan";options.definitions={profile};
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"Write",a::PermissionBehavior::Allow}});
        a::SessionStore store(f.options.sessionsDirectory);const auto parent=store.create("fixture","",f.work);int verifier=0,children=0;bool resultVisible=true,probeVisible=true,denied=true;
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){
            if(!request.verificationAgent){++children;return a::ModelReply{"CHILD_DONE"};}
            bool output=false,host=false;for(const auto& t:request.tools){output|=t.name=="StructuredOutput";host|=t.name=="HostProbe";}
            resultVisible&=output;probeVisible&=host;
            if(++verifier%2==1)return a::ModelReply{{},{{"write","Write",{{"path","plan.txt"},{"content","FORBIDDEN"}}},{"probe","HostProbe",{}}}};
            const auto n=request.messages.size();denied&=request.messages[n-2].isError&&!request.messages[n-1].isError;
            return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        a::Subagents agents(f.model,f.registry,policy,f.options,options);a::ToolContext context{parent.id,{},f.work};context.sessionSnapshot=std::make_shared<a::Session>(parent);
        const auto result=agents.run(context,{{"prompt","Review"},{"subagent_type","reviewer"}});QVERIFY2(!result.isError,qPrintable(result.text));
        QCOMPARE(verifier,4);QCOMPARE(children,1);QVERIFY(resultVisible&&probeVisible&&denied);QVERIFY(!QFileInfo::exists(f.work+"/plan.txt"));
        QVERIFY(QDir(f.root.filePath("children/sessions/hook-agents")).entryList(QDir::Dirs|QDir::NoDotAndDotDot).isEmpty());
    }
    void backgroundShellsAreStoppedWhenVerificationEnds() {
        Fixture f;f.registry=std::make_shared<a::ToolRegistry>();auto shells=std::make_shared<a::ShellTasks>(f.work,f.root.filePath("shells"));
        a::registerWorkspaceTools(*f.registry,f.work,shells,{f.options.sessionsDirectory});f.options.hooks={configured(f.work)};
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{{"Bash(sleep 30)",a::PermissionBehavior::Allow}});
        int turns=0;QString task,agent;
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){
            if(!request.verificationAgent)return a::ModelReply{"main"};
            if(++turns==1)return a::ModelReply{{},{{"start","Bash",{{"command","sleep 30"},{"run_in_background",true}}}}};
            task=request.messages.last().data["backgroundTaskId"].toString();return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        a::Engine engine(f.model,f.registry,policy,f.options);const auto session=engine.createSession("fixture",f.work);
        const auto result=engine.run({session.id,"verify"},[&](const a::Event& event){if(event.kind==a::EventKind::Hook)agent=event.data["agent_id"].toString();}).result.get();
        QCOMPARE(result.status,a::RunStatus::Completed);QVERIFY(!agent.isEmpty()&&!task.isEmpty());
        QCOMPARE(shells->output(agent,task,false)["task"].toObject()["status"],"killed");QVERIFY(shells->list(session.id).isEmpty());QVERIFY(f.clean());
    }
    void configurationDeduplicatesAgentAndPromptSeparately() {
        Fixture f;a::CommandHookOptions limits;limits.workingDirectory=f.work;
        QJsonObject one{{"type","agent"},{"prompt","same"},{"model","first"}},two=one,prompt{{"type","prompt"},{"prompt","same"}};
        two["model"]="last";two["once"]=true;
        a::CommandHooks hooks({{"hooks",QJsonObject{{"Stop",QJsonArray{QJsonObject{{"hooks",QJsonArray{one,two,prompt}}}}}}}},limits);
        QCOMPARE(hooks.describe()["provider"],"configured");QCOMPARE(hooks.describe()["hooks"].toArray().first().toObject()["timeout_ms"],60000);
        auto context=std::make_shared<a::ModelHookContext>();context->model=f.model;context->modelName="fixture";
        a::HookInput input{a::HookKind::Stop,"owner","run"};input.modelContext=context;
        auto result=hooks.callback()(input,{});QCOMPARE(result.diagnostics.first().toObject()["error_code"],"runtime_unavailable");
        int agents=0,prompts=0;QString selected;
        f.model->next=[&](const auto&,const auto&){++prompts;return a::ModelReply{"{\"ok\":true}"};};
        context->agentExecutor=[&](const a::AgentHookRequest& request,const auto&,const auto&){++agents;selected=request.model;a::AgentHookReply reply;reply.decision={{"ok",true}};return reply;};
        // Missing model code above must not consume the once entry; only a
        // started executor consumes it. Prompt provider is independently run.
        hooks.callback()(input,{});hooks.callback()(input,{});QCOMPARE(agents,1);QCOMPARE(selected,"last");QCOMPARE(prompts,2);
    }
};
QTEST_GUILESS_MAIN(AgentHooksTests)
#include "agent_hooks_tests.moc"
