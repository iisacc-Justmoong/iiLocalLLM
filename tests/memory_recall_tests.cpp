#include <agent/MemoryRecall.h>
#include <QtTest/QTest>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <atomic>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void put(const QString& path,const QByteArray& bytes) {QDir().mkpath(QFileInfo(path).absolutePath());QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())throw std::runtime_error("fixture write");}
class Model final:public a::Model {
public:
    std::function<a::ModelReply(const a::ModelRequest&,const CancellationToken&)> reply;
    int calls=0;a::ModelRequest request;
    a::ModelReply generate(const a::ModelRequest& value,const CancellationToken& token,const std::function<bool(const QString&)>&)override {
        ++calls;request=value;return reply?reply(value,token):a::ModelReply{"{\"selected_memories\":[\"selected.md\"]}",{},Usage{11,7}};
    }
};
struct Fixture {
    QTemporaryDir root;QString work=root.filePath("work");
    std::shared_ptr<a::ProjectMemory> memory;std::shared_ptr<Model> model=std::make_shared<Model>();
    Fixture() {QDir().mkpath(work);a::ProjectMemoryOptions o;o.directory=root.filePath("state");memory=std::make_shared<a::ProjectMemory>(o);memory->directory(work);}
    QString topic(const QString& name,const QByteArray& body) {auto path=QDir(memory->directory(work)).filePath(name);put(path,body);return path;}
    a::ToolContext context() {return {"session","run",work,root.filePath("artifacts"),{}};}
};
}
class MemoryRecallTests:public QObject {
    Q_OBJECT
private slots:
    void rankerSeesMetadataAndFiltersInventedPaths() {
        Fixture f;const auto one=f.topic("selected.md","---\nname: Build\ndescription: Packaging constraints\ntype: project\n---\nSECRET_TOPIC_BODY_748");
        f.topic("second.md","---\ndescription: Release checklist\n---\nOther content");f.topic("MEMORY.md","INDEX_IGNORED");
        f.topic("nested/MEMORY.md","---\ndescription: NESTED_INDEX_IGNORED\n---\nIndex");
        f.model->reply=[](const auto&,const auto&)->a::ModelReply{return {"{\"selected_memories\":[\"selected.md\",\"selected.md\",\"../escape.md\",\"MEMORY.md\",\"second.md\"]}",{},Usage{11,7}};};
        a::MemoryRecall recall(f.memory,f.model);auto result=recall.select(f.work,"fixture","Prepare the release package");
        QCOMPARE(result["status"].toString(),"completed");QCOMPARE(result["selected"].toArray().size(),2);
        const auto request=QJsonDocument(a::toJson(f.model->request.messages.first())).toJson();
        QVERIFY(request.contains("Packaging constraints"));QVERIFY(!request.contains("SECRET_TOPIC_BODY_748"));QVERIFY(!request.contains("INDEX_IGNORED"));
        QVERIFY(f.model->request.tools.isEmpty());QVERIFY(f.model->request.systemPromptOnly);QCOMPARE(f.model->request.toolChoice,"none");
        const auto paths=f.model->request.responseSchema["properties"].toObject()["selected_memories"].toObject()["items"].toObject()["enum"].toArray();
        QCOMPARE(paths.size(),2);QVERIFY(paths.contains("second.md"));QVERIFY(paths.contains("selected.md"));
        QCOMPARE(result["selected"].toArray().first().toObject()["absolute_path"].toString(),one);
        auto attached=recall.attach(result,f.context());QCOMPARE(attached.size(),2);QVERIFY(attached.first().text.contains("SECRET_TOPIC_BODY_748"));
        QCOMPARE(attached.first().role,a::MessageRole::User);QVERIFY(attached.first().metadata.contains("iilocal.memory_recall"));
    }
    void candidateEnumCountsTowardTheInputBudget() {
        Fixture f;
        for(int i=0;i<20;++i)f.topic(QString("topic-%1.md").arg(i,2,10,QChar('0')),
            "---\ndescription: A deployment reference with release and packaging instructions\n---\nBody");
        a::MemoryRecallOptions options;options.maxInputBytes=1600;
        f.model->reply=[](const auto&,const auto&)->a::ModelReply{return {"{\"selected_memories\":[]}",{}};};
        a::MemoryRecall recall(f.memory,f.model,options);auto result=recall.select(f.work,"fixture","Prepare the release");
        QCOMPARE(result["status"].toString(),"completed");QVERIFY(result["candidate_count"].toInt()>0);QVERIFY(result["candidate_count"].toInt()<20);
        const auto payload=QJsonDocument::fromJson(f.model->request.messages.first().text.toUtf8()).object();QJsonArray paths;
        for(const auto& value:payload["memories"].toArray())paths.append(value.toObject()["path"]);
        QCOMPARE(f.model->request.responseSchema["properties"].toObject()["selected_memories"].toObject()["items"].toObject()["enum"].toArray(),paths);
        const QJsonObject input{{"system",f.model->request.systemPrompt},{"input",payload},{"schema",f.model->request.responseSchema}};
        QVERIFY(QJsonDocument(input).toJson(QJsonDocument::Compact).size()<=options.maxInputBytes);
        QVERIFY(!result["diagnostics"].toArray().isEmpty());
    }
    void boundedSurfacingRecordsOnlyCompleteReadsAsEditable() {
        Fixture f;const auto path=f.topic("selected.md",QString(2000,QChar(0xAC00)).toUtf8());
        a::MemoryRecallOptions options;options.maxFileBytes=257;a::MemoryRecall recall(f.memory,f.model,options);
        auto result=recall.select(f.work,"fixture","Recall project details");auto context=f.context();
        auto messages=recall.attach(result,context);QCOMPARE(messages.size(),1);
        const auto metadata=messages.first().metadata["iilocal.memory_recall"].toObject();QVERIFY(metadata["truncated"].toBool());
        QCOMPARE(metadata["content_bytes"].toInt(),255);QVERIFY(!messages.first().text.contains(QChar::ReplacementCharacter));
        QVERIFY(messages.first().text.contains("Excerpt truncated: true"));
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,f.work);f.memory->bindWorkspaceTools(*registry);
        a::ToolRunner runner(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::AcceptEdits));
        QVERIFY(runner.run({"edit","Edit",{{"path",path},{"old_string",QString(1,QChar(0xAC00))},{"new_string","x"},{"replace_all",true}}},context).isError);
        put(path,"Complete old note");result=recall.select(f.work,"fixture","Recall project details");messages=recall.attach(result,context);QCOMPARE(messages.size(),1);
        QVERIFY(!runner.run({"edit2","Edit",{{"path",path},{"old_string","old"},{"new_string","new"}}},context).isError);
    }
    void staleSelectionAndFailedRankerDoNotInject() {
        Fixture f;const auto path=f.topic("selected.md","---\ndescription: Useful\n---\nOriginal");a::MemoryRecall recall(f.memory,f.model);
        auto result=recall.select(f.work,"fixture","Recall project context");put(path,"Changed during ranking");
        QVERIFY(recall.attach(result,f.context()).isEmpty());QVERIFY(!result["diagnostics"].toArray().isEmpty());
        f.model->reply=[](const auto&,const auto&)->a::ModelReply{return {"not json",{}};};
        QCOMPARE(recall.select(f.work,"fixture","Recall context")["status"].toString(),"failed");
        f.model->reply=[](const auto&,const auto& token)->a::ModelReply {while(!token.isCancelled())QThread::msleep(1);token.throwIfCancelled();return {};};
        a::MemoryRecallOptions options;options.timeoutMs=20;a::MemoryRecall timed(f.memory,f.model,options);
        QCOMPARE(timed.select(f.work,"fixture","Recall context")["status"].toString(),"timeout");
        CancellationToken cancelled;cancelled.cancel();QCOMPARE(timed.select(f.work,"fixture","Recall context",{},cancelled)["status"].toString(),"cancelled");
    }
    void deduplicationAndContextCapFollowVisibleMessages() {
        Fixture f;const auto path=f.topic("selected.md","Small note");a::MemoryRecall recall(f.memory,f.model);
        auto first=recall.select(f.work,"fixture","Recall useful context");auto messages=recall.attach(first,f.context());QCOMPARE(messages.size(),1);
        const auto calls=f.model->calls;auto duplicate=recall.select(f.work,"fixture","Recall useful context",messages);
        QCOMPARE(f.model->calls,calls);QVERIFY(duplicate["selected"].toArray().isEmpty());
        auto context=f.context();a::Message tool{{},a::MessageRole::Tool,"Read",{},"read",false,{{"path",path},{"sha256",first["selected"].toArray().first().toObject()["sha256"]},{"complete",true}}};
        QVERIFY(recall.attach(first,context,{tool}).isEmpty());
        a::MemoryRecallOptions options;options.maxContextBytes=1;a::MemoryRecall capped(f.memory,f.model,options);
        auto none=capped.select(f.work,"fixture","Recall useful context");QVERIFY(capped.attach(none,context).isEmpty());
        options.enabled=false;a::MemoryRecall disabled(f.memory,f.model,options);
        QCOMPARE(disabled.select(f.work,"fixture","Recall context")["status"].toString(),"disabled");
    }
    void successfulToolsExcludeErrorsAndOldConversationTurns() {
        QList<a::Message> messages{{"old",a::MessageRole::User,"Previous question"},
            {"a",a::MessageRole::Assistant,{},{{"r","Read",{}},{"w","Write",{}}}},
            {"tr",a::MessageRole::Tool,"ok",{},"r"},{"tw",a::MessageRole::Tool,"ok",{},"w"},
            {"now",a::MessageRole::User,"Current question"},{"b",a::MessageRole::Assistant,{},{{"r2","Read",{}}}},
            {"tr2",a::MessageRole::Tool,"failed",{},"r2",true}};
        QCOMPARE(a::MemoryRecall::recentSuccessfulTools(messages),QStringList{"Write"});
    }
};
QTEST_GUILESS_MAIN(MemoryRecallTests)
#include "memory_recall_tests.moc"
