#include "ModelRegistry.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QStorageInfo>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <limits>

namespace iiLocalLLM::detail {
namespace {
void require(bool ok, const QString& message)
{ if (!ok) throw Error(ErrorCode::InvalidManifest, message); }
bool allowedUrl(const QUrl& url)
{
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasFragment()
        && (url.scheme() == "https" || (url.scheme() == "http" && QHostAddress(url.host()).isLoopback()));
}
void download(const QUrl& url, const ModelFile& file, const QString& destination,
              const QString& uri, const CancellationToken& token, const PullCallback& progress)
{
    QFile output(destination);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) throw Error(ErrorCode::StorageFailure, output.errorString());
    QNetworkAccessManager network;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setMaximumRedirectsAllowed(8);
    request.setTransferTimeout(30000);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("User-Agent", "iiLocalLLMD/0.2.0");
    std::unique_ptr<QNetworkReply> reply(network.get(request));
    reply->setReadBufferSize(1024 * 1024);
    QEventLoop loop;
    QTimer poll; poll.setInterval(50);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 received = 0, reported = -1;
    std::exception_ptr failure;
    auto drain = [&] {
        if (failure) return;
        try {
            token.throwIfCancelled();
            // A redirect response body is not part of the model asset.
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status >= 300 && status < 400) { reply->readAll(); return; }
            const auto data = reply->readAll();
            if (data.size() > file.size - received) throw Error(ErrorCode::IntegrityFailure, QStringLiteral("Download exceeds pinned file size"));
            if (output.write(data) != data.size()) throw Error(ErrorCode::StorageFailure, output.errorString());
            hash.addData(data); received += data.size();
        } catch (...) { failure = std::current_exception(); reply->abort(); }
    };
    QObject::connect(reply.get(), &QNetworkReply::readyRead, &loop, drain);
    QObject::connect(reply.get(), &QNetworkReply::redirected, &loop, [&](const QUrl& target) {
        if (!allowedUrl(target) || (url.scheme() == "https" && target.scheme() != "https")) {
            failure = std::make_exception_ptr(Error(ErrorCode::InvalidManifest, QStringLiteral("Download redirect violates registry transport policy")));
            reply->abort();
        } else reply->redirectAllowed();
    });
    QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        try {
            token.throwIfCancelled();
            if (progress && received != reported) { progress({uri, file.path, received, file.size}); reported = received; }
        } catch (...) { failure = std::current_exception(); reply->abort(); }
    });
    poll.start();
    if (!reply->isFinished()) loop.exec();
    drain();
    if (failure) std::rethrow_exception(failure);
    token.throwIfCancelled();
    if (reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
        throw Error(ErrorCode::RuntimeFailure, QStringLiteral("Model download failed: ") + reply->errorString());
    if (received != file.size || QString::fromLatin1(hash.result().toHex()) != file.sha256)
        throw Error(ErrorCode::IntegrityFailure, QStringLiteral("Downloaded size or SHA-256 does not match the registry"));
    if (!output.flush()) throw Error(ErrorCode::StorageFailure, output.errorString());
    if (progress) progress({uri, file.path, received, file.size});
}
}
ModelRegistry::ModelRegistry(const QString& path)
{
    QFile file(path.isEmpty() ? QStringLiteral(":/iiLocalLLM/registry.json") : path);
    if (!file.open(QIODevice::ReadOnly)) throw Error(ErrorCode::StorageFailure, file.errorString());
    require(file.size() <= 1024 * 1024, QStringLiteral("Registry exceeds 1 MiB"));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    require(error.error == QJsonParseError::NoError && document.isObject() && document.object().value("models").isArray(),
        QStringLiteral("Registry must contain a models array"));
    for (const auto& value : document.object().value("models").toArray()) {
        const auto object = value.toObject();
        Package package{parseModelManifest(object.value("manifest").toObject()), {}};
        require(!package.manifest.files.isEmpty(), QStringLiteral("Registry files need pinned size and SHA-256"));
        const auto sources = object.value("sources").toObject();
        require(sources.size() == package.manifest.files.size(), QStringLiteral("Registry sources must match every manifest file"));
        for (const auto& file : package.manifest.files) {
            const QUrl url(sources.value(file.path).toString(), QUrl::StrictMode);
            require(allowedUrl(url) && file.size > 0 && file.size <= (qint64(1) << 40) && file.sha256.size() == 64,
                QStringLiteral("Registry source requires HTTPS (or literal loopback HTTP), a positive size and SHA-256"));
            package.urls.append(url);
        }
        const auto uri = modelUri(package.manifest.id);
        require(packages_.emplace(uri, package).second, QStringLiteral("Duplicate registry model"));
        require(object.value("aliases").isArray(), QStringLiteral("Registry aliases must be an array"));
        static const QRegularExpression aliasPattern(QStringLiteral("^[a-z0-9][a-z0-9._-]*(?::[a-z0-9][a-z0-9._-]*)?$"));
        for (const auto& alias : object.value("aliases").toArray()) {
            require(alias.isString() && alias.toString().size() <= 128 && aliasPattern.match(alias.toString()).hasMatch(), QStringLiteral("Invalid model alias"));
            require(aliases_.emplace(alias.toString(), uri).second, QStringLiteral("Duplicate registry alias"));
        }
    }
}
QString ModelRegistry::canonical(const QString& reference) const
{
    const auto it = aliases_.find(reference);
    if (it != aliases_.end()) return it->second;
    (void)modelId(reference); // Preserve strict model:// identifiers and reject file paths.
    return reference;
}
ModelRecord ModelRegistry::pull(const QString& reference, ModelCatalog& catalog, const QString& root,
                              const CancellationToken& token, const PullCallback& progress) const
{
    const auto uri = canonical(reference);
    token.throwIfCancelled();
    try {
        const auto existing = catalog.resolve(uri).record;
        const auto check = catalog.verify(uri, token);
        if (!check.valid) throw Error(ErrorCode::IntegrityFailure, check.issues.join(QStringLiteral("; ")));
        return existing;
    } catch (const Error& error) { if (error.code() != ErrorCode::NotFound) throw; }
    const auto entry = packages_.find(uri);
    if (entry == packages_.end()) throw Error(ErrorCode::NotFound, QStringLiteral("Model has no download source in the service registry"));
    const auto& package = entry->second;
    qint64 bytes = 0;
    for (const auto& file : package.manifest.files) {
        if (file.size > (std::numeric_limits<qint64>::max() - bytes) / 2) throw Error(ErrorCode::ResourceLimit, QStringLiteral("Package is too large"));
        bytes += file.size * 2; // Download staging + atomic catalog copy coexist during installation.
    }
    const QStorageInfo storage(root);
    if (storage.isValid() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < bytes)
        throw Error(ErrorCode::StorageFailure, QStringLiteral("Insufficient disk space for download and atomic installation"));
    QTemporaryDir stage(QDir(root).filePath(QStringLiteral(".pull-XXXXXX")));
    if (!stage.isValid()) throw Error(ErrorCode::StorageFailure, QStringLiteral("Cannot create pull staging directory"));
    for (qsizetype i = 0; i < package.manifest.files.size(); ++i) {
        token.throwIfCancelled();
        const auto& file = package.manifest.files[i];
        const auto destination = stage.filePath(file.path);
        if (!QDir().mkpath(QFileInfo(destination).absolutePath())) throw Error(ErrorCode::StorageFailure, QStringLiteral("Cannot create model asset directory"));
        download(package.urls[i], file, destination, uri, token, progress);
    }
    QFile manifest(stage.filePath(QStringLiteral("manifest.json")));
    if (!manifest.open(QIODevice::WriteOnly | QIODevice::NewOnly)) throw Error(ErrorCode::StorageFailure, manifest.errorString());
    const auto json = QJsonDocument(manifestObject(package.manifest)).toJson();
    if (manifest.write(json) != json.size() || !manifest.flush()) throw Error(ErrorCode::StorageFailure, manifest.errorString());
    manifest.close();
    token.throwIfCancelled();
    return catalog.install(stage.path(), token);
}
} // namespace iiLocalLLM::detail
