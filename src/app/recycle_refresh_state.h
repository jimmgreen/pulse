#pragma once
#include <cstdint>

namespace pulse::app {
struct RecycleRefreshGate {
    uint64_t generation = 0, next_request = 0, active_request = 0, active_generation = 0;
    uint64_t fallback_at = 0;
    bool visible = false;

    void Invalidate() { ++generation; }
    uint64_t Begin() {
        if (active_request) return 0;
        active_generation = generation;
        return active_request = ++next_request;
    }
    struct Completion { bool matched = false, current = false; };
    Completion Complete(uint64_t request) {
        if (!request || request != active_request) return {};
        active_request = 0;
        return {true, active_generation == generation};
    }
};
} // namespace pulse::app
