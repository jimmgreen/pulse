#include "window_factory.h"
#include "extensions/native/package_lifecycle.h"
#include "../ui/ui_renderer.h"

#include "app_messages.h"
#include "app_state.h"
#include "single_instance_coordinator.h"
#include "window_placement.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace pulse {
namespace {

// UI thread only.
std::vector<std::unique_ptr<AppState>> g_windows;
std::vector<std::unique_ptr<AppState>> g_retired;

// The layout a new window starts with: the opener's sidebar and details pane.
// Process-wide preferences live in the shared core already.
void InheritWindowLayout(const AppState& from, AppState& to) {
    to.sidebarCollapsedMask = from.sidebarCollapsedMask;
    to.sidebarHiddenMask = from.sidebarHiddenMask;
    to.sidebarOrder = from.sidebarOrder;
    to.sidebarQuickAccessHiddenMask = from.sidebarQuickAccessHiddenMask;
    to.starredExpanded = from.starredExpanded;
    to.showDetailsPanel = from.showDetailsPanel;
    to.detailsPanelWidth = from.detailsPanelWidth;
    to.detailsPreviewOnly = from.detailsPreviewOnly;
    to.detailsPreviewEnabled = from.detailsPreviewEnabled;
    to.detailsPreviewExpansion = from.detailsPreviewExpansion;
    to.showFps = from.showFps;
    to.forceStatusPerformance = from.forceStatusPerformance;
}

// Normal-state screen rectangle of a window (empty when it has none).
RECT NormalScreenRect(HWND hwnd) {
    WINDOWPLACEMENT placement{sizeof(placement)};
    MONITORINFO monitor{sizeof(monitor)};
    if (!hwnd || !GetWindowPlacement(hwnd, &placement) ||
        !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
        return RECT{};
    return app::WorkspaceToScreenRect(placement.rcNormalPosition, monitor.rcMonitor, monitor.rcWork);
}

// Normal-state screen rectangle of `from`, cascaded by 32 DIP within its
// monitor's work area (CascadeWindowRect). A maximized opener does not pass
// its maximized state on.
RECT CascadedRect(const AppState& from) {
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(from.hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) return RECT{};
    RECT normal{};
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (IsIconic(from.hwnd) || IsZoomed(from.hwnd)) {
        if (!GetWindowPlacement(from.hwnd, &placement)) return RECT{};
        normal = app::WorkspaceToScreenRect(placement.rcNormalPosition, monitor.rcMonitor, monitor.rcWork);
    } else if (!GetWindowRect(from.hwnd, &normal)) {
        return RECT{};
    }
    if (normal.right <= normal.left || normal.bottom <= normal.top) return RECT{};
    const int offset = static_cast<int>(std::lround(32.0f * (from.scale > 0.0f ? from.scale : 1.0f)));
    return app::CascadeWindowRect(normal, monitor.rcWork, offset);
}

}  // namespace

HWND OpenPulseWindow(AppState& from, const WindowInit& init) {
    ReleaseRetiredWindowStates();
    auto owned = std::make_unique<AppState>(from.core);
    AppState* const state = owned.get();
    state->isolatedTest = from.isolatedTest;
    state->isolatedTestPersist = from.isolatedTestPersist;
    InheritWindowLayout(from, *state);
    if (init.restore && init.restore->layout) ApplyWindowLayout(*state, *init.restore->layout);
    state->has_window_start = true;
    state->window_start_path = init.path;
    state->window_restore = init.restore;
    if (init.restore) state->identity = init.restore->identity;  // M7c
    state->window_adopt_from = init.adopt_from;
    state->window_adopt_index = init.adopt_index;
    g_windows.push_back(std::move(owned));

    RECT rect = init.rect ? *init.rect : CascadedRect(from);
    const HMONITOR from_monitor = MonitorFromWindow(from.hwnd, MONITOR_DEFAULTTONEAREST);
    POINT cursor{};
    const HMONITOR target_monitor = init.monitor ? init.monitor :
        GetCursorPos(&cursor) ? MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST) : from_monitor;
    if (!init.rect && !init.restore && target_monitor != from_monitor) {
        MONITORINFO there{sizeof(there)};
        MONITORINFO here{sizeof(here)};
        const RECT normal = NormalScreenRect(from.hwnd);
        if (normal.right > normal.left && GetMonitorInfoW(target_monitor, &there) && GetMonitorInfoW(from_monitor, &here))
            rect = app::CascadeWindowRect(app::MoveToWorkArea(normal, here.rcWork, there.rcWork),
                there.rcWork, static_cast<int>(std::lround(32.0f * (from.scale > 0.0f ? from.scale : 1.0f))));
    }
    bool maximize = false;
    if (init.restore) {
        const RECT& back = init.restore->rect;
        if (back.right > back.left && back.bottom > back.top && MonitorFromRect(&back, MONITOR_DEFAULTTONULL))
            rect = back;
        maximize = init.restore->maximized;
    }
    const bool placed = rect.right > rect.left;
    const HWND hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP, app::SingleInstanceCoordinator::WindowClassName(), L"Pulse", kMainWindowStyle,
        placed ? rect.left : CW_USEDEFAULT, placed ? rect.top : CW_USEDEFAULT,
        placed ? rect.right - rect.left : CW_USEDEFAULT, placed ? rect.bottom - rect.top : CW_USEDEFAULT,
        nullptr, nullptr, GetModuleHandleW(nullptr), state);
    if (!hwnd) {
        // A failed WM_CREATE still destroys the window, which retires the
        // state; otherwise it was never handed to a window at all.
        const auto it = std::find_if(g_windows.begin(), g_windows.end(),
                                     [state](const std::unique_ptr<AppState>& w) { return w.get() == state; });
        if (it != g_windows.end()) g_windows.erase(it);
        return nullptr;
    }
    // Each real owner participates so closing the first window cannot stop store trust monitoring.
    if (!extensions::native::StartPackageMonitoring(hwnd)) {
        DestroyWindow(hwnd);
        return nullptr;
    }
    ShowWindow(hwnd, maximize ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    return hwnd;
}

app::WindowLayoutSnapshot CaptureWindowLayout(const AppState& s) {
    app::WindowLayoutSnapshot layout;
    layout.sidebar_collapsed = static_cast<int>(s.sidebarCollapsedMask);
    layout.sidebar_hidden = static_cast<int>(s.sidebarHiddenMask);
    layout.sidebar_order = app::NormalizeSidebarOrder(s.sidebarOrder);
    layout.quick_access_hidden = static_cast<int>(s.sidebarQuickAccessHiddenMask);
    layout.starred_expanded = s.starredExpanded;
    layout.details_panel = s.showDetailsPanel;
    layout.details_panel_width = static_cast<int>(std::lround(s.detailsPanelWidth));
    layout.details_preview_only = s.detailsPreviewOnly;
    layout.details_preview = s.detailsPreviewEnabled;
    return layout;
}

// Mirrors WinMain's restore of the first window's layout from the session.
void ApplyWindowLayout(AppState& s, const app::WindowLayoutSnapshot& layout) {
    s.sidebarCollapsedMask = static_cast<uint32_t>(layout.sidebar_collapsed);
    s.sidebarHiddenMask = static_cast<uint32_t>(layout.sidebar_hidden);
    s.sidebarOrder = layout.sidebar_order.empty() ? app::DefaultSidebarOrder() : layout.sidebar_order;
    s.sidebarQuickAccessHiddenMask = static_cast<uint32_t>(layout.quick_access_hidden);
    s.starredExpanded = layout.starred_expanded;
    s.showDetailsPanel = layout.details_panel;
    s.detailsPreviewOnly = layout.details_preview_only;
    s.detailsPreviewEnabled = layout.details_preview;
    s.detailsPreviewExpansion = layout.details_preview_only ? 1.0f : 0.0f;
    s.detailsPanelWidth = static_cast<float>(layout.details_panel_width);
    s.renderer.SetDetailsPanelWidth(s.detailsPanelWidth);
}

void OpenSessionWindows(AppState& first) {
    std::vector<app::WindowSessionSnapshot> pending;
    pending.swap(first.core.pending_session_windows);
    if (pending.empty() || !first.hwnd) return;
    MONITORINFO primary{sizeof(primary)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &primary);
    const int offset = static_cast<int>(std::lround(32.0f * (first.scale > 0.0f ? first.scale : 1.0f)));
    std::vector<RECT> taken;
    if (const RECT own = NormalScreenRect(first.hwnd); own.right > own.left) taken.push_back(own);
    // Most recent first in the session: open the least recent first, so each
    // later one stacks above it.
    for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
        app::WindowSessionSnapshot& saved = *it;
        if (saved.layout_tabs.empty()) continue;
        RECT rect{};
        bool on_monitor = false;
        if (saved.window_rect.right > saved.window_rect.left && saved.window_rect.bottom > saved.window_rect.top) {
            rect = app::RestoredWindowRect(saved.window_rect);
            on_monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) != nullptr;
        } else if (!taken.empty()) {
            rect = taken.front();  // no saved place: cascade from the first window
            on_monitor = true;
        }
        MONITORINFO monitor{sizeof(monitor)};
        const RECT work = on_monitor && GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &monitor)
            ? monitor.rcWork : primary.rcWork;
        if (rect.right > rect.left) rect = app::PlaceSessionWindow(rect, on_monitor, work, taken, offset);
        taken.push_back(rect);

        app::ClosedWindow restore;
        restore.rect = rect;
        restore.maximized = saved.maximized;
        restore.layout_tabs = std::move(saved.layout_tabs);
        restore.tab_groups = std::move(saved.tab_groups);
        restore.active_layout_tab = saved.active_layout_tab;
        restore.layout = std::move(saved.layout);
        restore.identity = std::move(saved.identity);
        WindowInit init;
        init.restore = std::move(restore);
        OpenPulseWindow(first, init);
    }
    if (IsWindowVisible(first.hwnd)) SetForegroundWindow(first.hwnd);
}

bool RetireWindowState(AppState& s) {
    const auto it = std::find_if(g_windows.begin(), g_windows.end(),
                                 [&s](const std::unique_ptr<AppState>& w) { return w.get() == &s; });
    if (it == g_windows.end()) return false;
    g_retired.push_back(std::move(*it));
    g_windows.erase(it);
    if (const HWND host = s.core.host.Hwnd()) PostMessageW(host, WM_RELEASE_WINDOW_STATES, 0, 0);
    return true;
}

void ReleaseRetiredWindowStates() {
    // Moved out first: an AppState destructor must not see the list mid-change.
    std::vector<std::unique_ptr<AppState>> retired;
    retired.swap(g_retired);
}

void ReleaseWindowStatesAtExit() {
    std::vector<HWND> open;
    for (const auto& w : g_windows)
        if (w->hwnd && IsWindow(w->hwnd)) open.push_back(w->hwnd);
    for (HWND hwnd : open) DestroyWindow(hwnd);
    ReleaseRetiredWindowStates();
    g_windows.clear();
}

}  // namespace pulse
