// window_placement_test.cpp — saved window rectangle round trip.
// Regression: with a taskbar or app bar docked at the top, Pulse opened higher
// on every launch because rcNormalPosition (workspace coordinates) was passed
// to CreateWindowEx (screen coordinates).
#include "../app/window_placement.h"

#include <shellapi.h>
#include <cstdio>

using namespace pulse::app;

static int g_pass = 0, g_fail = 0;

static void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    ok ? ++g_pass : ++g_fail;
}

static bool Same(const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; }

static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void PumpMessages() {
    MSG msg;
    for (int i = 0; i < 20; ++i) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(25);
    }
}

// Saves a window the way CaptureWindowSession does and reopens it the way
// wWinMain does; returns the vertical drift after one launch.
static LONG RelaunchDrift(HINSTANCE inst, const wchar_t* cls, const RECT& start, bool convert) {
    HWND first = CreateWindowExW(0, cls, L"placement", WS_POPUP | WS_CAPTION | WS_THICKFRAME,
        start.left, start.top, start.right - start.left, start.bottom - start.top,
        nullptr, nullptr, inst, nullptr);
    WINDOWPLACEMENT wp{sizeof(wp)};
    GetWindowPlacement(first, &wp);
    DestroyWindow(first);
    const RECT r = convert ? RestoredWindowRect(wp.rcNormalPosition) : wp.rcNormalPosition;
    HWND second = CreateWindowExW(0, cls, L"placement", WS_POPUP | WS_CAPTION | WS_THICKFRAME,
        r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    RECT actual{};
    GetWindowRect(second, &actual);
    DestroyWindow(second);
    return actual.top - start.top;
}

int main() {
    // Same awareness as pulse.exe (per-monitor v2). The target API level is
    // Windows 8.1, so resolve the Windows 10 entry point at run time.
    using SetContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (auto set = reinterpret_cast<SetContext>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")))
        set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Pure conversion: a 48 px bar at the top, a 60 px bar at the left, a
    // bottom bar (no shift) and a secondary monitor to the left of the primary.
    const RECT mon{0, 0, 1920, 1080};
    RECT r = WorkspaceToScreenRect({100, 200, 900, 800}, mon, {0, 48, 1920, 1080});
    Check(Same(r, {100, 248, 900, 848}), "top taskbar: workspace y is shifted down by the bar");
    r = WorkspaceToScreenRect({100, 200, 900, 800}, mon, {60, 0, 1920, 1080});
    Check(Same(r, {160, 200, 960, 800}), "left taskbar: workspace x is shifted right by the bar");
    r = WorkspaceToScreenRect({100, 200, 900, 800}, mon, {0, 0, 1920, 1032});
    Check(Same(r, {100, 200, 900, 800}), "bottom taskbar: coordinates are unchanged");
    r = WorkspaceToScreenRect({-1800, 100, -1000, 700}, {-2560, -200, 0, 1240}, {-2560, -160, 0, 1240});
    Check(Same(r, {-1800, 140, -1000, 740}), "secondary monitor with a top bar keeps its own offset");
    // A snapped window saves its actual screen rectangle in workspace
    // coordinates: the round trip must land on the same screen rectangle,
    // including a portrait monitor left of the primary.
    const RECT portrait{-1080, -240, 0, 1680}, portrait_work{-1080, -200, 0, 1680};
    const RECT snapped{-1080, -200, 0, 1680};
    Check(Same(WorkspaceToScreenRect(ScreenToWorkspaceRect(snapped, portrait, portrait_work), portrait, portrait_work),
               snapped), "snapped rectangle on a left portrait monitor survives the workspace round trip");
    r = ScreenToWorkspaceRect({160, 200, 960, 800}, mon, {60, 0, 1920, 1080});
    Check(Same(r, {100, 200, 900, 800}), "screen to workspace undoes a left taskbar shift");

    // Multi-window M4b: a new window cascades from its opener.
    const RECT desk{0, 0, 1920, 1040};
    Check(Same(CascadeWindowRect({100, 100, 900, 700}, desk, 32), {132, 132, 932, 732}),
          "cascade: a new window opens 32 px down and right of its opener");
    Check(Same(CascadeWindowRect({1100, 100, 1900, 700}, desk, 32), {0, 0, 800, 600}),
          "cascade: past the right edge it wraps to the work area's top-left");
    Check(Same(CascadeWindowRect({100, 430, 900, 1030}, desk, 32), {0, 0, 800, 600}),
          "cascade: past the bottom edge it wraps to the top-left");
    Check(Same(CascadeWindowRect({-1800, 100, -1000, 700}, {-1920, 0, 0, 1040}, 32), {-1768, 132, -968, 732}),
          "cascade: stays on the opener's (left, negative) monitor");
    Check(Same(CascadeWindowRect({0, 0, 2500, 1500}, desk, 32), {0, 0, 1920, 1040}),
          "cascade: a window larger than the work area is shrunk to fit");
    Check(Same(CascadeWindowRect({0, 40, 800, 640}, {0, 40, 1920, 1080}, 32), {32, 72, 832, 672}),
          "cascade: a top taskbar work area is respected");
    // External open on another monitor (M7a): same spot relative to its work area.
    const RECT right_mon{1920, 0, 4480, 1400};
    Check(Same(MoveToWorkArea({100, 80, 900, 680}, desk, right_mon), {2020, 80, 2820, 680}),
          "other monitor: keeps its offset from the work area's top-left");
    Check(Same(MoveToWorkArea({1500, 600, 1900, 1000}, desk, {-1280, 0, 0, 984}), {-400, 584, 0, 984}),
          "other monitor: pulled back inside a smaller (left) work area");
    Check(Same(MoveToWorkArea({0, 0, 1900, 1040}, desk, {-1280, 0, 0, 984}), {-1280, 0, 0, 984}),
          "other monitor: shrunk to a smaller work area");
    Check(Same(MoveToWorkArea({100, 80, 900, 680}, desk, RECT{}), {100, 80, 900, 680}),
          "other monitor: without a work area the rectangle stays");
    // Session restore (M4d): monitor gone -> primary work area, same spot -> cascade.
    Check(Same(PlaceSessionWindow({300, 200, 1100, 800}, true, desk, {}, 32), {300, 200, 1100, 800}),
          "session window on a monitor keeps its rectangle");
    Check(Same(PlaceSessionWindow({2100, 140, 3300, 980}, false, desk, {}, 32), {0, 0, 1200, 840}),
          "session window whose monitor is gone opens at the primary work area's top-left, same size");
    Check(Same(PlaceSessionWindow({2000, 0, 4560, 1440}, false, desk, {}, 32), {0, 0, 1920, 1040}),
          "session window larger than the primary work area shrinks to fit");
    Check(Same(PlaceSessionWindow({100, 100, 900, 700}, true, desk, {{100, 100, 900, 700}}, 32),
               {132, 132, 932, 732}),
          "session window at the same spot as an earlier one moves 32 down and right");
    Check(Same(PlaceSessionWindow({100, 100, 900, 700}, true, desk,
                                  {{100, 100, 900, 700}, {132, 132, 932, 732}}, 32),
               {164, 164, 964, 764}),
          "session window keeps cascading past every taken spot");
    Check(Same(PlaceSessionWindow({2100, 140, 3300, 980}, false, desk, {{0, 0, 1200, 840}}, 32),
               {32, 32, 1232, 872}),
          "second window from a gone monitor does not cover the first");
    Check(Same(PlaceSessionWindow({100, 100, 900, 700}, true, desk, {{100, 100, 500, 400}}, 32),
               {132, 132, 932, 732}),
          "same top-left with a different size still counts as the same spot");
    Check(Same(CascadeWindowRect({10, 10, 810, 610}, RECT{}, 32), {42, 42, 842, 642}),
          "cascade: without a work area only the offset applies");

    // Live: dock a temporary app bar at the top of the primary monitor so the
    // work area really starts below the monitor top, then relaunch twice.
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"PulsePlacementTest";
    RegisterClassW(&wc);
    {
        // Not snapped (normal or maximized): the saved rectangle stays rcNormalPosition.
        HWND plain = CreateWindowExW(0, wc.lpszClassName, L"placement", WS_OVERLAPPEDWINDOW,
            200, 200, 640, 480, nullptr, nullptr, inst, nullptr);
        ShowWindow(plain, SW_SHOWNOACTIVATE);
        WINDOWPLACEMENT wp{sizeof(wp)};
        GetWindowPlacement(plain, &wp);
        Check(Same(SavedNormalRect(plain, wp), wp.rcNormalPosition), "unsnapped window saves rcNormalPosition");
        ShowWindow(plain, SW_MAXIMIZE);
        GetWindowPlacement(plain, &wp);
        Check(Same(SavedNormalRect(plain, wp), wp.rcNormalPosition), "maximized window saves its restore rectangle");
        DestroyWindow(plain);
    }
    HWND bar =CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"bar", WS_POPUP,
        0, 0, 10, 10, nullptr, nullptr, inst, nullptr);
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = bar;
    abd.uCallbackMessage = WM_APP + 1;
    bool registered = SHAppBarMessage(ABM_NEW, &abd) != 0;
    const LONG kBar = 64;
    if (registered) {
        abd.uEdge = ABE_TOP;
        abd.rc = {0, 0, GetSystemMetrics(SM_CXSCREEN), kBar};
        SHAppBarMessage(ABM_QUERYPOS, &abd);
        abd.rc.bottom = abd.rc.top + kBar;
        SHAppBarMessage(ABM_SETPOS, &abd);
        MoveWindow(bar, abd.rc.left, abd.rc.top, abd.rc.right - abd.rc.left, kBar, TRUE);
        PumpMessages();
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const LONG offset = mi.rcWork.top - mi.rcMonitor.top;
    if (!registered || offset <= 0) {
        std::printf("[SKIP] live app bar check: work area did not move (registered=%d offset=%ld)\n",
                    registered ? 1 : 0, offset);
    } else {
        const RECT start{300, 400, 1100, 1000};
        const LONG old_drift = RelaunchDrift(inst, wc.lpszClassName, start, false);
        const LONG new_drift = RelaunchDrift(inst, wc.lpszClassName, start, true);
        std::printf("work area offset=%ld old drift=%ld new drift=%ld\n", offset, old_drift, new_drift);
        Check(old_drift == -offset, "reproduced: the unconverted rectangle drifts up by the bar height");
        Check(new_drift == 0, "converted rectangle reopens at the same screen position");
    }
    if (registered) SHAppBarMessage(ABM_REMOVE, &abd);
    DestroyWindow(bar);

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
