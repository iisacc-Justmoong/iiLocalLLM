#include "Detection.h"
#include <sys/sysctl.h>
#import <Metal/Metal.h>

namespace iiLocalLLM::detail {
void probePlatformHardware(HardwareInfo& h)
{
    uint64_t memory = 0;
    size_t size = sizeof(memory);
    if (sysctlbyname("hw.memsize", &memory, &size, nullptr, 0) == 0) h.ramBytes = memory;
    int arm64 = 0;
    size = sizeof(arm64);
    h.appleSilicon = sysctlbyname("hw.optional.arm64", &arm64, &size, nullptr, 0) == 0 && arm64 != 0;
    // Native architecture, including when the service runs under Rosetta.
    if (h.appleSilicon) h.cpuArchitecture = QStringLiteral("arm64");
    @autoreleasepool {
        for (id<MTLDevice> device in MTLCopyAllDevices()) {
            GpuInfo gpu;
            gpu.id = QStringLiteral("metal/%1").arg(device.registryID);
            gpu.name = QString::fromUtf8(device.name.UTF8String);
            gpu.vendor = gpuVendor(gpu.name);
            gpu.unifiedMemory = bool(device.hasUnifiedMemory);
            gpu.recommendedWorkingSetBytes = device.recommendedMaxWorkingSetSize;
            // Metal's recommended working set is a budget, never dedicated VRAM.
            id<MTLCommandQueue> queue = [device newCommandQueue];
            id<MTLCommandBuffer> command = [queue commandBuffer];
            if (command) {
                [command commit];
                [command waitUntilCompleted];
                if (command.status == MTLCommandBufferStatusCompleted) gpu.availableBackends.append(ComputeBackend::Metal);
            }
            if (gpu.availableBackends.isEmpty()) h.diagnostics.append(QStringLiteral("Metal initialization failed: ") + gpu.name);
            h.gpus.append(gpu);
        }
    }
}
} // namespace iiLocalLLM::detail
