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

    // Live: dock a temporary app bar at the top of the primary monitor so the
    // work area really starts below the monitor top, then relaunch twice.
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"PulsePlacementTest";
    RegisterClassW(&wc);
    HWND bar = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"bar", WS_POPUP,
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
