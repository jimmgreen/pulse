#pragma once
#include <cstdint>

namespace pulse::app {
// The shell host expires sessions after 120 seconds. Renew prewarmed sessions
// well before that deadline; a cached layout is not a live COM session.
inline bool CanReuseContextSession(uint64_t started, uint64_t now) noexcept {
    return started != 0 && now >= started && now - started < 60000;
}
}
