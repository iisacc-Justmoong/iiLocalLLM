#pragma once
#include "Runtime.h"
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

// GGML stores Q8_0 and Q4_0 in blocks of 32 values, including a 2-byte scale.
// llama.cpp rounds each context to 256 tokens. This is an estimate of KV
// tensor payloads, not a reservation of GPU memory or a total process limit.
inline quint64 llamaKvReservation(quint64 layers, quint64 heads, quint64 key, quint64 value,
                                  int tokens, int contextSlots, LlamaKvCache cache)
{
    if (tokens < 1 || contextSlots < 1)
        throw Error(ErrorCode::InvalidArgument, QStringLiteral("Context reservation dimensions must be positive"));
    if (!layers || !heads || !key || !value) return 0;
    if (key > 1048576 || value > 1048576)
        throw Error(ErrorCode::ResourceLimit, QStringLiteral("Invalid attention dimensions"));
    if (cache != LlamaKvCache::F16 && cache != LlamaKvCache::Q8_0 && cache != LlamaKvCache::Q4_0)
        throw Error(ErrorCode::InvalidArgument, QStringLiteral("Unsupported llama KV cache type"));
    auto multiply = [](quint64 left, quint64 right) {
        if (right && left > std::numeric_limits<quint64>::max() / right)
            throw Error(ErrorCode::ResourceLimit, QStringLiteral("Context memory estimate overflows"));
        return left * right;
    };
    // Unsupported head widths cannot safely claim a quantized saving.
    if (key % 32 || value % 32) cache = LlamaKvCache::F16;
    const auto combined = key + value; // each dimension was bounded above
    quint64 bytes = cache == LlamaKvCache::F16 ? multiply(combined, 2)
        : multiply(combined / 32, cache == LlamaKvCache::Q8_0 ? 34 : 18);
    const quint64 paddedTokens = (quint64(tokens) + 255) / 256 * 256;
    for (const auto factor : {layers, heads, paddedTokens, quint64(contextSlots)}) bytes = multiply(bytes, factor);
    return bytes;
}
} // namespace iiLocalLLM::detail
