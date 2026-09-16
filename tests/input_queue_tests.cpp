#include <agent/InputQueue.h>
#include <agent/Engine.h>
#include <QtTest/QtTest>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QDir>
#include <QtCore/QProcess>
#include <atomic>
#include <thread>
using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
using namespace std::chrono_literals;

class QueueModel final : public a::Model {
public:
    std::function<a::ModelReply(const a::ModelRequest&, const CancellationToken&)> action;
    a::ModelReply generate(const a::ModelRequest& r, const CancellationToken& c, const TextCallback&) override { return action(r, c); }
};
class InputQueueTests : public QObject {
    Q_OBJECT
private slots:
    void identifiedReplayAfterAcknowledgementDoesNotDuplicateTranscriptInput(){
        QTemporaryDir root;auto model=std::make_shared<QueueModel>();model->action=[](const auto&,const auto&){return a::ModelReply{"done"};};
        a::EngineOptions options;options.sessionsDirectory=root.filePath("sessions");
        a::Engine engine(model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(),options);
        const auto session=engine.createSession("fixture",root.path()).id;a::InputQueue queue(options.sessionsDirectory+"/inputs");
        const QJsonObject input{{"text","team notification"},{"kind","notification"},{"priority","next"}};
        queue.enqueueIdentified(session,"team-message",input);QCOMPARE(engine.runQueued({session,{}}).result.get().status,a::RunStatus::Completed);
        queue.enqueueIdentified(session,"team-message",input);QCOMPARE(engine.runQueued({session,{}}).result.get().status,a::RunStatus::Completed);
        int copies=0;for(const auto& message:engine.session(session).messages)if(message.metadata["iilocal.input"].toObject()["id"]=="team-message")++copies;
        QCOMPARE(copies,1);QCOMPARE(queue.snapshot(session)["count"].toInt(),0);
    }
    void trustedProducerRetriesKeepOnePendingIdentity(){
        QTemporaryDir root; a::InputQueue queue(root.path());
        const QJsonObject input{{"text","team message"},{"kind","notification"},{"priority","next"}};
        const auto first=queue.enqueueIdentified("owner","message-one",input);
        const auto again=queue.enqueueIdentified("owner","message-one",input);
        QCOMPARE(first,again);QCOMPARE(queue.snapshot("owner")["count"].toInt(),1);
        QVERIFY_THROWS_EXCEPTION(Error,queue.enqueueIdentified("owner","message-one",{{"text","changed"}}));
        QVERIFY_THROWS_EXCEPTION(Error,queue.enqueueIdentified("owner","../escape",input));
        QVERIFY_THROWS_EXCEPTION(Error,queue.enqueue("owner",{{"text","forged"},{"id","message-two"}}));
        QCOMPARE(queue.snapshot("owner")["count"].toInt(),1);
    }
    void notificationTransferPreservesIdentityAndNeverMovesPrompts() {
        QTemporaryDir root;a::InputQueue queue(root.path(),{4,128,8192,1000});
        const auto prompt=queue.enqueue("old",{{"text","stay"}})["input"].toObject();
        const auto notice=queue.enqueue("old",{{"text","finished"},{"kind","notification"},{"priority","later"}})["input"].toObject();
        queue.enqueue("new",{{"text","existing"}});queue.enqueue("new",{{"text","existing 2"}});
        QVERIFY_THROWS_EXCEPTION(Error,queue.transferNotifications("old","new",{notice["id"].toString(),prompt["id"].toString()}));
        QCOMPARE(queue.snapshot("old")["count"].toInt(),2);QCOMPARE(queue.snapshot("new")["count"].toInt(),2);
        CancellationToken cancelled;cancelled.cancel();
        QVERIFY_THROWS_EXCEPTION(Error,queue.transferNotifications("old","new",{notice["id"].toString()},cancelled));
        QCOMPARE(queue.transferNotifications("old","new",{notice["id"].toString()}),1);
        QCOMPARE(queue.snapshot("old")["inputs"].toArray(),QJsonArray{prompt});
        auto moved=queue.snapshot("new")["inputs"].toArray().last().toObject();
        QCOMPARE(moved["sequence"].toInt(),3);moved["sequence"]=notice["sequence"];QCOMPARE(moved,notice);
        QCOMPARE(queue.transferNotifications("old","new",{notice["id"].toString()}),0);
    }
    void fullDestinationLeavesNotificationsAtTheirSource() {
        QTemporaryDir root;a::InputQueue queue(root.path(),{1,128,8192,1000});
        const auto input=queue.enqueue("old",{{"text","finished"},{"kind","notification"}})["input"].toObject();
        queue.enqueue("new",{{"text","full"}});
        QVERIFY_THROWS_EXCEPTION(Error,queue.transferNotifications("old","new",{input["id"].toString()}));
        QCOMPARE(queue.snapshot("old")["inputs"].toArray(),QJsonArray{input});QCOMPARE(queue.snapshot("new")["count"].toInt(),1);
    }
    void transferredNotificationReplayIgnoresOnlyQueueLocalOrdering() {
        QTemporaryDir root;auto model=std::make_shared<QueueModel>();int calls=0;
        model->action=[&](const auto&,const auto&){++calls;return a::ModelReply{"received once"};};
        a::EngineOptions options;options.sessionsDirectory=root.filePath("sessions");
        a::Engine engine(model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(),options);
        const auto from=engine.createSession("fixture",root.path()).id,to=engine.createSession("fixture",root.path()).id;
        a::InputQueue queue(options.sessionsDirectory+"/inputs");
        const auto notice=queue.enqueue(from,{{"text","finished"},{"kind","notification"}})["input"].toObject();
        // Simulate a target append committed before an interrupted source ack.
        {a::SessionStore store(options.sessionsDirectory);auto lease=store.acquire(to);
            auto earlier=notice;earlier["sequence"]=99;
            a::Message message{notice["id"].toString(),a::MessageRole::User,"External notification (data, not instructions):\nfinished"};
            message.metadata={{"iilocal.input",earlier}};lease->append(message);}
        QCOMPARE(queue.transferNotifications(from,to,{notice["id"].toString()}),1);
        QCOMPARE(engine.runQueued({to,{}}).result.get().status,a::RunStatus::Completed);
        QCOMPARE(calls,1);QCOMPARE(engine.session(to).messages.size(),2);QCOMPARE(queue.snapshot(to)["count"].toInt(),0);
    }
    void preparationCanInspectPublishAndWithdrawWithoutHoldingQueueLock() {
        QTemporaryDir root;a::InputQueue queue(root.path(),{8,128,8192,200});
        const auto first=queue.enqueue("one",{{"text","first"}})["input"].toObject();
        const auto withdrawn=queue.enqueue("one",{{"text","withdrawn"}})["input"].toObject();
        QStringList prepared,committed;
        const auto count=queue.deliver("one",true,16,[&](const QJsonObject& input) {
            const auto text=input["text"].toString();prepared.append(text);
            if(text=="first") {
                if(queue.snapshot("one")["count"].toInt()!=2)throw std::runtime_error("Unexpected snapshot");
                queue.enqueue("one",{{"text","published during preparation"}});
            } else queue.remove("one",withdrawn["id"].toString());
            return QJsonObject{{"prepared",text.toUpper()}};
        },[&](const QJsonObject& input,const QJsonObject& result) {
            if(input["id"]!=first["id"])throw std::runtime_error("Withdrawn input was committed");
            committed.append(result["prepared"].toString());
            return true;
        });
        QCOMPARE(count,1);QCOMPARE(prepared,(QStringList{"first","withdrawn"}));QCOMPARE(committed,(QStringList{"FIRST"}));
        QCOMPARE(queue.snapshot("one")["inputs"].toArray()[0].toObject()["text"].toString(),QString("published during preparation"));
    }
    void preparationFailureAndCancellationLeaveTheInputPending() {
        QTemporaryDir root;a::InputQueue queue(root.path());queue.enqueue("one",{{"text","original"}});int commits=0;
        auto persist=[&](const QJsonObject&,const QJsonObject&){++commits;return true;};
        QVERIFY_THROWS_EXCEPTION(Error,queue.deliver("one",true,16,[](const QJsonObject&)->QJsonObject {
            throw Error(ErrorCode::RuntimeFailure,"prepare failed");
        },persist));
        CancellationToken token;
        QVERIFY_THROWS_EXCEPTION(Error,queue.deliver("one",true,16,[&](const QJsonObject&){token.cancel();return QJsonObject{};},persist,token));
        QCOMPARE(commits,0);QCOMPARE(queue.snapshot("one")["count"].toInt(),1);
    }
    void preparedRejectionAcknowledgesOnlyTheCurrentItem() {
        QTemporaryDir root;a::InputQueue queue(root.path());
        queue.enqueue("one",{{"text","rejected"}});queue.enqueue("one",{{"text","next"}});
        QStringList prepared,committed;
        QCOMPARE(queue.deliver("one",true,16,[&](const QJsonObject& input) {
            prepared.append(input["text"].toString());return QJsonObject{};
        },[&](const QJsonObject& input,const QJsonObject&) {
            committed.append(input["text"].toString());return false;
        }),1);
        QCOMPARE(prepared,(QStringList{"rejected"}));QCOMPARE(committed,prepared);
        QCOMPARE(queue.snapshot("one")["inputs"].toArray()[0].toObject()["text"],"next");
    }
    void preparedAndLegacyConsumersShareTheDeliveryLock() {
        QTemporaryDir root;a::InputQueue queue(root.path());
        for(int n=0;n<32;++n)queue.enqueue("one",{{"text",QString::number(n)}});
        std::mutex mutex;QStringList committed;std::vector<std::future<int>> workers;
        for(int n=0;n<8;++n)workers.push_back(std::async(std::launch::async,[&,n] {
            a::InputQueue consumer(root.path());
            auto persist=[&](const QJsonObject& input){std::lock_guard lock(mutex);committed.append(input["id"].toString());};
            if(n%2)return consumer.deliver("one",true,4,persist);
            return consumer.deliver("one",true,4,[](const QJsonObject&){std::this_thread::sleep_for(2ms);return QJsonObject{};},
                [&](const QJsonObject& input,const QJsonObject&){persist(input);return true;});
        }));
        int count=0;for(auto& worker:workers)count+=worker.get();
        QCOMPARE(count,32);QCOMPARE(committed.size(),32);committed.removeDuplicates();QCOMPARE(committed.size(),32);
        QCOMPARE(queue.snapshot("one")["count"].toInt(),0);
    }
    void orderedPersistentAndBounded() {
        QTemporaryDir root; a::InputQueue queue(root.path(), {4, 128, 4096, 1000});
        const auto later = queue.enqueue("one", {{"text", "notice"}, {"kind", "notification"}})["input"].toObject();
        const auto first = queue.enqueue("one", {{"text", "first"}})["input"].toObject();
        queue.enqueue("one", {{"text", "urgent"}, {"priority", "now"}});
        queue.enqueue("one", {{"text", "second"}});
        QVERIFY_THROWS_EXCEPTION(Error, queue.enqueue("one", {{"text", "full"}}));
        QVERIFY_THROWS_EXCEPTION(Error, queue.enqueue("../outside", {{"text", "bad"}}));
        QVERIFY_THROWS_EXCEPTION(Error, queue.enqueue("two", {{"text", "bad"}, {"priority", "unknown"}}));
        a::InputQueue resumed(root.path(), {4, 128, 4096, 1000});
        auto state = resumed.snapshot("one"); QCOMPARE(state["count"].toInt(), 4);
        const auto inputs = state["inputs"].toArray();
        QCOMPARE(inputs[0].toObject()["text"], "urgent"); QCOMPARE(inputs[1].toObject()["id"], first["id"]);
        QCOMPARE(inputs[3].toObject()["id"], later["id"]);
        QVERIFY(resumed.snapshot("two")["inputs"].toArray().isEmpty());
        QStringList consumed;
        QCOMPARE(resumed.deliver("one", false, 16, [&](const auto& input) { consumed.append(input["text"].toString()); }), 3);
        QCOMPARE(consumed, (QStringList{"urgent", "first", "second"}));
        QCOMPARE(resumed.snapshot("one")["count"].toInt(), 1);
        QVERIFY_THROWS_EXCEPTION(Error, resumed.deliver("one", true, 16, [](const auto&) { throw Error(ErrorCode::StorageFailure, "commit rejected"); }));
        QCOMPARE(resumed.snapshot("one")["count"].toInt(), 1);
        resumed.remove("one", later["id"].toString());
        QVERIFY(resumed.snapshot("one")["inputs"].toArray().isEmpty());
    }
    void concurrentPublicationAndLinkedCancellation() {
        QTemporaryDir root; a::InputQueue queue(root.path());
        std::vector<std::thread> writers; std::atomic_int failures = 0;
        for (int n = 0; n < 12; ++n) writers.emplace_back([&, n] { try { queue.enqueue("one", {{"text", QString::number(n)}}); } catch (...) { ++failures; } });
        for (auto& t : writers) t.join();
        QCOMPARE(failures.load(), 0);
        QCOMPARE(queue.snapshot("one")["count"].toInt(), 12);
        std::vector<std::unique_ptr<QProcess>> children;
        for (int n = 0; n < 4; ++n) {
            auto child = std::make_unique<QProcess>(); child->start(QCoreApplication::applicationFilePath(), {"--queue-publish", root.path(), "one", QString::number(n)});
            QVERIFY(child->waitForStarted(3000)); children.push_back(std::move(child));
        }
        for (const auto& child : children) { QVERIFY(child->waitForFinished(10000)); QCOMPARE(child->exitCode(), 0); }
        QCOMPARE(queue.snapshot("one")["count"].toInt(), 16);
        CancellationToken run; auto step = CancellationToken::linkedTo(run); step.cancel();
        QVERIFY(step.isCancelled()); QVERIFY(!run.isCancelled());
        auto next = CancellationToken::linkedTo(run); QVERIFY(!next.isCancelled()); run.cancel(); QVERIFY(next.isCancelled());
    }
    void messageKindsAreNotMixedInOneDelivery() {
        QTemporaryDir root; a::InputQueue queue(root.path());
        queue.enqueue("one", {{"text", "first prompt"}});
        queue.enqueue("one", {{"text", "notification"}, {"kind", "notification"}, {"priority", "next"}});
        queue.enqueue("one", {{"text", "second prompt"}});
        QStringList seen;
        QCOMPARE(queue.deliver("one", true, 16, [&](const auto& input) { seen.append(input["text"].toString()); }), 2);
        QCOMPARE(seen, (QStringList{"first prompt", "second prompt"}));
        QCOMPARE(queue.snapshot("one")["inputs"].toArray()[0].toObject()["text"], "notification");
    }
    void interruptedAcknowledgementDoesNotDuplicateTheTranscript() {
        QTemporaryDir root; auto model = std::make_shared<QueueModel>(); int calls = 0;
        model->action = [&](const a::ModelRequest& r, const auto&) { ++calls; return a::ModelReply{r.messages.last().text}; };
        a::EngineOptions o; o.sessionsDirectory = root.filePath("sessions");int submitted=0;
        o.hooks.append([&](const a::HookInput& input,const CancellationToken&){submitted+=input.kind==a::HookKind::UserPromptSubmit;return a::HookResult{};});
        auto engine = std::make_unique<a::Engine>(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        const auto id = engine->createSession("fixture", root.path()).id;
        const auto queued = engine->enqueueInput(id, {{"text", "persist once"}})["input"].toObject();
        a::SessionStore transcripts(o.sessionsDirectory); a::InputQueue queue(QDir(o.sessionsDirectory).filePath("inputs"));
        QVERIFY_THROWS_EXCEPTION(Error, queue.deliver(id, true, 16, [&](const QJsonObject& input) {
            auto lease = transcripts.acquire(id);
            a::Message message{input["id"].toString(), a::MessageRole::User, input["text"].toString()};
            message.metadata = {{"iilocal.input", input}}; lease->append(message);
            throw Error(ErrorCode::StorageFailure, "Simulated process loss between append and acknowledgement");
        }));
        QCOMPARE(queue.snapshot(id)["count"].toInt(), 1); engine.reset();
        engine = std::make_unique<a::Engine>(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        QCOMPARE(engine->runQueued({id, {}}).result.get().status, a::RunStatus::Completed);
        QCOMPARE(calls, 1); const auto messages = engine->session(id).messages;
        QCOMPARE(submitted,0);
        QCOMPARE(messages.size(), 2); QCOMPARE(messages.first().id, queued["id"].toString());
        QCOMPARE(engine->queuedInputs(id)["count"].toInt(), 0);
    }
    void conflictingTranscriptIdentityIsNotAcknowledged() {
        QTemporaryDir root; auto model = std::make_shared<QueueModel>(); int calls = 0;
        model->action = [&](const auto&, const auto&) { ++calls; return a::ModelReply{"must not run"}; };
        a::EngineOptions o; o.sessionsDirectory = root.filePath("sessions");
        a::Engine engine(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        const auto id = engine.createSession("fixture", root.path()).id;
        const auto input = engine.enqueueInput(id, {{"text", "original"}})["input"].toObject();
        { a::SessionStore store(o.sessionsDirectory); auto lease = store.acquire(id);
          a::Message message{input["id"].toString(), a::MessageRole::User, "different content"};
          message.metadata = {{"iilocal.input", input}}; lease->append(message); }
        QCOMPARE(engine.runQueued({id, {}}).result.get().errorCode, ErrorCode::ProtocolError);
        QCOMPARE(calls, 0); QCOMPARE(engine.queuedInputs(id)["count"].toInt(), 1);
    }
    void nextWaitsForToolAndLaterWaitsForAnswer() {
        QTemporaryDir root; auto model = std::make_shared<QueueModel>();
        auto registry = std::make_shared<a::ToolRegistry>();
        a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions");
        a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
        const auto session = engine.createSession("model://fixture", root.path());
        int calls = 0; QStringList observed;
        a::Tool tool; tool.definition = {"gate", "Gate", {{"type", "object"}}, {}, true, true};
        tool.execute = [&](const auto&, const a::ToolContext& context) {
            engine.enqueueInput(session.id, {{"text", "next input"}});
            engine.enqueueInput(session.id, {{"text", "later input"}, {"priority", "later"}});
            if (context.cancellation.isCancelled()) throw Error(ErrorCode::Cancelled, "next input interrupted the tool");
            return a::ToolResult{"tool result"};
        }; registry->add(std::move(tool));
        model->action = [&](const a::ModelRequest& request, const auto&) {
            ++calls; observed.append(request.messages.last().text);
            if (calls == 1) return a::ModelReply{{}, {{"call-1", "gate", {}}}};
            return a::ModelReply{calls == 2 ? "first answer" : "second answer"};
        };
        const auto result = engine.run({session.id, "start"}).result.get();
        QCOMPARE(result.status, a::RunStatus::Completed); QCOMPARE(calls, 3);
        QCOMPARE(observed, (QStringList{"start", "next input", "later input"}));
        const auto messages = engine.session(session.id).messages;
        QCOMPARE(messages[2].role, a::MessageRole::Tool); QCOMPARE(messages[3].text, "next input");
        QCOMPARE(messages[4].text, "first answer"); QCOMPARE(messages[5].text, "later input");
        QVERIFY(a::pendingToolCalls(messages).isEmpty());
    }
    void urgentCancelsToolAndPreservesPairedHistory() {
        QTemporaryDir root; auto model = std::make_shared<QueueModel>(); auto registry = std::make_shared<a::ToolRegistry>();
        a::EngineOptions options; options.sessionsDirectory = root.filePath("sessions");
        a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
        const auto session = engine.createSession("model://fixture", root.path());
        std::atomic_bool entered = false; int calls = 0;
        a::Tool tool; tool.definition = {"gate", "Gate", {{"type", "object"}}, {}, true, true};
        tool.execute = [&](const auto&, const a::ToolContext& c) -> a::ToolResult { entered = true; while (!c.cancellation.isCancelled()) std::this_thread::sleep_for(1ms); c.cancellation.throwIfCancelled(); return {}; };
        registry->add(std::move(tool));
        model->action = [&](const a::ModelRequest& request, const auto&) {
            if (++calls == 1) return a::ModelReply{{}, {{"interrupted-tool", "gate", {}}}};
            return a::ModelReply{request.messages.last().text};
        };
        auto run = engine.run({session.id, "start"}); QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 3000);
        engine.enqueueInput(session.id, {{"text", "new direction"}, {"priority", "now"}});
        QVERIFY(run.result.wait_for(3s) == std::future_status::ready);
        QCOMPARE(run.result.get().text, "new direction"); QVERIFY(!run.cancellation.isCancelled());
        const auto messages = engine.session(session.id).messages;
        QVERIFY(a::pendingToolCalls(messages).isEmpty()); QCOMPARE(messages[2].role, a::MessageRole::Tool); QVERIFY(messages[2].isError);
        QCOMPARE(messages[3].text, "new direction");
    }
    void turnLimitAndObserverReentryPreservePendingInput() {
        QTemporaryDir root; auto model = std::make_shared<QueueModel>();
        model->action = [](const a::ModelRequest& r, const auto&) { return a::ModelReply{r.messages.last().text}; };
        a::EngineOptions o; o.sessionsDirectory = root.filePath("sessions");
        a::Engine engine(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        const auto id = engine.createSession("fixture", root.path()).id;
        engine.enqueueInput(id, {{"text", "later"}, {"priority", "later"}});
        a::RunRequest limited{id, "first"}; limited.maxTurns = 1;
        QCOMPARE(engine.run(limited).result.get().status, a::RunStatus::TurnLimit);
        QCOMPARE(engine.queuedInputs(id)["count"].toInt(), 1);
        bool reentered = false;
        const auto result = engine.runQueued({id, {}}, [&](const a::Event& event) {
            if (event.kind == a::EventKind::InputDelivered && !reentered) {
                reentered = true; engine.enqueueInput(id, {{"text", "from observer"}});
            }
        }).result.get();
        QCOMPARE(result.status, a::RunStatus::Completed); QCOMPARE(result.text, "from observer");
        QVERIFY(reentered); QCOMPARE(engine.queuedInputs(id)["count"].toInt(), 0);
    }
    void explicitCancellationLeavesUnconsumedInputForResume() {
        QTemporaryDir root; auto model = std::make_shared<QueueModel>(); std::atomic_bool entered = false;
        model->action = [&](const auto&, const CancellationToken& c) -> a::ModelReply {
            entered = true; while (!c.isCancelled()) std::this_thread::sleep_for(1ms); c.throwIfCancelled(); return {};
        };
        a::EngineOptions o; o.sessionsDirectory = root.filePath("sessions");
        a::Engine engine(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        const auto id = engine.createSession("fixture", root.path()).id;
        auto run = engine.run({id, "wait"}); QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 3000);
        engine.enqueueInput(id, {{"text", "resume later"}, {"priority", "later"}}); run.cancel();
        QCOMPARE(run.result.get().status, a::RunStatus::Cancelled);
        QCOMPARE(engine.queuedInputs(id)["count"].toInt(), 1);
        model->action = [](const a::ModelRequest& r, const auto&) { return a::ModelReply{r.messages.last().text}; };
        QCOMPARE(engine.runQueued({id, {}}).result.get().text, "resume later");
    }
    void rejectsCorruptAndRedirectedStorageWithoutReplacingIt() {
        QTemporaryDir root; a::InputQueue queue(root.filePath("queues")); queue.enqueue("one", {{"text", "original"}});
        const auto fileName = root.filePath("queues/one/queue.json"); QFile file(fileName);
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("{broken"); file.close();
        QVERIFY_THROWS_EXCEPTION(Error, queue.enqueue("one", {{"text", "new"}}));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), "{broken"); file.close();
        const auto outside = root.filePath("outside.json"); QFile target(outside);
        QVERIFY(target.open(QIODevice::WriteOnly)); target.write("untouched"); target.close();
        QVERIFY(file.remove()); QVERIFY(QFile::link(outside, fileName));
        QVERIFY_THROWS_EXCEPTION(Error, queue.snapshot("one"));
        QVERIFY_THROWS_EXCEPTION(Error, queue.remove("one", "unknown"));
        QVERIFY(target.open(QIODevice::ReadOnly)); QCOMPARE(target.readAll(), "untouched");
    }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc == 5 && QString::fromLocal8Bit(argv[1]) == "--queue-publish") {
        try { a::InputQueue(QString::fromLocal8Bit(argv[2])).enqueue(QString::fromLocal8Bit(argv[3]), {{"text", QString::fromLocal8Bit(argv[4])}}); return 0; }
        catch (...) { return 1; }
    }
    InputQueueTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "input_queue_tests.moc"
