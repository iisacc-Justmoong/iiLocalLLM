#include "Hardware.h"
#include "hardware/Detection.h"
#include <QtCore/QJsonArray>
#include <QtCore/QSysInfo>
#include <algorithm>

namespace iiLocalLLM {
QString enumName(ComputeBackend backend)
{
    switch (backend) {
    case ComputeBackend::Cpu: return QStringLiteral("cpu");
    case ComputeBackend::Metal: return QStringLiteral("metal");
    case ComputeBackend::Cuda: return QStringLiteral("cuda");
    case ComputeBackend::Vulkan: return QStringLiteral("vulkan");
    }
    return {};
}
QString enumName(GpuVendor vendor)
{
    switch (vendor) {
    case GpuVendor::Apple: return QStringLiteral("apple");
    case GpuVendor::Nvidia: return QStringLiteral("nvidia");
    case GpuVendor::Amd: return QStringLiteral("amd");
    case GpuVendor::Intel: return QStringLiteral("intel");
    case GpuVendor::Unknown: return QStringLiteral("unknown");
    }
    return {};
}
namespace detail {
GpuVendor gpuVendor(const QString& name)
{
    if (name.contains(QStringLiteral("Apple"), Qt::CaseInsensitive)) return GpuVendor::Apple;
    if (name.contains(QStringLiteral("NVIDIA"), Qt::CaseInsensitive)) return GpuVendor::Nvidia;
    if (name.contains(QStringLiteral("AMD"), Qt::CaseInsensitive)
        || name.contains(QStringLiteral("Radeon"), Qt::CaseInsensitive)) return GpuVendor::Amd;
    if (name.contains(QStringLiteral("Intel"), Qt::CaseInsensitive)) return GpuVendor::Intel;
    return GpuVendor::Unknown;
}
}
HardwareInfo detectHardware()
{
    // Driver initialization and the immutable inventory are shared by services in this process.
    static const HardwareInfo snapshot = [] {
        HardwareInfo h;
        h.cpuArchitecture = QSysInfo::currentCpuArchitecture();
        detail::probePlatformHardware(h);
        detail::probeLlamaHardware(h);
        for (const auto& gpu : h.gpus) {
            h.metalAvailable |= gpu.availableBackends.contains(ComputeBackend::Metal);
            h.cudaAvailable |= gpu.availableBackends.contains(ComputeBackend::Cuda);
            h.vulkanAvailable |= gpu.availableBackends.contains(ComputeBackend::Vulkan);
        }
        return h;
    }();
    return snapshot;
}
QList<RuntimeDevice> orderExecutionDevices(const HardwareInfo& h, const QList<RuntimeDevice>& supported)
{
    struct Ranked { RuntimeDevice device; int rank; quint64 memory; };
    QList<Ranked> ranked;
    for (const auto& device : supported) {
        if (std::any_of(ranked.begin(), ranked.end(), [&](const auto& item) { return item.device == device; })) continue;
        if (device.backend == ComputeBackend::Cpu) {
            if (device.deviceId.isEmpty()) ranked.append({device, 3, 0});
            continue;
        }
        const auto gpu = std::find_if(h.gpus.begin(), h.gpus.end(), [&](const auto& g) { return g.id == device.deviceId; });
        if (gpu == h.gpus.end() || !gpu->availableBackends.contains(device.backend)) continue;
        int rank = -1;
        if (h.appleSilicon && h.metalAvailable && gpu->vendor == GpuVendor::Apple && device.backend == ComputeBackend::Metal) rank = 0;
        else if (h.cudaAvailable && gpu->vendor == GpuVendor::Nvidia && device.backend == ComputeBackend::Cuda) rank = 1;
        else if (h.vulkanAvailable && (gpu->vendor == GpuVendor::Amd || gpu->vendor == GpuVendor::Intel)
                 && device.backend == ComputeBackend::Vulkan) rank = 2;
        if (rank >= 0) ranked.append({device, rank, gpu->vramBytes.value_or(gpu->recommendedWorkingSetBytes.value_or(0))});
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.memory != b.memory) return a.memory > b.memory;
        return a.device.deviceId < b.device.deviceId;
    });
    QList<RuntimeDevice> result;
    for (const auto& item : ranked) result.append(item.device);
    return result;
}
namespace {
QJsonValue bytes(const std::optional<quint64>& n)
{ return n ? QJsonValue(qint64(*n)) : QJsonValue(QJsonValue::Null); }
}
QJsonObject hardwareObject(const HardwareInfo& h)
{
    QJsonArray gpus;
    for (const auto& gpu : h.gpus) {
        QJsonArray backends;
        for (const auto backend : gpu.availableBackends) backends.append(enumName(backend));
        gpus.append(QJsonObject{{QStringLiteral("id"), gpu.id}, {QStringLiteral("name"), gpu.name},
            {QStringLiteral("vendor"), enumName(gpu.vendor)}, {QStringLiteral("vram_bytes"), bytes(gpu.vramBytes)},
            {QStringLiteral("unified_memory"), gpu.unifiedMemory ? QJsonValue(*gpu.unifiedMemory) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("recommended_working_set_bytes"), bytes(gpu.recommendedWorkingSetBytes)},
            {QStringLiteral("available_backends"), backends}});
    }
    return {{QStringLiteral("cpu_architecture"), h.cpuArchitecture}, {QStringLiteral("ram_bytes"), bytes(h.ramBytes)},
        {QStringLiteral("apple_silicon"), h.appleSilicon}, {QStringLiteral("gpus"), gpus},
        {QStringLiteral("metal_available"), h.metalAvailable}, {QStringLiteral("cuda_available"), h.cudaAvailable},
        {QStringLiteral("vulkan_available"), h.vulkanAvailable}, {QStringLiteral("diagnostics"), QJsonArray::fromStringList(h.diagnostics)}};
}
QJsonObject executionObject(const ExecutionSelection& s)
{
    return {{QStringLiteral("runtime"), s.runtime}, {QStringLiteral("backend"), enumName(s.device.backend)},
        {QStringLiteral("device_id"), s.device.deviceId}, {QStringLiteral("reason"), s.reason},
        {QStringLiteral("fallback_reasons"), QJsonArray::fromStringList(s.fallbackReasons)}};
}
} // namespace iiLocalLLM
