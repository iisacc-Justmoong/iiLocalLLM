#include <agent/QuestionInbox.h>
#include <agent/UserQuestions.h>
#include <agent/McpServer.h>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <future>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
QJsonObject question() {return {{"questions",QJsonArray{QJsonObject{{"question","Which renderer?"},{"header","Renderer"},
    {"options",QJsonArray{QJsonObject{{"label","Qt"},{"description","Native"}},QJsonObject{{"label","Web"},{"description","Browser"}}}}}}}};}
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("question-inbox-XXXXXX")};std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::RulePolicy> policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    Fixture(){registry->add(a::userQuestionTool({false,"markdown"}));}
    std::future<a::ToolResult> run(a::Hook hook,QString id="owner",CancellationToken token={}) {
        return std::async(std::launch::async,[=,this] {
            a::ToolRunnerOptions options;options.hooks={hook};
            return a::ToolRunner(registry,policy,options).run({id,"AskUserQuestion",question()},{id,"run",root.path(),{},token});
        });
    }
};
QString id(a::QuestionInbox& box){return box.requests().first().toMap()["request_id"].toString();}
}
class QuestionInboxTests:public QObject {
    Q_OBJECT
private slots:
    void mcpQuestionWaitDoesNotLockAppMutations() {
        Fixture f;a::QuestionInbox box;
        a::Tool mutation;mutation.definition={"navigate","Mutate app state",{{"type","object"}},{},false,false};
        mutation.execute=[](const auto&,const auto&){return a::ToolResult{"navigation completed"};};f.registry->add(mutation);
        a::McpServerOptions options;options.workingDirectory=f.root.path();options.tools.hooks={box.hook()};
        auto server=a::mcpServerOptions(f.registry,f.policy,options);iiLocalLLM::mcp::ServerRequestContext context;context.sessionId="connection";
        bool marked=false;for(const auto& row:server.lists.at("tools/list")(context))if(row.toObject()["name"]=="AskUserQuestion")
            marked=row.toObject()["_meta"].toObject()["iisacc/userInteraction"]==true;QVERIFY(marked);
        auto call=server.handlers.at("tools/call");
        auto pending=std::async(std::launch::async,[&]{return call({{"name","AskUserQuestion"},{"arguments",question()}},context);});
        QTRY_COMPARE(box.requests().size(),1);
        auto navigating=std::async(std::launch::async,[&]{return call({{"name","navigate"},{"arguments",QJsonObject{}}},context);});
        const auto independent=navigating.wait_for(std::chrono::seconds(1))==std::future_status::ready;
        box.close();QVERIFY(pending.get()["isError"].toBool());QVERIFY(!navigating.get()["isError"].toBool());QVERIFY(independent);
    }
    void workerQuestionAppearsAndReceivesGuiAnswer() {
        Fixture f;a::QuestionInbox box;auto pending=f.run(box.hook());QTRY_COMPARE(box.requests().size(),1);
        const auto request=box.requests().first().toMap();QCOMPARE(request["session_id"].toString(),"owner");
        QVERIFY(box.submit(id(box),{{"Which renderer?","Native Qt"}},{{"Which renderer?",QVariantMap{{"notes","사용자 메모"}}}}));
        auto result=pending.get();QVERIFY2(!result.isError,qPrintable(result.text));QCOMPARE(result.data["answers"].toObject()["Which renderer?"],"Native Qt");
        QCOMPARE(result.data["annotations"].toObject()["Which renderer?"].toObject()["notes"],"사용자 메모");QVERIFY(box.requests().isEmpty());
    }
    void stableSnapshotsDoNotResetTheUi() {
        Fixture f;a::QuestionInbox box;auto pending=f.run(box.hook());QTRY_COMPARE(box.requests().size(),1);
        QSignalSpy changed(&box,&a::QuestionInbox::requestsChanged);for(int i=0;i<10;++i)box.refresh();QCOMPARE(changed.size(),0);
        const auto first=id(box);QVERIFY(box.submit(first,{}));QVERIFY(!pending.get().isError);QCOMPARE(changed.size(),1);
        QVERIFY(!box.submit(first,{{"Which renderer?","late"}}));QVERIFY(!box.errorString().isEmpty());
    }
    void malformedUiInputDoesNotConsumeTheRequest() {
        Fixture f;a::QuestionInbox box;auto pending=f.run(box.hook());QTRY_COMPARE(box.requests().size(),1);const auto key=id(box);
        QVERIFY(!box.submit(key,{{"unasked","fake"}}));QCOMPARE(box.requests().size(),1);
        QVERIFY(!box.submit(key,{{"Which renderer?",42}}));QCOMPARE(box.requests().size(),1);
        QVERIFY(box.submit(key,{{"Which renderer?","Qt"}}));QVERIFY(box.errorString().isEmpty());QVERIFY(!pending.get().isError);
    }
    void concurrentOwnersAreAnsweredIndependently() {
        Fixture f;a::QuestionInbox box;auto first=f.run(box.hook(),"first");QTRY_COMPARE(box.requests().size(),1);const auto firstId=id(box);
        auto second=f.run(box.hook(),"second");QTRY_COMPARE(box.requests().size(),2);QCOMPARE(id(box),firstId);
        const auto secondId=box.requests().last().toMap()["request_id"].toString();
        QVERIFY(box.submit(secondId,{{"Which renderer?","SECOND"}}));QCOMPARE(second.get().data["answers"].toObject()["Which renderer?"],"SECOND");
        QCOMPARE(box.requests().size(),1);QCOMPARE(id(box),firstId);QVERIFY(box.reject(firstId,"Declined"));QVERIFY(first.get().isError);
    }
    void cancellationExpiryAndDestructionReleaseWorkers() {
        Fixture f;a::QuestionInbox box;CancellationToken token;auto pending=f.run(box.hook(),"cancelled",token);
        QTRY_COMPARE(box.requests().size(),1);token.cancel();QVERIFY_THROWS_EXCEPTION(Error,(void)pending.get());box.refresh();QVERIFY(box.requests().isEmpty());
        a::PermissionRequestsOptions config;config.timeoutMs=100;a::QuestionInbox timed(config);
        auto expires=f.run(timed.hook(),"expired");QVERIFY(expires.get().isError);timed.refresh();QVERIFY(timed.requests().isEmpty());
        auto owned=std::make_unique<a::QuestionInbox>();auto closing=f.run(owned->hook(),"closing");QTRY_COMPARE(owned->requests().size(),1);
        owned.reset();QVERIFY(closing.get().isError);
    }
    void borrowedChannelSurvivesViewDestruction() {
        auto broker=std::make_shared<a::PermissionRequests>();{
            a::QuestionInbox box(broker);QVERIFY(box.requests().isEmpty());
        }QVERIFY(!broker->pending()["closed"].toBool());
    }
};
QTEST_GUILESS_MAIN(QuestionInboxTests)
#include "question_inbox_tests.moc"
