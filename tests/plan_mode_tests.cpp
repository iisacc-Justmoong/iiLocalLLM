#include <agent/Engine.h>
#include <agent/Api.h>
#include <agent/McpServer.h>
#include <agent/PermissionRequests.h>
#include <agent/PermissionSettings.h>
#include <agent/CommandHooks.h>
#include <agent/Subagents.h>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <future>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
using namespace std::chrono_literals;
namespace {
void write(const QString& path,const QByteArray& bytes) {
    QFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size())throw std::runtime_error("Fixture write failed");
}
QByteArray read(const QString& path){QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Fixture read failed");return file.readAll();}
class Model final:public a::Model {
public:
    int turn=0;QList<a::ModelRequest> requests;
    std::function<a::ModelReply(const a::ModelRequest&)> generateReply;
    a::ModelReply generate(const a::ModelRequest& r,const CancellationToken&,const TextCallback&) override {
        requests.append(r);++turn;if(generateReply)return generateReply(r);
        if(turn==1)return {{},{{"enter","EnterPlanMode",{}}}};
        return {"DONE",{}};
    }
};
struct Fixture {
    QTemporaryDir root;QString workspace=root.filePath("work");
    std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<const a::PermissionPolicy> policy=std::make_shared<a::RulePolicy>();
    std::shared_ptr<a::PlanMode> plans; a::ToolRunnerOptions options;
    Fixture(bool withinWorkspace=false) {
        QDir().mkpath(workspace);plans=std::make_shared<a::PlanMode>(withinWorkspace?workspace+"/.private-plans":root.filePath("plans"));
        a::registerWorkspaceTools(*registry,workspace);for(auto tool:plans->tools(policy,false))registry->add(std::move(tool));
        options.planning=plans;options.permission=[](const auto& call,const auto&,const auto&){return call.name=="ExitPlanMode";};
    }
    a::ToolContext context(QString id="owner"){return {id,"run",workspace,root.filePath("artifacts")};}
    a::ToolResult run(QString name,QJsonObject args={},QString id="owner",a::EventCallback event={}) {
        return a::ToolRunner(registry,policy,options).run({"call",name,args},context(id),event);
    }
    QString plan(QString id="owner"){return plans->status(id)["plan_file_path"].toString();}
    void draft(QByteArray content="Read the source, change the parser, and run tests.") {
        const auto entered=run("EnterPlanMode");if(entered.isError)throw std::runtime_error(entered.text.toStdString());
        const auto value=run("Write",{{"path",plan()},{"content",QString::fromUtf8(content)}});
        if(value.isError)throw std::runtime_error(value.text.toStdString());
    }
};
}
class PlanModeTests final:public QObject {
    Q_OBJECT
private slots:
    void verificationAgentCannotReadAnotherSessionsPlanInsideWorkspace() {
        QTemporaryDir root;auto registry=std::make_shared<a::ToolRegistry>();auto model=std::make_shared<Model>();
        a::registerWorkspaceTools(*registry,root.path());
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions options;options.sessionsDirectory=root.filePath("state");options.planToolsEnabled=true;
        options.skills.enabled=false;options.projectContext.enabled=false;options.compaction.automatic=false;
        a::CommandHookOptions limits;limits.workingDirectory=root.path();
        options.hooks={a::CommandHooks({{"hooks",QJsonObject{{"Stop",QJsonArray{QJsonObject{{"hooks",QJsonArray{
            QJsonObject{{"type","agent"},{"prompt","Verify the result."}}}}}}}}}},limits).callback()};
        QString foreign,owned;int checks=0;bool denied=false,leaked=false,ownedRead=false,hiddenSearch=false;
        model->generateReply=[&](const a::ModelRequest& request)->a::ModelReply {
            if(!request.verificationAgent)return {"DONE"};
            if(++checks==1)return {{},{{"read","Read",{{"path",foreign}}},{"owned","Read",{{"path",owned}}},
                {"state","Read",{{"path",QFileInfo(owned).dir().filePath("state.json")}}},
                {"glob","Glob",{{"pattern","**/plan.md"}}},{"grep","Grep",{{"path",QFileInfo(foreign).dir().path()},{"pattern","FOREIGN_PLAN_SECRET"}}}}};
            const auto n=request.messages.size();denied=request.messages[n-5].isError&&request.messages[n-3].isError;
            leaked=request.messages[n-5].text.contains("FOREIGN_PLAN_SECRET");
            ownedRead=!request.messages[n-4].isError&&request.messages[n-4].text.contains("OWNER_PLAN");
            hiddenSearch=!request.messages[n-2].isError&&!request.messages[n-2].text.contains("plan.md")
                &&request.messages[n-1].isError;
            return {{},{{"result","StructuredOutput",{{"ok",true}}}}};
        };
        a::Engine engine(model,registry,policy,options);const auto owner=engine.createSession("fixture",root.path()).id;
        const auto other=engine.createSession("fixture",root.path()).id;
        QVERIFY(!engine.runPlanTool(owner,"EnterPlanMode").isError);QVERIFY(!engine.runPlanTool(other,"EnterPlanMode").isError);
        foreign=engine.planStatus(other)["plan_file_path"].toString();write(foreign,"FOREIGN_PLAN_SECRET");
        owned=engine.planStatus(owner)["plan_file_path"].toString();write(owned,"OWNER_PLAN");
        QCOMPARE(engine.run({owner,"Finish the task"}).result.get().status,a::RunStatus::Completed);
        QCOMPARE(checks,2);QVERIFY2(denied&&!leaked,"Verifier read another session's private plan");QVERIFY(ownedRead);QVERIFY(hiddenSearch);
    }
    void subagentKeepsPlanningRestrictionsAndCannotReadForeignPlan_data() {
        QTest::addColumn<bool>("configuredPlanning");
        QTest::newRow("engine-options")<<true;QTest::newRow("scoped-context-only")<<false;
    }
    void subagentKeepsPlanningRestrictionsAndCannotReadForeignPlan() {
        QFETCH(bool,configuredPlanning);
        QTemporaryDir root;auto registry=std::make_shared<a::ToolRegistry>();auto model=std::make_shared<Model>();
        const auto work=root.filePath("work");QVERIFY(QDir().mkpath(work));
        a::registerWorkspaceTools(*registry,work);auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions options;options.sessionsDirectory=work+"/state";options.planToolsEnabled=true;
        options.permission=[](const auto&,const auto&,const auto&){return true;};
        options.skills.enabled=false;options.projectContext.enabled=false;options.compaction.automatic=false;
        a::Engine engine(model,registry,policy,options);const auto parent=engine.createSession("fixture",work);
        const auto other=engine.createSession("fixture",work).id;
        QVERIFY(!engine.runPlanTool(parent.id,"EnterPlanMode").isError);QVERIFY(!engine.runPlanTool(other,"EnterPlanMode").isError);
        const auto foreign=engine.planStatus(other)["plan_file_path"].toString();write(foreign,"FOREIGN_PLAN_SECRET");
        bool readDenied=false,writeDenied=false,hidden=true,hiddenSearch=false;int attempts=0;
        model->generateReply=[&](const a::ModelRequest& request)->a::ModelReply {
            for(const auto& t:request.tools)hidden&=t.name!="EnterPlanMode"&&t.name!="ExitPlanMode";
            if(request.messages.last().role!=a::MessageRole::Tool) {
                const auto prefix=QString::number(++attempts)+"-";
                return {{},{{prefix+"read","Read",{{"path",foreign}}},
                    {prefix+"write","Write",{{"path","forbidden.txt"},{"content","NO"}}},{prefix+"glob","Glob",{{"pattern","**/plan.md"}}},
                    {prefix+"grep","Grep",{{"path",QFileInfo(foreign).dir().path()},{"pattern","FOREIGN_PLAN_SECRET"}}}}};
            }
            const auto n=request.messages.size();readDenied=request.messages[n-4].isError;writeDenied=request.messages[n-3].isError;
            hiddenSearch=!request.messages[n-2].isError&&!request.messages[n-2].text.contains("plan.md")
                &&request.messages[n-1].isError;
            return {"DONE"};
        };
        a::SubagentOptions child;child.workingDirectory=work;child.stateDirectory=root.filePath("children");
        auto childHost=options;childHost.planToolsEnabled=configuredPlanning;
        a::Subagents agents(model,registry,policy,childHost,child);a::ToolContext context{parent.id,{},work};
        context.sessionSnapshot=std::make_shared<a::Session>(parent);context=engine.planning()->scope(std::move(context),*policy);
        const auto result=agents.run(context,{{"prompt","Inspect and try a write"}});
        QVERIFY2(!result.isError,qPrintable(result.text));QVERIFY(hidden&&writeDenied);QVERIFY(!QFileInfo::exists(work+"/forbidden.txt"));
        QVERIFY2(readDenied,"Subagent read another session's private plan");QVERIFY(hiddenSearch);
        QVERIFY(!engine.runPlanTool(parent.id,"ExitPlanMode").isError);
        context={parent.id,{},work};context.sessionSnapshot=std::make_shared<a::Session>(parent);
        context=engine.planning()->scope(std::move(context),*policy);QVERIFY(!context.planModeActive);
        readDenied=false;writeDenied=false;hiddenSearch=false;
        const auto resumed=agents.run(context,{{"prompt","Inspect again"},{"resume",result.data["agentId"]}});
        QVERIFY2(!resumed.isError,QJsonDocument(resumed.data).toJson().constData());QVERIFY(readDenied&&writeDenied&&hiddenSearch);
    }
    void mcpEngineWrapperCanEnterPlanningAndKeepTaskControlsUsable() {
        QTemporaryDir root;auto registry=std::make_shared<a::ToolRegistry>();auto model=std::make_shared<Model>();
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions options;options.sessionsDirectory=root.filePath("sessions");options.planToolsEnabled=true;options.planToolsDeferred=false;
        options.taskToolsEnabled=true;options.projectContext.enabled=false;options.compaction.automatic=false;
        auto engine=std::make_shared<a::Engine>(model,registry,policy,options);
        a::McpServerOptions config;config.engine=engine;config.model="fixture";config.workingDirectory=root.path();
        auto server=a::mcpServerOptions(registry,policy,config);mcp::ServerRequestContext request;request.sessionId="wire";
        auto call=[&](const QString& name,const QJsonObject& args=QJsonObject{}){return server.handlers.at("tools/call")({{"name",name},{"arguments",args}},request);};
        const auto run=call("iiLocalLLM.agent.run",{{"prompt","Plan the change"}});QVERIFY2(!run["isError"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(run).toJson())));
        const auto status=server.controlHandlers.at("iisacc/plan/status")({},request);QCOMPARE(status["phase"],"planning");
        QVERIFY(!call("TaskCreate",{{"subject","Plan task"},{"description","Check a scoped task"}})["isError"].toBool());
        const auto continued=call("iiLocalLLM.agent.run",{{"prompt","Continue planning"}});QVERIFY2(!continued["isError"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(continued).toJson())));
        QVERIFY(!call("iiLocalLLM.agent.clear")["isError"].toBool());QCOMPARE(server.controlHandlers.at("iisacc/plan/status")({},request)["phase"],"inactive");
        server.onClosed("wire");
    }
    void transitionInvalidatesAnAlreadyPreparedProjectWrite() {
        Fixture f;f.policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        const auto result=f.run("Write",{{"path","project.txt"},{"content","should not commit"}},"owner",[&](const a::Event& event) {
            if(event.kind==a::EventKind::ToolStarted) {
                const auto entered=f.run("EnterPlanMode");if(entered.isError)throw std::runtime_error(entered.text.toStdString());
            }
        });
        QVERIFY(result.isError);QVERIFY(result.text.contains("Planning state changed"));QVERIFY(!QFileInfo::exists(f.workspace+"/project.txt"));
    }
    void permissiveCustomPolicyCannotBypassPlanning() {
        class Allow final:public a::PermissionPolicy {public:a::PermissionDecision decide(const a::ToolDefinition&,const QJsonObject&,const a::ToolContext&)const override{return {a::PermissionBehavior::Allow,{}};}};
        Fixture f;f.policy=std::make_shared<Allow>();f.draft();QVERIFY(f.run("Write",{{"path","bad.txt"},{"content","blocked"}}).isError);
        int reviews=0;f.options.permission=[&](const auto&,const auto&,const auto&){++reviews;return true;};
        QVERIFY(!f.run("ExitPlanMode").isError);QCOMPARE(reviews,1);
    }
    void deferredPlanningToolsCanBeDiscovered() {
        QTemporaryDir root;auto model=std::make_shared<Model>();model->generateReply=[raw=model.get()](const a::ModelRequest&) -> a::ModelReply {
            if(raw->turn==1)return {{},{{"search","ToolSearch",{{"query","select:EnterPlanMode"}}}}};
            if(raw->turn==2)return {{},{{"enter","EnterPlanMode",{}}}};return {"DONE",{}};
        };
        a::EngineOptions options;options.sessionsDirectory=root.filePath("sessions");options.planToolsEnabled=true;options.compaction.automatic=false;options.projectContext.enabled=false;
        a::Engine engine(model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(),options);
        const auto id=engine.createSession("fixture",root.path()).id;const auto result=engine.run({id,"Plan"}).result.get();
        QCOMPARE(result.status,a::RunStatus::Completed);QCOMPARE(engine.planStatus(id)["phase"],"planning");
        QVERIFY(std::any_of(model->requests[1].tools.begin(),model->requests[1].tools.end(),[](const auto& t){return t.name=="EnterPlanMode";}));
        model->generateReply={};
    }
    void modelCanEnterPlanningAndReceivesCurrentState() {
        QTemporaryDir root;a::EngineOptions options;options.sessionsDirectory=root.filePath("sessions");
        options.compaction.automatic=false;options.projectContext.enabled=false;options.planToolsEnabled=true;options.planToolsDeferred=false;
        auto model=std::make_shared<Model>();a::Engine engine(model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(),options);
        const auto id=engine.createSession("fixture",root.path()).id;
        QCOMPARE(engine.run({id,"Prepare an implementation plan"}).result.get().status,a::RunStatus::Completed);
        const auto session=engine.session(id);
        const auto result=std::find_if(session.messages.begin(),session.messages.end(),[](const auto& m){return m.toolCallId=="enter";});
        QVERIFY(result!=session.messages.end());QVERIFY2(!result->isError,qPrintable(result->text));
        QCOMPARE(engine.planStatus(id)["phase"],"planning");QCOMPARE(engine.permissions(id)["mode"],"plan");
        QVERIFY(std::any_of(model->requests.last().messages.begin(),model->requests.last().messages.end(),[](const auto& m){return m.metadata["iilocal.plan_state"].toBool();}));
    }
    void ownedPlanIsTheOnlyWritableFileAndApprovalRestoresPolicy() {
        Fixture f;f.policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);f.draft();
        QVERIFY(f.run("Write",{{"path","project.txt"},{"content","bad"}}).isError);
        QVERIFY(f.run("Bash",{{"command","printf forbidden > project.txt"}}).isError);QVERIFY(!QFileInfo::exists(f.workspace+"/project.txt"));
        QVERIFY(!f.run("Read",{{"path",f.plan()}}).isError);
        QVERIFY(!f.run("Edit",{{"path",f.plan()},{"old_string","parser"},{"new_string","tokenizer"}}).isError);
        int reviews=0;f.options.permission=[&](const auto&,const auto&,const auto&){++reviews;return true;};
        const auto exit=f.run("ExitPlanMode");QVERIFY2(!exit.isError,qPrintable(exit.text));QCOMPARE(reviews,1);
        QCOMPARE(f.plans->status("owner")["phase"],"approved");QVERIFY(f.plans->status("owner")["approval_current"].toBool());
        QVERIFY(!f.run("Write",{{"path","project.txt"},{"content","implemented"}}).isError);
        QVERIFY(f.run("Write",{{"path",f.plan()},{"content","unreviewed"}}).isError);
    }
    void deniedOrForgedExitLeavesPlanningActive() {
        Fixture f;f.draft();f.options.permission=[](const auto&,const auto&,const auto&){return false;};
        QVERIFY(f.run("ExitPlanMode").isError);QCOMPARE(f.plans->status("owner")["phase"],"planning");
        QVERIFY(f.run("ExitPlanMode",{{"plan","model-forged approval"}}).isError);
        QVERIFY(f.run("ExitPlanMode",{{"session_id","foreign"}}).isError);
        QVERIFY(f.run("ExitPlanMode",{},"foreign").isError);
    }
    void hostMayEditExactlyTheReviewedPlan() {
        Fixture f;f.draft();QJsonObject preview;
        f.options.permissionResponse=[](const auto&,const auto&,const auto&) {
            return a::PermissionResponse{a::PermissionBehavior::Allow,{},QJsonObject{{"plan","Host edited implementation plan."}}};
        };
        const auto result=f.run("ExitPlanMode",{},"owner",[&](const a::Event& e) {
            if(e.kind==a::EventKind::PermissionRequested)preview=e.data["permission_preview"].toObject()["_meta"].toObject()["plan"].toObject();
        });
        QVERIFY2(!result.isError,qPrintable(result.text));QCOMPARE(read(f.plan()),QByteArray("Host edited implementation plan."));
        QVERIFY(result.data["plan_was_edited"].toBool());QVERIFY(!preview["plan_sha256"].toString().isEmpty());
        QVERIFY(preview["plan"].toString().contains("parser"));QCOMPARE(result.data["plan"],"Host edited implementation plan.");
    }
    void changedPlanCannotReceiveAnOlderApproval_data() {
        QTest::addColumn<bool>("edit");QTest::newRow("allow")<<false;QTest::newRow("host-edit")<<true;
    }
    void changedPlanCannotReceiveAnOlderApproval() {
        QFETCH(bool,edit);Fixture f;f.draft();
        f.options.permissionResponse=[&](const auto&,const auto&,const auto&) {
            write(f.plan(),"Changed during the host review");a::PermissionResponse result{a::PermissionBehavior::Allow};
            if(edit)result.updatedArguments=QJsonObject{{"plan","Host edited an older version"}};return result;
        };
        const auto result=f.run("ExitPlanMode");QVERIFY(result.isError);QVERIFY(result.text.contains("changed during review"));
        QCOMPARE(f.plans->status("owner")["phase"],"planning");QCOMPARE(read(f.plan()),QByteArray("Changed during the host review"));
    }
    void changesAfterPermissionBeforeExecutionAreDetected() {
        Fixture f;f.draft();const auto result=f.run("ExitPlanMode",{},"owner",[&](const a::Event& e) {
            if(e.kind==a::EventKind::ToolStarted)write(f.plan(),"changed at the execution boundary");
        });
        QVERIFY(result.isError);QCOMPARE(f.plans->status("owner")["phase"],"planning");
    }
    void explicitDenyAndAskStillApplyToPlanWrites() {
        Fixture f;QVERIFY(!f.run("EnterPlanMode").isError);
        for(auto behavior:{a::PermissionBehavior::Deny,a::PermissionBehavior::Ask}) {
            f.policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"Write",behavior}});
            QVERIFY(f.run("Write",{{"path",f.plan()},{"content","blocked"}}).isError);QVERIFY(!QFileInfo::exists(f.plan()));
        }
    }
    void actionDescriptionsNeverGrantShellPermission() {
        Fixture f;f.draft();const auto result=f.run("ExitPlanMode",{{"allowedPrompts",QJsonArray{QJsonObject{{"tool","Bash"},{"prompt","run any command"}}}}});
        QVERIFY(!result.isError);QVERIFY(f.run("Bash",{{"command","printf unauthorized > marker"}}).isError);
        QVERIFY(!QFileInfo::exists(f.workspace+"/marker"));
    }
    void corruptStateAndSymlinkTargetsFailClosed() {
        Fixture f;f.draft();const auto state=QFileInfo(f.plan()).dir().filePath("state.json");write(state,"{broken");
        QVERIFY_THROWS_EXCEPTION(Error,f.plans->status("owner"));
        QVERIFY(f.run("Write",{{"path","project.txt"},{"content","blocked"}}).isError);
        Fixture other;other.draft();const auto target=other.workspace+"/private.txt";write(target,"SECRET");const auto planPath=other.plan();QVERIFY(QFile::remove(planPath));
        QVERIFY(QFile::link(target,planPath));QVERIFY_THROWS_EXCEPTION(Error,other.plans->status("owner"));
        QVERIFY(other.run("Read",{{"path",planPath}}).isError);QCOMPARE(read(target),QByteArray("SECRET"));
    }
    void planStateAndForeignFilesAreNotSearchable() {
        Fixture f(true);f.draft("PLAN_PRIVATE_SENTINEL");QVERIFY(!f.run("EnterPlanMode",{},"foreign").isError);
        QVERIFY(f.run("Read",{{"path",f.plan("owner")}},"foreign").isError);
        QVERIFY(f.run("Read",{{"path",QFileInfo(f.plan()).dir().filePath("state.json")}}).isError);
        const auto glob=f.run("Glob",{{"pattern","**/*"}});QVERIFY(!glob.isError);QVERIFY(!glob.text.contains("plan.md"));
        const auto grep=f.run("Grep",{{"pattern","PLAN_PRIVATE_SENTINEL"}});QVERIFY2(!grep.isError,qPrintable(grep.text));QVERIFY(!grep.text.contains("PLAN_PRIVATE_SENTINEL"));
        QVERIFY(!f.run("Read",{{"path",f.plan()}}).isError);
    }
    void restartAndForkPreserveContentWithoutTransferringApproval() {
        Fixture f;f.draft();QVERIFY(!f.run("ExitPlanMode").isError);
        a::PlanMode restarted(QFileInfo(f.plan()).dir().absolutePath()+"/..");QCOMPARE(restarted.status("owner")["phase"],"approved");
        restarted.fork("owner","fork");const auto fork=restarted.status("fork");
        QCOMPARE(fork["phase"],"planning");QCOMPARE(fork["plan"],f.plans->status("owner")["plan"]);QVERIFY(fork["plan_file_path"]!=f.plan());
        write(fork["plan_file_path"].toString(),"fork revision");QVERIFY(read(f.plan())!="fork revision");
    }
    void unreviewedExternalChangeReentersRestrictedState() {
        Fixture f;f.policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);f.draft();QVERIFY(!f.run("ExitPlanMode").isError);
        write(f.plan(),"unreviewed external revision");QVERIFY(f.run("Write",{{"path","project.txt"},{"content","blocked"}}).isError);
        QVERIFY(!f.run("EnterPlanMode").isError);QVERIFY(!f.run("ExitPlanMode").isError);
        QCOMPARE(f.plans->status("owner")["revision"].toInt(),2);
        QVERIFY(!f.run("Write",{{"path","project.txt"},{"content","reviewed"}}).isError);
    }
    void planByteAndTextLimitsAreEnforced() {
        Fixture f;QVERIFY(!f.run("EnterPlanMode").isError);
        QVERIFY(f.run("Write",{{"path",f.plan()},{"content",QString(40000,QChar(0xAC00))}}).isError);
        QVERIFY(f.run("Write",{{"path",f.plan()},{"content",QString("a")+QChar::Null+"b"}}).isError);
        QVERIFY(!QFileInfo::exists(f.plan()));
        write(f.plan(),QByteArray(65537,'x'));QVERIFY_THROWS_EXCEPTION(Error,f.plans->status("owner"));
    }
    void emptyPlanCanBeReviewedButCannotExitAnInactiveSession() {
        Fixture f;QVERIFY(f.run("ExitPlanMode").isError);QVERIFY(!f.run("EnterPlanMode").isError);
        const auto result=f.run("ExitPlanMode");QVERIFY(!result.isError);QVERIFY(result.data["plan"].isNull());
        QVERIFY(!result.data["file_exists"].toBool());QVERIFY(result.data["approval_current"].toBool());
    }
    void configuredPlanModeReturnsToDefaultAfterReview() {
        Fixture f;f.policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Plan);
        f.registry->remove("EnterPlanMode");f.registry->remove("ExitPlanMode");for(auto tool:f.plans->tools(f.policy,false))f.registry->add(std::move(tool));
        f.draft();QVERIFY(!f.run("ExitPlanMode").isError);
        const auto scoped=f.plans->scope(f.context(),*f.policy);QVERIFY(!scoped.planModeActive);QCOMPARE(scoped.permissionMode,a::PermissionMode::Default);
    }
    void brokerCancellationKeepsPlanAndOneConcurrentApprovalWins() {
        Fixture f;f.draft();f.options.permission={};auto broker=std::make_shared<a::PermissionRequests>();f.options.permissionRequests=broker;
        auto context=f.context();auto future=std::async(std::launch::async,[&]{return a::ToolRunner(f.registry,f.policy,f.options).run({"exit","ExitPlanMode",{}},context);});
        QTRY_VERIFY_WITH_TIMEOUT(!broker->pending()["requests"].toArray().isEmpty(),3000);context.cancellation.cancel();
        QVERIFY_THROWS_EXCEPTION(Error,(void)future.get());QCOMPARE(f.plans->status("owner")["phase"],"planning");
        broker->close();
        f.options.permissionRequests={};std::atomic<int> waiting=0;std::atomic_bool release=false;
        f.options.permission=[&](const auto&,const auto&,const auto& c){++waiting;while(!release){c.cancellation.throwIfCancelled();std::this_thread::sleep_for(1ms);}return true;};
        auto first=std::async(std::launch::async,[&]{return f.run("ExitPlanMode");});
        auto second=std::async(std::launch::async,[&]{return f.run("ExitPlanMode");});
        QTRY_COMPARE_WITH_TIMEOUT(waiting.load(),2,3000);release=true;const auto a=first.get(),b=second.get();
        QVERIFY(a.isError!=b.isError);QCOMPARE(f.plans->status("owner")["phase"],"approved");
    }
    void engineEndRetainsPlanClearStartsFreshAndDisabledHostsReject() {
        QTemporaryDir root;auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>();
        a::EngineOptions config;config.sessionsDirectory=root.filePath("sessions");config.planToolsEnabled=true;
        a::Engine engine(model,registry,policy,config);const auto id=engine.createSession("fixture",root.path()).id;
        QVERIFY(!engine.runPlanTool(id,"EnterPlanMode").isError);(void)engine.endSession(id);QCOMPARE(engine.planStatus(id)["phase"],"planning");
        const auto fork=engine.forkSession(id).id;QCOMPARE(engine.planStatus(fork)["phase"],"planning");
        const auto clear=engine.clearSession(id);QVERIFY(clear["complete"].toBool());QCOMPARE(engine.planStatus(clear["session_id"].toString())["phase"],"inactive");
        config.planToolsEnabled=false;config.sessionsDirectory=root.filePath("disabled");a::Engine disabled(model,registry,policy,config);
        const auto off=disabled.createSession("fixture",root.path()).id;QVERIFY_THROWS_EXCEPTION(Error,disabled.runPlanTool(off,"EnterPlanMode"));
    }
};
QTEST_GUILESS_MAIN(PlanModeTests)
#include "plan_mode_tests.moc"
