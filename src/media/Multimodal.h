#pragma once
#include <cstdint>

namespace iiLocalLLM {
// Native input requirements. Video is an ordered sequence of timestamped images.
enum class Modality : std::uint32_t { Text = 1, Image = 2, Audio = 4, Video = 8 };
using Modalities = std::uint32_t;
constexpr Modalities modality(Modality value) noexcept { return static_cast<Modalities>(value); }
constexpr Modalities allModalities = 15;
constexpr bool supportsModalities(Modalities available, Modalities required) noexcept {
    return (available & required) == required;
}
}
