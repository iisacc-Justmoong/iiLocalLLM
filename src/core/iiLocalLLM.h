#pragma once

#include "Types.h"
#include "Parameters.h"
#include "Hardware.h"
#include "ModelManifest.h"
#include "ModelCatalog.h"
#include "Runtime.h"
#include "Service.h"
#include "LocalIpcServer.h"
#include "HttpApiServer.h"

namespace iiLocalLLM {

/// Legacy 0.1 API, retained for source and binary compatibility.
[[nodiscard]] IILOCALLLM_EXPORT QString helloWorld();

} // namespace iiLocalLLM
