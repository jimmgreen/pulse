// frame_pump_test.cpp - app::FramePump paces to the display, coalesces, idles.
#include "../app/frame_pump.h"
#include "../app/ui_timer_pacer.h"
#include "../ui/ui_motion.h"
#include "../ui/ui_view_morph.h"
#include <cmath>
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <vector>

namespace {
int passed = 0, failed = 0;
void Check(bool value, const wchar_t* name) {
    std::wprintf(L"[%s] %s\n", value ? L"PASS" : L"FAIL", name);
    value ? ++passed : ++failed;
}
constexpr UINT kFrame = WM_APP + 1;

double NowMsExact() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

// Pumps kFrame messages for `ms`; consume=false leaves them queued.
std::vector<double> Collect(pulse::app::FramePump& pump, HWND hwnd, double ms, bool consume) {
    std::vector<double> times;
    const double end = NowMsExact() + ms;
    while (NowMsExact() < end) {
        if (!consume) { Sleep(1); continue; }
        MSG msg{};
        if (PeekMessageW(&msg, hwnd, kFrame, kFrame, PM_REMOVE)) {
            pump.FrameConsumed();
            times.push_back(NowMsExact());
        } else {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_POSTMESSAGE);
        }
    }
    return times;
}

int CountQueued(HWND hwnd) {
    int n = 0;
    MSG msg{};
    while (PeekMessageW(&msg, hwnd, kFrame, kFrame, PM_REMOVE)) ++n;
    return n;
}
// kTimerUi period policy: fast while active, slow once quiet, slower hidden.
void TestUiTimerPacer() {
    using pulse::app::UiTimerPacer;
    UiTimerPacer pacer;
    UINT period = 0;
    Check(pacer.Current() == UiTimerPacer::kActiveMs, L"pacer starts at the display-rate period");
    pacer.NoteActivity(1000);
    Check(!pacer.Update(true, 1500, period), L"activity keeps the fast period without re-arming the timer");
    Check(pacer.Update(true, 1000 + UiTimerPacer::kLingerMs, period) && period == UiTimerPacer::kIdleMs,
          L"quiet window relaxes to the idle poll");
    Check(!pacer.Update(true, 1000 + UiTimerPacer::kLingerMs + 500, period), L"idle period is set once, not every tick");
    Check(pacer.Update(true, 1000 + UiTimerPacer::kLingerMs + UiTimerPacer::kQuietAfterMs, period) &&
              period == UiTimerPacer::kQuietMs,
          L"a longer quiet spell relaxes to the quiet poll");
    Check(!pacer.Update(true, 9000, period), L"quiet period is set once, not every tick");
    pacer.NoteActivity(9000);
    Check(pacer.Update(true, 9000, period) && period == UiTimerPacer::kActiveMs,
          L"input or paint restores the fast period at once");
    Check(pacer.Update(false, 9001, period) && period == UiTimerPacer::kHiddenMs,
          L"hidden window polls at the hidden period even while active");
    Check(pacer.Update(true, 9002, period) && period == UiTimerPacer::kActiveMs,
          L"showing an active window returns to the fast period");
    Check(UiTimerPacer::kActiveMs < UiTimerPacer::kIdleMs && UiTimerPacer::kIdleMs < UiTimerPacer::kQuietMs &&
              UiTimerPacer::kQuietMs < UiTimerPacer::kHiddenMs,
          L"period ordering: active < idle < quiet < hidden");

    // Starved tick: a frame handler runs it once it is a period plus slack late.
    UiTimerPacer ticks;
    Check(!ticks.TickOverdue(5000), L"no tick yet: nothing is overdue");
    ticks.NoteActivity(5000);
    ticks.Update(true, 5000, period);
    ticks.NoteTick(5000);
    Check(!ticks.TickOverdue(5000 + UiTimerPacer::kActiveMs), L"a tick due on time is not overdue");
    Check(ticks.TickOverdue(5000 + UiTimerPacer::kActiveMs + UiTimerPacer::kOverdueSlackMs),
          L"a tick a period plus slack late is overdue");
    ticks.NoteTick(5040);
    Check(!ticks.TickOverdue(5041), L"running the tick clears the overdue state");
    Check(ticks.Update(true, 5000 + UiTimerPacer::kLingerMs, period) && period == UiTimerPacer::kIdleMs &&
              !ticks.TickOverdue(5040 + UiTimerPacer::kActiveMs + UiTimerPacer::kOverdueSlackMs) &&
              ticks.TickOverdue(5040 + UiTimerPacer::kIdleMs + UiTimerPacer::kOverdueSlackMs),
          L"overdue follows the current period (idle poll)");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && wcscmp(argv[1], L"--morph-text-warmup") == 0) {
        using VM = pulse::ui::motion::ViewMorphMotion;
        if (!pulse::ui::motion::SystemAnimationsEnabled()) {
            std::wprintf(L"[SKIP] system animations disabled; warmup transition not exercised\n");
            return 2;
        }
        const auto cell = D2D1::RectF(0, 0, 120, 80);
        const auto icon = D2D1::RectF(8, 8, 40, 40);
        VM morph;
        morph.BeginFrame(1, 0, 1, 1000, true);
        morph.Record(7, cell, icon);
        morph.AdvanceTextWarmup(37);
        morph.BeginFrame(1, 1, 2, 1016, true);
        Check(morph.Running() && morph.TextWarmupNext() == 0, L"new mode resets prior text warmup cursor");
        Check(morph.TextOpacity() == 0.0f, L"new morph starts with hidden target text");
        morph.AdvanceTextWarmup(12);
        morph.Record(7, cell, icon);
        morph.BeginFrame(1, 1, 3, 1032, true);
        Check(morph.TextWarmupNext() == 12, L"ordinary frame preserves completed warmup rows");
        morph.AdvanceTextWarmup(24);
        morph.Record(7, cell, icon);
        morph.BeginFrame(1, 2, 4, 1048, true);
        Check(morph.Running() && morph.TextWarmupNext() == 0 && morph.FramesSinceStart() == 1,
              L"rapid second mode switch starts its own warmup at zero");
        Check(morph.TextOpacity() == 0.0f, L"rapid switch restarts target text opacity");
        morph.AdvanceTextWarmup(9);
        float previous = 0;
        bool monotonic = true, saw_fade = false, preserved = true;
        for (uint64_t frame = 5, now = 1064; frame < 40; ++frame, now += 16) {
            morph.Record(7, cell, icon);
            morph.BeginFrame(1, 2, frame, now, true);
            const float alpha = morph.TextOpacity();
            monotonic &= alpha >= previous && alpha >= 0 && alpha <= 1;
            saw_fade |= alpha > 0 && alpha < 1;
            preserved &= morph.TextWarmupNext() == 9;
            previous = alpha;
        }
        Check(preserved, L"ordinary fade and settled frames preserve warmup progress");
        Check(monotonic && saw_fade, L"target text fades continuously through intermediate opacity");
        Check(!morph.Running() && morph.TextOpacity() == 1.0f, L"settled text is fully visible");
        std::wprintf(L"== morph text warmup: %d passed, %d failed ==\n", passed, failed);
        return failed ? 1 : 0;
    }
    TestUiTimerPacer();
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    Check(hwnd != nullptr, L"message window");

    // Motion clock: millisecond steps, unlike GetTickCount64's 15.6 ms.
    {
        std::vector<uint64_t> seen;
        const double end = NowMsExact() + 40.0;
        while (NowMsExact() < end) {
            const uint64_t t = pulse::ui::motion::NowMs();
            if (seen.empty() || seen.back() != t) seen.push_back(t);
        }
        bool monotonic = std::is_sorted(seen.begin(), seen.end());
        std::wprintf(L"[INFO] NowMs distinct values in 40 ms: %zu\n", seen.size());
        Check(monotonic && seen.size() >= 30, L"motion::NowMs is monotonic with 1 ms resolution");
    }

    // Motion must settle by time alone: the hover plate starts a glide, then
    // the pointer leaves the list and Update is never called again. Active()
    // used to stay true for ever, keeping the frame pump (and a full-window
    // repaint) running at 60 Hz until the next hover.
    {
        namespace motion = pulse::ui::motion;
        motion::RectMotion plate;
        const D2D1_RECT_F row_a = D2D1::RectF(0, 0, 100, 20), row_b = D2D1::RectF(0, 40, 100, 60);
        plate.Update(1, 1, row_a, 1, 1000, 100);
        plate.Update(1, 2, row_b, 2, 1016, 100);       // glide A -> B starts
        const bool animations = motion::SystemAnimationsEnabled();
        Check(!animations || plate.Active(1030), L"hover glide is active mid-flight");
        Check(!plate.Active(1016 + 100 + motion::kSettleGraceMs),
              L"abandoned hover glide stops asking for frames after its duration");
        motion::RectMotion fresh;
        Check(!fresh.Active(0) && !fresh.Active(123456), L"idle motion never asks for frames");
        motion::ListShiftMotion shift;
        shift.BeginFrame(7, 0, 0, 10, 1, 2000, true);
        shift.EndFrame();
        shift.BeginFrame(7, 0, 0, 11, 2, 2016, true);   // one row inserted
        shift.Offset(99, D2D1::Point2F(0, 0));           // new row: flash starts
        shift.EndFrame();                                // pane then stops drawing
        Check(!animations || shift.Active(2100), L"row flash is active right after the insert");
        Check(!shift.Active(2016 + motion::ListShiftMotion::kFlashMs + motion::kSettleGraceMs),
              L"abandoned row flash stops asking for frames after its duration");
        shift.BeginFrame(8, 0, 0, 15, 3, 2032, true); // expanded tree revision
        const auto offset = shift.Offset(99, D2D1::Point2F(0, 80));
        shift.Offset(100, D2D1::Point2F(0, 100));
        Check(offset.x == 0 && offset.y == 0 && shift.Flash(99) == 0 && shift.Flash(100) == 0,
              L"tree revision clears old flashes and does not flash revealed rows");
        shift.EndFrame();
    }

    // View-mode morph (grid -> list): shared-element travel, spring timing,
    // time-bounded, cheap enough for every visible item every frame.
    {
        namespace motion = pulse::ui::motion;
        using VM = motion::ViewMorphMotion;
        const bool animations = motion::SystemAnimationsEnabled();
        auto same = [](const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
            return std::fabs(a.left - b.left) < 0.01f && std::fabs(a.top - b.top) < 0.01f &&
                   std::fabs(a.right - b.right) < 0.01f && std::fabs(a.bottom - b.bottom) < 0.01f;
        };
        bool monotonic = true, bounded = true;
        float prev = 0.0f, half_t = -1.0f;
        for (int k = 0; k <= 400; ++k) {
            const float t = static_cast<float>(k) / 400.0f;
            const float v = motion::MorphSpring(t);
            if (v + 1e-6f < prev) monotonic = false;
            if (v < 0.0f || v > 1.0f) bounded = false;
            if (half_t < 0.0f && v >= 0.5f) half_t = t;
            prev = v;
        }
        Check(motion::MorphSpring(0.0f) == 0.0f && motion::MorphSpring(1.0f) == 1.0f,
              L"morph spring starts at 0 and lands exactly on 1 (no end snap)");
        Check(monotonic && bounded, L"morph spring is monotonic with no overshoot");
        std::wprintf(L"[INFO] morph spring reaches 50%% at %.0f ms\n", half_t * VM::kMorphMs);
        Check(half_t > 0.15f && half_t < 0.35f, L"morph spring: quick response, soft landing");

        const D2D1_RECT_F grid_cell = D2D1::RectF(100, 100, 200, 220), grid_icon = D2D1::RectF(110, 104, 190, 184);
        const D2D1_RECT_F list_cell = D2D1::RectF(0, 40, 300, 62), list_icon = D2D1::RectF(4, 43, 20, 59);
        constexpr int kMedium = 2, kList = 4;  // ViewMode::MediumIcons, ViewMode::List
        // Steps a morph like the frame pump does: one frame every 16 ms.
        auto run = [](VM& v, uint64_t folder, int mode, uint64_t& frame, uint64_t& now, uint64_t ms) {
            for (uint64_t end = now + ms; now < end;) {
                now += 16;
                v.BeginFrame(folder, mode, ++frame, now, true);
            }
        };
        VM m;
        m.BeginFrame(9, kMedium, 1, 1000, true);
        m.NoteLayout(D2D1::RectF(0, 0, 800, 600), 0.0f, 240.0f, 28.0f, 50);
        m.Record(42, grid_cell, grid_icon);
        m.BeginFrame(9, kList, 2, 1016, true);           // user switches view
        m.NoteLayout(D2D1::RectF(0, 30, 800, 600), 0.0f, 0.0f, 28.0f, 50);
        Check(!animations || (m.FromLayout().valid && m.FromLayout().scroll_y == 240.0f &&
                              m.FromLayout().viewport.top == 0.0f),
              L"morph keeps the previous view's exact layout inputs (scroll, viewport)");
        Check(!animations || m.FromOpacity() == 1.0f, L"previous labels are fully visible on the first frame");
        Check(!animations || m.Running(), L"view switch in the same folder starts a morph");
        if (animations) {
            const VM::Sample first = m.Get(42, list_cell, list_icon, 0, 12.0f);
            Check(first.animating && same(first.cell, grid_cell) && same(first.icon, grid_icon),
                  L"morph first frame draws the item where it was in the grid");
            Check(same(first.from_icon, grid_icon), L"morph reports the start icon (image-list size lock)");
            const VM::Sample fresh = m.Get(77, list_cell, list_icon, 0, 12.0f);
            Check(fresh.entering && fresh.opacity == 0.0f && fresh.cell.top > list_cell.top,
                  L"items new to the screen fade in from slightly below");
            Check(m.TextOpacity() == 0.0f, L"labels are hidden while shapes start moving");
            uint64_t frame = 2, now = 1016;
            run(m, 9, kList, frame, now, 150);
            Check(m.FromOpacity() == 0.0f, L"previous labels are gone before the shapes land");
            const VM::Sample mid = m.Get(42, list_cell, list_icon, 0, 12.0f);
            Check(mid.icon.left < grid_icon.left && mid.icon.left > list_icon.left &&
                  mid.icon.right - mid.icon.left < 80.0f && mid.icon.right - mid.icon.left > 16.0f,
                  L"mid-morph the icon is between grid and row, position and size");
            const VM::Sample late = m.Get(42, list_cell, list_icon, 20, 12.0f);
            Check(late.progress < mid.progress, L"later items start slightly later (stagger)");
            // Switch back mid-flight: continue from where the item is drawn now.
            m.Record(42, mid.cell, mid.icon);
            now += 16;
            m.BeginFrame(9, kMedium, ++frame, now, true);
            const VM::Sample back = m.Get(42, grid_cell, grid_icon, 0, 12.0f);
            Check(same(back.icon, mid.icon), L"re-switch mid-morph continues from the drawn position");
            run(m, 9, kMedium, frame, now, VM::kTotalMs);
            const VM::Sample done = m.Get(42, grid_cell, grid_icon, 0, 12.0f);
            Check(!m.Running() && !done.animating && same(done.icon, grid_icon) && m.TextOpacity() == 1.0f,
                  L"morph settles exactly on the target layout");
        }
        // A 124 ms first frame (cold caches) must not skip a third of the glide.
        if (animations) {
            VM slow;
            slow.BeginFrame(4, kMedium, 1, 100, true);
            slow.Record(42, grid_cell, grid_icon);
            slow.BeginFrame(4, kList, 2, 116, true);
            slow.BeginFrame(4, kList, 3, 116 + 124, true);   // the slow frame
            const VM::Sample after = slow.Get(42, list_cell, list_icon, 0, 12.0f);
            Check(after.progress <= motion::MorphSpring(static_cast<float>(VM::kMaxStepMs) / VM::kMorphMs) + 1e-4f,
                  L"a slow frame slows the morph instead of skipping ahead");
            uint64_t frame = 3, now = 240;
            run(slow, 4, kList, frame, now, VM::kTotalMs);
            Check(!slow.Running(), L"a slowed morph still finishes");
        }
        // Abandoned morph (pane hidden mid-flight) must stop asking for frames.
        VM a;
        a.BeginFrame(1, kMedium, 1, 5000, true);
        a.Record(1, grid_cell, grid_icon);
        a.BeginFrame(1, kList, 2, 5016, true);
        Check(!animations || a.Active(5100), L"morph asks for frames while running");
        Check(!a.Active(5016 + VM::kTotalMs + motion::kSettleGraceMs),
              L"abandoned morph stops asking for frames after its duration");
        // No morph across folders, after a gap in frames, or while loading.
        VM f;
        f.BeginFrame(1, kMedium, 1, 100, true); f.Record(1, grid_cell, grid_icon);
        f.BeginFrame(2, kList, 2, 116, true);
        Check(!f.Running(), L"navigating to another folder never morphs");
        VM g;
        g.BeginFrame(1, kMedium, 1, 100, true); g.Record(1, grid_cell, grid_icon);
        g.BeginFrame(1, kList, 5, 116, true);
        Check(!g.Running(), L"a pane that was not drawn in between snaps instead of morphing");
        VM l;
        l.BeginFrame(1, kMedium, 1, 100, true); l.Record(1, grid_cell, grid_icon);
        l.BeginFrame(1, kList, 2, 116, false);
        Check(!l.Running(), L"loading pane snaps instead of morphing");

        // Per-frame cost: 600 visible items (a maximised 4K list) sampled and
        // recorded, both passes, for a whole morph.
        VM p;
        p.BeginFrame(3, kMedium, 1, 0, true);
        for (uint64_t k = 0; k < 600; ++k) p.Record(k, grid_cell, grid_icon);
        const double t0 = NowMsExact();
        uint64_t frame = 2;
        int frames = 0;
        for (uint64_t now = 16; now < VM::kTotalMs + 32; now += 16, ++frame, ++frames) {
            p.BeginFrame(3, kList, frame, now, true);
            for (int pass = 0; pass < 2; ++pass) {
                for (uint64_t k = 0; k < 600; ++k) {
                    const VM::Sample s = p.Get(k, list_cell, list_icon, static_cast<int>(k), 12.0f);
                    if (pass == 0) p.Record(k, s.cell, s.icon);
                }
            }
        }
        const double per_frame = (NowMsExact() - t0) / std::max(1, frames);
        std::wprintf(L"[INFO] morph bookkeeping: %.3f ms per frame for 600 items\n", per_frame);
        Check(per_frame < 1.0, L"morph bookkeeping stays well under 1 ms per frame");
    }

    // Baseline for comparison: the 16 ms UI timer the pump replaces for motion.
    {
        SetTimer(hwnd, 1, 16, nullptr);
        std::vector<double> ticks;
        const double end = NowMsExact() + 1000.0;
        while (NowMsExact() < end) {
            MSG msg{};
            if (PeekMessageW(&msg, hwnd, WM_TIMER, WM_TIMER, PM_REMOVE)) ticks.push_back(NowMsExact());
            else MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_TIMER);
        }
        KillTimer(hwnd, 1);
        std::vector<double> gaps;
        for (size_t i = 1; i < ticks.size(); ++i) gaps.push_back(ticks[i] - ticks[i - 1]);
        std::sort(gaps.begin(), gaps.end());
        std::wprintf(L"[INFO] baseline SetTimer(16 ms): %zu ticks/s, median gap %.2f ms\n", ticks.size(),
                     gaps.empty() ? 0.0 : gaps[gaps.size() / 2]);
    }

    pulse::app::FramePump pump;
    Check(pump.Start(hwnd, kFrame), L"pump starts");
    std::wprintf(L"[INFO] clock: %ls\n", pump.ClockName());
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    const DWORD hz = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1
                         ? mode.dmDisplayFrequency : 60;
    std::wprintf(L"[INFO] display refresh: %lu Hz\n", hz);

    Check(Collect(pump, hwnd, 150, true).empty(), L"idle pump posts nothing");

    pump.Arm();
    const std::vector<double> times = Collect(pump, hwnd, 1000, true);
    std::vector<double> gaps;
    for (size_t i = 1; i < times.size(); ++i) gaps.push_back(times[i] - times[i - 1]);
    std::sort(gaps.begin(), gaps.end());
    const double median = gaps.empty() ? 0 : gaps[gaps.size() / 2];
    const double p95 = gaps.empty() ? 0 : gaps[gaps.size() * 95 / 100];
    std::wprintf(L"[INFO] armed: %zu frames/s, median gap %.2f ms, p95 %.2f ms\n", times.size(), median, p95);
    // A 16 ms WM_TIMER manages ~32/s; the pump must track the display.
    Check(times.size() >= 50, L"armed pump delivers at least 50 frames per second");
    Check(median >= 3.5 && median <= 1000.0 / 50.0, L"frame gaps follow the display refresh");
    Check(p95 <= 34.0, L"no frame gap longer than two 60 Hz frames at p95");

    // Not consuming: at most one message waits in the queue.
    Collect(pump, hwnd, 120, false);
    Check(CountQueued(hwnd) <= 1, L"frames coalesce: never more than one queued");
    pump.FrameConsumed();

    pump.Disarm();
    Collect(pump, hwnd, 60, true);  // one in flight may still land
    Check(Collect(pump, hwnd, 200, true).empty(), L"disarmed pump goes quiet");

    // Decorative motion: ArmPaced keeps ~30 fps on clock ticks; Arm lifts it.
    pump.ArmPaced(1000.0 / 30.0);
    const std::vector<double> paced = Collect(pump, hwnd, 1000, true);
    std::vector<double> paced_gaps;
    for (size_t i = 1; i < paced.size(); ++i) paced_gaps.push_back(paced[i] - paced[i - 1]);
    std::sort(paced_gaps.begin(), paced_gaps.end());
    const double paced_median = paced_gaps.empty() ? 0 : paced_gaps[paced_gaps.size() / 2];
    const double paced_min = paced_gaps.empty() ? 0 : paced_gaps.front();
    std::wprintf(L"[INFO] paced: %zu frames/s, median gap %.2f ms, min %.2f ms\n", paced.size(), paced_median, paced_min);
    Check(paced.size() >= 20 && paced.size() <= 34, L"paced pump delivers about 30 frames per second");
    Check(paced_min >= 1000.0 / 30.0 - 8.0, L"paced frames never come faster than the interval");
    pump.Arm();
    const std::vector<double> lifted = Collect(pump, hwnd, 500, true);
    Check(lifted.size() >= 25, L"Arm after ArmPaced returns to the display rate");
    pump.Disarm();
    Collect(pump, hwnd, 60, true);

    pump.Arm();
    Check(!Collect(pump, hwnd, 100, true).empty(), L"re-armed pump resumes");
    const double stop_start = NowMsExact();
    pump.Stop();
    const double stop_ms = NowMsExact() - stop_start;
    std::wprintf(L"[INFO] stop took %.1f ms\n", stop_ms);
    Check(!pump.Running() && stop_ms < 250, L"stop joins the worker promptly");
    CountQueued(hwnd);
    Check(Collect(pump, hwnd, 80, true).empty(), L"stopped pump posts nothing");

    DestroyWindow(hwnd);
    std::wprintf(L"== frame pump test: %d passed, %d failed ==\n", passed, failed);
    return failed ? 1 : 0;
}
