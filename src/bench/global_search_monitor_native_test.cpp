#include "../app/global_search_monitor.h"
#include <cstdio>
#ifdef PULSE_SEARCH_PLACEMENT_BASELINE
#include PULSE_SEARCH_PLACEMENT_BASELINE
#define SearchWindowPlacement BaselineSearchWindowPlacement
#endif
int main() {
    using namespace pulse::app;
    using SetAwareness = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto awareness = reinterpret_cast<SetAwareness>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (awareness) awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    unsigned checks = 0, failures = 0, displays = 0;
    auto check = [&](bool ok, const char* label) { ++checks; failures += !ok; std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); };
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT, LPARAM param) -> BOOL {
        ++*reinterpret_cast<unsigned*>(param); return TRUE;
    }, reinterpret_cast<LPARAM>(&displays));
    HMONITOR target = SearchSummonMonitor();
    const RECT work = SearchMonitorWorkArea(target);
    HWND window = CreateWindowExW(0, L"STATIC", L"Search placement fixture", WS_POPUP,
        work.left, work.top, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "create hidden native HWND without activating or showing it");
    if (!window) return 2;
    float scale = 1;
    using MonitorDpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    const HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        const auto dpi = reinterpret_cast<MonitorDpi>(GetProcAddress(shcore, "GetDpiForMonitor"));
        UINT x = 96, y = 96;
        if (dpi && SUCCEEDED(dpi(target, 0, &x, &y))) scale = static_cast<float>(x) / 96;
        FreeLibrary(shcore);
    }
    const auto apply = [&](RECT rect) {
        check(SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
            SWP_NOACTIVATE | SWP_NOZORDER) != FALSE, "apply production placement to real hidden HWND");
        RECT actual{};
        check(GetWindowRect(window, &actual) && EqualRect(&actual, &rect), "Windows reports the requested actual window rectangle");
        return actual;
    };
    const POINT removed{1000000, 1000000};
    const auto centered = apply(SearchWindowPlacement(work, scale, true, removed, {800, 500}, SearchRestorePosition(target, true, removed)));
    check(centered.left == work.left + (work.right - work.left - (centered.right - centered.left)) / 2 &&
        centered.top == work.top + (work.bottom - work.top - (centered.bottom - centered.top)) / 2,
        "removed or different monitor saved coordinates center instead of clamping to edge");
    check(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) == target, "actual HWND belongs to pointer-selected physical screen");
    const POINT saved{work.left + 24, work.top + 24};
    const auto restored = apply(SearchWindowPlacement(work, scale, true, saved, {800, 500}, SearchRestorePosition(target, true, saved)));
    check(restored.left == saved.x && restored.top == saved.y, "same-screen stored placement survives cached HWND repositioning");
    for (float dpi : {1.0f, 1.25f, 1.5f, 2.0f}) {
        const RECT modeled_work{-2560, -1440, 0, 0};
        const auto rect = apply(SearchWindowPlacement(modeled_work, dpi, true, saved, {800, 500}, false));
        const int width = static_cast<int>(std::lround(800 * dpi)), height = static_cast<int>(std::lround(500 * dpi));
        check(rect.right - rect.left == width && rect.bottom - rect.top == height, "modeled mixed DPI preserves DIP dimensions");
        check(rect.left == -2560 + (2560 - width) / 2 && rect.top == -1440 + (1440 - height) / 2,
            "negative and above-primary modeled monitor coordinates are centered");
    }
    const RECT small_work{0, 48, 600, 700};
    const auto clipped = apply(SearchWindowPlacement(small_work, 2, true, removed, {10000, 10000}, false));
    check(clipped.left >= small_work.left && clipped.top >= small_work.top && clipped.right <= small_work.right && clipped.bottom <= small_work.bottom,
        "oversized saved window fits small taskbar-adjusted work area");
    check(!IsWindowVisible(window), "native test never displays a window or sends input");
    DestroyWindow(window);
    std::printf("NATIVE_PLACEMENT checks=%u failures=%u physical_monitors=%u actual_scale=%.2f mixed_dpi=model geometry=production hwnd=real hidden=1 input_sent=0\n",
        checks, failures, displays, scale);
    return failures ? 1 : 0;
}
