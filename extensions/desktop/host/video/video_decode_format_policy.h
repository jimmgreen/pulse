#pragma once
#include <cstdint>

namespace pulse::desktop::video {

// Only these AVC profiles are known to be 8-bit. Unknown/High10/High422/High444
// retain the existing P010-first negotiation rather than silently discarding precision.
constexpr bool IsKnownEightBitAvcProfile(uint32_t profile) noexcept {
    return profile == 66 || profile == 77 || profile == 88 || profile == 100;
}

} // namespace pulse::desktop::video
