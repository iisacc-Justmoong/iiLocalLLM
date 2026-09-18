#include "ModelCatalog.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <algorithm>
#include <map>
#include <optional>

namespace iiLocalLLM {
namespace {
constexpr qint64 maxManifestBytes = 4 * 1024 * 1024;
void require(bool ok, ErrorCode code, const QString& message)
{ if (!ok) throw Error(code, message); }
ModelManifest readManifest(const QString& directory)
{
    const QFileInfo info(QDir(directory).filePath(QStringLiteral("manifest.json")));
    require(info.isFile() && !info.isSymbolicLink(), ErrorCode::InvalidManifest, QStringLiteral("Package requires a regular manifest.json"));
    require(info.size() <= maxManifestBytes, ErrorCode::InvalidManifest, QStringLiteral("Manifest exceeds 4 MiB"));
    QFile file(info.absoluteFilePath());
    require(file.open(QIODevice::ReadOnly), ErrorCode::StorageFailure, file.errorString());
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.read(maxManifestBytes + 1), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(), ErrorCode::InvalidManifest, QStringLiteral("Invalid manifest JSON"));
    return parseModelManifest(document.object());
}
void assetPaths(const QDir& root, const QString& relative, QStringList& files, const CancellationToken& token, int depth = 0)
{
    token.throwIfCancelled();
    require(depth <= 64, ErrorCode::InvalidManifest, QStringLiteral("Package directory nesting exceeds 64 levels"));
    const QDir directory(relative.isEmpty() ? root.absolutePath() : root.filePath(relative));
    require(directory.isReadable(), ErrorCode::StorageFailure, QStringLiteral("Package directory is unreadable"));
    const auto entries = directory.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& entry : entries) {
        token.throwIfCancelled();
        const auto path = relative.isEmpty() ? entry.fileName() : relative + QLatin1Char('/') + entry.fileName();
        require(!entry.isSymbolicLink(), ErrorCode::IntegrityFailure, QStringLiteral("Package symlinks are not allowed: ") + path);
        if (entry.isDir()) assetPaths(root, path, files, token, depth + 1);
        else {
            require(entry.isFile(), ErrorCode::IntegrityFailure, QStringLiteral("Package contains a non-regular file: ") + path);
            if (path == QStringLiteral("manifest.json")) continue;
            files.append(path);
            require(files.size() <= 10000, ErrorCode::ResourceLimit, QStringLiteral("Package exceeds 10000 files"));
        }
    }
}
ModelFile hashFile(const QDir& root, const QString& relative, const CancellationToken& token, const QString& outputDirectory = {})
{
    QFile input(root.filePath(relative));
    require(input.open(QIODevice::ReadOnly), ErrorCode::StorageFailure, input.errorString());
    QFile output;
    if (!outputDirectory.isEmpty()) {
        const auto path = QDir(outputDirectory).filePath(relative);
        require(QDir().mkpath(QFileInfo(path).absolutePath()), ErrorCode::StorageFailure, QStringLiteral("Cannot create staging directory"));
        output.setFileName(path);
        require(output.open(QIODevice::WriteOnly | QIODevice::NewOnly), ErrorCode::StorageFailure, output.errorString());
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 size = 0;
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        token.throwIfCancelled();
        const auto count = input.read(buffer.data(), buffer.size());
        require(count >= 0, ErrorCode::StorageFailure, input.errorString());
        if (!count) break;
        hash.addData(QByteArrayView(buffer.constData(), count));
        size += count;
        if (output.isOpen()) require(output.write(buffer.constData(), count) == count, ErrorCode::StorageFailure, output.errorString());
    }
    if (output.isOpen()) require(output.flush(), ErrorCode::StorageFailure, output.errorString());
    return {relative, size, QString::fromLatin1(hash.result().toHex())};
}
QString entryProblem(const QString& directory, const ModelManifest& manifest)
{
    const auto path = QDir(directory).filePath(manifest.entryPoint);
    if (manifest.format == QStringLiteral("mlx")) {
        QDir model(path);
        if (!model.exists() || !QFileInfo(model.filePath(QStringLiteral("config.json"))).isFile()
            || (!QFileInfo(model.filePath(QStringLiteral("tokenizer.json"))).isFile()
                && !QFileInfo(model.filePath(QStringLiteral("tokenizer.model"))).isFile())
            || model.entryList({QStringLiteral("*.safetensors")}, QDir::Files).isEmpty())
            return QStringLiteral("MLX entry_point requires config, tokenizer and safetensors files");
    } else {
        if (!QFileInfo(path).isFile()) return QStringLiteral("entry_point is not a model file");
        if (manifest.format == QStringLiteral("gguf")) {
            QFile model(path);
            if (!model.open(QIODevice::ReadOnly) || model.read(4) != QByteArrayLiteral("GGUF")) return QStringLiteral("GGUF entry_point has invalid magic");
        }
    }
    return {};
}
ModelVerification inspect(const QString& directory, const ModelManifest& manifest, const CancellationToken& token,
                          QList<ModelFile>* inventory = nullptr, const QString& outputDirectory = {})
{
    ModelVerification result{modelUri(manifest.id)};
    QStringList paths;
    assetPaths(QDir(directory), {}, paths, token);
    std::sort(paths.begin(), paths.end());
    std::map<QString, ModelFile> expected;
    for (const auto& file : manifest.files) expected.emplace(file.path, file);
    for (const auto& path : paths) {
        const auto file = hashFile(QDir(directory), path, token, outputDirectory);
        ++result.checkedFiles;
        result.checkedBytes += file.size;
        if (inventory) inventory->append(file);
        if (manifest.files.isEmpty()) continue; // Source manifest: installation records its complete inventory.
        const auto found = expected.find(path);
        if (found == expected.end()) result.issues.append(QStringLiteral("Unlisted file: ") + path);
        else {
            if (file.size != found->second.size || file.sha256 != found->second.sha256)
                result.issues.append(QStringLiteral("Size/SHA-256 mismatch: ") + path);
            expected.erase(found);
        }
    }
    for (const auto& [path, file] : expected) result.issues.append(QStringLiteral("Missing file: ") + path);
    const auto problem = entryProblem(directory, manifest);
    if (!problem.isEmpty()) result.issues.append(problem);
    result.valid = result.issues.isEmpty();
    return result;
}
}
class ModelCatalog::Impl {
public:
    explicit Impl(QString directory)
    {
        require(!directory.trimmed().isEmpty() && !directory.startsWith(':'), ErrorCode::InvalidArgument, QStringLiteral("Models directory is required"));
        root = QFileInfo(directory).absoluteFilePath();
        require(!QDir(root).isRoot(), ErrorCode::InvalidArgument, QStringLiteral("Models directory cannot be a filesystem root"));
    }
    QString root;
    std::unique_ptr<QLockFile> lock;
    void own()
    {
        if (lock) return;
        require(QDir().mkpath(root), ErrorCode::StorageFailure, QStringLiteral("Cannot create Models directory"));
        root = QDir(root).canonicalPath();
        auto candidate = std::make_unique<QLockFile>(QDir(root).filePath(QStringLiteral(".iilocal-llm.lock")));
        candidate->setStaleLockTime(0);
        require(candidate->tryLock(0), ErrorCode::ModelInUse, QStringLiteral("Models directory is owned by another catalog/service"));
        lock = std::move(candidate);
    }
    QList<ResolvedModel> scan(QList<CatalogIssue>& issues)
    {
        own();
        QList<ResolvedModel> models;
        QSet<QString> ids;
        const auto entries = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System, QDir::Name);
        for (const auto& entry : entries) {
            if (entry.fileName().startsWith('.')) continue; // Staging/trash never appears in the public catalog.
            try {
                require(!entry.isSymbolicLink() && entry.isDir(), ErrorCode::InvalidManifest, QStringLiteral("Model directory must not be a symlink"));
                auto manifest = readManifest(entry.absoluteFilePath());
                require(!manifest.files.isEmpty(), ErrorCode::InvalidManifest, QStringLiteral("Package has no integrity inventory; use install()"));
                if (ids.contains(manifest.id)) issues.append({entry.fileName(), ErrorCode::AlreadyExists, QStringLiteral("Duplicate manifest id: ") + manifest.id});
                ids.insert(manifest.id);
                models.append({{modelUri(manifest.id), manifest}, entry.absoluteFilePath(), QDir(entry.absoluteFilePath()).filePath(manifest.entryPoint)});
            } catch (const Error& error) { issues.append({entry.fileName(), error.code(), QString::fromUtf8(error.what())}); }
        }
        return models;
    }
    ResolvedModel find(const QString& uri)
    {
        const auto id = modelId(uri);
        QList<CatalogIssue> issues;
        std::optional<ResolvedModel> found;
        for (auto& model : scan(issues)) {
            if (model.record.manifest.id != id) continue;
            require(!found.has_value(), ErrorCode::AlreadyExists, QStringLiteral("Ambiguous model URI: duplicate manifest id"));
            found = std::move(model);
        }
        require(found.has_value(), ErrorCode::NotFound, QStringLiteral("Model is not installed; inspect models.list issues for invalid packages"));
        return std::move(*found);
    }
};
ModelCatalog::ModelCatalog(QString directory) : d(std::make_unique<Impl>(std::move(directory))) {}
ModelCatalog::~ModelCatalog() = default;
ModelListing ModelCatalog::list()
{
    ModelListing listing;
    for (const auto& model : d->scan(listing.issues)) listing.models.append(model.record);
    return listing;
}
ResolvedModel ModelCatalog::resolve(const QString& uri) { return d->find(uri); }
ModelVerification ModelCatalog::verify(const QString& uri, const CancellationToken& token)
{
    token.throwIfCancelled();
    const auto model = d->find(uri);
    try { return inspect(model.directory, model.record.manifest, token); }
    catch (const Error& error) {
        if (error.code() == ErrorCode::Cancelled) throw;
        return {uri, false, 0, 0, {QString::fromUtf8(error.what())}};
    }
}
ModelRecord ModelCatalog::install(const QString& packageDirectory, const CancellationToken& token)
{
    token.throwIfCancelled();
    const QFileInfo source(packageDirectory);
    require(source.isDir() && !source.isSymbolicLink(), ErrorCode::InvalidArgument, QStringLiteral("Installation requires a local package directory"));
    const auto sourcePath = source.canonicalFilePath();
    auto manifest = readManifest(sourcePath);
    d->own();
    require(d->root != sourcePath && !d->root.startsWith(sourcePath + QLatin1Char('/')), ErrorCode::InvalidArgument,
            QStringLiteral("Models storage must not be inside the installation source"));
    QList<CatalogIssue> issues;
    for (const auto& model : d->scan(issues)) require(model.record.manifest.id != manifest.id, ErrorCode::AlreadyExists, QStringLiteral("Model id is already installed"));
    const auto destination = QDir(d->root).filePath(manifest.id);
    require(!QFileInfo::exists(destination) && !QFileInfo(destination).isSymbolicLink(), ErrorCode::AlreadyExists, QStringLiteral("Installation directory already exists"));
    QTemporaryDir staging(QDir(d->root).filePath(QStringLiteral(".install-XXXXXX")));
    require(staging.isValid(), ErrorCode::StorageFailure, QStringLiteral("Cannot create installation staging directory"));
    QList<ModelFile> inventory;
    const auto result = inspect(sourcePath, manifest, token, &inventory, staging.path());
    require(result.valid, ErrorCode::IntegrityFailure, result.issues.join(QStringLiteral("; ")));
    manifest.files = std::move(inventory);
    // Normalize and validate generated file names as well as caller-supplied inventories.
    manifest = parseModelManifest(manifestObject(manifest));
    require(!manifest.files.isEmpty(), ErrorCode::InvalidManifest, QStringLiteral("Empty model package"));
    const auto bytes = QJsonDocument(manifestObject(manifest)).toJson();
    require(bytes.size() <= maxManifestBytes, ErrorCode::ResourceLimit, QStringLiteral("Generated manifest exceeds 4 MiB"));
    QSaveFile file(QDir(staging.path()).filePath(QStringLiteral("manifest.json")));
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(), ErrorCode::StorageFailure,
            QStringLiteral("Cannot commit installed manifest"));
    const auto copied = inspect(staging.path(), manifest, token);
    require(copied.valid, ErrorCode::IntegrityFailure, QStringLiteral("Copied package verification failed: ") + copied.issues.join(QStringLiteral("; ")));
    token.throwIfCancelled();
    require(QDir().rename(staging.path(), destination), ErrorCode::StorageFailure, QStringLiteral("Cannot publish staged model directory"));
    staging.setAutoRemove(false);
    return {modelUri(manifest.id), manifest};
}
void ModelCatalog::remove(const QString& uri)
{
    const auto model = d->find(uri);
    QTemporaryDir trash(QDir(d->root).filePath(QStringLiteral(".remove-XXXXXX")));
    require(trash.isValid(), ErrorCode::StorageFailure, QStringLiteral("Cannot create removal staging directory"));
    require(QDir().rename(model.directory, QDir(trash.path()).filePath(QStringLiteral("package"))),
            ErrorCode::StorageFailure, QStringLiteral("Cannot detach model directory for removal"));
    if (!trash.remove()) {
        trash.setAutoRemove(false);
        throw Error(ErrorCode::StorageFailure, QStringLiteral("Model detached, but removal is incomplete at ") + trash.path());
    }
}
} // namespace iiLocalLLM
