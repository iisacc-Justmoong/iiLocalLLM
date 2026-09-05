#pragma once

#include <QtCore/QString>
#include <QtCore/qglobal.h>

#if defined(IILOCALLLM_BUILDING_LIBRARY)
#  define IILOCALLLM_EXPORT Q_DECL_EXPORT
#else
#  define IILOCALLLM_EXPORT Q_DECL_IMPORT
#endif

namespace iiLocalLLM {

/// Returns the placeholder greeting; no domain functionality is implemented.
[[nodiscard]] IILOCALLLM_EXPORT QString helloWorld();

} // namespace iiLocalLLM
