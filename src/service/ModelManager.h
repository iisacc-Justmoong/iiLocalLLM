#pragma once
#include "Managers.h"
#include "ModelCatalog.h"
#include "ModelInfo.h"
#include "ModelResidencyManager.h"
#include "ModelRegistry.h"

namespace iiLocalLLM::detail {

class ModelManager {
public:
    ModelManager(const ServiceOptions& options, HardwareInfo hardware);
    void addRuntime(std::shared_ptr<Runtime> runtime);
    ModelRecord install(const QString& directory, const CancellationToken& token);
    ModelRecord pull(const QString& reference, const CancellationToken& token, const PullCallback& progress);
    QString canonical(const QString& reference) const { return registry_.canonical(reference); }
    void remove(const QString& uri);
    ModelListing list();
    ModelRecord resolve(const QString& uri);
    ModelVerification verify(const QString& uri, const CancellationToken& token);
    ModelInfo load(const ModelLoadRequest& request, const CancellationToken& token);
    LoadedModel& acquire(const QString& id, qint64 keepAlive, const CancellationToken& token);
    void release(const QString& id);
    void maintain();
    void describe(ServiceStats& stats) const;
    void setBeforeUnload(std::function<void(const QString&)> callback) { beforeUnload_ = std::move(callback); }
    void setChanged(std::function<void(QList<ModelInfo>)> callback) { changed_ = std::move(callback); }
    void unload(const QString& uri);
    QList<ModelInfo> loaded() const;
    ModelInfo loadedInfo(const QString& id) const;
    LoadedModel& get(const QString& id);
private:
    ServiceOptions options_;
    ModelCatalog catalog_;
    ModelRegistry registry_;
    RuntimeManager runtimes_;
    ModelResidencyManager residency_;
    std::map<QString, ModelInfo> loaded_;
    std::map<QString, ModelLoadRequest> remembered_;
    std::function<void(const QString&)> beforeUnload_;
    std::function<void(QList<ModelInfo>)> changed_;
    void publish() const { if (changed_) changed_(loaded()); }
    void evict(const QString& id, bool automatic);
};
} // namespace iiLocalLLM::detail
