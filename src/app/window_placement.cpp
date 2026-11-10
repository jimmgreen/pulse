// window_placement.cpp — see window_placement.h.
#include "window_placement.h"

#include <algorithm>

namespace pulse::app {
namespace {

bool MonitorRects(HMONITOR monitor, RECT& bounds, RECT& work) {
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return false;
    bounds = info.rcMonitor;
    work = info.rcWork;
    return true;
}

} // namespace

RECT WorkspaceToScreenRect(const RECT& workspace, const RECT& monitor, const RECT& work) {
    RECT screen = workspace;
    OffsetRect(&screen, work.left - monitor.left, work.top - monitor.top);
    return screen;
}

RECT ScreenToWorkspaceRect(const RECT& screen, const RECT& monitor, const RECT& work) {
    RECT workspace = screen;
    OffsetRect(&workspace, monitor.left - work.left, monitor.top - work.top);
    return workspace;
}

RECT SavedNormalRect(HWND hwnd, const WINDOWPLACEMENT& placement) {
    if (placement.showCmd != SW_SHOWNORMAL || !hwnd) return placement.rcNormalPosition;
    // Windows 10 1903+; the target API level is Windows 8.1.
    using IsArranged = BOOL(WINAPI*)(HWND);
    static const auto is_arranged = reinterpret_cast<IsArranged>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "IsWindowArranged"));
    RECT screen{}, bounds{}, work{};
    if (!is_arranged || !is_arranged(hwnd) || !GetWindowRect(hwnd, &screen) ||
        screen.right <= screen.left || screen.bottom <= screen.top ||
        !MonitorRects(MonitorFromRect(&screen, MONITOR_DEFAULTTONEAREST), bounds, work))
        return placement.rcNormalPosition;
    return ScreenToWorkspaceRect(screen, bounds, work);
}

RECT RestoredWindowRect(const RECT& workspace) {
    RECT bounds{}, work{};
    HMONITOR monitor = MonitorFromRect(&workspace, MONITOR_DEFAULTTONEAREST);
    if (!MonitorRects(monitor, bounds, work)) return workspace;
    RECT screen = WorkspaceToScreenRect(workspace, bounds, work);
    // Near a monitor edge the unconverted rectangle can pick the neighbour;
    // the converted one names the monitor whose work area it belongs to.
    HMONITOR converted = MonitorFromRect(&screen, MONITOR_DEFAULTTONEAREST);
    if (converted != monitor && MonitorRects(converted, bounds, work))
        screen = WorkspaceToScreenRect(workspace, bounds, work);
    return screen;
}

RECT CascadeWindowRect(const RECT& opener, const RECT& work, int offset) {
    const LONG work_w = work.right - work.left;
    const LONG work_h = work.bottom - work.top;
    const bool has_work = work_w > 0 && work_h > 0;
    LONG w = opener.right - opener.left;
    LONG h = opener.bottom - opener.top;
    if (has_work) {
        w = (std::min)(w, work_w);
        h = (std::min)(h, work_h);
    }
    RECT r{opener.left + offset, opener.top + offset, 0, 0};
    if (has_work && (r.left < work.left || r.top < work.top || r.left + w > work.right || r.top + h > work.bottom)) {
        r.left = work.left;
        r.top = work.top;
    }
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}

RECT MoveToWorkArea(const RECT& rect, const RECT& from_work, const RECT& to_work) {
    const LONG work_w = to_work.right - to_work.left;
    const LONG work_h = to_work.bottom - to_work.top;
    if (work_w <= 0 || work_h <= 0) return rect;
    const LONG w = (std::min)(rect.right - rect.left, work_w);
    const LONG h = (std::min)(rect.bottom - rect.top, work_h);
    LONG left = to_work.left + (rect.left - from_work.left);
    LONG top = to_work.top + (rect.top - from_work.top);
    left = (std::max)(to_work.left, (std::min)(left, to_work.right - w));
    top = (std::max)(to_work.top, (std::min)(top, to_work.bottom - h));
    return RECT{left, top, left + w, top + h};
}

RECT PlaceSessionWindow(const RECT& saved, bool on_monitor, const RECT& work,
                        const std::vector<RECT>& taken, int offset) {
    RECT r = saved;
    if (!on_monitor) {
        LONG w = saved.right - saved.left;
        LONG h = saved.bottom - saved.top;
        if (work.right > work.left && work.bottom > work.top) {
            w = (std::min)(w, work.right - work.left);
            h = (std::min)(h, work.bottom - work.top);
        }
        r = {work.left, work.top, work.left + w, work.top + h};
    }
    const auto clashes = [&taken](const RECT& c) {
        return std::any_of(taken.begin(), taken.end(),
                           [&c](const RECT& t) { return t.left == c.left && t.top == c.top; });
    };
    // Each step lands on a new position or wraps to the top-left, so
    // taken.size() + 1 steps are enough unless every position is taken.
    for (size_t step = 0; step <= taken.size() && clashes(r); ++step)
        r = CascadeWindowRect(r, work, offset);
    return r;
}

} // namespace pulse::app
