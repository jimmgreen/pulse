// ui_timer_pacer.h - Chooses the main window's kTimerUi period.
//
// kTimerUi polls background results and steps timer-driven animations. At a
// fixed 16 ms it woke the UI thread ~40-60 times a second even when nothing on
// screen could change, which keeps the CPU out of its deep idle states. The
// pacer keeps the fast period while the user interacts or a tick produced
// visible change, and relaxes to a slow poll once the window has been quiet
// for a moment. Any input, paint or dirty tick restores the fast period at
// once, so animations still start on the next display-rate tick; only work
// that begins with no input and no repaint waits for the slow poll. After a
// longer quiet spell the poll slows once more: the producers whose results
// people wait for (folder changes, folder sizes) wake the window themselves
// with WM_UI_KICK, so the slower poll only delays the rest.
#pragma once
#include <windows.h>

namespace pulse::app {

class UiTimerPacer {
public:
    static constexpr UINT kActiveMs = 16;
    static constexpr UINT kIdleMs = 100;
    // Each wake of a visible foreground window costs several context
    // switches beyond the tick itself, so a window left alone polls slower.
    static constexpr UINT kQuietMs = 500;
    static constexpr ULONGLONG kQuietAfterMs = 5000;  // after the fast linger
    // Hidden to the tray or minimized: background results only need to be
    // picked up eventually, and showing the window is activity (fast again).
    static constexpr UINT kHiddenMs = 1000;
    static constexpr UINT kTrayHibernatedMs = 5000;
    // How long the fast period lasts after the last activity.
    static constexpr ULONGLONG kLingerMs = 1000;

    void NoteActivity(ULONGLONG now) noexcept { active_until_ = now + kLingerMs; }

    UINT Want(bool visible, ULONGLONG now, bool tray_hibernated = false) const noexcept {
        if (!visible) return tray_hibernated ? kTrayHibernatedMs : kHiddenMs;
        if (now < active_until_) return kActiveMs;
        return now < active_until_ + kQuietAfterMs ? kIdleMs : kQuietMs;
    }

    // True when the period changes; `period` then holds the new value.
    bool Update(bool visible, ULONGLONG now, UINT& period, bool tray_hibernated = false) noexcept {
        const UINT want = Want(visible, now, tray_hibernated);
        if (want == current_) return false;
        current_ = want;
        period = want;
        return true;
    }

    UINT Current() const noexcept { return current_; }

    // Windows generates WM_TIMER only once the queue holds nothing else, and
    // the frame pump posts a message every display frame: a long full-rate
    // animation could stop the tick for seconds (hover select, tooltips and
    // slow-click rename waited while the task pill held its finished state).
    // The frame handler runs the tick itself once it is this late.
    static constexpr ULONGLONG kOverdueSlackMs = 16;

    void NoteTick(ULONGLONG now) noexcept { last_tick_ = now; }
    bool TickOverdue(ULONGLONG now) const noexcept {
        return last_tick_ != 0 && now - last_tick_ >= current_ + kOverdueSlackMs;
    }

private:
    UINT current_ = kActiveMs;
    ULONGLONG active_until_ = 0;
    ULONGLONG last_tick_ = 0;
};

} // namespace pulse::app
