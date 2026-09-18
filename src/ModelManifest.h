#pragma once
#include "Types.h"

namespace iiLocalLLM {

struct ModelFile {
    QString path;
    qint64 size = 0;
    QString sha256;
    bool operator==(const ModelFile&) const = default;
};
struct ModelManifest {
    QString id;
    QString architecture;
    QString format;
    QString quantization;
    int contextLength = 0;
    QStringList capabilities;
    QString entryPoint;
    QList<ModelFile> files;
};
struct ModelRecord {
    QString uri;
    ModelManifest manifest;
    bool loaded = false;
};
struct CatalogIssue {
    QString directory; // Relative package directory, for diagnosing an unreadable manifest.
    ErrorCode code = ErrorCode::InvalidManifest;
    QString message;
};
struct ModelListing {
    QList<ModelRecord> models;
    QList<CatalogIssue> issues;
};
struct ModelVerification {
    QString model;
    bool valid = false;
    int checkedFiles = 0;
    qint64 checkedBytes = 0;
    QStringList issues;
};

IILOCALLLM_EXPORT QString modelUri(const QString& id);
IILOCALLLM_EXPORT QString modelId(const QString& uri);
IILOCALLLM_EXPORT ModelManifest parseModelManifest(const QJsonObject& object);
IILOCALLLM_EXPORT QJsonObject manifestObject(const ModelManifest& manifest, bool includeFiles = true);
IILOCALLLM_EXPORT QJsonObject modelRecordObject(const ModelRecord& model);
IILOCALLLM_EXPORT QJsonObject modelListingObject(const ModelListing& listing);
IILOCALLLM_EXPORT QJsonObject verificationObject(const ModelVerification& verification);

} // namespace iiLocalLLM
