// external_open.cpp - plan §6 rules (see external_open.h).
#include "external_open.h"

namespace pulse::app {

bool ContinuesOpenBatch(const ExternalOpenBatch& batch, const ExternalOpenRequest& request,
                        const ExternalOpenTuning& tuning) {
    return batch.count > 0 && batch.tick != 0 && request.tick >= batch.tick &&
           request.tick - batch.tick <= tuning.batch_ms;
}

ExternalOpenDecision DecideOpenTarget(const ExternalOpenRequest& request,
                                      const std::vector<ExternalOpenWindow>& windows,
                                      const ExternalOpenBatch& batch, ExternalOpenPolicy policy,
                                      const ExternalOpenTuning& tuning) {
    ExternalOpenDecision d;
    if (windows.empty()) {
        d.action = ExternalOpenAction::NewWindow;
        d.reason = ExternalOpenReason::Policy;
        return d;
    }
    const auto restore_of = [&windows](size_t i) { return windows[i].minimized || windows[i].hidden; };

    // 1. Already shown somewhere: the most recently used such window.
    if (request.can_dedupe) {
        for (size_t i = 0; i < windows.size(); ++i) {
            if (!windows[i].shows_folder) continue;
            d.action = ExternalOpenAction::Activate;
            d.reason = ExternalOpenReason::Duplicate;
            d.window = i;
            d.restore = restore_of(i);
            return d;
        }
    }
    // 1.5 The Win+E wake-only setting: the most recent window comes forward
    // (restored when minimized or in the tray), no new window or tab.
    if (request.activate_only) {
        d.action = ExternalOpenAction::Activate;
        d.reason = ExternalOpenReason::ActivateOnly;
        d.window = 0;
        d.restore = restore_of(0);
        return d;
    }
    // 2. Explicitly a new window.
    if (request.new_window) {
        d.action = ExternalOpenAction::NewWindow;
        d.reason = ExternalOpenReason::Explicit;
        return d;
    }
    // 3. Part of a batch: all of it ends up in one window.
    if (ContinuesOpenBatch(batch, request, tuning)) {
        for (size_t i = 0; i < windows.size(); ++i) {
            if (!windows[i].batch_window) continue;
            d.reason = ExternalOpenReason::Batch;
            d.window = i;
            const bool promote = !batch.window_is_new && batch.tab_movable && policy != ExternalOpenPolicy::AlwaysTab;
            d.action = promote ? ExternalOpenAction::PromoteBatch : ExternalOpenAction::JoinBatch;
            d.restore = !promote && restore_of(i);
            return d;
        }
        // The batch's window closed meanwhile: decide afresh.
    }
    // 4. A fixed policy.
    if (policy == ExternalOpenPolicy::AlwaysWindow) {
        d.action = ExternalOpenAction::NewWindow;
        d.reason = ExternalOpenReason::Policy;
        return d;
    }
    d.window = 0;
    if (policy == ExternalOpenPolicy::AlwaysTab) {
        d.action = ExternalOpenAction::Tab;
        d.reason = ExternalOpenReason::Policy;
        d.restore = restore_of(0);
        return d;
    }
    // 5. Smart.
    const ExternalOpenWindow& mru = windows.front();
    if (mru.minimized || mru.hidden) {
        d.action = ExternalOpenAction::Tab;
        d.reason = ExternalOpenReason::Hidden;
        d.restore = true;
        return d;
    }
    if (mru.busy) {
        d.action = ExternalOpenAction::NewWindow;
        d.reason = ExternalOpenReason::Busy;
        return d;
    }
    if (mru.last_input_tick != 0 && request.tick >= mru.last_input_tick &&
        request.tick - mru.last_input_tick < tuning.recent_input_ms) {
        d.action = ExternalOpenAction::NewWindow;
        d.reason = ExternalOpenReason::RecentInput;
        return d;
    }
    d.action = ExternalOpenAction::Tab;
    d.reason = ExternalOpenReason::Idle;
    return d;
}

ExternalOpenBatch NextOpenBatch(const ExternalOpenBatch& batch, const ExternalOpenRequest& request,
                                const ExternalOpenDecision& decision, bool opened_tab, bool tab_movable,
                                const ExternalOpenTuning& tuning) {
    // Nothing new opened (an already open folder, or a failure): the batch
    // neither grows nor ends, so the folders around it still come together.
    if (!opened_tab) return ContinuesOpenBatch(batch, request, tuning) ? batch : ExternalOpenBatch{};
    ExternalOpenBatch next;
    next.tick = request.tick;
    switch (decision.action) {
    case ExternalOpenAction::JoinBatch:
        next = batch;
        next.tick = request.tick;
        ++next.count;
        // The first tab already moved out, or the batch owns its window.
        next.tab_movable = false;
        return next;
    case ExternalOpenAction::PromoteBatch:
        next.count = batch.count + 1;
        next.window_is_new = true;
        return next;
    case ExternalOpenAction::NewWindow:
        next.count = 1;
        next.window_is_new = true;
        return next;
    case ExternalOpenAction::Tab:
        next.count = 1;
        next.tab_movable = tab_movable;
        return next;
    case ExternalOpenAction::Activate:
        break;
    }
    return ContinuesOpenBatch(batch, request, tuning) ? batch : ExternalOpenBatch{};
}

OpenConversion OfferedConversion(const ExternalOpenDecision& decision, ExternalOpenPolicy policy,
                                 bool in_new_window, bool tab_movable) {
    if (policy != ExternalOpenPolicy::Smart || decision.action == ExternalOpenAction::Activate)
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
