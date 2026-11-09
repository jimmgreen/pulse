#pragma once

#include "explorer_handoff_diagnostics.h"
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

enum class HandoffState { Pending, Ready, Cancelled, Expired, Closed, Closing };

// Shared across the UI and the Explorer STA; it never owns COM interfaces.
struct ExplorerHandoff {
    explicit ExplorerHandoff(uint64_t deadline) : deadline_tick(deadline) {}
    const uint64_t deadline_tick;
    HandoffDiagnostics diagnostics;
    std::atomic<HandoffState> state{HandoffState::Pending};
    std::atomic<bool> received{false};

    bool Receive(uint64_t now) {
        if (now >= deadline_tick) { Cancel(HandoffState::Expired); return false; }
        const bool accepted = state == HandoffState::Pending && !received.exchange(true);
        if (accepted) diagnostics.Emit(HandoffPhase::Received);
        return accepted;
    }

    bool Ready(uint64_t now) {
        if (now >= deadline_tick) { Cancel(HandoffState::Expired); return false; }
        auto expected = HandoffState::Pending;
        const bool changed = state.compare_exchange_strong(expected, HandoffState::Ready);
        if (changed) diagnostics.Emit(HandoffPhase::Prepared);
        return changed;
    }
    void Cancel(HandoffState reason = HandoffState::Cancelled) {
        auto current = state.load();
        while ((current == HandoffState::Pending || current == HandoffState::Ready) &&
               !state.compare_exchange_weak(current, reason)) {}
        if (current == HandoffState::Pending || current == HandoffState::Ready)
            diagnostics.Emit(HandoffPhase::Cancelled, static_cast<uint64_t>(reason));
    }
    bool ClaimClose(uint64_t now) {
        if (now >= deadline_tick) { Cancel(HandoffState::Expired); return false; }
        auto expected = HandoffState::Ready;
        const bool changed = state.compare_exchange_strong(expected, HandoffState::Closing);
        if (changed) diagnostics.Emit(HandoffPhase::CloseRequested);
        return changed;
    }
    // Quit accepting the command does not prove the original window closed.
    // Once issued, cancellation must wait for the STA's observed outcome.
    bool ConfirmClose(bool source_gone) {
        if (!source_gone) return false;
        auto expected = HandoffState::Closing;
        const bool changed = state.compare_exchange_strong(expected, HandoffState::Closed);
        if (changed) diagnostics.Emit(HandoffPhase::Closed);
        return changed;
    }
    bool FailClose(HandoffState reason = HandoffState::Cancelled) {
        if (reason != HandoffState::Cancelled && reason != HandoffState::Expired) return false;
        auto expected = HandoffState::Closing;
        const bool changed = state.compare_exchange_strong(expected, reason);
        if (changed) diagnostics.Emit(HandoffPhase::CloseFailed, static_cast<uint64_t>(reason));
        return changed;
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
    bool presentation_complete = false;
    uint64_t prepared_tick = 0;
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
        const auto state = handoff->state.load();
        const bool closing = state == HandoffState::Closing || state == HandoffState::Closed;
        if (!closing && now >= handoff->deadline_tick) { handoff->Cancel(HandoffState::Expired); return false; }
        listing_complete = closing || state == HandoffState::Pending;
        return listing_complete;
    }
    bool RetainCommittedRefresh(std::wstring_view current_path, bool background) const {
        return background && handoff && (handoff->state == HandoffState::Closing || handoff->state == HandoffState::Closed) &&
            !presentation_complete && current_path == path;
    }
    bool Retain(std::wstring_view current_path, uint64_t pending, uint64_t applied, bool visible, uint64_t now) {
        if (!handoff) return false;
        const bool closed = handoff->state == HandoffState::Closed;
        if (!closed && now >= handoff->deadline_tick) { handoff->Cancel(HandoffState::Expired); return false; }
        if (!visible || current_path != path || (pending && pending != generation) ||
            (!pending && applied != generation)) { handoff->Cancel(); return false; }
        return closed || handoff->state == HandoffState::Pending || handoff->state == HandoffState::Ready;
    }
    // Prepare an enumerated background page without claiming that it was drawn.
    // The UI exposes that page only after the source destruction is confirmed.
    bool Prepared(std::wstring_view current_path, uint64_t pending, uint64_t applied,
                  const std::vector<std::wstring>& selected, uint64_t now) {
        if (!handoff || !listing_complete || pending) return false;
        if (now >= handoff->deadline_tick) { handoff->Cancel(HandoffState::Expired); return false; }
        auto expected = names, actual = selected;
        std::sort(expected.begin(), expected.end()); std::sort(actual.begin(), actual.end());
        if (current_path != path || applied != generation || expected != actual) {
            handoff->Cancel(); return false;
        }
        if (!handoff->Ready(now)) return false;
        prepared_tick = now;
        return true;
    }
    bool Presented(std::wstring_view current_path, uint64_t pending, uint64_t applied,
                   const std::vector<std::wstring>& selected, bool visible, bool frame_succeeded, uint64_t now) {
        if (!handoff || handoff->state != HandoffState::Closed) return false;
        if (!Retain(current_path, pending, applied, visible, now) || !listing_complete || pending) return false;
        auto expected = names, actual = selected;
        std::sort(expected.begin(), expected.end()); std::sort(actual.begin(), actual.end());
        if (expected != actual) { handoff->Cancel(); return false; }
        if (!frame_succeeded || handoff->state != HandoffState::Closed) return false;
        presentation_complete = true;
        handoff->diagnostics.Emit(HandoffPhase::Presented);
        return true;
    }
    ~ExplorerNavigationLease() { if (handoff) handoff->Cancel(); }
};

} // namespace pulse::app
