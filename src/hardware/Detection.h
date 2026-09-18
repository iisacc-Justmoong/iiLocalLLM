#pragma once
#include "Hardware.h"

namespace iiLocalLLM::detail {
GpuVendor gpuVendor(const QString& description);
void probePlatformHardware(HardwareInfo& hardware);
void probeLlamaHardware(HardwareInfo& hardware);
} // namespace iiLocalLLM::detail
