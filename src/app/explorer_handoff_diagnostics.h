#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

namespace pulse::app {
enum class HandoffPhase : uint8_t {
    Created, Submitted, Received, Prepared, CloseRequested, QuitResult, Closed,
    Cancelled, CloseFailed, Rejected, Deferred, Rollback, Committed, Presented
};

// Numeric-only, bounded to one event per phase even across STA/UI retries.
// Correlate request_id with the runtime log's process/session identifiers.
struct HandoffDiagnostics {
    using Sink = void (*)(uint64_t, HandoffPhase, uint64_t, uint64_t, uint64_t) noexcept;
    inline static std::atomic<Sink> sink{nullptr};
    inline static std::atomic<uint64_t> next_id{0};
    const uint64_t id = ++next_id;
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::atomic<uint32_t> phases{0};
    HandoffDiagnostics() noexcept { Emit(HandoffPhase::Created); }
    void Emit(HandoffPhase phase, uint64_t code = 0, uint64_t detail = 0) noexcept {
        const auto bit = uint32_t{1} << static_cast<unsigned>(phase);
        if (phases.fetch_or(bit, std::memory_order_relaxed) & bit) return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        if (const auto write = sink.load(std::memory_order_acquire))
            write(id, phase, code, detail, static_cast<uint64_t>(elapsed));
    }
};
}
