#include <windows.h>
#include <cstdio>
namespace fixture {
HMONITOR primary = reinterpret_cast<HMONITOR>(1), secondary = reinterpret_cast<HMONITOR>(2);
POINT cursor{100, 100};
bool cursor_ok = true, secondary_present = true, info_ok = true, work_ok = true;
BOOL WINAPI Cursor(LPPOINT point) { *point = cursor; return cursor_ok; }
HWND WINAPI Foreground() { return reinterpret_cast<HWND>(3); }
HMONITOR WINAPI Window(HWND window, DWORD) { return window && secondary_present ? secondary : primary; }
HMONITOR WINAPI Point(POINT point, DWORD flags) {
    if (point.y >= 0 && point.y < 1440 && point.x >= 0 && point.x < 2560) return primary;
    if (secondary_present && point.y >= 0 && point.y < 1440 && point.x >= -2560 && point.x < 0) return secondary;
    if (flags == MONITOR_DEFAULTTONULL) return nullptr;
    return secondary_present && point.x < 0 ? secondary : primary;
}
BOOL WINAPI Info(HMONITOR monitor, LPMONITORINFO info) {
    if (!info_ok || (monitor != primary && monitor != secondary) || (monitor == secondary && !secondary_present)) return FALSE;
    info->rcWork = monitor == primary ? RECT{0, 0, 2560, 1400} : RECT{-2560, 40, 0, 1440};
    return TRUE;
}
BOOL WINAPI Parameters(UINT, UINT, PVOID output, UINT) { *static_cast<RECT*>(output) = {0, 0, 1920, 1040}; return work_ok; }
int WINAPI Metrics(int value) { return value == SM_CXSCREEN ? 1920 : 1080; }
}
#define GetCursorPos fixture::Cursor
#define GetForegroundWindow fixture::Foreground
#define MonitorFromWindow fixture::Window
#define MonitorFromPoint fixture::Point
#define GetMonitorInfoW fixture::Info
#define SystemParametersInfoW fixture::Parameters
#define GetSystemMetrics fixture::Metrics
#include "../app/global_search_monitor.h"
int main() {
    using namespace pulse::app;
    unsigned checks = 0, failures = 0;
    auto check = [&](bool ok, const char* label) { ++checks; failures += !ok; std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); };
    check(SearchSummonMonitor() == fixture::primary, "pointer screen overrides foreground screen");
    check(!SearchRestorePosition(fixture::primary, true, {-2000, 100}), "saved secondary position does not pin the summon monitor");
    fixture::cursor = {-100, 100};
    check(SearchSummonMonitor() == fixture::secondary, "next summon follows pointer to negative-coordinate screen");
    check(SearchRestorePosition(fixture::secondary, true, {-2000, 100}), "same-screen saved position is retained");
    check(!SearchRestorePosition(fixture::secondary, false, {-2000, 100}), "unsaved coordinates never become a placement");
    check(!SearchRestorePosition(nullptr, true, {1000000, 1000000}), "failed monitor lookup cannot restore disconnected coordinates");
    fixture::cursor_ok = false;
    check(SearchSummonMonitor() == fixture::secondary, "cursor query failure falls back to foreground screen");
    fixture::secondary_present = false;
    check(!SearchRestorePosition(fixture::primary, true, {-2000, 100}), "disconnected screen coordinates are not restored on nearest screen");
    HMONITOR target = fixture::secondary;
    const RECT fallback = SearchMonitorWorkArea(target);
    check(target == fixture::primary && fallback.right == 2560 && fallback.bottom == 1400, "removed monitor falls back to primary work area");
    fixture::info_ok = false;
    check(SearchMonitorWorkArea(target).bottom == 1040, "monitor-info failure uses system work area");
    fixture::work_ok = false;
    const RECT metrics = SearchMonitorWorkArea(target);
    check(metrics.right == 1920 && metrics.bottom == 1080, "last fallback has positive screen dimensions");
    std::printf("MONITOR_POLICY checks=%u failures=%u os_monitor_api=modeled\n", checks, failures);
    return failures ? 1 : 0;
}
