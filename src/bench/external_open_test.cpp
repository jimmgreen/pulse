// external_open_test.cpp - plan §6 rules for folders opened from outside
// Pulse (multi-window M7a, app/external_open.h). [PASS]/[FAIL] per check.
#include "../app/external_open.h"

#include <cstdio>

using namespace pulse::app;

namespace {
int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_pass : g_fail)++;
}

constexpr uint64_t kNow = 1'000'000;

ExternalOpenRequest At(uint64_t tick) {
    ExternalOpenRequest r;
    r.tick = tick;
    return r;
}

ExternalOpenWindow Idle() { return ExternalOpenWindow{}; }

bool Is(const ExternalOpenDecision& d, ExternalOpenAction a, ExternalOpenReason why, size_t window) {
    return d.action == a && d.reason == why && d.window == window;
}
}  // namespace

int main() {
    using A = ExternalOpenAction;
    using R = ExternalOpenReason;
    using P = ExternalOpenPolicy;
    const ExternalOpenBatch none;
    ExternalOpenWindow busy; busy.busy = true;
    ExternalOpenWindow hidden; hidden.hidden = true;
    ExternalOpenWindow minimized; minimized.minimized = true;
    ExternalOpenWindow duplicate; duplicate.shows_folder = true;
    ExternalOpenWindow busy_duplicate = duplicate; busy_duplicate.busy = true;
    {
        for (const auto window : {Idle(), hidden, minimized}) {
            for (int repeat = 0; repeat != 3; ++repeat)
                Check(Is(DecideActivationTarget({window}), A::Activate, R::Explicit, 0),
                    "taskbar repeated activation only reveals the existing window");
        }
        Check(Is(DecideActivationTarget({busy, Idle()}), A::Deferred, R::Busy, 0),
            "taskbar activation defers busy MRU without creating or switching windows");
        Check(DecideActivationTarget({}).action == A::NewWindow,
            "cold taskbar activation permits one initial window");
    }
    {
        Check(Is(DecideLaunchTarget(0, {busy}), A::NewWindow, R::Policy, 0), "launch mode 0 explicitly creates despite busy existing window");
        Check(Is(DecideLaunchTarget(0, {Idle(), hidden}), A::Activate, R::Policy, 1) &&
            DecideLaunchTarget(0, {Idle(), hidden}).restore, "launch mode 0 brings back a window closed to the tray instead of adding one");
        ExternalOpenWindow busy_hidden = hidden; busy_hidden.busy = true;
        Check(Is(DecideLaunchTarget(0, {busy_hidden}), A::NewWindow, R::Policy, 0), "launch mode 0 still creates when the tray window is busy");
        Check(Is(DecideLaunchTarget(0, {hidden}, true), A::NewWindow, R::Explicit, 0), "explicit new window is not redirected to the tray window");
        Check(Is(DecideLaunchTarget(1, {busy, hidden}), A::Tab, R::Policy, 1) &&
            DecideLaunchTarget(1, {busy, hidden}).restore, "launch mode 1 chooses available MRU and restores it");
        Check(Is(DecideLaunchTarget(1, {busy}), A::Deferred, R::Busy, 0) &&
            !DecideLaunchTarget(1, {busy}).restore, "launch mode 1 all busy defers without a new window");
        Check(Is(DecideLaunchTarget(2, {busy, minimized}), A::Activate, R::Policy, 1) &&
            DecideLaunchTarget(2, {busy, minimized}).restore, "launch mode 2 only activates available MRU, never navigates");
        Check(Is(DecideLaunchTarget(2, {busy}), A::Deferred, R::Busy, 0), "launch mode 2 never interrupts busy modal target");
        Check(DecideLaunchTarget(1, {}).action == A::NewWindow && DecideLaunchTarget(2, {}).action == A::NewWindow,
            "empty launch candidates preserve first-window creation");
        Check(Is(DecideLaunchTarget(2, {duplicate}, true), A::NewWindow, R::Explicit, 0), "explicit launch overrides activation and reuse");
    }
    {
        for (const auto policy : {P::Smart, P::AlwaysTab, P::AlwaysWindow}) {
            auto request = At(kNow); request.new_window = true;
            const ExternalOpenBatch old_batch{kNow - 1, 8, true, true};
            auto matching = duplicate; matching.batch_window = true;
            const auto first = DecideOpenTarget(request, {matching, busy}, old_batch, policy);
            request.tick++;
            const auto second = DecideOpenTarget(request, {matching, busy}, old_batch, policy);
            Check(Is(first, A::NewWindow, R::Explicit, 0) && Is(second, A::NewWindow, R::Explicit, 0),
                "each explicit new-window intent bypasses same-path reuse and timing batch");
            Check(Is(DecideOpenTarget(At(kNow), {Idle(), duplicate}, none, policy), A::Activate, R::Duplicate, 1),
                "non-explicit request retains available same-folder reuse across policies");
        }
        Check(Is(DecideOpenTarget(At(kNow), {busy_duplicate, duplicate}, none, P::Smart), A::Activate, R::Duplicate, 1),
            "duplicate search skips busy match and chooses next available matching window");
        Check(Is(DecideOpenTarget(At(kNow), {busy_duplicate, Idle()}, none, P::Smart), A::Tab, R::Idle, 1),
            "busy duplicate does not prevent available MRU tab");
        auto hidden_duplicate = duplicate; hidden_duplicate.hidden = true;
        Check(DecideOpenTarget(At(kNow), {hidden_duplicate}, none, P::Smart).restore, "available hidden duplicate restores");
        auto request = At(kNow); request.can_dedupe = false;
        Check(Is(DecideOpenTarget(request, {duplicate}, none, P::Smart), A::Tab, R::Idle, 0), "This PC no-dedupe intent remains a new tab");
    }
    {
        for (const auto policy : {P::Smart, P::AlwaysTab}) {
            const auto reason = policy == P::Smart ? R::Idle : R::Policy;
            Check(Is(DecideOpenTarget(At(kNow), {busy, Idle()}, none, policy), A::Tab, reason, 1),
                "tab policies choose available MRU rather than manufacture a window");
            auto hidden_busy = hidden; hidden_busy.busy = true;
            auto minimized_busy = minimized; minimized_busy.busy = true;
            const auto deferred = DecideOpenTarget(At(kNow), {hidden_busy, minimized_busy, busy_duplicate}, none, policy);
            Check(Is(deferred, A::Deferred, R::Busy, 0) && !deferred.restore,
                "all busy including hidden/minimized/duplicate candidates defers without restore");
            const auto restored = DecideOpenTarget(At(kNow), {busy, hidden}, none, policy);
            Check(restored.action == A::Tab && restored.window == 1 && restored.restore, "nonbusy hidden target can receive tab");
        }
        Check(Is(DecideOpenTarget(At(kNow), {busy}, none, P::AlwaysWindow), A::NewWindow, R::Policy, 0),
            "fixed new-window policy remains an explicit user policy, independent of busy candidates");
        Check(DecideOpenTarget(At(kNow), {}, none, P::Smart).action == A::NewWindow, "first external open creates initial window");
    }
    // Replace time-heuristic expectations with invariance across the old boundaries.
    {
        const ExternalOpenBatch batch{kNow, 5, false, true};
        for (const uint64_t input : {uint64_t{0}, kNow - 8001, kNow - 8000, kNow - 7999, kNow, kNow + 50}) {
            auto window = Idle(); window.last_input_tick = input; window.batch_window = true;
            for (const uint64_t tick : {kNow - 1, kNow, kNow + 200, kNow + 500, kNow + 501}) {
                Check(Is(DecideOpenTarget(At(tick), {window}, batch, P::Smart), A::Tab, R::Idle, 0),
                    "Smart decision is invariant to recent-input and batch-time boundaries");
            }
        }
        auto previous_batch_window = Idle(); previous_batch_window.batch_window = true;
        Check(Is(DecideOpenTarget(At(kNow + 1), {Idle(), previous_batch_window}, batch, P::Smart), A::Tab, R::Idle, 0),
            "old batch cannot steal target selection from available MRU");
        Check(Is(DecideOpenTarget(At(kNow + 1), {previous_batch_window}, batch, P::AlwaysWindow), A::NewWindow, R::Policy, 0),
            "AlwaysWindow is never converted into JoinBatch or PromoteBatch");
        Check(Is(DecideOpenTarget(At(kNow + 1), {previous_batch_window}, batch, P::AlwaysTab), A::Tab, R::Policy, 0),
            "AlwaysTab stays an ordinary tab without a timing batch");
        Check(!ContinuesOpenBatch(batch, At(kNow + 1)), "legacy time helper never asserts common request intent");
        const auto after_open = NextOpenBatch(batch, At(kNow + 1), {A::Tab, R::Idle, 0, false}, true, true);
        const auto after_deferred = NextOpenBatch(batch, At(kNow + 1), {A::Deferred, R::Busy, 0, false}, false, false);
        Check(!after_open.count && !after_open.tick && !after_open.tab_movable && !after_open.window_is_new &&
            !after_deferred.count && !after_deferred.tick, "legacy batch state clears for both completed and deferred requests");
        Check(Is(DecideOpenTarget(At(kNow), {Idle()}, batch, P::Smart, {UINT64_MAX, UINT64_MAX}), A::Tab, R::Idle, 0),
            "legacy tuning cannot reenable timing heuristics");
    }

    // Rule 7 (M7b): what the toast offers, and the prompt after 3 conversions.
    {
        using C = OpenConversion;
        const ExternalOpenDecision tab{A::Tab, R::Idle, 0, false};
        const ExternalOpenDecision window{A::NewWindow, R::Explicit, 0, false};
        const ExternalOpenDecision dup{A::Activate, R::Duplicate, 0, false};
        Check(OfferedConversion(window, P::Smart, true, false) == C::ToTab, "toast: a new window offers \"make it a tab\"");
        Check(OfferedConversion(tab, P::Smart, false, true) == C::ToWindow, "toast: a tab offers \"new window\"");
        Check(OfferedConversion(tab, P::Smart, false, false) == C::None,
              "toast: a window's only tab offers nothing (it cannot leave)");
        Check(OfferedConversion(dup, P::Smart, false, true) == C::None, "toast: an already open folder offers nothing");
        Check(OfferedConversion({A::JoinBatch, R::Batch, 0, false}, P::Smart, true, true) == C::ToTab,
              "toast: a batch in its new window offers \"make it a tab\"");
        Check(OfferedConversion(tab, P::AlwaysTab, false, true) == C::None &&
                  OfferedConversion(window, P::AlwaysWindow, true, false) == C::None,
              "toast: a fixed policy (the user's choice) offers nothing");
        Check(OfferedConversion({A::Deferred, R::Busy, 0, false}, P::Smart, true, true) == C::None,
              "toast: deferred request offers no conversion before any open");
        OpenConversionStreak streak;
        const bool p1 = NoteOpenConversion(streak, C::ToTab);
        const bool p2 = NoteOpenConversion(streak, C::ToTab);
        const bool p3 = NoteOpenConversion(streak, C::ToTab);
        Check(!p1 && !p2 && p3, "streak: the third conversion the same way prompts");
        Check(!NoteOpenConversion(streak, C::ToTab) && streak.count == 1, "streak: after the prompt it counts again from 1");
        NoteOpenConversion(streak, C::ToTab);
        Check(!NoteOpenConversion(streak, C::ToWindow) && streak.direction == C::ToWindow && streak.count == 1,
              "streak: the other direction starts over");
        Check(!NoteOpenConversion(streak, C::None) && streak.count == 1, "streak: no conversion changes nothing");
        Check(SuggestedOpenPolicy(C::ToTab) == P::AlwaysTab && SuggestedOpenPolicy(C::ToWindow) == P::AlwaysWindow,
              "streak: the prompt proposes the direction the user kept choosing");
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
