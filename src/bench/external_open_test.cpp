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
    const ExternalOpenBatch none;
    const ExternalOpenTuning tuning;
    using A = ExternalOpenAction;
    using R = ExternalOpenReason;
    using P = ExternalOpenPolicy;

    // Rule 5, smart, one window.
    {
        auto d = DecideOpenTarget(At(kNow), {Idle()}, none, P::Smart);
        Check(Is(d, A::Tab, R::Idle, 0) && !d.restore, "smart: an idle window gets a new tab");
        ExternalOpenWindow w;
        w.last_input_tick = kNow - 3000;
        d = DecideOpenTarget(At(kNow), {w}, none, P::Smart);
        Check(Is(d, A::NewWindow, R::RecentInput, 0), "smart: input 3 s ago opens a new window instead");
        w.last_input_tick = kNow - tuning.recent_input_ms;
        d = DecideOpenTarget(At(kNow), {w}, none, P::Smart);
        Check(Is(d, A::Tab, R::Idle, 0), "smart: input exactly 8 s ago no longer counts");
        w.last_input_tick = kNow - tuning.recent_input_ms + 1;
        d = DecideOpenTarget(At(kNow), {w}, none, P::Smart);
        Check(d.action == A::NewWindow, "smart: input just under 8 s ago still counts");
        w.last_input_tick = kNow + 50;  // clock read before the input landed
        d = DecideOpenTarget(At(kNow), {w}, none, P::Smart);
        Check(Is(d, A::Tab, R::Idle, 0), "smart: an input tick after the request is ignored");
        ExternalOpenWindow busy;
        busy.busy = true;
        d = DecideOpenTarget(At(kNow), {busy}, none, P::Smart);
        Check(Is(d, A::NewWindow, R::Busy, 0), "smart: renaming, dragging or a dialog opens a new window");
        ExternalOpenWindow min;
        min.minimized = true;
        min.busy = true;
        min.last_input_tick = kNow - 100;
        d = DecideOpenTarget(At(kNow), {min}, none, P::Smart);
        Check(Is(d, A::Tab, R::Hidden, 0) && d.restore,
              "smart: a minimized window gets the tab and is restored, even with recent input");
        ExternalOpenWindow tray;
        tray.hidden = true;
        d = DecideOpenTarget(At(kNow), {tray}, none, P::Smart);
        Check(Is(d, A::Tab, R::Hidden, 0) && d.restore, "smart: a window in the tray gets the tab and comes back");
    }
    // Smart looks at the most recently used window only.
    {
        ExternalOpenWindow recent;
        recent.last_input_tick = kNow - 1000;
        auto d = DecideOpenTarget(At(kNow), {Idle(), recent}, none, P::Smart);
        Check(Is(d, A::Tab, R::Idle, 0), "smart: input in an older window does not matter");
        d = DecideOpenTarget(At(kNow), {recent, Idle()}, none, P::Smart);
        Check(d.action == A::NewWindow, "smart: input in the most recent window does");
    }
    // Rule 1, dedupe, before everything else.
    {
        ExternalOpenWindow a = Idle(), b = Idle(), c = Idle();
        b.shows_folder = true;
        c.shows_folder = true;
        auto d = DecideOpenTarget(At(kNow), {a, b, c}, none, P::Smart);
        Check(Is(d, A::Activate, R::Duplicate, 1), "dedupe: the most recent window showing the folder wins");
        ExternalOpenRequest explicit_new = At(kNow);
        explicit_new.new_window = true;
        d = DecideOpenTarget(explicit_new, {a, b}, none, P::AlwaysWindow);
        Check(Is(d, A::Activate, R::Duplicate, 1), "dedupe: beats an explicit new window and the policy");
        b.hidden = true;
        d = DecideOpenTarget(At(kNow), {a, b}, none, P::Smart);
        Check(d.action == A::Activate && d.restore, "dedupe: a hidden window showing it is restored");
        ExternalOpenRequest this_pc = At(kNow);
        this_pc.can_dedupe = false;
        d = DecideOpenTarget(this_pc, {a, b}, none, P::Smart);
        Check(Is(d, A::Tab, R::Idle, 0), "dedupe: This PC is never deduplicated");
    }
    // Rule 2.
    {
        ExternalOpenRequest r = At(kNow);
        r.new_window = true;
        auto d = DecideOpenTarget(r, {Idle()}, none, P::AlwaysTab);
        Check(Is(d, A::NewWindow, R::Explicit, 0), "explicit: a new window even with \"always tab\"");
    }
    // Rule 1.5, the Win+E wake-only setting.
    {
        ExternalOpenRequest r = At(kNow);
        r.new_window = true;
        r.activate_only = true;
        auto d = DecideOpenTarget(r, {Idle()}, none, P::Smart);
        Check(Is(d, A::Activate, R::ActivateOnly, 0) && !d.restore,
              "wake-only: the most recent window comes forward, no new tab");
        ExternalOpenWindow min;
        min.minimized = true;
        d = DecideOpenTarget(r, {min, Idle()}, none, P::AlwaysWindow);
        Check(Is(d, A::Activate, R::ActivateOnly, 0) && d.restore,
              "wake-only: a minimized window is restored, the policy does not matter");
        d = DecideOpenTarget(r, {}, none, P::Smart);
        Check(d.action == A::NewWindow, "wake-only: no window at all still opens one");
    }
    // Rule 4.
    {
        ExternalOpenWindow w;
        w.last_input_tick = kNow - 100;
        auto d = DecideOpenTarget(At(kNow), {w}, none, P::AlwaysTab);
        Check(Is(d, A::Tab, R::Policy, 0), "always tab: a tab despite recent input");
        ExternalOpenWindow tray;
        tray.hidden = true;
        d = DecideOpenTarget(At(kNow), {tray}, none, P::AlwaysTab);
        Check(Is(d, A::Tab, R::Policy, 0) && d.restore, "always tab: a hidden window is restored");
        d = DecideOpenTarget(At(kNow), {Idle()}, none, P::AlwaysWindow);
        Check(Is(d, A::NewWindow, R::Policy, 0), "always window: a new window for an idle window");
        d = DecideOpenTarget(At(kNow), {}, none, P::Smart);
        Check(d.action == A::NewWindow, "no window at all: a new window");
    }
    // Rule 3, batches: five folders opened at once become one window.
    {
        // First request: smart picks a tab in the idle window.
        ExternalOpenWindow w = Idle();
        auto first = DecideOpenTarget(At(kNow), {w}, none, P::Smart);
        ExternalOpenBatch batch = NextOpenBatch(none, At(kNow), first, true, true);
        Check(batch.count == 1 && batch.tab_movable && !batch.window_is_new,
              "batch: a tab in an existing window starts a movable batch");
        // Second, 200 ms later: a new window takes that tab out.
        w.batch_window = true;
        w.last_input_tick = kNow - 10;  // the user clicked the first tab meanwhile
        auto second = DecideOpenTarget(At(kNow + 200), {w}, batch, P::Smart);
        Check(Is(second, A::PromoteBatch, R::Batch, 0), "batch: the second request moves the first tab to a new window");
        batch = NextOpenBatch(batch, At(kNow + 200), second, true, false);
        Check(batch.count == 2 && batch.window_is_new && !batch.tab_movable, "batch: now owns its new window");
        // Third, 450 ms after the second: joins the new window (now the most recent).
        ExternalOpenWindow fresh = Idle();
        fresh.batch_window = true;
        w.batch_window = false;
        auto third = DecideOpenTarget(At(kNow + 650), {fresh, w}, batch, P::Smart);
        Check(Is(third, A::JoinBatch, R::Batch, 0), "batch: the chain continues within 500 ms of the last request");
        batch = NextOpenBatch(batch, At(kNow + 650), third, true, false);
        Check(batch.count == 3 && batch.tick == kNow + 650, "batch: counts and moves on");
        // An already open folder in the middle neither ends nor joins it.
        ExternalOpenWindow showing = w;
        showing.shows_folder = true;
        auto dup = DecideOpenTarget(At(kNow + 700), {fresh, showing}, batch, P::Smart);
        Check(dup.action == A::Activate, "batch: an open folder is still just activated");
        const ExternalOpenBatch kept = NextOpenBatch(batch, At(kNow + 700), dup, false, false);
        Check(kept.count == 3 && kept.tick == kNow + 650, "batch: an activation leaves the batch as it was");
        // Too late: a new decision.
        auto late = DecideOpenTarget(At(kNow + 650 + 501), {fresh, w}, batch, P::Smart);
        Check(late.reason != R::Batch, "batch: 501 ms after the last request is a new decision");
        Check(ContinuesOpenBatch(batch, At(kNow + 650 + 500)), "batch: exactly 500 ms still continues");
        Check(!ContinuesOpenBatch(batch, At(kNow + 600)), "batch: a request older than the batch does not");
    }
    {
        // Busy first window: the first request opens a new window, the rest join it.
        ExternalOpenWindow busy;
        busy.busy = true;
        auto first = DecideOpenTarget(At(kNow), {busy}, none, P::Smart);
        ExternalOpenBatch batch = NextOpenBatch(none, At(kNow), first, true, false);
        Check(first.action == A::NewWindow && batch.window_is_new, "batch: a new first window owns the batch");
        ExternalOpenWindow fresh;
        fresh.batch_window = true;
        auto second = DecideOpenTarget(At(kNow + 100), {fresh, busy}, batch, P::Smart);
        Check(Is(second, A::JoinBatch, R::Batch, 0), "batch: the second request joins that window");
    }
    {
        // A first tab that cannot move (its window's only tab): join, never promote.
        ExternalOpenBatch batch = NextOpenBatch(none, At(kNow), {A::Tab, R::Idle, 0, false}, true, false);
        ExternalOpenWindow w;
        w.batch_window = true;
        auto d = DecideOpenTarget(At(kNow + 100), {w}, batch, P::Smart);
        Check(Is(d, A::JoinBatch, R::Batch, 0), "batch: an unmovable first tab keeps the batch in its window");
        // "Always tab": the batch stays in the window, as the user chose.
        batch = NextOpenBatch(none, At(kNow), {A::Tab, R::Policy, 0, false}, true, true);
        d = DecideOpenTarget(At(kNow + 100), {w}, batch, P::AlwaysTab);
        Check(Is(d, A::JoinBatch, R::Batch, 0), "batch: \"always tab\" never promotes to a new window");
        // "Always window": one window for the batch instead of one each.
        batch = NextOpenBatch(none, At(kNow), {A::NewWindow, R::Policy, 0, false}, true, false);
        d = DecideOpenTarget(At(kNow + 100), {w}, batch, P::AlwaysWindow);
        Check(Is(d, A::JoinBatch, R::Batch, 0), "batch: \"always window\" puts the batch in one window");
        // The batch's window closed: decide afresh.
        ExternalOpenWindow other;
        d = DecideOpenTarget(At(kNow + 100), {other}, batch, P::Smart);
        Check(Is(d, A::Tab, R::Idle, 0), "batch: its window gone, the request is decided normally");
        // A failed open does not start a batch.
        const ExternalOpenBatch failed = NextOpenBatch(none, At(kNow), {A::NewWindow, R::Busy, 0, false}, false, false);
        Check(failed.count == 0, "batch: an open that failed starts nothing");
    }

    // Rule 7 (M7b): what the toast offers, and the prompt after 3 conversions.
    {
        using C = OpenConversion;
        const ExternalOpenDecision tab{A::Tab, R::Idle, 0, false};
        const ExternalOpenDecision window{A::NewWindow, R::RecentInput, 0, false};
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
