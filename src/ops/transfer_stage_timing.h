#pragma once
#include <cstdint>

namespace pulse::ops {
// Worker wall time, not disk-write latency or CPU time. No per-item storage.
struct TransferStageTiming {
    uint64_t started = 0, preparation_ended = 0, first_copy_callback = 0;
    bool preparation_complete = false, copy_callback_seen = false;
    explicit TransferStageTiming(uint64_t now) noexcept : started(now) {}
    void EndPreparation(uint64_t now) noexcept {
        if (!preparation_complete) { preparation_ended = now; preparation_complete = true; }
    }
    void ObserveCopyCallback(uint64_t now, uint64_t bytes) noexcept {
        if (bytes && !copy_callback_seen) { first_copy_callback = now; copy_callback_seen = true; }
    }
    uint64_t PreparationMs(uint64_t now) const noexcept {
        return (preparation_complete ? preparation_ended : now) - started;
    }
    uint64_t ExecutionWallMs(uint64_t now) const noexcept {
        return preparation_complete ? now - preparation_ended : 0;
    }
    uint64_t FirstCopyCallbackMs() const noexcept {
        return copy_callback_seen ? first_copy_callback - started : 0;
    }
};
}
