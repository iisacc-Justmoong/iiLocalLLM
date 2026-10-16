#pragma once
#include "Types.h"
#include "Multimodal.h"

namespace iiLocalLLM {
// JSON boundary adapter for MCP media. The native policy uses only C++23 masks.
// Inline bytes only: no URL fetching or arbitrary file access during inference.
IILOCALLLM_EXPORT QJsonArray nativeContent(const QJsonArray&, int maxCharacters = 1024 * 1024);
IILOCALLLM_EXPORT Modalities contentModalities(const QJsonArray&);
IILOCALLLM_EXPORT QByteArray mediaBytes(const QJsonObject&);
IILOCALLLM_EXPORT QJsonArray mediaEvidence(const QJsonArray&); // Digests/size/type, never base64 payloads.
}
