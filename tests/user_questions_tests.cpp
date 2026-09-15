#include <agent/Engine.h>
#include <agent/PermissionRequests.h>
#include <agent/McpServer.h>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <future>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
QJsonObject question() {
    return {{"questions",QJsonArray{QJsonObject{{"question","Which renderer?"},{"header","Renderer"},
        {"options",QJsonArray{QJsonObject{{"label","Qt"},{"description","Native UI"},{"preview","**Qt**"}},
            QJsonObject{{"label","Web"},{"description","Browser UI"}}}},{"multiSelect",false}}}}};
}
class Model final:public a::Model {
public:
    int turn=0;QList<a::ModelRequest> requests;
    std::function<a::ModelReply(const a::ModelRequest&)> reply;
    a::ModelReply generate(const a::ModelRequest& r,const CancellationToken&,const TextCallback&)override {
        if(reply){requests.append(r);++turn;return reply(r);}
        requests.append(r);if(++turn==1)return {{},{{"question","AskUserQuestion",question()}}};
        return {r.messages.last().text,{}};
    }
};
struct Fixture {
    QTemporaryDir root;std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<const a::PermissionPolicy> policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    a::ToolRunnerOptions options;a::ToolContext context{"owner","run",root.path(),root.filePath("artifacts")};
    explicit Fixture(QString format="markdown") {registry->add(a::userQuestionTool({false,format}));}
    a::ToolResult run(QJsonObject args=question(),a::EventCallback event={}) {
        return a::ToolRunner(registry,policy,options).run({"call","AskUserQuestion",args},context,event);
    }
    void answer(QJsonObject answers={{"Which renderer?","Qt"}},QJsonObject annotations={}) {
        options.permissionResponse=[=](const auto& call,const auto&,const auto&) {
            auto args=call.arguments;args["answers"]=answers;if(!annotations.isEmpty())args["annotations"]=annotations;
            return a::PermissionResponse{a::PermissionBehavior::Allow,{},args};
        };
    }
};
}
class UserQuestionsTests final:public QObject {
    Q_OBJECT
private slots:
    void deferredQuestionsAreDiscoveredAndResumeWithTheAnswer() {
        QTemporaryDir root;auto model=std::make_shared<Model>();a::EngineOptions config;config.sessionsDirectory=root.filePath("sessions");
        config.userQuestionsEnabled=true;config.projectContext.enabled=false;config.compaction.automatic=false;config.skills.enabled=false;
        config.permissionResponse=[](const auto& call,const auto&,const auto&){auto args=call.arguments;args["answers"]=QJsonObject{{"Which renderer?","discovered"}};return a::PermissionResponse{a::PermissionBehavior::Allow,{},args};};
        model->reply=[raw=model.get()](const auto& r)->a::ModelReply {
            if(raw->turn==1)return {{},{{"search","ToolSearch",{{"query","select:AskUserQuestion"}}}}};
            if(raw->turn==2)return {{},{{"ask","AskUserQuestion",question()}}};return {r.messages.last().text,{}};
        };
        a::Engine engine(model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(),config);
        auto id=engine.createSession("fixture",root.path()).id;const auto result=engine.run({id,"Clarify UI requirements"}).result.get();
        QCOMPARE(result.status,a::RunStatus::Completed);QVERIFY(result.text.contains("discovered"));
        auto has=[](const auto& r){return std::any_of(r.tools.begin(),r.tools.end(),[](const auto& t){return t.name=="AskUserQuestion";});};
        QVERIFY(!has(model->requests.first()));QVERIFY(has(model->requests[1]));
        const auto fork=engine.forkSession(id);QVERIFY(std::any_of(fork.messages.begin(),fork.messages.end(),[](const auto& m){return m.role==a::MessageRole::Tool&&m.text.contains("discovered");}));
    }
    void missingResponseAndDontAskNeverInventAnAnswer() {
        Fixture f;QVERIFY(f.run().isError);f.options.permission=[](const auto&,const auto&,const auto&){return true;};
        auto skipped=f.run();QVERIFY(!skipped.isError);QVERIFY(skipped.data["answers"].toObject().isEmpty());
        f.policy=std::make_shared<a::RulePolicy>(a::PermissionMode::DontAsk);int called=0;
        f.options.permission=[&](const auto&,const auto&,const auto&){++called;return true;};
        QVERIFY(f.run().isError);QCOMPARE(called,0);
        f.policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"AskUserQuestion",a::PermissionBehavior::Deny}});
        QVERIFY(f.run().isError);QCOMPARE(called,0);
    }
    void questionsOptionsFreeTextAndAnnotationsRoundTrip() {
        Fixture f;auto args=question();auto list=args["questions"].toArray();auto q=list.first().toObject();
        q["question"]="Which features?";q["multiSelect"]=true;list.append(q);q["question"]="Any constraints?";list.append(q);args["questions"]=list;
        args["metadata"]=QJsonObject{{"source","society.settings"}};
        const QJsonObject answers{{"Which renderer?","A custom renderer"},{"Which features?","Qt, Web"}};
        const QJsonObject annotations{{"Which renderer?",QJsonObject{{"preview","**Custom**"},{"notes","한국어 메모"}}}};
        f.answer(answers,annotations);QJsonObject preview;int requests=0;
        auto result=f.run(args,[&](const auto& e){if(e.kind==a::EventKind::PermissionRequested){++requests;preview=e.data["permission_preview"].toObject()["_meta"].toObject()["user_question"].toObject();}});
        QVERIFY2(!result.isError,qPrintable(result.text));QCOMPARE(requests,1);QCOMPARE(result.data["questions"].toArray(),list);
        QCOMPARE(result.data["answers"],answers);QCOMPARE(result.data["annotations"],annotations);
        QCOMPARE(preview["input"],args);QCOMPARE(preview["preview_format"],"markdown");QVERIFY(preview["partial_answers"].toBool());
        QVERIFY(!result.data.contains("metadata"));QVERIFY(result.text.contains("한국어 메모"));
    }
    void malformedQuestionsFailBeforeReview_data() {
        QTest::addColumn<QJsonObject>("args");auto base=question();
        auto add=[&](const char* name,QJsonObject value){QTest::newRow(name)<<value;};
        auto v=base;v["answers"]=QJsonObject{{"Which renderer?","forged"}};add("model-answers",v);
        v=base;v["annotations"]=QJsonObject{};add("model-annotations",v);
        v=base;v["session_id"]="foreign";add("wire-owner",v);
        v=base;v["questions"]=QJsonArray{};add("empty",v);
        const auto q=base["questions"].toArray().first().toObject();
        v=base;v["questions"]=QJsonArray{q,q};add("duplicate",v);
        v=base;v["questions"]=QJsonArray{q,q,q,q,q};add("too-many",v);
        auto altered=q;altered["options"]=QJsonArray{q["options"].toArray().first()};v["questions"]=QJsonArray{altered};add("one-option",v);
        altered=q;altered["options"]=QJsonArray{q["options"].toArray().first(),q["options"].toArray().first()};v["questions"]=QJsonArray{altered};add("duplicate-label",v);
        altered=q;altered["question"]="   ";v["questions"]=QJsonArray{altered};add("blank",v);
        altered=q;altered["question"]=QString("bad")+QChar::Null;v["questions"]=QJsonArray{altered};add("nul",v);
        altered=q;altered["multiSelect"]="true";v["questions"]=QJsonArray{altered};add("type",v);
        altered=q;altered["header"]=QString(129,'x');v["questions"]=QJsonArray{altered};add("header-limit",v);
        auto largeOptions=q["options"].toArray();auto option=largeOptions.first().toObject();option["preview"]=QString(32768,QChar(0x97d3));
        largeOptions[0]=option;option["label"]="second";largeOptions[1]=option;option["label"]="third";largeOptions.append(option);
        altered=q;altered["options"]=largeOptions;v["questions"]=QJsonArray{altered};add("utf8-payload-limit",v);
    }
    void malformedQuestionsFailBeforeReview() {
        QFETCH(QJsonObject,args);Fixture f;int reviews=0;f.options.permission=[&](const auto&,const auto&,const auto&){++reviews;return true;};
        QVERIFY(f.run(args).isError);QCOMPARE(reviews,0);
    }
    void hostResponseCannotReplaceQuestionsOrInjectUnknownAnswers_data() {
        QTest::addColumn<QString>("change");for(auto name:{"question","options","metadata","missing-questions","unknown-answer","unknown-annotation","answer-type"})QTest::newRow(name)<<QString(name);
    }
    void hostResponseCannotReplaceQuestionsOrInjectUnknownAnswers() {
        QFETCH(QString,change);Fixture f;f.options.permissionResponse=[&](const auto& call,const auto&,const auto&) {
            auto args=call.arguments;args["answers"]=QJsonObject{{"Which renderer?","Qt"}};
            if(change=="metadata")args["metadata"]=QJsonObject{{"source","altered"}};
            else if(change=="missing-questions")args.remove("questions");
            else if(change=="unknown-answer")args["answers"]=QJsonObject{{"unasked","fake"}};
            else if(change=="unknown-annotation")args["annotations"]=QJsonObject{{"unasked",QJsonObject{{"notes","fake"}}}};
            else if(change=="answer-type")args["answers"]=QJsonObject{{"Which renderer?",QJsonArray{"Qt","Web"}}};
            else {auto q=args["questions"].toArray().first().toObject();q[change]=change=="question"?QJsonValue("Different?"):QJsonValue(QJsonArray{});args["questions"]=QJsonArray{q};}
            return a::PermissionResponse{a::PermissionBehavior::Allow,{},args};
        };
        int started=0;const auto result=f.run(question(),[&](const auto& e){if(e.kind==a::EventKind::ToolStarted)++started;});
        QVERIFY(result.isError);QCOMPARE(started,0);
    }
    void htmlValidationIsAnIntentCheckNotASanitizer() {
        QVERIFY_THROWS_EXCEPTION(Error,a::userQuestionTool({false,"xml"}));Fixture f("html");f.answer();
        auto args=question();auto q=args["questions"].toArray().first().toObject();auto opts=q["options"].toArray();auto opt=opts.first().toObject();
        for(auto preview:{"plain text","<script>x</script>","<style>p{}</style>","<!DOCTYPE html><p>x</p>","<BODY><p>x</p></BODY>"}) {
            opt["preview"]=preview;opts[0]=opt;q["options"]=opts;args["questions"]=QJsonArray{q};QVERIFY(f.run(args).isError);
        }
        opt["preview"]="<div onclick='untrusted()'>Render only after host sanitization</div>";opts[0]=opt;q["options"]=opts;args["questions"]=QJsonArray{q};
        QVERIFY(!f.run(args).isError);f.answer({{"Which renderer?","Qt"}},{{"Which renderer?",QJsonObject{{"preview","<script>x</script>"}}}});QVERIFY(f.run(args).isError);
    }
    void beforeToolCannotForgeAnswersButPermissionRequestHookCanRespond() {
        Fixture f;f.options.hooks={[](const auto& input,const auto&) {
            a::HookResult result;if(input.kind==a::HookKind::BeforeTool){auto args=input.call.arguments;args["answers"]=QJsonObject{{"Which renderer?","fake"}};result.updatedArguments=args;}return result;
        }};QVERIFY(f.run().isError);
        f.options.hooks={[](const auto& input,const auto&) {
            a::HookResult result;if(input.kind==a::HookKind::PermissionRequest){auto args=input.call.arguments;args["answers"]=QJsonObject{{"Which renderer?","host-hook"}};
                result.permissionResponse=a::PermissionResponse{a::PermissionBehavior::Allow,{},args};}return result;
        }};
        auto result=f.run();QVERIFY(!result.isError);QCOMPARE(result.data["answers"].toObject()["Which renderer?"],"host-hook");
        f.context.verificationAgent=true;QVERIFY(f.run().isError);
    }
    void channelAnswerIsOwnedAndReplayCannotChangeIt() {
        Fixture f;auto channel=std::make_shared<a::PermissionRequests>();f.options.permissionRequests=channel;a::PermissionRequests foreign;
        QString id;QJsonObject response;bool resolved=false;
        auto result=f.run(question(),[&](const auto& e) {
            if(e.kind==a::EventKind::PermissionRequested) {
                id=e.data["request_id"].toString();QCOMPARE(e.data["session_id"],"owner");
                auto args=question();args["answers"]=QJsonObject{{"Which renderer?","channel"}};response={{"behavior","allow"},{"updatedInput",args}};
                QVERIFY_THROWS_EXCEPTION(Error,foreign.respond(id,response));QVERIFY(channel->respond(id,response)["accepted"].toBool());
            }
            if(e.kind==a::EventKind::PermissionResolved)resolved=true;
        });
        QVERIFY2(!result.isError,qPrintable(result.text));QVERIFY(resolved);QCOMPARE(result.data["answers"].toObject()["Which renderer?"],"channel");
        QVERIFY(channel->pending()["requests"].toArray().isEmpty());QVERIFY(channel->respond(id,response)["accepted"].toBool());
        QVERIFY_THROWS_EXCEPTION(Error,channel->respond(id,{{"behavior","deny"}}));
    }
    void expiredAndCancelledQuestionsCannotComplete() {
        Fixture f;a::PermissionRequestsOptions limits;limits.timeoutMs=25;auto channel=std::make_shared<a::PermissionRequests>(limits);f.options.permissionRequests=channel;
        QString id;QString status;auto observe=[&](const auto& e){if(e.kind==a::EventKind::PermissionRequested)id=e.data["request_id"].toString();if(e.kind==a::EventKind::PermissionResolved)status=e.data["status"].toString();};
        QVERIFY(f.run(question(),observe).isError);QCOMPARE(status,"expired");QVERIFY(!channel->respond(id,{{"behavior","allow"}})["accepted"].toBool());
        f.context.cancellation=CancellationToken{};
        QVERIFY_THROWS_EXCEPTION(Error,f.run(question(),[&](const auto& e){observe(e);if(e.kind==a::EventKind::PermissionRequested)f.context.cancellation.cancel();}));
        QVERIFY(channel->pending()["requests"].toArray().isEmpty());QVERIFY(!channel->respond(id,{{"behavior","allow"}})["accepted"].toBool());
    }
    void disabledEngineAndReservedIdentity() {
        QTemporaryDir root;a::EngineOptions config;config.sessionsDirectory=root.filePath("sessions");
        auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>();
        a::Engine disabled(model,registry,policy,config);QVERIFY(!disabled.userQuestionTool());const auto id=disabled.createSession("fixture",root.path()).id;
        QVERIFY_THROWS_EXCEPTION(Error,disabled.runQuestionTool(id,question()));
        config.userQuestionsEnabled=true;registry->add(a::userQuestionTool());QVERIFY_THROWS_EXCEPTION(Error,a::Engine(model,registry,policy,config));
    }
    void endingSessionCancelsAQuestionAndPlanCanContinueAfterAnswer() {
        QTemporaryDir root;a::EngineOptions config;config.sessionsDirectory=root.filePath("sessions");config.userQuestionsEnabled=true;config.planToolsEnabled=true;
        config.permissionRequests=std::make_shared<a::PermissionRequests>();config.projectContext.enabled=false;config.skills.enabled=false;
        a::Engine engine(std::make_shared<Model>(),std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),config);
        const auto id=engine.createSession("fixture",root.path()).id;QVERIFY(!engine.runPlanTool(id,"EnterPlanMode").isError);
        auto answered=engine.runQuestionTool(id,question(),{},[&](const auto& e) {
            if(e.kind==a::EventKind::PermissionRequested){auto args=question();args["answers"]=QJsonObject{{"Which renderer?","Qt"}};
                config.permissionRequests->respond(e.data["request_id"].toString(),{{"behavior","allow"},{"updatedInput",args}});}
        });QVERIFY(!answered.isError);QCOMPARE(engine.planStatus(id)["phase"],"planning");
        auto pending=std::async(std::launch::async,[&]{return engine.runQuestionTool(id,question());});
        QTRY_VERIFY_WITH_TIMEOUT(!config.permissionRequests->pending()["requests"].toArray().isEmpty(),3000);
        (void)engine.endSession(id);QVERIFY_THROWS_EXCEPTION(Error,(void)pending.get());QVERIFY(config.permissionRequests->pending()["requests"].toArray().isEmpty());
    }
    void modelConsumesTrustedHostAnswer() {
        QTemporaryDir root;auto model=std::make_shared<Model>();a::EngineOptions config;config.sessionsDirectory=root.filePath("sessions");
        config.projectContext.enabled=false;config.compaction.automatic=false;config.skills.enabled=false;
        config.userQuestionsEnabled=true;config.userQuestions.deferred=false;
        int reviews=0;config.permissionResponse=[&](const auto& call,const auto&,const auto&) {
            ++reviews;auto args=call.arguments;args["answers"]=QJsonObject{{"Which renderer?","HOST_QT"}};
            return a::PermissionResponse{a::PermissionBehavior::Allow,{},args};
        };
        a::Engine engine(model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),config);
        const auto id=engine.createSession("fixture",root.path()).id;
        const auto result=engine.run({id,"Ask which renderer to use"}).result.get();
        QCOMPARE(result.status,a::RunStatus::Completed);QVERIFY2(result.text.contains("HOST_QT"),qPrintable(result.text));QCOMPARE(reviews,1);
        QVERIFY(!model->requests.last().messages.last().isError);
    }
};
QTEST_GUILESS_MAIN(UserQuestionsTests)
#include "user_questions_tests.moc"
