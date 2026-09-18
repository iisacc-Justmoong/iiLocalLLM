#pragma once
#include "ModelCatalog.h"
#include "ModelPull.h"
#include <QtCore/QUrl>
#include <map>

namespace iiLocalLLM::detail {
class ModelRegistry {
public:
    explicit ModelRegistry(const QString& file);
    QString canonical(const QString& reference) const;
    ModelRecord pull(const QString& reference, ModelCatalog& catalog, const QString& root,
                     const CancellationToken& token, const PullCallback& progress) const;
private:
    struct Package { ModelManifest manifest; QList<QUrl> urls; };
    std::map<QString, Package> packages_;
    std::map<QString, QString> aliases_;
};
} // namespace iiLocalLLM::detail
