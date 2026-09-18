#pragma once
#include "Memory.h"
#include <limits>

namespace iiLocalLLM::detail {
inline quint64 kvReservation(quint64 layers, quint64 heads, quint64 key, quint64 value, int tokens, int contextSlots)
{
    if (tokens < 1 || contextSlots < 1) throw Error(ErrorCode::InvalidArgument, QStringLiteral("Context reservation dimensions must be positive"));
    if (!layers || !heads || !key || !value) return 0;
    quint64 bytes = 2; // f16 K/V. Runtime scratch and allocator overhead are reserved separately.
    if (key > 1048576 || value > 1048576) throw Error(ErrorCode::ResourceLimit, QStringLiteral("Invalid attention dimensions"));
    for (auto factor : {layers, heads, key + value, quint64(tokens), quint64(contextSlots)}) {
        if (factor > std::numeric_limits<quint64>::max() / bytes)
            throw Error(ErrorCode::ResourceLimit, QStringLiteral("Context memory estimate overflows"));
        bytes *= factor;
    }
    return bytes;
}
} // namespace iiLocalLLM::detail
