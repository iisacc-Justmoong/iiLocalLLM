#include <agent/Teams.h>
#include <agent/Subagents.h>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QJsonDocument>
#include <QtTest/QTest>
#include <thread>
#include <atomic>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
class FunctionModel final:public a::Model {
public:
    std::function<a::ModelReply(const a::ModelRequest&,const CancellationToken&)> reply;
    a::ModelReply generate(const a::ModelRequest& r,const CancellationToken& t,const TextCallback&)override{return reply(r,t);}
};
class ReplyModel final : public a::Model {
public:
    a::ModelReply generate(const a::ModelRequest& r,const CancellationToken&,const TextCallback&) override {
        if(r.messages.last().role==a::MessageRole::Tool)return {"done",{}};
        const auto prompt=r.messages.last().text;
        if(prompt.contains("shutdown_request")) {
            // Shutdown tests use trusted host stop; protocol decisions are tested separately.
            return {"request observed",{}};
        }
        return {{},{{"message-"+QString::number(r.messages.size()),"SendMessage",{
            {"to","team-lead"},{"summary","Member completed the requested observation"},{"message","MEMBER_REPLY"}}}}};
    }
};
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("team-test-XXXXXX")};
    QString work=root.filePath("work"),state=root.filePath("sessions");
    std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::RulePolicy> policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    a::EngineOptions options;
    std::shared_ptr<a::Teams> teams;
    std::unique_ptr<a::Engine> engine;
    Fixture(std::shared_ptr<a::Model> model=std::make_shared<ReplyModel>(),std::function<void(a::TeamsOptions&)> configure={},std::function<void(a::EngineOptions&)> configureEngine={}){
        if(!QDir().mkpath(work))throw std::runtime_error("fixture workspace");
        a::registerWorkspaceTools(*registry,work);options.sessionsDirectory=state;
        options.skills.enabled=false;options.projectContext.enabled=false;options.toolSearch.enabled=false;
        options.compaction.automatic=false;options.taskToolsEnabled=true;
        if(configureEngine)configureEngine(options);
        a::TeamsOptions config;config.workingDirectory=work;
        if(configure)configure(config);
        teams=std::make_shared<a::Teams>(model,registry,policy,options,config);
        a::Teams::attach(options,teams);engine=std::make_unique<a::Engine>(model,registry,policy,options);
    }
    ~Fixture(){engine.reset();if(teams)teams->close();}
    a::ToolContext owner(){const auto session=engine->createSession("fixture",work);return {session.id,{},work};}
};
}
class TeamTests:public QObject {
    Q_OBJECT
private slots:
    void teamAttachmentPreservesDisabledTaskToolsForEveryMember(){
        auto model=std::make_shared<FunctionModel>();bool exposed=false;
        model->reply=[&](const a::ModelRequest& r,const auto&)->a::ModelReply{for(const auto& tool:r.tools)exposed|=tool.name.startsWith("Task");return {"done",{}};};
        Fixture f(model,{},[](auto& options){options.taskToolsEnabled=false;});const auto leader=f.owner();QVERIFY(!f.engine->taskToolsEnabled());
        f.teams->create(leader,{{"team_name","no-tasks"}});f.teams->spawn(leader,{{"name","worker"},{"prompt","Inspect available tools"}});
        QVERIFY(f.teams->wait(leader.sessionId,10000)["idle"].toBool());QVERIFY(!exposed);
        QVERIFY_THROWS_EXCEPTION(Error,f.engine->runTaskTool(leader.sessionId,"TaskList"));
    }
    void failedMemberPublicationLeavesNoUnownedConversation(){
        Fixture f;const auto leader=f.owner();const auto created=f.teams->create(leader,{{"team_name","publication"}});
        const auto path=created.data["team_file_path"].toString(),backup=path+".saved";
        QVERIFY(QFile::rename(path,backup));QVERIFY(QFile::link(backup,path));
        QVERIFY_THROWS_EXCEPTION(Error,f.teams->spawn(leader,{{"name","unpublished"},{"prompt","Must not execute"}}));
        QVERIFY(a::SessionStore(f.state+"/teams/sessions").list().isEmpty());
        QCOMPARE(f.teams->status(leader.sessionId)["team"].toObject()["members"].toArray().size(),1);
        QVERIFY(QFile::remove(path));QVERIFY(QFile::rename(backup,path));QVERIFY(!f.teams->remove(leader).isError);
    }
    void shutdownResponseCannotBeReplayedOrBorrowAnotherMembersRequest(){
        Fixture f;const auto leader=f.owner();f.teams->create(leader,{{"team_name","protocol"}});
        const auto first=f.teams->spawn(leader,{{"name","first"},{"prompt","Be ready"}}).data["session_id"].toString();
        const auto second=f.teams->spawn(leader,{{"name","second"},{"prompt","Be ready"}}).data["session_id"].toString();QVERIFY(f.teams->wait(leader.sessionId,10000)["idle"].toBool());
        const auto sent=f.teams->send(leader,{{"to","first"},{"message",QJsonObject{{"type","shutdown_request"}}}});
        const auto request=sent.data["messages"].toArray().first().toObject()["message"].toObject()["request_id"].toString();QVERIFY(!request.isEmpty());
        QVERIFY(f.teams->wait(leader.sessionId,10000)["idle"].toBool());
        QJsonObject reply{{"to","team-lead"},{"message",QJsonObject{{"type","shutdown_response"},{"request_id",request},{"approve",false}}}};
        QVERIFY_THROWS_EXCEPTION(Error,f.teams->send({second,{},f.work},reply));
        QVERIFY_THROWS_EXCEPTION(Error,f.teams->send({first,{},f.work},reply)); // Refusal needs a reason.
        auto protocol=reply["message"].toObject();protocol["reason"]="Still working";reply["message"]=protocol;
        QVERIFY(!f.teams->send({first,{},f.work},reply).isError);
        QVERIFY_THROWS_EXCEPTION(Error,f.teams->send({first,{},f.work},reply));
        QVERIFY(!f.teams->send(leader,{{"to","first"},{"summary","Continue after refusal"},{"message","Reply again"}}).isError);
        QVERIFY(f.teams->wait(leader.sessionId,10000)["idle"].toBool());
    }
    void boundedMailboxPreservesInputsTheLeaderHasNotConsumed(){
        Fixture f(std::make_shared<ReplyModel>(),[](auto& options){options.maxMailboxMessages=2;});const auto leader=f.owner();f.teams->create(leader,{{"team_name","bounded"}});
        f.teams->spawn(leader,{{"name","worker"},{"prompt","Reply"}});QVERIFY(f.teams->wait(leader.sessionId,10000)["idle"].toBool());
        const auto before=f.engine->queuedInputs(leader.sessionId)["inputs"].toArray();QCOMPARE(before.size(),2);
        f.teams->send(leader,{{"to","worker"},{"summary","Another run"},{"message","Reply again"}});QVERIFY(f.teams->wait(leader.sessionId,10000)["idle"].toBool());
        QCOMPARE(f.engine->queuedInputs(leader.sessionId)["inputs"].toArray(),before);QCOMPARE(f.teams->inbox(leader.sessionId)["total"].toInt(),2);
        QVERIFY(!f.teams->remove(leader).isError);QCOMPARE(f.engine->queuedInputs(leader.sessionId)["count"].toInt(),0);
    }
    void activeMembersPreventDeletionAndStopCooperatesWithCancellation(){
        auto model=std::make_shared<FunctionModel>();std::atomic_bool entered=false,cancelled=false;
        model->reply=[&](const auto&,const CancellationToken& token)->a::ModelReply{entered=true;while(!token.isCancelled())std::this_thread::sleep_for(std::chrono::milliseconds(1));cancelled=true;token.throwIfCancelled();return {};};
        Fixture f(model);const auto leader=f.owner();f.teams->create(leader,{{"team_name","busy"}});
        QVERIFY(!f.engine->runTeamTool(leader.sessionId,"Agent",{{"name","worker"},{"prompt","wait"}}).isError);QTRY_VERIFY_WITH_TIMEOUT(entered.load(),5000);
        QVERIFY_THROWS_EXCEPTION(Error,f.teams->remove(leader));QVERIFY(!f.teams->wait(leader.sessionId,0)["idle"].toBool());
        CancellationToken stop;stop.cancel();QVERIFY_THROWS_EXCEPTION(Error,f.teams->wait(leader.sessionId,1000,stop));
        const auto ended=f.engine->endSession(leader.sessionId);
        // The parent has not run a model turn: no SessionStart activation ends,
        // but owned teammates must still be cancelled and joined.
        QVERIFY(!ended["ended"].toBool());QVERIFY2(ended["diagnostics"].toArray().isEmpty(),QJsonDocument(ended).toJson().constData());QVERIFY(cancelled.load());
        QVERIFY(f.teams->wait(leader.sessionId,100)["idle"].toBool());QVERIFY(!f.teams->remove(leader).isError);
    }
    void nativeMemberTasksShareTheLeaderBoard(){
        auto model=std::make_shared<FunctionModel>();std::atomic_int calls=0;
        model->reply=[&](const auto&,const auto&)->a::ModelReply{
            switch(calls++){
            case 0:return {{},{{"create","TaskCreate",{{"subject","MEMBER_TASK"},{"description","Visible to the leader"}}}}};
            case 1:return {{},{{"notify","SendMessage",{{"to","team-lead"},{"summary","Shared task created"},{"message","TASK_READY"}}}}};
            default:return {"done",{}};}
        };
        Fixture f(model);const auto leader=f.owner();f.teams->create(leader,{{"team_name","board"}});
        f.teams->spawn(leader,{{"name","builder"},{"prompt","Create a task"}});QVERIFY(f.teams->wait(leader.sessionId,10000)["idle"].toBool());
        const auto tasks=f.engine->runTaskTool(leader.sessionId,"TaskList").data["tasks"].toArray();QCOMPARE(tasks.size(),1);QCOMPARE(tasks.first().toObject()["subject"],"MEMBER_TASK");
    }
    void readOnlyMemberCanSendTextButCannotWriteOrEscalate(){
        auto model=std::make_shared<FunctionModel>();std::atomic_int calls=0;bool denied=false;
        model->reply=[&](const a::ModelRequest& r,const auto&)->a::ModelReply{
            switch(calls++){
            case 0:return {{},{{"write","Write",{{"file_path","forbidden.txt"},{"content","bad"}}}}};
            case 1:denied=r.messages.last().isError;return {{},{{"tell","SendMessage",{{"to","team-lead"},{"summary","Read-only scope enforced"},{"message","SCOPED_REPLY"}}}}};
            default:return {"done",{}};}
        };
        Fixture f(model,[](auto& options){a::SubagentDefinition p;p.name="reader";p.readOnly=true;options.definitions={p};});const auto leader=f.owner();
        f.teams->create(leader,{{"team_name","permissions"}});f.teams->spawn(leader,{{"name","reader"},{"subagent_type","reader"},{"prompt","Check scope"}});
        QVERIFY(f.teams->wait(leader.sessionId,10000)["idle"].toBool());QVERIFY(denied);QVERIFY(!QFileInfo::exists(f.work+"/forbidden.txt"));
        bool reply=false;for(const auto& v:f.teams->inbox(leader.sessionId)["messages"].toArray())reply|=v.toObject()["message"]=="SCOPED_REPLY";QVERIFY(reply);
    }
    void modelShutdownDecisionPersistsBeforeTheMemberStops(){
        auto model=std::make_shared<FunctionModel>();std::atomic_int decisions=0,afterDecision=0;
        model->reply=[&](const a::ModelRequest& r,const auto&)->a::ModelReply{
            if(decisions.load()){++afterDecision;return {"must not run after shutdown",{}};}
            const auto text=r.messages.last().text;const auto envelope=QJsonDocument::fromJson(text.mid(text.indexOf('{')).toUtf8()).object();
            const auto protocol=envelope["message"].toObject();if(protocol["type"]=="shutdown_request"){
                ++decisions;return {{},{{"shutdown","SendMessage",{{"to","team-lead"},{"message",QJsonObject{{"type","shutdown_response"},{"request_id",protocol["request_id"]},{"approve",true}}}}}}};
            }return {"idle",{}};
        };
        Fixture f(model);const auto leader=f.owner();f.teams->create(leader,{{"team_name","shutdown"}});
        const auto member=f.teams->spawn(leader,{{"name","worker"},{"prompt","Be ready"}}).data["session_id"].toString();QVERIFY(f.teams->wait(leader.sessionId,10000)["idle"].toBool());
        f.teams->send(leader,{{"to","worker"},{"message",QJsonObject{{"type","shutdown_request"},{"reason","Work completed"}}}});
        QVERIFY(f.teams->wait(leader.sessionId,10000)["idle"].toBool());QCOMPARE(decisions.load(),1);QCOMPARE(afterDecision.load(),0);
        const auto history=a::SessionStore(f.state+"/teams/sessions").load(member);bool paired=false;
        for(const auto& m:history.messages)if(m.role==a::MessageRole::Tool&&m.toolCallId=="shutdown"){paired=true;QVERIFY(!m.isError);QVERIFY(m.data["success"].toBool());}QVERIFY(paired);
        QVERIFY_THROWS_EXCEPTION(Error,f.teams->send(leader,{{"to","worker"},{"summary","late"},{"message","must remain stopped"}}));
    }
    void clearTransfersTheTeamBoardAndPendingLeaderNotifications(){
        Fixture f;auto leader=f.owner();f.teams->create(leader,{{"team_name","transfer"}});
        const auto board=f.teams->taskList(leader.sessionId);f.teams->spawn(leader,{{"name","worker"},{"prompt","reply"}});QVERIFY(f.teams->wait(leader.sessionId,10000)["idle"].toBool());
        const auto before=f.engine->queuedInputs(leader.sessionId)["count"].toInt();QVERIFY(before>0);
        const auto cleared=f.engine->clearSession(leader.sessionId);QVERIFY2(cleared["complete"].toBool(),QJsonDocument(cleared).toJson().constData());
        const auto next=cleared["session_id"].toString();QVERIFY2(!next.isEmpty(),QJsonDocument(cleared).toJson().constData());
        QCOMPARE(f.teams->taskList(next),board);QVERIFY(f.teams->status(leader.sessionId)["team"].isNull());
        QCOMPARE(f.engine->queuedInputs(leader.sessionId)["count"].toInt(),0);QCOMPARE(f.engine->queuedInputs(next)["count"].toInt(),before);
        leader.sessionId=next;QVERIFY(!f.teams->send(leader,{{"to","worker"},{"summary","Continue after clear"},{"message","Reply to the new leader"}}).isError);
        QVERIFY(f.teams->wait(next,10000)["idle"].toBool());QVERIFY(f.engine->queuedInputs(next)["count"].toInt()>before);
        QVERIFY(!f.teams->remove(leader).isError);QCOMPARE(f.engine->queuedInputs(next)["count"].toInt(),0);
    }
    void restartRetainsIdentityButDoesNotRepeatMemberWork(){
        auto model=std::make_shared<FunctionModel>();std::atomic_int calls=0;model->reply=[&](const auto&,const auto&)->a::ModelReply{++calls;return {"finished",{}};};
        Fixture f(model);const auto leader=f.owner();f.teams->create(leader,{{"team_name","persistent"}});f.teams->spawn(leader,{{"name","worker"},{"prompt","Once"}});
        QVERIFY(f.teams->wait(leader.sessionId,10000)["idle"].toBool());const auto count=calls.load();const auto board=f.teams->taskList(leader.sessionId);
        f.engine.reset();f.teams->close();f.options.additionalTools.clear();f.options.additionalToolsProvider={};f.options.taskListId={};f.options.hooks.clear();f.teams.reset();
        a::TeamsOptions options;options.workingDirectory=f.work;a::Teams reopened(model,f.registry,f.policy,f.options,options);
        QCOMPARE(reopened.taskList(leader.sessionId),board);QCOMPARE(calls.load(),count);QCOMPARE(reopened.status(leader.sessionId)["team"].toObject()["members"].toArray().size(),2);
        QVERIFY_THROWS_EXCEPTION(Error,reopened.send(leader,{{"to","worker"},{"summary","explicit restart is not yet supported"},{"message","do not silently replay"}}));
        QVERIFY(!reopened.remove(leader).isError);
    }
    void teamIdentityIsolationAndFreshTaskNamespaces(){
        Fixture f;const auto leader=f.owner(),other=f.owner();
        const auto made=f.teams->create(leader,{{"team_name","studio"},{"description","Coordinate work"}});
        QVERIFY(!made.isError);QCOMPARE(made.data["team_name"].toString(),"studio");
        QVERIFY(!f.teams->status(leader.sessionId)["team"].isNull());QVERIFY(f.teams->status(other.sessionId)["team"].isNull());
        QVERIFY_THROWS_EXCEPTION(Error,f.teams->create(leader,{{"team_name","another"}}));
        const auto second=f.teams->create(other,{{"team_name","studio"}});QVERIFY(second.data["team_name"]!=made.data["team_name"]);
        const auto task=f.engine->runTaskTool(leader.sessionId,"TaskCreate",{{"subject","Shared work"},{"description","Team task"}});
        QVERIFY2(!task.isError,qPrintable(task.text));QCOMPARE(f.engine->runTaskTool(leader.sessionId,"TaskList").data["tasks"].toArray().size(),1);
        QVERIFY(f.engine->runTaskTool(other.sessionId,"TaskList").data["tasks"].toArray().isEmpty());
        QVERIFY(!f.teams->remove(leader).isError);QVERIFY(f.teams->status(leader.sessionId)["team"].isNull());
        QVERIFY(f.engine->runTaskTool(leader.sessionId,"TaskList").data["tasks"].toArray().isEmpty());
    }
    void memberActuallyRunsAndRepliesToTheLeader(){
        Fixture f;const auto leader=f.owner();f.teams->create(leader,{{"team_name","workers"}});
        const auto started=f.teams->spawn(leader,{{"name","reader"},{"prompt","Report your result"}});
        QVERIFY2(!started.isError,qPrintable(started.text));QCOMPARE(started.data["name"].toString(),"reader");
        const auto result=f.teams->wait(leader.sessionId,10000);QVERIFY2(result["idle"].toBool(),QJsonDocument(result).toJson().constData());
        const auto messages=f.teams->inbox(leader.sessionId)["messages"].toArray();bool found=false;
        for(const auto& value:messages){const auto m=value.toObject();if(m["message"]=="MEMBER_REPLY"){found=true;QCOMPARE(m["from"].toString(),"reader");}}
        QVERIFY(found);const auto member=started.data["session_id"].toString();
        QVERIFY(!member.isEmpty());QCOMPARE(f.teams->taskList(member),f.teams->taskList(leader.sessionId));
        QVERIFY(!f.teams->send(leader,{{"to","reader"},{"summary","Continue with a second observation"},{"message","Reply again"}}).isError);
        QVERIFY(f.teams->wait(leader.sessionId,10000)["idle"].toBool());
        const auto status=f.teams->status(leader.sessionId);QCOMPARE(status["team"].toObject()["members"].toArray().size(),2);
        QVERIFY(f.teams->stop(leader.sessionId,"reader")["stopped"].toBool());
        QVERIFY(!f.teams->remove(leader).isError);
    }
    void recipientsAndStructuredMessagesCannotForgeAuthority(){
        Fixture f;const auto leader=f.owner(),outsider=f.owner();f.teams->create(leader,{{"team_name","private"}});
        QVERIFY_THROWS_EXCEPTION(Error,f.teams->send(outsider,{{"to","team-lead"},{"summary","forged"},{"message","bad"}}));
        QVERIFY_THROWS_EXCEPTION(Error,f.teams->send(leader,{{"to","nobody"},{"summary","missing"},{"message","bad"}}));
        QVERIFY_THROWS_EXCEPTION(Error,f.teams->send(leader,{{"to","team-lead"},{"message","summary required"}}));
        QVERIFY_THROWS_EXCEPTION(Error,f.teams->send(leader,{{"to","*"},{"message",QJsonObject{{"type","shutdown_request"}}}}));
        QVERIFY_THROWS_EXCEPTION(Error,f.teams->send(leader,{{"to","uds:/tmp/socket"},{"summary","not a team member"},{"message","bad"}}));
        const auto sent=f.teams->send(leader,{{"to","*"},{"summary","No other members to notify"},{"message","hello"}});
        QVERIFY(!sent.isError);QVERIFY(sent.data["recipients"].toArray().isEmpty());
    }
};
QTEST_GUILESS_MAIN(TeamTests)
#include "team_tests.moc"
