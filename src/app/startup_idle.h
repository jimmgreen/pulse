#pragma once
#include "app_messages.h"

namespace pulse::app {
inline constexpr UINT_PTR kStartupIdleTimer = 0x5354;

// WM_TIMER runs after queued input, directory results and paint messages.
// The timer belongs to the window and cannot retain AppState after destruction.
inline void ScheduleStartupIdle(HWND hwnd, UINT step) {
    if (step > 2) return;
    const UINT_PTR timer = kStartupIdleTimer + step;
    if (!SetTimer(hwnd, timer, USER_TIMER_MINIMUM,
            [](HWND window, UINT, UINT_PTR id, DWORD) {
                KillTimer(window, id);
                SendMessageW(window, WM_DEFERRED_STARTUP, id - kStartupIdleTimer, 0);
            }))
        PostMessageW(hwnd, WM_DEFERRED_STARTUP, step, 0);
}
}
