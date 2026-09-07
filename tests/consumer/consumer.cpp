#include <iiLocalLLM.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <iostream>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir storage(QDir::current().filePath(QStringLiteral("installed-api-XXXXXX")));
    if (!storage.isValid()) return 4;
    const auto bundle = storage.filePath(QStringLiteral("bundle"));
    if (!QDir().mkpath(bundle)) return 5;
    const auto manifest = iiLocalLLM::parseModelManifest({
        {"id", "consumer"}, {"architecture", "fixture"}, {"format", "gguf"},
        {"quantization", "none"}, {"context_length", 512}, {"capabilities", QJsonArray{"chat"}}
    });
    const auto write = [](const QString& path, const QByteArray& bytes) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    if (!write(QDir(bundle).filePath("manifest.json"), QJsonDocument(iiLocalLLM::manifestObject(manifest)).toJson())
        || !write(QDir(bundle).filePath("model.gguf"), QByteArrayLiteral("GGUF-catalog-fixture"))) return 6;
    const auto uri = iiLocalLLM::modelUri(manifest.id);
    const auto root = storage.filePath(QStringLiteral("Models"));
    {
        iiLocalLLM::ModelCatalog catalog(root);
        const auto installed = catalog.install(bundle);
        if (installed.uri != uri || !catalog.verify(uri).valid || catalog.resolve(uri).entryPath.isEmpty()) return 7;
    }
    iiLocalLLM::ServiceOptions options;
    options.modelsDirectory = root;
    iiLocalLLM::Service service(options);
    if (service.hardware().cpuArchitecture.isEmpty()) return 3;
    const auto stats = service.stats().get();
    if (!stats.memoryBudgetBytes || iiLocalLLM::parseKeepAlive("5m") != 300000
        || iiLocalLLM::MemoryEstimate{1, 2, 3}.totalBytes() != 6 || !service.models().get().isEmpty()) return 14;
    iiLocalLLM::LocalIpcServer ipc(service);
    iiLocalLLM::HttpApiServer http(service);
    if (!http.listen() || !http.port()) return 12;
    if (stats.loadedModels != 0 || stats.cachedContexts != 0) return 1;
    const auto listing = service.installedModels().get();
    if (service.pullModel(uri).result.get().uri != uri) return 15; // Existing installed bundle; no network.
    try {
        (void)service.resolveModel("qwen3:8b").get();
        return 16;
    } catch (const iiLocalLLM::Error& error) {
        if (error.code() != iiLocalLLM::ErrorCode::NotFound) return 17; // Bundled alias resolves, model is not installed.
    }
    if (listing.models.size() != 1 || !listing.issues.isEmpty()
        || service.resolveModel(uri).get().uri != uri || !service.verifyModel(uri).get().valid) return 8;
    try {
        service.loadModel({QStringLiteral("models/model.gguf")}).get();
        return 9;
    } catch (const iiLocalLLM::Error& error) {
        if (error.code() != iiLocalLLM::ErrorCode::InvalidArgument) return 10;
    }
    const auto missing = service.chat({QStringLiteral("missing"), QStringLiteral("hello"), {}}).result.get();
    if (missing.errorCode != iiLocalLLM::ErrorCode::NotFound) return 2;
    const auto invalid = service.complete({QStringLiteral("/model.gguf"), {{iiLocalLLM::Role::User, QStringLiteral("hello")}}, {}}).result.get();
    if (invalid.errorCode != iiLocalLLM::ErrorCode::InvalidArgument) return 13;
    http.close();
    service.removeModel(uri).get();
    if (!service.installedModels().get().models.isEmpty() || !QFile::exists(QDir(bundle).filePath("model.gguf"))) return 11;
    std::cout << "Installed manifest/catalog/URI/pull/residency/service/runtime/IPC/HTTP API linked and executed\n";
    return 0;
}
