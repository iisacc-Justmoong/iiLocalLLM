#pragma once
#include "ModelManifest.h"

namespace iiLocalLLM {

// Host-side resolution. Runtime adapters receive these paths; client APIs return ModelRecord.
struct ResolvedModel {
    ModelRecord record;
    QString directory;
    QString entryPath;
};

// Engine-independent, serial filesystem owner. One owner per Models directory across processes.
// Source bundles are copied locally; installation never downloads or executes model code.
class IILOCALLLM_EXPORT ModelCatalog {
public:
    explicit ModelCatalog(QString directory);
    ~ModelCatalog();
    ModelCatalog(const ModelCatalog&) = delete;
    ModelCatalog& operator=(const ModelCatalog&) = delete;
    ModelRecord install(const QString& packageDirectory, const CancellationToken& cancellation = {});
    void remove(const QString& uri);
    ModelListing list();
    ResolvedModel resolve(const QString& uri);
    ModelVerification verify(const QString& uri, const CancellationToken& cancellation = {});
private:
    class Impl;
    std::unique_ptr<Impl> d;
};
} // namespace iiLocalLLM
