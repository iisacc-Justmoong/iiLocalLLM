#include <QtTest/QtTest>
#include "agent/ProjectContext.h"
#include "agent/Engine.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QCryptographicHash>
using namespace iiLocalLLM;
namespace a = iiLocalLLM::agent;
namespace {
void put(const QString& path, const QByteArray& value) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) qFatal("mkdir failed");
    QFile file(path); if (!file.open(QIODevice::WriteOnly) || file.write(value) != value.size()) qFatal("write failed");
}
QStringList names(const a::ProjectContext& context) {
    QStringList result; for (const auto& file : context.files) result.append(QFileInfo(file.path).fileName()); return result;
}
class ObservingModel final : public a::Model {
public:
    QList<a::ModelRequest> requests;
    QString changedFile;
    a::ModelReply generate(const a::ModelRequest& request, const CancellationToken& token, const TextCallback&) override {
        token.throwIfCancelled(); requests.append(request);
        if (requests.size() == 1) {
            put(changedFile, "Changed root instructions");
            return {{}, {{"read-source", "Read", {{"path", "src/code.cpp"}}}}};
        }
        return {"done", {}};
    }
};
}
class ContextTests : public QObject {
    Q_OBJECT
private slots:
    void utf8BomAndWindowsNewlinesKeepRuleScope() {
        QTemporaryDir dir;
        const auto bytes = QByteArray::fromHex("efbbbf") + QStringLiteral("---\r\npaths: 'src/*.cpp'\r\n---\r\n한국어 규칙 @../../지침.md\r\n").toUtf8();
        put(dir.filePath(".claude/rules/windows.md"), bytes);
        put(dir.filePath(QStringLiteral("지침.md")), QStringLiteral("추가 지침").toUtf8());
        QVERIFY(a::loadProjectContext(dir.path()).files.isEmpty());
        const auto context = a::loadProjectContext(dir.path(), {"src/code.cpp"});
        QCOMPARE(context.files.size(), 2);
        QVERIFY(context.message().text.contains(QStringLiteral("추가 지침")));
        QCOMPARE(context.files.first().sha256, QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
        QVERIFY(context.files.first().transformed);
    }
    void markdownBoundariesAndEscapedPaths() {
        QTemporaryDir dir;
        put(dir.filePath("CLAUDE.md"), "# Heading @./heading.md\n\n- List @./space\\ name.md#section\n\n<!-- hidden @./hidden.md --> @./residue.md\n\nInline <!-- keep this --> comment.\n\n    @./code.md\n\n<!-- unclosed @./hidden.md\n");
        for (const auto& name : {"heading.md", "space name.md", "hidden.md", "residue.md", "code.md"}) put(dir.filePath(name), name);
        const auto context = a::loadProjectContext(dir.path());
        QCOMPARE(names(context), (QStringList{"CLAUDE.md", "heading.md", "space name.md", "residue.md"}));
        QVERIFY(context.files.first().content.contains("Inline <!-- keep this --> comment."));
        QVERIFY(context.files.first().content.contains("<!-- unclosed"));
        QVERIFY(!context.files.first().content.contains("<!-- hidden"));
        QVERIFY(context.files.first().content.contains("    @./code.md"));
    }
    void excludesCyclesAndResourceLimits() {
        QTemporaryDir dir;
        put(dir.filePath("CLAUDE.md"), "@./one.md\n@./two.md");
        put(dir.filePath("one.md"), "one @./CLAUDE.md"); put(dir.filePath("two.md"), "two");
        a::ProjectContextOptions options; options.excludes = {"two.md"};
        QCOMPARE(names(a::loadProjectContext(dir.path(), {}, options)), (QStringList{"CLAUDE.md", "one.md"}));
        options.maxFiles = 1;
        QVERIFY_THROWS_EXCEPTION(Error, a::loadProjectContext(dir.path(), {}, options));
        options = {}; options.maxImportDepth = 1;
        QVERIFY_THROWS_EXCEPTION(Error, a::loadProjectContext(dir.path(), {}, options));
        options = {}; options.maxTotalBytes = 25;
        QVERIFY_THROWS_EXCEPTION(Error, a::loadProjectContext(dir.path(), {}, options));
        put(dir.filePath("CLAUDE.md"), "Root");
        put(dir.filePath(".claude/rules/a.md"), "A"); put(dir.filePath(".claude/rules/b.md"), "B");
        options = {}; options.maxScannedEntries = 1;
        QVERIFY_THROWS_EXCEPTION(Error, a::loadProjectContext(dir.path(), {}, options));
        put(dir.filePath(".claude/rules/a.md"), "---\npaths: '{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}{a,b}'\n---\nA");
        QVERIFY_THROWS_EXCEPTION(Error, a::loadProjectContext(dir.path()));
    }
    void internalSymlinkDedupAndInvalidRunDoesNotPersist() {
        QTemporaryDir dir;
        put(dir.filePath("CLAUDE.md"), "@./alias.md\n@./import.md"); put(dir.filePath("import.md"), "Shared");
        QVERIFY(QFile::link(dir.filePath("import.md"), dir.filePath("alias.md")));
        QCOMPARE(names(a::loadProjectContext(dir.path())), (QStringList{"CLAUDE.md", "import.md"}));
        auto model = std::make_shared<ObservingModel>(); auto registry = std::make_shared<a::ToolRegistry>();
        a::EngineOptions options; options.sessionsDirectory = dir.filePath("sessions");
        a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
        const auto session = engine.createSession("fixture", dir.path());
        a::RunRequest request{session.id, "bad context"}; request.contextPaths = {"../outside"};
        QCOMPARE(engine.run(request).result.get().status, a::RunStatus::Failed);
        QVERIFY(engine.session(session.id).messages.isEmpty()); QVERIFY(model->requests.isEmpty());
        a::SessionStore store(options.sessionsDirectory);
        { auto lease = store.acquire(session.id); a::Message message{{}, a::MessageRole::User, "scoped input"};
          message.metadata = {{"iilocal.context_paths", QJsonArray{"sub/new.cpp"}}}; lease->append(message); }
        put(dir.filePath("sub/AGENTS.md"), "Persisted nested scope");
        QVERIFY(engine.context(session.id).message().text.contains("Persisted nested scope"));
        const auto fork = engine.forkSession(session.id);
        QVERIFY(engine.context(fork.id).message().text.contains("Persisted nested scope"));
    }
    void engineLoadsAndRefreshesRealToolContext() {
        QTemporaryDir dir;
        put(dir.filePath("AGENTS.md"), "Original root instructions");
        put(dir.filePath("src/AGENTS.md"), "Nested source instructions");
        put(dir.filePath("src/code.cpp"), "observed source");
        auto model = std::make_shared<ObservingModel>(); model->changedFile = dir.filePath("AGENTS.md");
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, dir.path());
        auto policy = std::make_shared<a::RulePolicy>();
        a::EngineOptions options; options.sessionsDirectory = dir.filePath("sessions");
        a::Engine engine(model, registry, policy, options);
        const auto session = engine.createSession("fixture", dir.path(), "Host policy");
        const auto run = engine.run({session.id, "Read the source"}).result.get();
        QVERIFY2(run.status == a::RunStatus::Completed, qPrintable(run.errorMessage));
        QCOMPARE(model->requests.size(), 2);
        QCOMPARE(model->requests[0].systemPrompt, "Host policy");
        QVERIFY(model->requests[0].messages.first().text.contains("Original root instructions"));
        QVERIFY(!model->requests[0].messages.first().text.contains("Nested source instructions"));
        QVERIFY(model->requests[1].messages.first().text.contains("Changed root instructions"));
        QVERIFY(!model->requests[1].messages.first().text.contains("Original root instructions"));
        QVERIFY(model->requests[1].messages.first().text.contains("Nested source instructions"));
        const auto restored = engine.session(session.id);
        QCOMPARE(restored.messages.size(), 4);
        QVERIFY(!restored.messages.first().metadata.contains("iilocal.project_context"));
        QVERIFY(a::projectContextPaths(restored.messages).contains(dir.filePath("src/code.cpp")));
        a::ToolRunner runner(registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass));
        a::ToolContext context; context.sessionId = session.id; context.workingDirectory = dir.path(); context.artifactsDirectory = dir.filePath("artifacts");
        QVERIFY(runner.run({"edit", "Edit", {{"path", "AGENTS.md"}, {"old_string", "Changed"}, {"new_string", "Replaced"}}}, context).isError);
    }
    void orderImportsAndMarkdown() {
        QTemporaryDir dir;
        put(dir.filePath("CLAUDE.md"), "Root @./import.md\n\n<!-- private note -->\n\n`@./inline.md`\n\n```text\n@./code.md\n```\n");
        put(dir.filePath("import.md"), "Imported @./CLAUDE.md\n");
        put(dir.filePath("inline.md"), "MUST NOT IMPORT INLINE");
        put(dir.filePath("code.md"), "MUST NOT IMPORT CODE");
        put(dir.filePath(".claude/CLAUDE.md"), "Dot instructions");
        put(dir.filePath(".claude/rules/z.md"), "Z");
        put(dir.filePath(".claude/rules/sub/a.md"), "A");
        put(dir.filePath("CLAUDE.local.md"), "Local");
        put(dir.filePath("AGENTS.md"), "Agent instructions");
        const auto snapshot = a::loadProjectContext(dir.path());
        QCOMPARE(names(snapshot), (QStringList{"CLAUDE.md", "import.md", "CLAUDE.md", "a.md", "z.md", "CLAUDE.local.md", "AGENTS.md"}));
        QVERIFY(!snapshot.files.first().content.contains("private note"));
        QVERIFY(snapshot.files.first().content.contains("`@./inline.md`"));
        QVERIFY(snapshot.files.first().content.contains("@./code.md"));
        QCOMPARE(snapshot.files[1].parent, snapshot.files[0].path);
        QVERIFY(snapshot.files.first().transformed);
        QVERIFY(!snapshot.files[1].transformed);
        QCOMPARE(snapshot.message().role, a::MessageRole::User);
        QVERIFY(!snapshot.toJson(false).value("files").toArray().first().toObject().contains("content"));
        QVERIFY(snapshot.message().text.contains("Agent instructions"));
    }
    void scopedRulesAndNestedDirectories() {
        QTemporaryDir dir;
        put(dir.filePath("AGENTS.md"), "Root");
        put(dir.filePath("src/AGENTS.md"), "Source only");
        put(dir.filePath("other/AGENTS.md"), "Unrelated");
        put(dir.filePath(".claude/rules/cpp.md"), "---\npaths: [\"src/**/*.{cpp,h}\", \"!src/generated/**\"]\n---\nC++ rules\n");
        put(dir.filePath(".claude/rules/global.md"), "---\npaths: '**'\n---\nGlobal\n");
        put(dir.filePath("src/.claude/rules/header.md"), "---\npaths:\n  - '*.h'\n---\nHeaders\n");
        QCOMPARE(names(a::loadProjectContext(dir.path())), (QStringList{"global.md", "AGENTS.md"}));
        const auto cpp = a::loadProjectContext(dir.path(), {"src/a.cpp"});
        QVERIFY(names(cpp).contains("cpp.md")); QVERIFY(!names(cpp).contains("header.md"));
        QVERIFY(cpp.message().text.contains("Source only")); QVERIFY(!cpp.message().text.contains("Unrelated"));
        QVERIFY(a::loadProjectContext(dir.path(), {"src/deep/a.h"}).message().text.contains("Headers"));
        QVERIFY(!names(a::loadProjectContext(dir.path(), {"src/generated/a.cpp"})).contains("cpp.md"));
        put(dir.filePath(".claude/rules/cpp.md"), "---\npaths: 'src/**/*.cpp'\n---\n@../../cpp-style.md");
        put(dir.filePath("cpp-style.md"), "Imported C++ style");
        const auto imported = a::loadProjectContext(dir.path(), {"src/a.cpp"});
        QVERIFY(imported.message().text.contains("Imported C++ style"));
        for (const auto& file : imported.files) if (file.path.endsWith("cpp-style.md")) QCOMPARE(file.patterns, (QStringList{"src/**/*.cpp"}));
        QVERIFY(!a::loadProjectContext(dir.path()).message().text.contains("Imported C++ style"));
        a::ProjectContextOptions options; options.rootDirectory = dir.path();
        const auto nested = a::loadProjectContext(dir.filePath("src"), {}, options);
        QVERIFY(nested.message().text.contains("Root")); QVERIFY(nested.message().text.contains("Source only"));
    }
    void refreshAndBounds() {
        QTemporaryDir dir; put(dir.filePath("AGENTS.md"), "Version one");
        auto first = a::loadProjectContext(dir.path());
        QCOMPARE(a::loadProjectContext(dir.path()).fingerprint, first.fingerprint);
        put(dir.filePath("AGENTS.md"), "Version two");
        auto second = a::loadProjectContext(dir.path());
        QVERIFY(first.fingerprint != second.fingerprint);
        QCOMPARE(second.files.first().sha256, QString::fromLatin1(QCryptographicHash::hash("Version two", QCryptographicHash::Sha256).toHex()));
        QVERIFY(QFile::remove(dir.filePath("AGENTS.md")));
        QVERIFY(a::loadProjectContext(dir.path()).files.isEmpty());
        a::ProjectContextOptions limits; limits.maxFileBytes = 8;
        put(dir.filePath("AGENTS.md"), "Too much content");
        QVERIFY_THROWS_EXCEPTION(Error, a::loadProjectContext(dir.path(), {}, limits));
        limits.enabled = false;
        QVERIFY(a::loadProjectContext(dir.path(), {}, limits).files.isEmpty());
        limits = {}; limits.maxTargetPaths = 1;
        QVERIFY_THROWS_EXCEPTION(Error, a::loadProjectContext(dir.path(), {"a", "b"}, limits));
        CancellationToken token; token.cancel();
        QVERIFY_THROWS_EXCEPTION(Error, a::loadProjectContext(dir.path(), {}, {}, token));
    }
    void confinementAndMalformedInput() {
        QTemporaryDir dir; QDir().mkpath(dir.filePath("workspace"));
        put(dir.filePath("outside.md"), "Outside");
        put(dir.filePath("workspace/CLAUDE.md"), "@../outside.md");
        QVERIFY_THROWS_EXCEPTION(Error, a::loadProjectContext(dir.filePath("workspace")));
        put(dir.filePath("workspace/CLAUDE.md"), "@./link.md");
        QVERIFY(QFile::link(dir.filePath("outside.md"), dir.filePath("workspace/link.md")));
        QVERIFY_THROWS_EXCEPTION(Error, a::loadProjectContext(dir.filePath("workspace")));
        put(dir.filePath("workspace/CLAUDE.md"), "Safe");
        QVERIFY_THROWS_EXCEPTION(Error, a::loadProjectContext(dir.filePath("workspace"), {"../outside.md"}));
        put(dir.filePath("workspace/.claude/rules/bad.md"), "---\npaths: {invalid: map}\n---\nInvalid rule");
        QVERIFY_THROWS_EXCEPTION(Error, a::loadProjectContext(dir.filePath("workspace")));
        put(dir.filePath("workspace/.claude/rules/bad.md"), "---\npaths: [*alias]\n---\nInvalid alias");
        QVERIFY_THROWS_EXCEPTION(Error, a::loadProjectContext(dir.filePath("workspace")));
        put(dir.filePath("workspace/.claude/rules/bad.md"), QByteArray("invalid\0bytes", 13));
        QVERIFY_THROWS_EXCEPTION(Error, a::loadProjectContext(dir.filePath("workspace")));
    }
};
QTEST_GUILESS_MAIN(ContextTests)
#include "context_tests.moc"
