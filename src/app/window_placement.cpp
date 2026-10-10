// window_placement.cpp — see window_placement.h.
#include "window_placement.h"

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

} // namespace pulse::app
