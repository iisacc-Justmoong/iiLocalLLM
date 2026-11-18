#include "native_link.h"
#include <iiLocalLLM.h>
#include "CTestSupport.h"
#include <QtCore/QCoreApplication>
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
class CatalogTests {
public:
    void minimalManifestAndCanonicalUri()
    {
        const auto m = parseModelManifest(minimal());
        CTEST_COMPARE(m.id, QStringLiteral("qwen3-8b-q4"));
        CTEST_COMPARE(m.entryPoint, QStringLiteral("model.gguf"));
        CTEST_COMPARE(m.contextLength, 32768);
        CTEST_CHECK(m.files.isEmpty());
        CTEST_CHECK(m.capabilities.contains(QStringLiteral("tool-calling")));
        CTEST_COMPARE(modelId(modelUri(m.id)), m.id);
        for (const QString uri : {"qwen3-8b-q4", "/model.gguf", "model://../secret", "model://id/path", "model://id?x=1", "model://id#x", "model://%69d", "model://ID", "model://id:42", "model://con"})
            CTEST_THROWS(Error, modelId(uri));
    }
    void invalidManifestsAreRejected()
    {
        for (const QString key : {"id", "architecture", "format", "quantization", "context_length", "capabilities"}) {
            auto object = minimal(); object.remove(key);
            CTEST_THROWS(Error, parseModelManifest(object));
        }
        for (const auto& patch : QList<QJsonObject>{{{"context_length", 2.5}}, {{"context_length", 0}}, {{"schema_version", 2}},
            {{"runtime", "llama.cpp"}}, {{"entry_point", "../model.gguf"}}, {{"entry_point", "/model.gguf"}},
            {{"entry_point", "C:\\model.gguf"}}, {{"capabilities", QJsonArray{"chat", "chat"}}}}) {
            auto object = minimal();
            for (auto it = patch.begin(); it != patch.end(); ++it) object.insert(it.key(), it.value());
            CTEST_THROWS(Error, parseModelManifest(object));
        }
        auto custom = minimal(); custom["format"] = "safetensors"; custom["entry_point"] = "model.safetensors";
        CTEST_COMPARE(parseModelManifest(custom).format, QStringLiteral("safetensors"));
        for (const auto& name : {QStringLiteral("model.onnx"), QStringLiteral("MODEL.ONNX")}) {
            auto invalid = custom; invalid["entry_point"] = name;
            CTEST_THROWS(Error, parseModelManifest(invalid));
            invalid = minimal(); invalid["projector"] = name;
            CTEST_THROWS(Error, parseModelManifest(invalid));
            invalid = minimal(); invalid["files"] = QJsonArray{QJsonObject{{"path", name}, {"size", 1}, {"sha256", QString(64, 'a')}}};
            CTEST_THROWS(Error, parseModelManifest(invalid));
        }
        auto onnx = custom; onnx["format"] = "onnx";
        CTEST_THROWS(Error, parseModelManifest(onnx));
    }
    void installRejectsUnlistedOnnxWithoutChangingSource() {
        QTemporaryDir root(QDir::temp().filePath("catalog-onnx-XXXXXX")); CTEST_CHECK(root.isValid());
        const auto source = package(root.filePath("source"));
        write(QDir(source).filePath("nested/unsafe.ONNX"), "fixture");
        ModelCatalog catalog(root.filePath("Models"));
        CTEST_THROWS(Error, catalog.install(source));
        CTEST_CHECK(QFileInfo::exists(QDir(source).filePath("model.gguf")));
        CTEST_CHECK(QFileInfo::exists(QDir(source).filePath("nested/unsafe.ONNX")));
        CTEST_CHECK(!QFileInfo::exists(root.filePath("Models/qwen3-8b-q4")));
    }
    void projectorIsPackageOwnedAndIntegrityChecked() {
        auto manifest=minimal();manifest["projector"]="mmproj.gguf";
        CTEST_COMPARE(manifestObject(parseModelManifest(manifest))["projector"],"mmproj.gguf");
        for(const auto& value:QStringList{"../mmproj.gguf","/mmproj.gguf","model.gguf"}) {
            auto invalid=manifest;invalid["projector"]=value;CTEST_THROWS(Error,parseModelManifest(invalid));
        }
        QTemporaryDir root(QDir::temp().filePath("projector-XXXXXX"));CTEST_CHECK(root.isValid());
        const auto source=package(root.filePath("package"));
        write(QDir(source).filePath("manifest.json"),QJsonDocument(manifest).toJson());
        ModelCatalog catalog(root.filePath("Models"));
        CTEST_THROWS(Error,catalog.install(source));
        write(QDir(source).filePath("mmproj.gguf"),"GGUF-projector-fixture");
        const auto installed=catalog.install(source);CTEST_CHECK(catalog.verify(installed.uri).valid);
        const auto resolved=catalog.resolve(installed.uri);CTEST_COMPARE(resolved.record.manifest.projector,"mmproj.gguf");
        write(QDir(resolved.directory).filePath("mmproj.gguf"),"GGUF-tampered-projector");
        CTEST_CHECK(!catalog.verify(installed.uri).valid);
    }
    void installVerifyResolvePersistAndRemove()
    {
        QTemporaryDir root(QDir::temp().filePath("catalog-XXXXXX")); CTEST_CHECK(root.isValid());
        const auto source = package(root.filePath("source/qwen3-8b"));
        const auto storage = root.filePath("Models");
        const auto uri = QStringLiteral("model://qwen3-8b-q4");
        {
            ModelCatalog catalog(storage);
            const auto installed = catalog.install(source);
            CTEST_COMPARE(installed.uri, uri);
            CTEST_COMPARE(installed.manifest.files.size(), 2);
            CTEST_COMPARE(catalog.list().models.size(), 1);
            CTEST_CHECK(catalog.list().issues.isEmpty());
            const auto verified = catalog.verify(uri);
            CTEST_CHECK(verified.valid); CTEST_COMPARE(verified.checkedFiles, 2); CTEST_COMPARE(verified.checkedBytes, qint64(19));
            const auto resolved = catalog.resolve(uri);
            CTEST_COMPARE(resolved.entryPath, storage + QStringLiteral("/qwen3-8b-q4/model.gguf"));
            CTEST_CHECK(!modelRecordObject(installed).contains(QStringLiteral("path")));
            CTEST_THROWS(Error, catalog.install(source));
            ModelCatalog competing(storage);
            CTEST_THROWS(Error, competing.resolve(uri));
        }
        ModelCatalog catalog(storage);
        CTEST_CHECK(catalog.verify(uri).valid); // Survives object/process lifetimes with no in-memory registry.
        CTEST_CHECK(QDir(storage).rename("qwen3-8b-q4", "qwen3-8b"));
        CTEST_CHECK(catalog.resolve(uri).directory.endsWith(QStringLiteral("/qwen3-8b"))); // Identity comes from manifest, not folder name.
        catalog.remove(uri);
        CTEST_CHECK(catalog.list().models.isEmpty());
        CTEST_CHECK(QFileInfo(source + QStringLiteral("/model.gguf")).exists());
        CTEST_THROWS(Error, catalog.resolve(uri));
    }
    void integrityDetectsChangedMissingAndUnlistedFiles()
    {
        QTemporaryDir root(QDir::temp().filePath("catalog-XXXXXX")); CTEST_CHECK(root.isValid());
        ModelCatalog catalog(root.filePath("Models"));
        const auto installed = catalog.install(package(root.filePath("source")));
        const auto resolved = catalog.resolve(installed.uri);
        write(resolved.entryPath, QByteArrayLiteral("GGUF-tampered!!!!"));
        CTEST_CHECK(!catalog.verify(installed.uri).valid);
        write(resolved.entryPath, QByteArrayLiteral("GGUF-test-fixture"));
        CTEST_CHECK(QFile::remove(QDir(resolved.directory).filePath("tokenizer/vocab.json")));
        CTEST_CHECK(!catalog.verify(installed.uri).valid);
        write(QDir(resolved.directory).filePath("tokenizer/vocab.json"), QByteArrayLiteral("{}"));
        write(QDir(resolved.directory).filePath("extra.bin"), QByteArrayLiteral("extra"));
        CTEST_CHECK(!catalog.verify(installed.uri).valid);
        catalog.remove(installed.uri); // Broken weights remain removable.
        CTEST_CHECK(catalog.list().models.isEmpty());
    }
    void expectedHashesAndInterruptedInstallationNeverPublish()
    {
        QTemporaryDir root(QDir::temp().filePath("catalog-XXXXXX")); CTEST_CHECK(root.isValid());
        auto m = minimal();
        m["files"] = QJsonArray{QJsonObject{{"path", "model.gguf"}, {"size", 17}, {"sha256", QString(64, QLatin1Char('0'))}}};
        const auto source = package(root.filePath("source"), m);
        ModelCatalog catalog(root.filePath("Models"));
        CTEST_THROWS(Error, catalog.install(source));
        CTEST_CHECK(catalog.list().models.isEmpty());
        CTEST_CHECK(QDir(root.filePath("Models")).entryList({".install-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
        CancellationToken token; token.cancel();
        CTEST_THROWS(Error, catalog.install(package(root.filePath("other")), token));
        CTEST_CHECK(catalog.list().models.isEmpty());
    }
    void metadataProblemsAreVisibleAndDoNotHideOtherModels()
    {
        QTemporaryDir root(QDir::temp().filePath("catalog-XXXXXX")); CTEST_CHECK(root.isValid());
        ModelCatalog catalog(root.filePath("Models"));
        const auto installed = catalog.install(package(root.filePath("source")));
        write(root.filePath("Models/broken/manifest.json"), QByteArrayLiteral("not json"));
        CTEST_COMPARE(catalog.list().models.size(), 1);
        CTEST_COMPARE(catalog.list().issues.size(), 1);
        CTEST_COMPARE(catalog.resolve(installed.uri).record.uri, installed.uri);
        write(root.filePath("Models/duplicate/manifest.json"), QJsonDocument(manifestObject(installed.manifest)).toJson());
        CTEST_THROWS(Error, catalog.resolve(installed.uri));
        CTEST_CHECK(!catalog.list().issues.isEmpty());
    }
    void mlxDirectoryAndDeclaredFormat()
    {
        QTemporaryDir root(QDir::temp().filePath("catalog-XXXXXX")); CTEST_CHECK(root.isValid());
        auto m = minimal("mlx-fixture"); m["format"] = "mlx"; m["quantization"] = "4bit";
        const auto source = root.filePath("source");
        write(source + "/manifest.json", QJsonDocument(m).toJson());
        write(source + "/config.json", "{}"); write(source + "/tokenizer.json", "{}"); write(source + "/model.safetensors", "fixture");
        ModelCatalog catalog(root.filePath("Models"));
        const auto installed = catalog.install(source);
        CTEST_COMPARE(installed.manifest.entryPoint, QStringLiteral("."));
        CTEST_CHECK(catalog.verify(installed.uri).valid);
        auto gguf = minimal("bad-format");
        package(root.filePath("bad"), gguf);
        write(root.filePath("bad/model.gguf"), "not a GGUF file");
        CTEST_THROWS(Error, catalog.install(root.filePath("bad")));
    }
    void symlinksAndManifestTraversalAreRejected()
    {
        auto m = minimal();
        m["files"] = QJsonArray{QJsonObject{{"path", "../outside"}, {"size", 1}, {"sha256", QString(64, QLatin1Char('0'))}}};
        CTEST_THROWS(Error, parseModelManifest(m));
#ifdef Q_OS_UNIX
        QTemporaryDir root(QDir::temp().filePath("catalog-XXXXXX")); CTEST_CHECK(root.isValid());
        const auto source = package(root.filePath("source"));
        write(root.filePath("outside/keep.txt"), "keep");
        CTEST_CHECK(createNativeTestLink(root.filePath("outside"), source + "/linked"));
        ModelCatalog catalog(root.filePath("Models"));
        CTEST_THROWS(Error, catalog.install(source));
        CTEST_CHECK(QFileInfo(root.filePath("outside/keep.txt")).exists());
        CTEST_CHECK(QFile::remove(source + "/linked"));
        const auto installed = catalog.install(source);
        CTEST_CHECK(createNativeTestLink(root.filePath("outside"), catalog.resolve(installed.uri).directory + "/linked"));
        CTEST_CHECK(!catalog.verify(installed.uri).valid);
        catalog.remove(installed.uri);
        CTEST_CHECK(QFileInfo(root.filePath("outside/keep.txt")).exists());
#endif
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    iillm_ctest::CTestRunner runner;
    CatalogTests tests;
    runner.run("minimalManifestAndCanonicalUri", [&] { tests.minimalManifestAndCanonicalUri(); });
    runner.run("invalidManifestsAreRejected", [&] { tests.invalidManifestsAreRejected(); });
    runner.run("installRejectsUnlistedOnnxWithoutChangingSource", [&] { tests.installRejectsUnlistedOnnxWithoutChangingSource(); });
    runner.run("projectorIsPackageOwnedAndIntegrityChecked", [&] { tests.projectorIsPackageOwnedAndIntegrityChecked(); });
    runner.run("installVerifyResolvePersistAndRemove", [&] { tests.installVerifyResolvePersistAndRemove(); });
    runner.run("integrityDetectsChangedMissingAndUnlistedFiles", [&] { tests.integrityDetectsChangedMissingAndUnlistedFiles(); });
    runner.run("expectedHashesAndInterruptedInstallationNeverPublish", [&] { tests.expectedHashesAndInterruptedInstallationNeverPublish(); });
    runner.run("metadataProblemsAreVisibleAndDoNotHideOtherModels", [&] { tests.metadataProblemsAreVisibleAndDoNotHideOtherModels(); });
    runner.run("mlxDirectoryAndDeclaredFormat", [&] { tests.mlxDirectoryAndDeclaredFormat(); });
    runner.run("symlinksAndManifestTraversalAreRejected", [&] { tests.symlinksAndManifestTraversalAreRejected(); });
    return runner.result();
}
