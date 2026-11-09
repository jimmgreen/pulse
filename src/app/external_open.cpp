// external_open.cpp - plan §6 rules (see external_open.h).
#include "external_open.h"

namespace pulse::app {

ExternalOpenDecision DecideLaunchTarget(int mode, const std::vector<ExternalOpenWindow>& windows,
                                      bool explicit_window) {
    ExternalOpenDecision decision;
    decision.reason = explicit_window ? ExternalOpenReason::Explicit : ExternalOpenReason::Policy;
    decision.action = ExternalOpenAction::NewWindow;
    if (explicit_window || windows.empty()) return decision;
    if (mode == 0) {
        // A window closed to the tray is gone as far as the user can tell:
        // bring it back instead of opening a second one beside it (which
        // left a hidden window registered and counted as one of several).
        for (size_t i = 0; i < windows.size(); ++i) {
            if (!windows[i].hidden || windows[i].busy) continue;
            decision.action = ExternalOpenAction::Activate;
            decision.window = i;
            decision.restore = true;
            return decision;
        }
        return decision;
    }
    for (size_t i = 0; i < windows.size(); ++i) {
        if (windows[i].busy) continue;
        decision.action = mode == 2 ? ExternalOpenAction::Activate : ExternalOpenAction::Tab;
        decision.window = i;
        decision.restore = windows[i].hidden || windows[i].minimized;
        return decision;
    }
    decision.action = ExternalOpenAction::Deferred;
    decision.reason = ExternalOpenReason::Busy;
    return decision;
}

ExternalOpenDecision DecideActivationTarget(const std::vector<ExternalOpenWindow>& windows) {
    ExternalOpenDecision decision;
    decision.reason = ExternalOpenReason::Explicit;
    decision.action = windows.empty() ? ExternalOpenAction::NewWindow :
        windows.front().busy ? ExternalOpenAction::Deferred : ExternalOpenAction::Activate;
    if (!windows.empty()) {
        if (windows.front().busy) decision.reason = ExternalOpenReason::Busy;
        else decision.restore = windows.front().hidden || windows.front().minimized;
    }
    return decision;
}

bool ContinuesOpenBatch(const ExternalOpenBatch&, const ExternalOpenRequest&,
                        const ExternalOpenTuning&) {
    return false; // Time proximity is not evidence of a shared request intent.
}

ExternalOpenDecision DecideOpenTarget(const ExternalOpenRequest& request,
                                      const std::vector<ExternalOpenWindow>& windows,
                                      const ExternalOpenBatch&, ExternalOpenPolicy policy,
                                      const ExternalOpenTuning&) {
    ExternalOpenDecision decision;
    if (request.new_window || windows.empty()) {
        decision.action = ExternalOpenAction::NewWindow;
        decision.reason = request.new_window ? ExternalOpenReason::Explicit : ExternalOpenReason::Policy;
        return decision;
    }
    if (request.can_dedupe) {
        for (size_t i = 0; i < windows.size(); ++i) {
            if (windows[i].busy || !windows[i].shows_folder) continue;
            decision.action = ExternalOpenAction::Activate;
            decision.reason = ExternalOpenReason::Duplicate;
            decision.window = i;
            decision.restore = windows[i].hidden || windows[i].minimized;
            return decision;
        }
    }
    if (policy == ExternalOpenPolicy::AlwaysWindow) {
        decision.action = ExternalOpenAction::NewWindow;
        decision.reason = ExternalOpenReason::Policy;
        return decision;
    }
    for (size_t i = 0; i < windows.size(); ++i) {
        if (windows[i].busy) continue;
        decision.action = ExternalOpenAction::Tab;
        decision.window = i;
        decision.restore = windows[i].hidden || windows[i].minimized;
        decision.reason = policy == ExternalOpenPolicy::AlwaysTab ? ExternalOpenReason::Policy :
            decision.restore ? ExternalOpenReason::Hidden : ExternalOpenReason::Idle;
        return decision;
    }
    decision.action = ExternalOpenAction::Deferred;
    decision.reason = ExternalOpenReason::Busy;
    return decision;
}

ExternalOpenBatch NextOpenBatch(const ExternalOpenBatch&, const ExternalOpenRequest&,
                                const ExternalOpenDecision&, bool, bool,
                                const ExternalOpenTuning&) {
    return {}; // Clear legacy state; routing must never promote a time-based batch.
}

OpenConversion OfferedConversion(const ExternalOpenDecision& decision, ExternalOpenPolicy policy,
                                 bool in_new_window, bool tab_movable) {
    if (policy != ExternalOpenPolicy::Smart || decision.action == ExternalOpenAction::Activate ||
        decision.action == ExternalOpenAction::Deferred)
        return OpenConversion::None;
    if (in_new_window) return OpenConversion::ToTab;
    return tab_movable ? OpenConversion::ToWindow : OpenConversion::None;
}

bool NoteOpenConversion(OpenConversionStreak& streak, OpenConversion direction, int prompt_after) {
    if (direction == OpenConversion::None) return false;
    if (streak.direction != direction) streak = {direction, 0};
    if (++streak.count < prompt_after) return false;
    streak.count = 0;
    return true;
}

ExternalOpenPolicy SuggestedOpenPolicy(OpenConversion direction) {
    return direction == OpenConversion::ToWindow ? ExternalOpenPolicy::AlwaysWindow : ExternalOpenPolicy::AlwaysTab;
}

}  // namespace pulse::app
