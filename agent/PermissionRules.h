#pragma once
#include "Types.h"

namespace iiLocalLLM::agent {
// Split comma/whitespace lists outside argument parentheses and validate rules.
// Throws on malformed input; at most 256 rules / 64 KiB per list.
IILOCALLLM_EXPORT QStringList parsePermissionRules(const QStringList&);
}
