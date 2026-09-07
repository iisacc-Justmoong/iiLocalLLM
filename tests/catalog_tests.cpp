#include <iiLocalLLM.h>
#include <QtTest/QtTest>
#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryDir>

using namespace iiLocalLLM;
namespace {
QJsonObject minimal(const QString& id = QStringLiteral("qwen3-8b-q4"))
{
    return {{"id", id}, {"architecture", "qwen3"}, {"format", "gguf"}, {"quantization", "Q4_K_M"},
        {"context_length", 32768}, {"capabilities", QJsonArray{"text-generation", "chat", "tool-calling"}}};
}
void write(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) throw std::runtime_error("Cannot create test directory");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) throw std::runtime_error("Cannot write test fixture");
}
QString package(const QString& directory, const QJsonObject& manifest = minimal())
{
    write(QDir(directory).filePath("manifest.json"), QJsonDocument(manifest).toJson());
    write(QDir(directory).filePath("model.gguf"), QByteArrayLiteral("GGUF-test-fixture"));
    write(QDir(directory).filePath("tokenizer/vocab.json"), QByteArrayLiteral("{}"));
    return directory;
}
}
class CatalogTests : public QObject {
    Q_OBJECT
private slots:
    void minimalManifestAndCanonicalUri()
    {
        const auto m = parseModelManifest(minimal());
        QCOMPARE(m.id, QStringLiteral("qwen3-8b-q4"));
        QCOMPARE(m.entryPoint, QStringLiteral("model.gguf"));
        QCOMPARE(m.contextLength, 32768);
        QVERIFY(m.files.isEmpty());
        QVERIFY(m.capabilities.contains(QStringLiteral("tool-calling")));
        QCOMPARE(modelId(modelUri(m.id)), m.id);
        for (const QString uri : {"qwen3-8b-q4", "/model.gguf", "model://../secret", "model://id/path", "model://id?x=1", "model://id#x", "model://%69d", "model://ID", "model://id:42", "model://con"})
            QVERIFY_THROWS_EXCEPTION(Error, modelId(uri));
    }
    void invalidManifestsAreRejected()
    {
        for (const QString key : {"id", "architecture", "format", "quantization", "context_length", "capabilities"}) {
            auto object = minimal(); object.remove(key);
            QVERIFY_THROWS_EXCEPTION(Error, parseModelManifest(object));
        }
        for (const auto& patch : QList<QJsonObject>{{{"context_length", 2.5}}, {{"context_length", 0}}, {{"schema_version", 2}},
            {{"runtime", "llama.cpp"}}, {{"entry_point", "../model.gguf"}}, {{"entry_point", "/model.gguf"}},
            {{"entry_point", "C:\\model.gguf"}}, {{"capabilities", QJsonArray{"chat", "chat"}}}}) {
            auto object = minimal();
            for (auto it = patch.begin(); it != patch.end(); ++it) object.insert(it.key(), it.value());
            QVERIFY_THROWS_EXCEPTION(Error, parseModelManifest(object));
        }
        auto custom = minimal(); custom["format"] = "onnx";
        QVERIFY_THROWS_EXCEPTION(Error, parseModelManifest(custom));
        custom["entry_point"] = "model.onnx";
        QCOMPARE(parseModelManifest(custom).format, QStringLiteral("onnx"));
    }
    void installVerifyResolvePersistAndRemove()
    {
        QTemporaryDir root(QDir::current().filePath("catalog-XXXXXX")); QVERIFY(root.isValid());
        const auto source = package(root.filePath("source/qwen3-8b"));
        const auto storage = root.filePath("Models");
        const auto uri = QStringLiteral("model://qwen3-8b-q4");
        {
            ModelCatalog catalog(storage);
            const auto installed = catalog.install(source);
            QCOMPARE(installed.uri, uri);
            QCOMPARE(installed.manifest.files.size(), 2);
            QCOMPARE(catalog.list().models.size(), 1);
            QVERIFY(catalog.list().issues.isEmpty());
            const auto verified = catalog.verify(uri);
            QVERIFY(verified.valid); QCOMPARE(verified.checkedFiles, 2); QCOMPARE(verified.checkedBytes, qint64(19));
            const auto resolved = catalog.resolve(uri);
            QCOMPARE(resolved.entryPath, storage + QStringLiteral("/qwen3-8b-q4/model.gguf"));
            QVERIFY(!modelRecordObject(installed).contains(QStringLiteral("path")));
            QVERIFY_THROWS_EXCEPTION(Error, catalog.install(source));
            ModelCatalog competing(storage);
            QVERIFY_THROWS_EXCEPTION(Error, competing.resolve(uri));
        }
        ModelCatalog catalog(storage);
        QVERIFY(catalog.verify(uri).valid); // Survives object/process lifetimes with no in-memory registry.
        QVERIFY(QDir(storage).rename("qwen3-8b-q4", "qwen3-8b"));
        QVERIFY(catalog.resolve(uri).directory.endsWith(QStringLiteral("/qwen3-8b"))); // Identity comes from manifest, not folder name.
        catalog.remove(uri);
        QVERIFY(catalog.list().models.isEmpty());
        QVERIFY(QFileInfo(source + QStringLiteral("/model.gguf")).exists());
        QVERIFY_THROWS_EXCEPTION(Error, catalog.resolve(uri));
    }
    void integrityDetectsChangedMissingAndUnlistedFiles()
    {
        QTemporaryDir root(QDir::current().filePath("catalog-XXXXXX")); QVERIFY(root.isValid());
        ModelCatalog catalog(root.filePath("Models"));
        const auto installed = catalog.install(package(root.filePath("source")));
        const auto resolved = catalog.resolve(installed.uri);
        write(resolved.entryPath, QByteArrayLiteral("GGUF-tampered!!!!"));
        QVERIFY(!catalog.verify(installed.uri).valid);
        write(resolved.entryPath, QByteArrayLiteral("GGUF-test-fixture"));
        QVERIFY(QFile::remove(QDir(resolved.directory).filePath("tokenizer/vocab.json")));
        QVERIFY(!catalog.verify(installed.uri).valid);
        write(QDir(resolved.directory).filePath("tokenizer/vocab.json"), QByteArrayLiteral("{}"));
        write(QDir(resolved.directory).filePath("extra.bin"), QByteArrayLiteral("extra"));
        QVERIFY(!catalog.verify(installed.uri).valid);
        catalog.remove(installed.uri); // Broken weights remain removable.
        QVERIFY(catalog.list().models.isEmpty());
    }
    void expectedHashesAndInterruptedInstallationNeverPublish()
    {
        QTemporaryDir root(QDir::current().filePath("catalog-XXXXXX")); QVERIFY(root.isValid());
        auto m = minimal();
        m["files"] = QJsonArray{QJsonObject{{"path", "model.gguf"}, {"size", 17}, {"sha256", QString(64, QLatin1Char('0'))}}};
        const auto source = package(root.filePath("source"), m);
        ModelCatalog catalog(root.filePath("Models"));
        QVERIFY_THROWS_EXCEPTION(Error, catalog.install(source));
        QVERIFY(catalog.list().models.isEmpty());
        QVERIFY(QDir(root.filePath("Models")).entryList({".install-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
        CancellationToken token; token.cancel();
        QVERIFY_THROWS_EXCEPTION(Error, catalog.install(package(root.filePath("other")), token));
        QVERIFY(catalog.list().models.isEmpty());
    }
    void metadataProblemsAreVisibleAndDoNotHideOtherModels()
    {
        QTemporaryDir root(QDir::current().filePath("catalog-XXXXXX")); QVERIFY(root.isValid());
        ModelCatalog catalog(root.filePath("Models"));
        const auto installed = catalog.install(package(root.filePath("source")));
        write(root.filePath("Models/broken/manifest.json"), QByteArrayLiteral("not json"));
        QCOMPARE(catalog.list().models.size(), 1);
        QCOMPARE(catalog.list().issues.size(), 1);
        QCOMPARE(catalog.resolve(installed.uri).record.uri, installed.uri);
        write(root.filePath("Models/duplicate/manifest.json"), QJsonDocument(manifestObject(installed.manifest)).toJson());
        QVERIFY_THROWS_EXCEPTION(Error, catalog.resolve(installed.uri));
        QVERIFY(!catalog.list().issues.isEmpty());
    }
    void mlxDirectoryAndDeclaredFormat()
    {
        QTemporaryDir root(QDir::current().filePath("catalog-XXXXXX")); QVERIFY(root.isValid());
        auto m = minimal("mlx-fixture"); m["format"] = "mlx"; m["quantization"] = "4bit";
        const auto source = root.filePath("source");
        write(source + "/manifest.json", QJsonDocument(m).toJson());
        write(source + "/config.json", "{}"); write(source + "/tokenizer.json", "{}"); write(source + "/model.safetensors", "fixture");
        ModelCatalog catalog(root.filePath("Models"));
        const auto installed = catalog.install(source);
        QCOMPARE(installed.manifest.entryPoint, QStringLiteral("."));
        QVERIFY(catalog.verify(installed.uri).valid);
        auto gguf = minimal("bad-format");
        package(root.filePath("bad"), gguf);
        write(root.filePath("bad/model.gguf"), "not a GGUF file");
        QVERIFY_THROWS_EXCEPTION(Error, catalog.install(root.filePath("bad")));
    }
    void symlinksAndManifestTraversalAreRejected()
    {
        auto m = minimal();
        m["files"] = QJsonArray{QJsonObject{{"path", "../outside"}, {"size", 1}, {"sha256", QString(64, QLatin1Char('0'))}}};
        QVERIFY_THROWS_EXCEPTION(Error, parseModelManifest(m));
#ifdef Q_OS_UNIX
        QTemporaryDir root(QDir::current().filePath("catalog-XXXXXX")); QVERIFY(root.isValid());
        const auto source = package(root.filePath("source"));
        write(root.filePath("outside/keep.txt"), "keep");
        QVERIFY(QFile::link(root.filePath("outside"), source + "/linked"));
        ModelCatalog catalog(root.filePath("Models"));
        QVERIFY_THROWS_EXCEPTION(Error, catalog.install(source));
        QVERIFY(QFileInfo(root.filePath("outside/keep.txt")).exists());
        QVERIFY(QFile::remove(source + "/linked"));
        const auto installed = catalog.install(source);
        QVERIFY(QFile::link(root.filePath("outside"), catalog.resolve(installed.uri).directory + "/linked"));
        QVERIFY(!catalog.verify(installed.uri).valid);
        catalog.remove(installed.uri);
        QVERIFY(QFileInfo(root.filePath("outside/keep.txt")).exists());
#endif
    }
};
QTEST_GUILESS_MAIN(CatalogTests)
#include "catalog_tests.moc"
