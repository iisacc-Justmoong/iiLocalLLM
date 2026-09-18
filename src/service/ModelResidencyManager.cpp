#include "ModelResidencyManager.h"
#include <algorithm>

namespace iiLocalLLM::detail {
namespace { constexpr quint64 GiB = quint64(1) << 30; }
ModelResidencyManager::ModelResidencyManager(const ServiceOptions& o, const HardwareInfo& hardware) : limit_(o.maxModels)
{
    const auto total = hardware.ramBytes.value_or(4 * GiB);
    const auto system = std::max(2 * GiB, total / 4);
    const auto automatic = total > system ? total - system : total / 2;
    budget_ = o.memoryBudgetBytes ? std::min(o.memoryBudgetBytes, total) : automatic;
    reserve_ = o.memoryReserveBytes;
    keepAlive_ = o.keepAliveMs < 0 ? (total <= 8 * GiB ? 0 : 300000) : o.keepAliveMs;
}
quint64 ModelResidencyManager::bytes() const
{
    quint64 result = 0;
    for (const auto& [id, e] : entries_) result += e.memory.totalBytes();
    return result;
}
bool ModelResidencyManager::fits(quint64 required, std::optional<quint64> available) const
{
    const auto used = bytes();
    return entries_.size() < size_t(limit_) && used <= budget_ && required <= budget_ - used
        && (!available || (*available >= reserve_ && required <= *available - reserve_));
}
QString ModelResidencyManager::oldestIdle() const
{
    const Entry* oldest = nullptr;
    QString result;
    for (const auto& [id, e] : entries_)
        if (e.active == 0 && (!oldest || e.access < oldest->access)) { oldest = &e; result = id; }
    return result;
}
QList<QString> ModelResidencyManager::expired(Time now) const
{
    QList<QString> result;
    for (const auto& [id, e] : entries_) if (e.active == 0 && now >= e.expires) result.append(id);
    return result;
}
void ModelResidencyManager::loaded(const QString& id, MemoryEstimate memory, qint64 keepAlive, Time now)
{
    if (entries_.contains(id)) throw Error(ErrorCode::AlreadyExists, QStringLiteral("Residency already registered"));
    if (!fits(memory.totalBytes(), std::nullopt)) throw Error(ErrorCode::ResourceLimit, QStringLiteral("Model exceeds residency budget"));
    const auto ttl = keepAlive < 0 ? keepAlive_ : keepAlive;
    entries_.emplace(id, Entry{std::move(memory), ttl, now + std::chrono::milliseconds(ttl), ++clock_});
    ++loads_;
}
void ModelResidencyManager::touch(const QString& id, qint64 keepAlive, Time now)
{
    auto& e = entries_.at(id);
    if (keepAlive >= 0) e.keepAliveMs = keepAlive;
    e.access = ++clock_;
    e.expires = now + std::chrono::milliseconds(e.keepAliveMs);
}
void ModelResidencyManager::acquire(const QString& id, qint64 keepAlive) { touch(id, keepAlive); ++entries_.at(id).active; }
void ModelResidencyManager::release(const QString& id, Time now)
{
    auto& e = entries_.at(id);
    if (e.active <= 0) throw Error(ErrorCode::RuntimeFailure, QStringLiteral("Unbalanced model lease"));
    --e.active;
    touch(id, -1, now);
}
void ModelResidencyManager::erased(const QString& id, bool eviction)
{
    const auto it = entries_.find(id);
    if (it == entries_.end()) return;
    if (it->second.active) throw Error(ErrorCode::ModelInUse, QStringLiteral("Active model cannot be evicted"));
    entries_.erase(it);
    if (eviction) ++evictions_;
}
const ModelResidencyManager::Entry& ModelResidencyManager::get(const QString& id) const { return entries_.at(id); }
} // namespace iiLocalLLM::detail
