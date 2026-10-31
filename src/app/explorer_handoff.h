#pragma once

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

enum class HandoffState { Pending, Ready, Cancelled, Expired, Closed };

// Shared across the UI and the Explorer STA; it never owns COM interfaces.
struct ExplorerHandoff {
    explicit ExplorerHandoff(uint64_t deadline) : deadline_tick(deadline) {}
    const uint64_t deadline_tick;
    std::atomic<HandoffState> state{HandoffState::Pending};
    std::atomic<bool> received{false};

    bool Receive(uint64_t now) {
        if (now >= deadline_tick) { Cancel(HandoffState::Expired); return false; }
        return state == HandoffState::Pending && !received.exchange(true);
    }

    bool Ready(uint64_t now) {
        if (now >= deadline_tick) { Cancel(HandoffState::Expired); return false; }
        auto expected = HandoffState::Pending;
        return state.compare_exchange_strong(expected, HandoffState::Ready);
    }
    void Cancel(HandoffState reason = HandoffState::Cancelled) {
        auto current = state.load();
        while ((current == HandoffState::Pending || current == HandoffState::Ready) &&
               !state.compare_exchange_weak(current, reason)) {}
    }
    bool ClaimClose(uint64_t now) {
        if (now >= deadline_tick) { Cancel(HandoffState::Expired); return false; }
        auto expected = HandoffState::Ready;
        return state.compare_exchange_strong(expected, HandoffState::Closed);
    }
};

// A tab owns the lease: closing it or replacing its navigation cancels a
// pending handoff, including a Ready acknowledgement not consumed by the STA.
struct ExplorerNavigationLease {
    std::shared_ptr<ExplorerHandoff> handoff;
    std::wstring path;
    uint64_t generation = 0;
    std::vector<std::wstring> names;
    bool listing_complete = false;
    bool initial_arm_transferred = false;
    uint64_t initial_selection_revision = 0;
    static bool FrameSucceeded(int32_t draw_result, int32_t present_result, bool hidden_capture) {
        return draw_result == 0 && present_result == 0 && !hidden_capture;
    }
    bool RecordListing(std::wstring_view current_path, uint64_t current_generation, bool success,
                  std::vector<std::wstring> selected, uint64_t now) {
        if (!handoff || generation != current_generation) return false;
        auto expected = names;
        std::sort(expected.begin(), expected.end());
        std::sort(selected.begin(), selected.end());
        if (!success || path != current_path || expected != selected) {
            handoff->Cancel();
            return false;
        }
        if (now >= handoff->deadline_tick) { handoff->Cancel(HandoffState::Expired); return false; }
        listing_complete = handoff->state == HandoffState::Pending;
        return listing_complete;
    }
    bool Retain(std::wstring_view current_path, uint64_t pending, uint64_t applied, bool visible, uint64_t now) {
        if (!handoff) return false;
        if (now >= handoff->deadline_tick) { handoff->Cancel(HandoffState::Expired); return false; }
        if (!visible || current_path != path || (pending && pending != generation) ||
            (!pending && applied != generation)) { handoff->Cancel(); return false; }
        return handoff->state == HandoffState::Pending || handoff->state == HandoffState::Ready;
    }
    bool Presented(std::wstring_view current_path, uint64_t pending, uint64_t applied,
                   const std::vector<std::wstring>& selected, bool visible, bool frame_succeeded, uint64_t now) {
        if (!Retain(current_path, pending, applied, visible, now) || !listing_complete || pending) return false;
        auto expected = names, actual = selected;
        std::sort(expected.begin(), expected.end()); std::sort(actual.begin(), actual.end());
        if (expected != actual) { handoff->Cancel(); return false; }
        return frame_succeeded && handoff->Ready(now);
    }
    ~ExplorerNavigationLease() { if (handoff) handoff->Cancel(); }
};

} // namespace pulse::app
