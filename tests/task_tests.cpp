#include "native_link.h"
#include "agent/TaskStore.h"
#include "agent/Engine.h"
#include <iiFileProviderDevice.h>
#include <QtTest/QtTest>
#include <QtCore/QFile>
#include <QtCore/QLockFile>
#include <QtCore/QDir>
#include <QtCore/QProcess>
#include <QtCore/QJsonDocument>
#include <iostream>
#include <atomic>
#include <future>
#include <thread>
#include <set>
using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
using namespace std::chrono_literals;
namespace {
QJsonObject create(a::TaskStore& s, const QString& board, const QString& subject = "Build application") {
    return s.execute(board, "TaskCreate", {{"subject", subject}, {"description", "Build and verify the application"}}).data;
}
template<class F> void fails(F fn, ErrorCode code = ErrorCode::InvalidArgument) {
    try { fn(); QFAIL("Expected task error"); } catch (const Error& e) { QCOMPARE(e.code(), code); }
}
class Model final : public a::Model {
public:
    QList<a::ModelRequest> requests;
    QList<a::ModelReply> replies;
    a::ModelReply generate(const a::ModelRequest& request, const CancellationToken&, const TextCallback&) override {
        requests.append(request); if (replies.isEmpty()) return {"done", {}}; return replies.takeFirst();
    }
};
}
class TaskTests : public QObject {
    Q_OBJECT
private slots:
#ifdef Q_OS_WIN
    void nativeLockSharingWaitsWithoutLosingTaskState() {
        iiFileProvider::TemporaryDirectory root;
        a::TaskStore store(root.filePath("tasks")); create(store,"one");
        const auto path=root.filePath("tasks/one/board.lock").toStdWString();
        // Delete on close avoids a second close/delete race with QLockFile's
        // probes while guaranteeing the native handle denies lock acquisition.
        const auto handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,
            0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        QVERIFY(handle!=INVALID_HANDLE_VALUE);
        std::atomic_bool released=false;
        std::jthread release([&,handle] { std::this_thread::sleep_for(100ms); released=true; CloseHandle(handle); });
        try { create(store,"one","After deletion"); }
        catch(const Error& error) { QFAIL(error.what()); }
        QVERIFY(released.load());
        release.join();
        const auto board=store.snapshot("one");
        QCOMPARE(board["revision"].toInt(),2); QCOMPARE(board["tasks"].toArray().size(),2);
    }
    void nativeLockSharingCanBeCancelled() {
        iiFileProvider::TemporaryDirectory root;
        a::TaskStore store(root.filePath("tasks")); create(store,"one");
        const auto path=root.filePath("tasks/one/board.lock").toStdWString();
        const auto handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,
            0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        QVERIFY(handle!=INVALID_HANDLE_VALUE);
        CancellationToken token;
        std::jthread cancel([&] { std::this_thread::sleep_for(50ms); token.cancel(); });
        bool cancelled=false; QString observed="Lock acquisition completed before cancellation";
        try { store.snapshot("one",token); }
        catch(const Error& error) { cancelled=error.code()==ErrorCode::Cancelled; observed=QString::fromUtf8(error.what()); }
        cancel.join(); CloseHandle(handle); QVERIFY2(cancelled,qPrintable(observed));
        QCOMPARE(store.snapshot("one")["revision"].toInt(),1);
    }
    void atomicTaskPublicationWaitsForAReader() {
        iiFileProvider::TemporaryDirectory root;
        a::TaskStore store(root.filePath("tasks")); create(store,"one");
        const auto path=root.filePath("tasks/one/board.json").toStdWString();
        const auto handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        QVERIFY(handle!=INVALID_HANDLE_VALUE);
        std::jthread release([handle] { std::this_thread::sleep_for(100ms); CloseHandle(handle); });
        try { create(store,"one","After reader"); }
        catch(const Error& error) { QFAIL(error.what()); }
        release.join(); QCOMPARE(store.snapshot("one")["revision"].toInt(),2);
    }
    void persistentTaskSharingFailurePreservesTheOriginalBoard() {
        iiFileProvider::TemporaryDirectory root; a::TaskStoreOptions options; options.lockTimeoutMs=100;
        a::TaskStore store(root.filePath("tasks"),options); create(store,"one");
        const auto before=store.snapshot("one");
        const auto path=root.filePath("tasks/one/board.json").toStdWString();
        const auto handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        QVERIFY(handle!=INVALID_HANDLE_VALUE);
        bool rejected=false;
        try { create(store,"one","Must not be published"); }
        catch(const Error& error) { rejected=error.code()==ErrorCode::StorageFailure; }
        CloseHandle(handle); QVERIFY(rejected); QCOMPARE(store.snapshot("one"),before);
    }
    void taskPublicationCanBeCancelledWithoutChangingTheBoard() {
        iiFileProvider::TemporaryDirectory root;
        a::TaskStore store(root.filePath("tasks")); create(store,"one");
        const auto before=store.snapshot("one");
        const auto path=root.filePath("tasks/one/board.json").toStdWString();
        const auto handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        QVERIFY(handle!=INVALID_HANDLE_VALUE);
        CancellationToken token;
        std::jthread cancel([&] { std::this_thread::sleep_for(50ms); token.cancel(); });
        bool cancelled=false;
        try { store.execute("one","TaskCreate",{{"subject","Cancelled"},{"description","Must not publish"}},token); }
        catch(const Error& error) { cancelled=error.code()==ErrorCode::Cancelled; }
        cancel.join(); CloseHandle(handle); QVERIFY(cancelled); QCOMPARE(store.snapshot("one"),before);
    }
#endif
    void retiredNamespaceCannotBeRecreatedByStaleOwners() {
        iiFileProvider::TemporaryDirectory root;a::TaskStore first(root.filePath("tasks")),stale(root.filePath("tasks"));
        create(first,"team");first.retire("team");
        QVERIFY(!QFileInfo::exists(root.filePath("tasks/team/board.json")));
        fails([&]{stale.snapshot("team");},ErrorCode::NotFound);
        fails([&]{create(stale,"team");},ErrorCode::NotFound);
        first.retire("team");QVERIFY(stale.snapshot("new-team")["tasks"].toArray().isEmpty());
        QVERIFY(QFile::remove(root.filePath("tasks/team/retired.json")));
        QVERIFY(createNativeTestLink(root.filePath("absent"),root.filePath("tasks/team/retired.json")));
        fails([&]{stale.snapshot("team");},ErrorCode::StorageFailure);
    }
    void verificationCanReadTheBoardBeforePublication() {
        iiFileProvider::TemporaryDirectory root; a::TaskStoreOptions options; options.lockTimeoutMs=150;
        a::TaskStore store(root.filePath("tasks"),options);create(store,"s","EXISTING");
        bool inspected=false;
        try {
            const auto output=store.execute("s","TaskCreate",{{"subject","PENDING"},{"description","verify before commit"}}, {},
                [&](const a::TaskChange&,const CancellationToken& token) {
                    const auto list=store.execute("s","TaskList",{},token).data;
                    inspected=list["total"]==1&&list["tasks"].toArray().first().toObject()["subject"]=="EXISTING";
                });
            QCOMPARE(output.data["revision"].toInt(),2);
        } catch(const Error& error) {QFAIL(error.what());}
        QVERIFY(inspected);QCOMPARE(store.snapshot("s")["tasks"].toArray().size(),2);
    }
    void verificationSideEffectsCannotBeOverwrittenByAStaleCommit() {
        iiFileProvider::TemporaryDirectory root;a::TaskStoreOptions options;options.lockTimeoutMs=150;
        a::TaskStore store(root.filePath("tasks"),options);create(store,"s","EXISTING");int calls=0;
        fails([&] {store.execute("s","TaskCreate",{{"subject","STALE"},{"description","must not overwrite"}}, {},
            [&](const a::TaskChange&,const CancellationToken& token) {
                ++calls;store.execute("s","TaskUpdate",{{"taskId","1"},{"subject","VERIFIED_UPDATE"}},token);
            });},ErrorCode::AlreadyExists);
        QCOMPARE(calls,1);const auto board=store.snapshot("s");QCOMPARE(board["revision"].toInt(),2);
        QCOMPARE(board["tasks"].toArray().size(),1);QCOMPARE(board["tasks"].toArray().first().toObject()["subject"],"VERIFIED_UPDATE");
    }
    void persistentLifecycleAndDependencies() {
        iiFileProvider::TemporaryDirectory root; a::TaskStore store(root.filePath("tasks"));
        const auto one = create(store, "session"); QCOMPARE(one["task"].toObject()["id"], "1");
        QCOMPARE(one["revision"].toInt(), 1);
        create(store, "session", "Package application");
        store.execute("session", "TaskUpdate", {{"taskId", "2"}, {"addBlockedBy", QJsonArray{"1"}},
            {"metadata", QJsonObject{{"priority", 3}, {"nested", QJsonObject{{"value", true}}}}}});
        auto board = store.snapshot("session");
        QCOMPARE(board["tasks"].toArray()[0].toObject()["blocks"].toArray(), QJsonArray{"2"});
        fails([&] { store.execute("session", "TaskUpdate", {{"taskId", "1"}, {"addBlockedBy", QJsonArray{"2"}}}); });
        QCOMPARE(store.snapshot("session"), board);
        const auto blocked = store.execute("session", "TaskClaim", {{"taskId", "2"}, {"owner", "builder"}});
        QVERIFY(!blocked.data["success"].toBool()); QCOMPARE(blocked.data["reason"], "blocked");
        store.execute("session", "TaskUpdate", {{"taskId", "1"}, {"status", "completed"}});
        QVERIFY(store.execute("session", "TaskClaim", {{"taskId", "2"}, {"owner", "builder"}}).data["success"].toBool());
        auto listed = store.execute("session", "TaskList", {{"limit", 1}}).data;
        QCOMPARE(listed["tasks"].toArray().size(), 1); QCOMPARE(listed["nextOffset"].toInt(), 1);
        auto task = store.execute("session", "TaskGet", {{"taskId", "2"}}).data["task"].toObject();
        QCOMPARE(task["status"], "in_progress"); QCOMPARE(task["blockedBy"].toArray(), QJsonArray{"1"});
        QVERIFY(task["unresolvedBlockedBy"].toArray().isEmpty());
        store.execute("session", "TaskUpdate", {{"taskId", "2"}, {"metadata", QJsonObject{{"priority", QJsonValue::Null}}}});
        task = store.execute("session", "TaskGet", {{"taskId", "2"}}).data["task"].toObject();
        QVERIFY(!task["metadata"].toObject().contains("priority")); QVERIFY(task["metadata"].toObject().contains("nested"));
        store.execute("session", "TaskUpdate", {{"taskId", "1"}, {"status", "deleted"}});
        QVERIFY(store.execute("session", "TaskGet", {{"taskId", "2"}}).data["task"].toObject()["blockedBy"].toArray().isEmpty());
        a::TaskStore reopened(root.filePath("tasks")); QCOMPARE(reopened.snapshot("session"), store.snapshot("session"));
        QCOMPARE(create(reopened, "session")["task"].toObject()["id"], "3");
        QVERIFY(reopened.snapshot("another")["tasks"].toArray().isEmpty());
    }
    void validationTransactionsAndHooks() {
        iiFileProvider::TemporaryDirectory root; a::TaskStore store(root.filePath("tasks")); create(store, "a");
        auto before = store.snapshot("a");
        for (const auto& invalid : QList<QJsonObject>{
            {{"taskId", "1"}, {"status", "bogus"}},
            {{"taskId", "1"}, {"addBlocks", QJsonArray{"missing"}}},
            {{"taskId", "1"}, {"addBlocks", QJsonArray{"1"}}},
            {{"taskId", "1"}, {"owner", 3}},
            {{"taskId", "1"}, {"status", "deleted"}, {"subject", "Ambiguous deletion"}},
            {{"taskId", "1"}, {"expectedRevision", 0}}
        }) {
            QVERIFY_THROWS_EXCEPTION(Error, store.execute("a", "TaskUpdate", invalid));
            QCOMPARE(store.snapshot("a"), before);
        }
        fails([&] { create(store, "../escape"); });
        fails([&] { store.execute("a", "TaskList", {{"limit", 101}}); });
        fails([&] { store.execute("a", "TaskCreate", {{"subject", ""}, {"description", "x"}}); });
        int invoked = 0;
        a::TaskCommitCallback veto = [&](const a::TaskChange& change, const CancellationToken&) {
            ++invoked; QCOMPARE(change.after["status"], "completed");
            throw Error(ErrorCode::InvalidArgument, "Verification is missing");
        };
        fails([&] { store.execute("a", "TaskUpdate", {{"taskId", "1"}, {"status", "completed"}}, {}, veto); });
        QCOMPARE(invoked, 1); QCOMPARE(store.snapshot("a"), before);
        store.execute("a", "TaskUpdate", {{"taskId", "1"}, {"subject", "Build application"}});
        QCOMPARE(store.snapshot("a"), before); // A no-op preserves revision and persisted content.
        CancellationToken cancelled; cancelled.cancel();
        fails([&] { store.execute("a", "TaskCreate", {{"subject", "x"}, {"description", "x"}}, cancelled); }, ErrorCode::Cancelled);
        QCOMPARE(store.snapshot("a"), before);
    }
    void concurrentCreationAndAtomicClaim() {
        iiFileProvider::TemporaryDirectory root;
        a::TaskStoreOptions options; options.lockTimeoutMs = 15000;
        auto store = std::make_shared<a::TaskStore>(root.filePath("tasks"), options);
        std::vector<std::future<QString>> creates;
        for (int n = 0; n < 16; ++n) creates.emplace_back(std::async(std::launch::async, [&, n] {
            a::TaskStore independent(root.filePath("tasks"), options);
            return create(independent, "shared", QString::number(n))["task"].toObject()["id"].toString();
        }));
        std::set<QString> ids; QStringList failures;
        for (auto& result : creates) {
            try { ids.insert(result.get()); }
            catch (const std::exception& error) { failures.append(QString::fromUtf8(error.what())); }
        }
        QVERIFY2(failures.isEmpty(),qPrintable(failures.join('\n')));
        QCOMPARE(ids.size(), size_t(16)); QCOMPARE(store->snapshot("shared")["revision"].toInt(), 16);
        std::vector<std::future<bool>> claims;
        for (int n = 0; n < 12; ++n) claims.emplace_back(std::async(std::launch::async, [&, n] {
            return store->execute("shared", "TaskClaim", {{"taskId", "1"}, {"owner", "agent-" + QString::number(n)}}).data["success"].toBool();
        }));
        int won = 0;
        for (auto& result : claims) {
            try { if (result.get()) ++won; }
            catch (const std::exception& error) { failures.append(QString::fromUtf8(error.what())); }
        }
        QVERIFY2(failures.isEmpty(),qPrintable(failures.join('\n'))); QCOMPARE(won, 1);
        const auto owner = store->execute("shared", "TaskGet", {{"taskId", "1"}}).data["task"].toObject()["owner"].toString();
        const auto busy = store->execute("shared", "TaskClaim", {{"taskId", "2"}, {"owner", owner}, {"checkOwnerBusy", true}}).data;
        QCOMPARE(busy["reason"], "owner_busy"); QVERIFY(!busy["success"].toBool());
    }
    void unicodeOwnerSurvivesPersistentReload() {
        iiFileProvider::TemporaryDirectory root; a::TaskStore store(root.filePath("tasks")); create(store, "a");
        const auto owner = QString::fromUtf8("\xf0\x9f\x9b\xa0").repeated(128);
        store.execute("a", "TaskClaim", {{"taskId", "1"}, {"owner", owner}});
        a::TaskStore reopened(root.filePath("tasks"));
        QCOMPARE(reopened.execute("a", "TaskGet", {{"taskId", "1"}}).data["task"].toObject()["owner"].toString(), owner);
        const auto before = reopened.snapshot("a");
        fails([&] { reopened.execute("a", "TaskUpdate", {{"taskId", "1"}, {"owner", owner + "x"}}); });
        QCOMPARE(reopened.snapshot("a"), before);
    }
    void todosIsolationAndCorruptStorage() {
        iiFileProvider::TemporaryDirectory root; a::TaskStore store(root.filePath("tasks"));
        const QJsonArray todos{QJsonObject{{"content", "Verify build"}, {"activeForm", "Verifying build"}, {"status", "in_progress"}}};
        QCOMPARE(store.execute("a", "TodoWrite", {{"todos", todos}}).data["newTodos"].toArray(), todos);
        QCOMPARE(store.execute("a", "TodoRead", {}).data["todos"].toArray(), todos);
        QVERIFY(store.execute("b", "TodoRead", {}).data["todos"].toArray().isEmpty());
        auto done = todos[0].toObject(); done["status"] = "completed";
        store.execute("a", "TodoWrite", {{"todos", QJsonArray{done}}});
        QCOMPARE(store.execute("a", "TodoRead", {}).data["todos"].toArray(), QJsonArray{done});
        // Completed evidence remains visible until an explicit empty replacement.
        fails([&] { store.execute("a", "TodoWrite", {{"todos", QJsonArray{QJsonObject{{"content", ""}}}}}); });
        QFile file(root.filePath("tasks/a/board.json")); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{broken"); file.close();
        fails([&] { create(store, "a"); }, ErrorCode::ProtocolError);
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("{broken"));
    }
    void processesCoordinateAndWaitingCanBeCancelled() {
        iiFileProvider::TemporaryDirectory root; a::TaskStore store(root.filePath("tasks"));
        std::vector<std::unique_ptr<QProcess>> children;
        for (int n = 0; n < 8; ++n) {
            auto process = std::make_unique<QProcess>();
            process->start(QCoreApplication::applicationFilePath(), {"--task-worker", root.filePath("tasks"), "shared", "create", QString::number(n)});
            QVERIFY(process->waitForStarted(5000)); children.push_back(std::move(process));
        }
        for (auto& child : children) {
            QVERIFY(child->state()==QProcess::NotRunning || child->waitForFinished(15000));
            const auto diagnostics=child->readAllStandardError();
            QVERIFY2(child->exitStatus()==QProcess::NormalExit && child->exitCode()==0,diagnostics.constData());
            QJsonParseError error;
            const auto output=child->readAllStandardOutput();
            const auto document=QJsonDocument::fromJson(output,&error);
            QVERIFY2(error.error==QJsonParseError::NoError && document.isObject(),output.constData());
            QVERIFY(document.object()["task"].isObject());
        }
        QCOMPARE(store.snapshot("shared")["tasks"].toArray().size(), 8);
        for (int n = 0; n < 8; ++n)
            children[n]->start(QCoreApplication::applicationFilePath(), {"--task-worker", root.filePath("tasks"), "shared", "claim", QString::number(n)});
        int wins = 0;
        for (auto& child : children) {
            QVERIFY(child->state()==QProcess::NotRunning || child->waitForFinished(15000));
            const auto diagnostics=child->readAllStandardError();
            QVERIFY2(child->exitStatus()==QProcess::NormalExit && child->exitCode()==0,diagnostics.constData());
            QJsonParseError error;
            const auto output=child->readAllStandardOutput();
            const auto document=QJsonDocument::fromJson(output,&error);
            QVERIFY2(error.error==QJsonParseError::NoError && document.isObject(),output.constData());
            QVERIFY(document.object()["success"].isBool());
            if (document.object()["success"].toBool()) ++wins;
        }
        QCOMPARE(wins, 1);
        QLockFile lock(root.filePath("tasks/shared/board.lock")); lock.setStaleLockTime(0); QVERIFY(lock.tryLock(0));
        CancellationToken token;
        auto waiting = std::async(std::launch::async, [&] {
            try { store.snapshot("shared", token); return ErrorCode::None; } catch (const Error& e) { return e.code(); }
        });
        std::this_thread::sleep_for(50ms); token.cancel();
        QVERIFY(waiting.wait_for(1s) == std::future_status::ready); QCOMPARE(waiting.get(), ErrorCode::Cancelled);
        lock.unlock();
#ifdef Q_OS_UNIX
        QVERIFY(QFile::rename(root.filePath("tasks/shared/board.json"), root.filePath("saved.json")));
        QVERIFY(createNativeTestLink(root.filePath("saved.json"), root.filePath("tasks/shared/board.json")));
        fails([&] { store.snapshot("shared"); }, ErrorCode::StorageFailure);
#endif
    }
    void modelToolsResumeContextPolicyAndFork() {
        iiFileProvider::TemporaryDirectory root; a::EngineOptions options{.decision={.enabled=false}}; options.sessionsDirectory = root.filePath("sessions");
        options.taskToolsEnabled = true; options.projectContext.enabled = false; options.compaction.automatic = false;
        int createdHooks = 0; bool rejectCompletion = false;
        options.hooks.append([&](const a::HookInput& input, const CancellationToken&) {
            if (input.kind == a::HookKind::TaskCreated) ++createdHooks;
            return a::HookResult{input.kind == a::HookKind::TaskCompleted && rejectCompletion, {}};
        });
        auto model = std::make_shared<Model>(); auto registry = std::make_shared<a::ToolRegistry>();
        model->replies = {{{}, {{"find", "ToolSearch", {{"query", "select:TaskCreate"}}}}},
            {{}, {{"create", "TaskCreate", {{"subject", "Verify"}, {"description", "Run tests"}}}}}, {"done", {}}};
        QString id;
        {
            a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Plan), options);
            id = engine.createSession("fixture", root.path()).id;
            QCOMPARE(engine.run({id, "Create a verification task"}).result.get().status, a::RunStatus::Completed);
            QCOMPARE(engine.runTaskTool(id, "TaskList").data["tasks"].toArray().size(), 1);
            QCOMPARE(createdHooks, 1); rejectCompletion = true;
            QVERIFY(engine.runTaskTool(id, "TaskUpdate", {{"taskId", "1"}, {"status", "completed"}}).isError);
            QCOMPARE(engine.runTaskTool(id, "TaskGet", {{"taskId", "1"}}).data["task"].toObject()["status"], "pending");
            QCOMPARE(model->requests.size(), 3);
            QVERIFY(std::any_of(model->requests[2].messages.begin(), model->requests[2].messages.end(),
                [](const auto& m) { return m.metadata.contains("iilocal.task_state"); }));
            const auto fork = engine.forkSession(id); QVERIFY(engine.runTaskTool(fork.id, "TaskList").data["tasks"].toArray().isEmpty());
            QCOMPARE(engine.run({fork.id, "Continue with this conversation's task list"}).result.get().status, a::RunStatus::Completed);
            const auto& messages = model->requests.last().messages;
            QVERIFY(std::any_of(messages.begin(), messages.end(), [](const auto& m) {
                return m.metadata.contains("iilocal.task_state") && m.text.contains("\"taskCount\":0");
            })); // A fork must not mistake copied parent tool results for its live task state.
        }
        a::Engine resumed(model, registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Default,
            QList<a::PermissionRule>{{"TaskUpdate", a::PermissionBehavior::Deny}}), options);
        QVERIFY(resumed.runTaskTool(id, "TaskUpdate", {{"taskId", "1"}, {"status", "completed"}}).isError);
        QCOMPARE(resumed.runTaskTool(id, "TaskGet", {{"taskId", "1"}}).data["task"].toObject()["status"], "pending");
        fails([&] { resumed.runTaskTool("00000000-0000-0000-0000-000000000000", "TaskList"); }, ErrorCode::NotFound);
    }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); const auto args = app.arguments();
    if (args.size() == 6 && args[1] == "--task-worker") {
        try {
            a::TaskStore store(args[2]);
            auto value = args[4] == "create" ? create(store, args[3], args[5])
                : store.execute(args[3], "TaskClaim", {{"taskId", "1"}, {"owner", args[5]}}).data;
            std::cout << QJsonDocument(value).toJson(QJsonDocument::Compact).constData() << std::endl;
            if (!std::cout) { std::cerr << "Cannot flush task worker response" << std::endl; return 2; }
            return 0;
        } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    }
    TaskTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "task_tests.moc"
