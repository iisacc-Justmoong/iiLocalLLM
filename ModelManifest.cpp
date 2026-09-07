#include "ModelManifest.h"
#include <QtCore/QJsonArray>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <cmath>

namespace iiLocalLLM {
namespace {
void require(bool condition, const QString& message)
{ if (!condition) throw Error(ErrorCode::InvalidManifest, message); }
bool validId(const QString& id)
{
    static const QRegularExpression syntax(QStringLiteral("^[a-z0-9](?:[a-z0-9._-]{0,126}[a-z0-9])?$"));
    static const QRegularExpression reserved(QStringLiteral("^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\\.|$)"));
    return syntax.match(id).hasMatch() && !reserved.match(id).hasMatch();
}
QString text(const QJsonObject& o, const char* field, int limit = 128)
{
    const auto value = o.value(QString::fromLatin1(field));
    require(value.isString() && !value.toString().trimmed().isEmpty() && value.toString().size() <= limit,
            QString::fromLatin1(field) + QStringLiteral(" must be a nonempty bounded string"));
    return value.toString();
}
bool relativeFile(const QString& path)
{
    if (path.isEmpty() || path.size() > 1024 || path.startsWith('/') || path.contains('\\') || path.contains(':') || path.contains(QChar(0))) return false;
    for (const auto& part : path.split('/')) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..") || part.endsWith('.') || part.endsWith(' ')) return false;
    }
    return true;
}
}
QString modelUri(const QString& id)
{
    if (!validId(id)) throw Error(ErrorCode::InvalidArgument, QStringLiteral("Invalid portable model id"));
    return QStringLiteral("model://") + id;
}
QString modelId(const QString& uri)
{
    if (!uri.startsWith(QStringLiteral("model://")) || !validId(uri.sliced(8)))
        throw Error(ErrorCode::InvalidArgument, QStringLiteral("Expected a canonical model://id without paths, query, fragment or escaping"));
    return uri.sliced(8);
}
ModelManifest parseModelManifest(const QJsonObject& o)
{
    static const QSet<QString> fields{"schema_version", "id", "architecture", "format", "quantization", "context_length", "capabilities", "entry_point", "files"};
    for (auto it = o.begin(); it != o.end(); ++it) require(fields.contains(it.key()), QStringLiteral("Unknown manifest field: ") + it.key());
    const auto version = o.value(QStringLiteral("schema_version"));
    require(version.isUndefined() || (version.isDouble() && version.toDouble() == 1), QStringLiteral("Unsupported manifest schema_version"));
    ModelManifest m;
    m.id = text(o, "id");
    require(validId(m.id), QStringLiteral("Invalid portable manifest id"));
    m.architecture = text(o, "architecture");
    m.format = text(o, "format", 32);
    require(QRegularExpression(QStringLiteral("^[a-z][a-z0-9-]*$")).match(m.format).hasMatch(), QStringLiteral("Invalid format name"));
    m.quantization = text(o, "quantization", 64);
    const auto context = o.value(QStringLiteral("context_length"));
    require(context.isDouble() && context.toDouble() == context.toInt() && context.toInt() >= 2 && context.toInt() <= 1024 * 1024,
            QStringLiteral("context_length must be an integer in [2, 1048576]"));
    m.contextLength = context.toInt();
    const auto capabilities = o.value(QStringLiteral("capabilities"));
    require(capabilities.isArray() && !capabilities.toArray().isEmpty() && capabilities.toArray().size() <= 32,
            QStringLiteral("capabilities must contain 1 to 32 names"));
    for (const auto& value : capabilities.toArray()) {
        require(value.isString() && QRegularExpression(QStringLiteral("^[a-z][a-z0-9-]{0,63}$")).match(value.toString()).hasMatch()
                && !m.capabilities.contains(value.toString()), QStringLiteral("Invalid or duplicate capability"));
        m.capabilities.append(value.toString());
    }
    if (o.contains(QStringLiteral("entry_point"))) m.entryPoint = text(o, "entry_point", 1024);
    else if (m.format == QStringLiteral("gguf")) m.entryPoint = QStringLiteral("model.gguf");
    else if (m.format == QStringLiteral("mlx")) m.entryPoint = QStringLiteral(".");
    else throw Error(ErrorCode::InvalidManifest, QStringLiteral("Custom formats require an explicit entry_point"));
    require((m.entryPoint == QStringLiteral(".") && m.format == QStringLiteral("mlx")) || relativeFile(m.entryPoint),
            QStringLiteral("entry_point must remain inside the model package"));
    const auto files = o.value(QStringLiteral("files"));
    require(files.isUndefined() || files.isArray(), QStringLiteral("files must be an array"));
    require(files.toArray().size() <= 10000, QStringLiteral("Too many manifest files"));
    QSet<QString> seen;
    for (const auto& value : files.toArray()) {
        require(value.isObject(), QStringLiteral("Each file record must be an object"));
        const auto file = value.toObject();
        require(file.size() == 3, QStringLiteral("File records require only path, size and sha256"));
        ModelFile f;
        f.path = text(file, "path", 1024);
        require(relativeFile(f.path) && f.path != QStringLiteral("manifest.json") && !seen.contains(f.path.toCaseFolded()),
                QStringLiteral("Invalid or colliding file path: ") + f.path);
        const auto size = file.value(QStringLiteral("size"));
        require(size.isDouble() && size.toDouble() >= 0 && size.toDouble() <= 9007199254740991.0
                && std::trunc(size.toDouble()) == size.toDouble(), QStringLiteral("Invalid file size"));
        f.size = size.toInteger();
        f.sha256 = text(file, "sha256", 64).toLower();
        require(QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(f.sha256).hasMatch(), QStringLiteral("Invalid SHA-256"));
        seen.insert(f.path.toCaseFolded());
        m.files.append(f);
    }
    return m;
}
QJsonObject manifestObject(const ModelManifest& m, bool includeFiles)
{
    QJsonObject out{{"schema_version", 1}, {"id", m.id}, {"architecture", m.architecture}, {"format", m.format},
        {"quantization", m.quantization}, {"context_length", m.contextLength}, {"capabilities", QJsonArray::fromStringList(m.capabilities)},
        {"entry_point", m.entryPoint}};
    if (includeFiles) {
        QJsonArray files;
        for (const auto& file : m.files) files.append(QJsonObject{{"path", file.path}, {"size", file.size}, {"sha256", file.sha256}});
        out.insert(QStringLiteral("files"), files);
    }
    return out;
}
QJsonObject modelRecordObject(const ModelRecord& m)
{ return {{"model", m.uri}, {"manifest", manifestObject(m.manifest, false)}, {"loaded", m.loaded}}; }
QJsonObject modelListingObject(const ModelListing& listing)
{
    QJsonArray models, issues;
    for (const auto& model : listing.models) models.append(modelRecordObject(model));
    for (const auto& issue : listing.issues) issues.append(QJsonObject{{"directory", issue.directory}, {"code", enumName(issue.code)}, {"message", issue.message}});
    return {{"models", models}, {"issues", issues}};
}
QJsonObject verificationObject(const ModelVerification& v)
{
    return {{"model", v.model}, {"valid", v.valid}, {"checked_files", v.checkedFiles}, {"checked_bytes", v.checkedBytes},
        {"issues", QJsonArray::fromStringList(v.issues)}};
}
} // namespace iiLocalLLM
