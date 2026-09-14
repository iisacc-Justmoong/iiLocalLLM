#include "agent/ShellTasks.h"
#include <QtTest/QtTest>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <future>
#include <thread>
using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
using namespace std::chrono_literals;
namespace {
struct Fixture {
    QTemporaryDir root;
    std::shared_ptr<a::ShellTasks> tasks;
    std::shared_ptr<a::ToolRegistry> registry = std::make_shared<a::ToolRegistry>();
    a::ToolContext context;
    explicit Fixture(a::ShellTaskOptions options = {}, bool nested = false) {
        context.sessionId = "session-a"; context.runId = "run-a";
        context.workingDirectory = root.filePath("workspace"); QDir().mkpath(context.workingDirectory);
        tasks = std::make_shared<a::ShellTasks>(context.workingDirectory, root.filePath(nested ? "workspace/.iilocal-llm/shells" : "state"), options);
        a::registerWorkspaceTools(*registry, context.workingDirectory, tasks);
    }
    a::ToolResult call(QString name, QJsonObject args, a::ToolContext c = {}) {
        if (c.sessionId.isEmpty()) c = context;
        a::ToolRunner runner(registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass));
        return runner.run({"call", name, args}, c);
    }
    QString start(QString command, int timeout = 30000) {
        auto r = call("Bash", {{"command", command}, {"run_in_background", true}, {"timeout_ms", timeout}});
        if (r.isError) throw std::runtime_error(r.text.toStdString());
        return r.data["backgroundTaskId"].toString();
    }
    QJsonObject output(QString id, bool block = true, int timeout = 5000) {
        auto r = call("TaskOutput", {{"task_id", id}, {"block", block}, {"timeout", timeout}});
        if (r.isError) throw std::runtime_error(r.text.toStdString());
        return r.data;
    }
};
QByteArray read(const QString& path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
bool executing(const QByteArray& pid) {
    if (pid.toLongLong() < 2) throw std::runtime_error("Invalid process fixture PID");
    QProcess process; process.start("/bin/ps", {"-o", "stat=", "-p", QString::fromLatin1(pid)});
    if (!process.waitForFinished(1000)) throw std::runtime_error("Cannot inspect fixture process");
    const auto state = process.readAllStandardOutput().trimmed(); return !state.isEmpty() && !state.startsWith('Z');
}
}
class ShellTaskTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
#if !defined(Q_OS_UNIX) || defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
        QSKIP("Background shell execution requires a desktop POSIX host");
#endif
    }
    void lifecycleOutputAndPersistentHistory() {
        Fixture f; const auto id = f.start("printf 'native-'; sleep 0.15; printf 'background'; printf 'error-stream' >&2");
        QVERIFY(id.startsWith("sh-"));
        const auto done = f.output(id); QCOMPARE(done["retrieval_status"], "success");
        const auto task = done["task"].toObject(); QCOMPARE(task["status"], "completed"); QCOMPARE(task["exitCode"].toInt(-1), 0);
        QVERIFY(task["output"].toString().contains("native-background")); QVERIFY(task["output"].toString().contains("error-stream"));
        const auto path = task["output_file"].toString(); QVERIFY(QFileInfo(path).isFile());
        QVERIFY(!f.call("Read", {{"path", path}}).isError);
        auto other = f.context; other.sessionId = "session-b";
        QVERIFY(f.call("TaskOutput", {{"task_id", id}}, other).isError);
        QVERIFY(f.call("Read", {{"path", path}}, other).isError);
        QVERIFY(f.call("TaskStop", {{"task_id", id}}).isError);
        f.tasks->close();
        a::ShellTasks reopened(f.context.workingDirectory, f.root.filePath("state"));
        const auto recovered = reopened.output(f.context.sessionId, id, false, 0, 0, 65536);
        QCOMPARE(recovered["task"].toObject()["output"], task["output"]);
        QCOMPARE(reopened.list(f.context.sessionId).size(), 1); QVERIFY(reopened.list("session-b").isEmpty());
    }
    void waitCancellationDoesNotStopExecution() {
        Fixture f; const auto id = f.start("printf started; sleep 30");
        QCOMPARE(f.output(id, false)["retrieval_status"], "not_ready");
        QCOMPARE(f.output(id, true, 20)["retrieval_status"], "timeout");
        auto context = f.context; context.cancellation = CancellationToken{};
        auto waiting = std::async(std::launch::async, [&] { return f.call("TaskOutput", {{"task_id", id}, {"timeout", 30000}}, context); });
        context.cancellation.cancel();
        try { auto response = waiting.get(); QVERIFY(response.isError); }
        catch (const Error& error) { QCOMPARE(error.code(), ErrorCode::Cancelled); }
        QCOMPARE(f.output(id, false)["task"].toObject()["status"], "running");
        const auto stopped = f.call("TaskStop", {{"task_id", id}}); QVERIFY2(!stopped.isError, qPrintable(stopped.text));
        QCOMPARE(f.output(id)["task"].toObject()["status"], "killed");
    }
    void stopsProcessGroupAndShutdownJoins() {
        Fixture f;
        const auto id = f.start("printf '%s' \"$$\" > shell.pid; sleep 30 & printf '%s' \"$!\" > child.pid; wait; printf late > late.txt");
        QTRY_VERIFY_WITH_TIMEOUT(read(f.root.filePath("workspace/child.pid")).toLongLong() > 1, 3000);
        const auto parent = read(f.root.filePath("workspace/shell.pid")), child = read(f.root.filePath("workspace/child.pid"));
        QVERIFY(executing(parent)); QVERIFY(executing(child));
        QVERIFY(!f.call("TaskStop", {{"shell_id", id}}).isError);
        QTRY_VERIFY_WITH_TIMEOUT(!executing(parent) && !executing(child), 3000);
        QCOMPARE(f.output(id)["task"].toObject()["status"], "killed");
        QVERIFY(!QFileInfo::exists(f.root.filePath("workspace/late.txt")));
        const auto second = f.start("sleep 30; printf leaked > leaked.txt");
        f.tasks->close(); QCOMPARE(f.tasks->output(f.context.sessionId, second, false, 0, 0, 1024)["task"].toObject()["status"], "killed");
        QVERIFY(!QFileInfo::exists(f.root.filePath("workspace/leaked.txt")));
        QVERIFY(f.call("Bash", {{"command", "printf bad"}, {"run_in_background", true}}).isError);
    }
    void preservesOutputWrittenDuringTermination() {
        Fixture f;
        const auto id = f.start("trap 'printf stopped-log; exit 0' TERM; printf ready > trap.ready; while :; do sleep 30 & wait $!; done");
        QTRY_COMPARE_WITH_TIMEOUT(read(f.root.filePath("workspace/trap.ready")), QByteArray("ready"), 3000);
        QVERIFY(!f.call("TaskStop", {{"task_id", id}}).isError);
        const auto task = f.output(id)["task"].toObject();
        QCOMPARE(task["status"], "killed");
        QVERIFY2(task["output"].toString().contains("stopped-log"), qPrintable(task["output"].toString()));
        QCOMPARE(read(task["output_file"].toString()), QByteArray::fromBase64(task["output_base64"].toString().toLatin1()));
    }
    void timeoutCapacityAndOutputLimit() {
        a::ShellTaskOptions options; options.maxConcurrent = 1; options.maxOutputBytes = 1024;
        Fixture f(options); auto id = f.start("sleep 30");
        QVERIFY(f.call("Bash", {{"command", "printf should-not-start"}, {"run_in_background", true}}).isError);
        QVERIFY(!f.call("TaskStop", {{"task_id", id}}).isError);
        id = f.start("sleep 30", 100);
        auto task = f.output(id)["task"].toObject(); QCOMPARE(task["status"], "failed"); QCOMPARE(task["error_code"], "timeout");
        id = f.start("while :; do printf 0123456789012345678901234567890123456789; done");
        task = f.output(id)["task"].toObject(); QCOMPARE(task["error_code"], "resource_limit");
        QVERIFY(read(task["output_file"].toString()).size() <= 1024);
        id = f.start("printf failure >&2; exit 7"); task = f.output(id)["task"].toObject();
        QCOMPARE(task["status"], "failed"); QCOMPARE(task["exitCode"].toInt(-1), 7);
    }
    void permissionsAndInputValidationPrecedeStart() {
        Fixture f;
        a::ToolRunner denied(f.registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Plan));
        auto r = denied.run({"x", "Bash", {{"command", "printf forbidden > forbidden.txt"}, {"run_in_background", true}}}, f.context);
        QVERIFY(r.isError); QVERIFY(f.tasks->list(f.context.sessionId).isEmpty());
        QVERIFY(!QFileInfo::exists(f.root.filePath("workspace/forbidden.txt")));
        QVERIFY(f.call("Bash", {{"command", "echo bad"}, {"run_in_background", "true"}}).isError);
        QVERIFY(f.call("TaskOutput", {{"task_id", "../../etc/passwd"}}).isError);
        QVERIFY(f.call("TaskStop", {}).isError);
        QVERIFY(f.tasks->list(f.context.sessionId).isEmpty());
    }
    void crashDoesNotExposeAnUndefinedExitCode() {
        Fixture f; const auto id = f.start("kill -KILL $$");
        const auto task = f.output(id)["task"].toObject();
        QCOMPARE(task["status"], "failed"); QCOMPARE(task["error_code"], "runtime_failure");
        QVERIFY(task["exitCode"].isNull());
    }
    void nestedStateProtectionAndBytePaging() {
        Fixture f({}, true); const auto id = f.start("printf 'private_marker_123456789'");
        const auto task = f.output(id)["task"].toObject(); const auto output = task["output_file"].toString();
        const auto state = QFileInfo(output).dir().filePath("state.json");
        QVERIFY(f.call("Read", {{"path", state}}).isError);
        QVERIFY(f.call("Grep", {{"pattern", "private_marker"}}).data["matches"].toArray().isEmpty());
        auto other = f.context; other.sessionId = "another";
        QVERIFY(f.call("Read", {{"path", output}}, other).isError);
        const auto page = f.tasks->output(f.context.sessionId, id, false, 0, 8, 6)["task"].toObject();
        QCOMPARE(page["output"], "marker"); QCOMPARE(page["next_offset"].toInt(), 14); QVERIFY(page["has_more"].toBool());
    }
    void recoveryDoesNotReplayAndRejectsCorruptState() {
        Fixture f; const auto id = f.start("printf once >> invocation-count; printf saved-output");
        auto task = f.output(id)["task"].toObject(); f.tasks->close();
        const auto path = QFileInfo(task["output_file"].toString()).dir().filePath("state.json");
        auto metadata = QJsonDocument::fromJson(read(path)).object(); metadata["status"] = "running";
        metadata["finished_at"] = QJsonValue::Null; metadata["exitCode"] = QJsonValue::Null;
        auto write = [&] { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write(QJsonDocument(metadata).toJson()); };
        write();
        { a::ShellTasks recovered(f.context.workingDirectory, f.root.filePath("state"));
          task = recovered.output(f.context.sessionId, id, false, 0, 0, 1024)["task"].toObject();
          QCOMPARE(task["status"], "interrupted"); QCOMPARE(task["error_code"], "host_interrupted");
          QCOMPARE(task["output"], "saved-output"); }
        QCOMPARE(read(f.root.filePath("workspace/invocation-count")), QByteArray("once"));
        metadata = QJsonDocument::fromJson(read(path)).object(); metadata["exitCode"] = "not-an-integer"; write();
        QVERIFY_THROWS_EXCEPTION(Error, a::ShellTasks(f.context.workingDirectory, f.root.filePath("state")));
    }
};
QTEST_GUILESS_MAIN(ShellTaskTests)
#include "shell_tasks_tests.moc"
