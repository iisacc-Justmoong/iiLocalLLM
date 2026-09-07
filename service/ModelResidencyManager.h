#pragma once
#include "Hardware.h"
#include "Memory.h"
#include <chrono>
#include <map>

namespace iiLocalLLM::detail {
// Worker-confined policy. ModelManager owns eviction execution and runtime lifetimes.
class ModelResidencyManager {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    struct Entry {
        MemoryEstimate memory;
        qint64 keepAliveMs;
        Time expires;
        quint64 access;
        int active = 0;
    };
    ModelResidencyManager(const ServiceOptions& options, const HardwareInfo& hardware);
    bool fits(quint64 bytes, std::optional<quint64> available) const;
    QString oldestIdle() const;
    QList<QString> expired(Time now = Clock::now()) const;
    void loaded(const QString& id, MemoryEstimate memory, qint64 keepAlive, Time now = Clock::now());
    void acquire(const QString& id, qint64 keepAlive);
    void release(const QString& id, Time now = Clock::now());
    void touch(const QString& id, qint64 keepAlive, Time now = Clock::now());
    void erased(const QString& id, bool eviction);
    const Entry& get(const QString& id) const;
    quint64 bytes() const;
    quint64 budget() const { return budget_; }
    quint64 reserve() const { return reserve_; }
    qint64 defaultKeepAlive() const { return keepAlive_; }
    quint64 loads() const { return loads_; }
    quint64 evictions() const { return evictions_; }
private:
    int limit_;
    quint64 budget_, reserve_, clock_ = 0, loads_ = 0, evictions_ = 0;
    qint64 keepAlive_;
    std::map<QString, Entry> entries_;
};
} // namespace iiLocalLLM::detail
