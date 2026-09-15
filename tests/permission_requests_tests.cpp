#include "agent/PermissionRequests.h"
#include "agent/PermissionSettings.h"
#include "agent/Api.h"
#include "agent/McpServer.h"
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <future>
#include <atomic>
#include <barrier>
#include <mutex>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
using namespace std::chrono_literals;
class WriteModel final:public a::Model {
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken& token,const TextCallback&) override {
        token.throwIfCancelled();
        for(const auto& message:request.messages)if(message.role==a::MessageRole::Tool)return {"DONE"};
        return {{},{{"write","Write",{{"path","original.txt"},{"content","ORIGINAL"}}}}};
    }
};
QJsonObject apiCall(a::Api& api,const QString& method,const QJsonObject& params={},const QString& auth=QString(48,'a')) {
    auto handle=api.dispatch(method,params,auth);if(handle.result.wait_for(3s)!=std::future_status::ready)throw std::runtime_error("API timeout");
    return handle.result.get().toObject();
}
struct Wire {
    iiLocalLLM::mcp::ServerSession session;int sequence=0;QHash<int,QJsonObject> replies;QJsonArray notifications;
    explicit Wire(iiLocalLLM::mcp::ServerOptions options):session(options) {
        call("initialize",{{"protocolVersion","2025-11-25"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","permission-test"},{"version","1"}}}});
        session.receive({{"jsonrpc","2.0"},{"method","notifications/initialized"}});
    }
    int send(const QString& method,const QJsonObject& params={}) {
        const auto id=++sequence;session.receive({{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}});return id;
    }
    QJsonObject get(int id) {
        QElapsedTimer timer;timer.start();while(!replies.contains(id)&&timer.elapsed()<3000) {
            for(const auto& value:session.takeMessages(10)){const auto message=value.toObject();
                if(message.contains("id"))replies.insert(message["id"].toInt(),message);else notifications.append(message);}
        }
        if(!replies.contains(id))throw std::runtime_error("MCP response timeout");return replies.take(id);
    }
    QJsonObject call(const QString& method,const QJsonObject& params={}) {return get(send(method,params));}
};
}
class PermissionRequestsTests final:public QObject {
    Q_OBJECT
private slots:
    void mcpModelAndChildRequestsReachTheConnectionChannel() {
        QTemporaryDir root;const auto work=root.filePath("work");QVERIFY(QDir().mkpath(work));
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,work);
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{
            {"iiLocalLLM.agent.run",a::PermissionBehavior::Allow},{"iiLocalLLM.agent.agents.run",a::PermissionBehavior::Allow},{"Agent",a::PermissionBehavior::Allow}});
        auto model=std::make_shared<WriteModel>();a::EngineOptions engineOptions;engineOptions.sessionsDirectory=root.filePath("sessions");engineOptions.compaction.automatic=false;
        a::SubagentOptions childOptions;childOptions.workingDirectory=work;childOptions.stateDirectory=root.filePath("children");
        auto children=std::make_shared<a::Subagents>(model,registry,policy,engineOptions,childOptions);a::Subagents::attach(engineOptions,children);
        auto engine=std::make_shared<a::Engine>(model,registry,policy,engineOptions);a::McpServerOptions config;
        config.workingDirectory=work;config.engine=engine;config.model="fixture";config.permissionRequests=a::PermissionRequestsOptions{};
        Wire wire(a::mcpServerOptions(registry,policy,config));QString parent;
        for(bool child:{false,true}) {
            const auto operation=wire.send("tools/call",{{"name",child?"iiLocalLLM.agent.agents.run":"iiLocalLLM.agent.run"},
                {"arguments",QJsonObject{{"prompt","write"}}},{"_meta",QJsonObject{{"progressToken",child?"child":"parent"}}}});
            QJsonArray requests;QTRY_VERIFY_WITH_TIMEOUT(!(requests=wire.call("iisacc/permissions/pending")["result"].toObject()["requests"].toArray()).isEmpty(),3000);
            const auto prompt=requests.first().toObject();if(child)QVERIFY(prompt["session_id"]!=parent);else parent=prompt["session_id"].toString();
            QCOMPARE(prompt["request"].toObject()["tool_name"],"Write");
            QVERIFY(wire.call("iisacc/permissions/respond",{{"request_id",prompt["request_id"]},{"decision",QJsonObject{{"behavior","allow"},
                {"updatedInput",QJsonObject{{"path",child?"child.txt":"parent.txt"},{"content","CHANNEL"}}}}}})["result"].toObject()["accepted"].toBool());
            const auto result=wire.get(operation);QVERIFY2(!result.contains("error"),qPrintable(QString::fromUtf8(QJsonDocument(result).toJson())));
            QVERIFY(!result["result"].toObject()["isError"].toBool());QVERIFY(QFileInfo::exists(QDir(work).filePath(child?"child.txt":"parent.txt")));
        }
    }
    void boundedPendingAndHistoryRetainOnlyValidPagesAndRecentResponses() {
        QTemporaryDir work;a::PermissionRequestsOptions limits;limits.maxPending=2;limits.maxHistory=1;
        a::PermissionRequests broker(limits);auto make=[&](QString id){return broker.begin({id,"Write",{}},{a::PermissionBehavior::Ask},{"session","run",work.path()});};
        const auto one=make("one"),two=make("two");QVERIFY_THROWS_EXCEPTION(Error,make("three"));
        const auto page=broker.pending(0,1);QCOMPARE(page["requests"].toArray().size(),1);QCOMPARE(page["pending_count"],2);
        QCOMPARE(broker.pending(page["next_cursor"].toInteger())["requests"].toArray().first().toObject()["request_id"],two.request["request_id"]);
        QVERIFY(broker.respond(one.request["request_id"].toString(),{{"behavior","deny"}})["accepted"].toBool());
        const auto three=make("three");QVERIFY(broker.respond(two.request["request_id"].toString(),{{"behavior","deny"}})["accepted"].toBool());
        QVERIFY_THROWS_EXCEPTION(Error,broker.respond(one.request["request_id"].toString(),{{"behavior","deny"}}));
        QCOMPARE(broker.wait(one).behavior,a::PermissionBehavior::Deny); // A live ticket survives history eviction.
        broker.close();QJsonObject status;(void)broker.wait(three,{},&status);QCOMPARE(status["status"],"closed");
    }
    void apiControlsBypassFullQueueAndChildrenUseTheAuthenticatedChannel() {
        QTemporaryDir root;a::ApiOptions config;config.workingDirectory=root.filePath("work");QVERIFY(QDir().mkpath(config.workingDirectory));
        config.stateDirectory=root.filePath("state");config.clientTokens={{"society",QString(48,'a')},{"dreamscapes",QString(48,'b')}};
        config.permissionRequests=a::PermissionRequestsOptions{};config.maxConcurrentRequests=1;config.maxQueuedRequests=0;
        config.subagentsEnabled=true;config.engine.compaction.automatic=false;
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,config.workingDirectory);
        a::PermissionSettingsOptions settings;settings.workingDirectory=config.workingDirectory;
        auto policy=std::make_shared<a::SettingsPermissionPolicy>(settings,QList<a::PermissionRule>{{"Agent",a::PermissionBehavior::Allow}});
        a::Api api(std::make_shared<WriteModel>(),registry,policy,config);
        QVERIFY(apiCall(api,"agent.info")["permission_requests_enabled"].toBool());
        const auto id=apiCall(api,"agent.sessions.create",{{"model","fixture"}}).value("session_id");
        for(bool child:{false,true}) {
            auto running=api.dispatch(child?"agent.agents.run":"agent.run",{{"session_id",id},{"prompt","write"}},QString(48,'a'));
            QJsonArray requests;QTRY_VERIFY_WITH_TIMEOUT(!(requests=apiCall(api,"agent.permissions.pending")["requests"].toArray()).isEmpty(),3000);
            QCOMPARE(requests.size(),1);const auto prompt=requests.first().toObject();
            QCOMPARE(prompt["request"].toObject()["tool_name"],"Write");
            if(child)QVERIFY(prompt["session_id"]!=id);else QCOMPARE(prompt["session_id"],id);
            QVERIFY(apiCall(api,"agent.permissions.pending",{},QString(48,'b'))["requests"].toArray().isEmpty());
            const QJsonObject decision{{"behavior","allow"},{"updatedInput",QJsonObject{{"path",child?"child.txt":"approved.txt"},{"content","CLIENT"}}}};
            const QJsonObject response{{"request_id",prompt["request_id"]},{"decision",decision}};
            QVERIFY_THROWS_EXCEPTION(Error,apiCall(api,"agent.permissions.respond",response,QString(48,'b')));
            QVERIFY_THROWS_EXCEPTION(Error,apiCall(api,"agent.permissions.respond",response,"invalid"));
            QVERIFY_THROWS_EXCEPTION(Error,apiCall(api,"agent.info")); // Ordinary worker capacity is exhausted.
            QVERIFY(apiCall(api,"agent.permissions.respond",response)["accepted"].toBool());
            QVERIFY(running.result.wait_for(3s)==std::future_status::ready);const auto result=running.result.get().toObject();
            if(child)QVERIFY2(!result["is_error"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(result).toJson())));
            else QCOMPARE(result["status"],"completed");
            QFile file(QDir(config.workingDirectory).filePath(child?"child.txt":"approved.txt"));QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("CLIENT"));
            QVERIFY(apiCall(api,"agent.permissions.pending")["requests"].toArray().isEmpty());
        }
        QVERIFY(!QFileInfo::exists(QDir(config.workingDirectory).filePath("original.txt")));
    }
    void mcpControlPoolCannotBeStarvedByWaitingToolsAndRepliesStayConnectionBound() {
        QTemporaryDir work;auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,work.path());
        a::McpServerOptions config;config.workingDirectory=work.path();config.permissionRequests=a::PermissionRequestsOptions{};
        auto options=a::mcpServerOptions(registry,std::make_shared<a::RulePolicy>(),config);
        options.maxConcurrentRequests=1;options.maxQueuedRequests=0;Wire first(options),other(options);
        const auto tools=first.call("tools/list")["result"].toObject()["tools"].toArray();
        for(const auto& tool:tools)QVERIFY(!tool.toObject()["name"].toString().contains("permissions.respond"));
        const auto writing=first.send("tools/call",{{"name","Write"},{"arguments",QJsonObject{{"path","original.txt"},{"content","ORIGINAL"}}},
            {"_meta",QJsonObject{{"progressToken","write-progress"}}}});
        QJsonArray requests;QTRY_VERIFY_WITH_TIMEOUT(!(requests=first.call("iisacc/permissions/pending")["result"].toObject()["requests"].toArray()).isEmpty(),3000);
        const auto prompt=requests.first().toObject();QVERIFY(other.call("iisacc/permissions/pending")["result"].toObject()["requests"].toArray().isEmpty());
        const QJsonObject response{{"request_id",prompt["request_id"]},{"decision",QJsonObject{{"behavior","allow"},{"updatedInput",QJsonObject{{"path","approved.txt"},{"content","MCP"}}}}}};
        QVERIFY(other.call("iisacc/permissions/respond",response).contains("error"));
        QVERIFY(first.call("tools/list").contains("error"));
        QVERIFY(first.call("iisacc/permissions/respond",response)["result"].toObject()["accepted"].toBool());
        QVERIFY(!first.get(writing)["result"].toObject()["isError"].toBool());
        QVERIFY(first.call("iisacc/permissions/respond",response)["result"].toObject()["replayed"].toBool());
        QFile file(work.filePath("approved.txt"));QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("MCP"));
        QVERIFY(!QFileInfo::exists(work.filePath("original.txt")));bool asked=false,resolved=false;double last=-1;
        for(const auto& v:first.notifications){const auto p=v.toObject()["params"].toObject();if(!p.contains("progress"))continue;
            QVERIFY(p["progress"].toDouble()>last);last=p["progress"].toDouble();const auto event=p["_meta"].toObject()["iisacc/agentEvent"].toObject();
            asked|=event["event"]=="permission_requested";resolved|=event["event"]=="permission_resolved";}
        QVERIFY(asked);QVERIFY(resolved);
    }
    void clientWinsAgainstACooperativeHookAndOnlyItsUpdatesAreApplied() {
        QTemporaryDir work;auto broker=std::make_shared<a::PermissionRequests>();
        a::PermissionSettingsOptions settings;settings.workingDirectory=work.path();auto policy=std::make_shared<a::SettingsPermissionPolicy>(settings);
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,work.path());
        std::promise<void> entered;std::atomic_bool stopped=false;
        a::ToolRunnerOptions options;options.permissionRequests=broker;
        options.hooks.append([&](const a::HookInput& input,const CancellationToken& token){a::HookResult result;
            if(input.kind!=a::HookKind::PermissionRequest)return result;
            entered.set_value();while(!token.isCancelled())QThread::msleep(1);stopped=true;
            result.permissionResponse=a::PermissionResponse{a::PermissionBehavior::Allow};
            result.permissionResponse->updatedArguments=QJsonObject{{"path","loser.txt"},{"content","LOSER"}};return result;});
        CancellationToken cancellation;
        auto running=std::async(std::launch::async,[&]{return a::ToolRunner(registry,policy,options).run({"write","Write",{{"path","original.txt"},{"content","ORIGINAL"}}},{"session","run",work.path(),{},cancellation});});
        struct CancelOnExit {CancellationToken token;~CancelOnExit(){token.cancel();}} cleanup{cancellation};
        QVERIFY(entered.get_future().wait_for(std::chrono::seconds(3))==std::future_status::ready);
        const auto pending=broker->pending()["requests"].toArray();QCOMPARE(pending.size(),1);const auto id=pending.first().toObject()["request_id"].toString();
        const QJsonArray updates{QJsonObject{{"type","addRules"},{"destination","session"},{"behavior","allow"},
            {"rules",QJsonArray{QJsonObject{{"toolName","Write"},{"ruleContent","/future.txt"}}}}}};
        QVERIFY(broker->respond(id,{{"behavior","allow"},{"updatedInput",QJsonObject{{"path","approved.txt"},{"content","CLIENT"}}},{"updatedPermissions",updates}})["accepted"].toBool());
        const auto result=running.get();QVERIFY2(!result.isError,qPrintable(result.text));QVERIFY(stopped);
        QFile file(work.filePath("approved.txt"));QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("CLIENT"));
        QVERIFY(!QFileInfo::exists(work.filePath("original.txt")));QVERIFY(!QFileInfo::exists(work.filePath("loser.txt")));
        QCOMPARE(policy->decide({"Write"},{{"path","future.txt"}},{"session",{},work.path()}).behavior,a::PermissionBehavior::Allow);
    }
    void hookWinnerDismissesTheRemotePromptAndHostDenyNeverPrompts() {
        QTemporaryDir work;auto broker=std::make_shared<a::PermissionRequests>();auto registry=std::make_shared<a::ToolRegistry>();
        a::registerWorkspaceTools(*registry,work.path());a::ToolRunnerOptions options;options.permissionRequests=broker;
        options.hooks.append([](const a::HookInput& input,const auto&){a::HookResult r;if(input.kind==a::HookKind::PermissionRequest)r.permissionResponse=a::PermissionResponse{a::PermissionBehavior::Allow};return r;});
        QString id;bool resolved=false;
        auto observe=[&](const a::Event& event){if(event.kind==a::EventKind::PermissionRequested)id=event.data["request_id"].toString();if(event.kind==a::EventKind::PermissionResolved)resolved=true;};
        const auto result=a::ToolRunner(registry,std::make_shared<a::RulePolicy>(),options).run({"write","Write",{{"path","hook.txt"},{"content","HOOK"}}},{"session","run",work.path()},observe);
        QVERIFY(!result.isError);QVERIFY(!id.isEmpty());QVERIFY(resolved);
        QVERIFY(!broker->respond(id,{{"behavior","deny"},{"message","LATE"}})["accepted"].toBool());
        auto deny=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{{"Write",a::PermissionBehavior::Deny}});id.clear();
        QVERIFY(a::ToolRunner(registry,deny,options).run({"denied","Write",{{"path","denied.txt"},{"content","NO"}}},{"session","run",work.path()},observe).isError);
        QVERIFY(id.isEmpty());QVERIFY(!QFileInfo::exists(work.filePath("denied.txt")));
    }
    void repliesAreBoundToOneChannelAndCannotBeReplayedAsNewWork() {
        QTemporaryDir work;a::PermissionRequests first,other;
        auto ticket=first.begin({"write-1","Write",{{"path","out.txt"},{"content","ORIGINAL"}}},
            {a::PermissionBehavior::Ask,"Host decision"},{"session","run",work.path()});
        const auto id=ticket.request["request_id"].toString();QVERIFY(!id.isEmpty());
        const QJsonObject response{{"behavior","allow"},{"updatedInput",QJsonObject{{"path","approved.txt"},{"content","APPROVED"}}}};
        QVERIFY_THROWS_EXCEPTION(Error,other.respond(id,response));
        QCOMPARE(first.pending()["requests"].toArray().size(),1);
        QVERIFY(first.respond(id,response)["accepted"].toBool());
        QVERIFY(first.respond(id,response)["replayed"].toBool());
        QVERIFY_THROWS_EXCEPTION(Error,first.respond(id,{{"behavior","deny"},{"message","LATE"}}));
        const auto result=first.wait(ticket);QCOMPARE(result.behavior,a::PermissionBehavior::Allow);
        QCOMPARE(result.updatedArguments->value("content"),"APPROVED");QVERIFY(first.pending()["requests"].toArray().isEmpty());
    }
    void cancellationTimeoutAndCloseLeaveNoPendingApproval() {
        QTemporaryDir work;a::PermissionRequestsOptions options;options.timeoutMs=25;a::PermissionRequests broker(options);
        auto make=[&]{return broker.begin({"write","Write",{}},{a::PermissionBehavior::Ask},{"session","run",work.path()});};
        const auto expired=make();QJsonObject status;QCOMPARE(broker.wait(expired,{},&status).behavior,a::PermissionBehavior::Deny);
        QCOMPARE(status["status"],"expired");QVERIFY(broker.pending()["requests"].toArray().isEmpty());
        QVERIFY(!broker.respond(expired.request["request_id"].toString(),{{"behavior","allow"}})["accepted"].toBool());
        const auto cancelled=make();CancellationToken token;token.cancel();QVERIFY_THROWS_EXCEPTION(Error,broker.wait(cancelled,token));
        const auto closed=make();broker.close();QCOMPARE(broker.wait(closed).behavior,a::PermissionBehavior::Deny);
        QVERIFY(broker.pending()["requests"].toArray().isEmpty());QVERIFY_THROWS_EXCEPTION(Error,make());
    }
    void invalidAndOversizedResponsesCannotConsumeTheRequest() {
        QTemporaryDir work;a::PermissionRequests broker;auto ticket=broker.begin({"call","Write",{}},{a::PermissionBehavior::Ask},{"session","run",work.path()});
        const auto id=ticket.request["request_id"].toString();
        for(const auto& response:QJsonArray{QJsonObject{{"behavior","ask"}},QJsonObject{{"behavior","allow"},{"interrupt",true}},
            QJsonObject{{"behavior","allow"},{"updatedPermissions",QJsonArray{QJsonObject{{"type","unknown"}}}}},
            QJsonObject{{"behavior","allow"},{"toolUseID","wrong"}}}) {
            QVERIFY_THROWS_EXCEPTION(Error,broker.respond(id,response.toObject()));QCOMPARE(broker.pending()["requests"].toArray().size(),1);
        }
        QVERIFY_THROWS_EXCEPTION(Error,broker.respond(id,{{"behavior","allow"},{"updatedInput",QJsonObject{{"content",QString(2*1024*1024,'x')}}}}));
        QCOMPARE(broker.pending()["requests"].toArray().size(),1);
        QVERIFY(broker.respond(id,{{"behavior","allow"},{"updatedInput",QJsonObject{}}})["accepted"].toBool());
        QVERIFY(!broker.wait(ticket).updatedArguments); // Mobile empty-input approval retains the original input.
    }
    void simultaneousConflictingResponsesHaveExactlyOneWinner() {
        QTemporaryDir work;a::PermissionRequests broker;const auto ticket=broker.begin({"call","Write",{}},{a::PermissionBehavior::Ask},{"session","run",work.path()});
        std::barrier ready(8);std::atomic_int winner=-1,accepted=0,conflicts=0;std::vector<std::future<void>> jobs;
        for(int i=0;i<8;++i)jobs.push_back(std::async(std::launch::async,[&,i]{ready.arrive_and_wait();try{
            const auto reply=broker.respond(ticket.request["request_id"].toString(),{{"behavior","deny"},{"message",QString::number(i)}});
            if(reply["accepted"].toBool()){++accepted;winner=i;}
        }catch(const Error& error){if(error.code()==ErrorCode::AlreadyExists)++conflicts;else throw;}}));
        for(auto& job:jobs)job.get();QCOMPARE(accepted.load(),1);QCOMPARE(conflicts.load(),7);QCOMPARE(broker.wait(ticket).message,QString::number(winner.load()));
    }
};
QTEST_GUILESS_MAIN(PermissionRequestsTests)
#include "permission_requests_tests.moc"
