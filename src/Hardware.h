#pragma once
#include "Types.h"
#include <optional>

namespace iiLocalLLM {

enum class ComputeBackend { Cpu, Metal, Cuda, Vulkan };
enum class GpuVendor { Unknown, Apple, Nvidia, Amd, Intel };
IILOCALLLM_EXPORT QString enumName(ComputeBackend backend);
IILOCALLLM_EXPORT QString enumName(GpuVendor vendor);

// An adapter as observed through an OS/driver API. IDs are opaque and local to this boot.
// The same physical GPU can have several API entries; memory must not be summed across them.
struct GpuInfo {
    QString id;
    QString name;
    GpuVendor vendor = GpuVendor::Unknown;
    std::optional<quint64> vramBytes;
    std::optional<bool> unifiedMemory;
    std::optional<quint64> recommendedWorkingSetBytes;
    QList<ComputeBackend> availableBackends;
};
struct HardwareInfo {
    QString cpuArchitecture;
    std::optional<quint64> ramBytes;
    bool appleSilicon = false;
    QList<GpuInfo> gpus;
    bool metalAvailable = false;
    bool cudaAvailable = false;
    bool vulkanAvailable = false;
    QStringList diagnostics;
};
struct RuntimeDevice {
    ComputeBackend backend = ComputeBackend::Cpu;
    QString deviceId; // Empty for CPU; otherwise a HardwareInfo GPU id.
    bool operator==(const RuntimeDevice&) const = default;
};
struct ExecutionSelection {
    QString runtime;
    RuntimeDevice device;
    QString reason;
    QStringList fallbackReasons;
};

// Read-only boot snapshot and deterministic service policy; neither accepts an app override.
IILOCALLLM_EXPORT HardwareInfo detectHardware();
IILOCALLLM_EXPORT QList<RuntimeDevice> orderExecutionDevices(
    const HardwareInfo& hardware, const QList<RuntimeDevice>& supported);
IILOCALLLM_EXPORT QJsonObject hardwareObject(const HardwareInfo& hardware);
IILOCALLLM_EXPORT QJsonObject executionObject(const ExecutionSelection& selection);

} // namespace iiLocalLLM
