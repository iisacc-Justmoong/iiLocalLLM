#include "Detection.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#elif defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace iiLocalLLM::detail {
namespace {
GpuVendor pciVendor(unsigned id)
{
    switch (id) {
    case 0x10de: return GpuVendor::Nvidia;
    case 0x1002: case 0x1022: return GpuVendor::Amd;
    case 0x8086: return GpuVendor::Intel;
    case 0x106b: return GpuVendor::Apple;
    default: return GpuVendor::Unknown;
    }
}
#ifdef Q_OS_LINUX
QByteArray read(const QString& path)
{ QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed() : QByteArray{}; }
#endif
}
void probePlatformHardware(HardwareInfo& h)
{
#ifdef Q_OS_WIN
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) h.ramBytes = memory.ullTotalPhys;
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    if (system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) h.cpuArchitecture = QStringLiteral("arm64");
    else if (system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) h.cpuArchitecture = QStringLiteral("x86_64");
    else if (system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) h.cpuArchitecture = QStringLiteral("i386");
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        for (UINT index = 0;; ++index) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(index, &adapter) != S_OK) break;
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            GpuInfo gpu;
            gpu.id = QStringLiteral("dxgi/%1:%2").arg(quint32(description.AdapterLuid.HighPart)).arg(description.AdapterLuid.LowPart);
            gpu.name = QString::fromWCharArray(description.Description);
            gpu.vendor = pciVendor(description.VendorId);
            if (description.DedicatedVideoMemory) gpu.vramBytes = description.DedicatedVideoMemory;
            h.gpus.append(gpu);
        }
    }
#elif defined(Q_OS_UNIX)
    const auto pages = sysconf(_SC_PHYS_PAGES), pageSize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && pageSize > 0) h.ramBytes = quint64(pages) * quint64(pageSize);
#ifdef Q_OS_LINUX
    QDir devices(QStringLiteral("/sys/bus/pci/devices"));
    for (const auto& entry : devices.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const auto path = devices.filePath(entry);
        bool ok = false;
        const auto deviceClass = read(path + QStringLiteral("/class")).toUInt(&ok, 0);
        if (!ok || (deviceClass >> 16) != 0x03) continue;
        GpuInfo gpu;
        gpu.id = QStringLiteral("pci/") + entry;
        gpu.vendor = pciVendor(read(path + QStringLiteral("/vendor")).toUInt(nullptr, 0));
        gpu.name = enumName(gpu.vendor) + QStringLiteral(" PCI ") + entry;
        const auto vram = read(path + QStringLiteral("/mem_info_vram_total")).toULongLong(&ok);
        if (ok && vram > 0) gpu.vramBytes = vram;
        h.gpus.append(gpu);
    }
#endif
#endif
}
} // namespace iiLocalLLM::detail
