// tray_reveal.cpp — Closing to the tray and coming back (see tray_reveal.h).
#include "tray_reveal.h"
#include "app_internal.h"
#include "extensions/native/session.h"
#include "startup_location.h"
#include "window_identity_ui.h"
#include "../common/runtime_log.h"

namespace pulse {

void HideMainWindowToTray(AppState& s) {
    s.hidden_to_tray = true;
    s.detailsPreview.Close();
    s.tray_controller.HideWindow(WantsTrayIcon(s, true));
    SyncWindowIdentities(s);  // it no longer counts as an open window
}

void RevealWindow(AppState& s) {
    const HWND window = s.hwnd;
    const auto started = GetTickCount64();
    const auto before = app::InspectWindowRevealState(window);
    const bool was_hidden_to_tray = s.hidden_to_tray;
    bool operation_started = false, via_tray = false;
    const auto finish = [&](const wchar_t* reason) {
        const auto after = app::InspectWindowRevealState(window);
        const bool target_changed = s.hwnd != window;
        const bool revealed = !target_changed && after.Restored();
        diagnostics::runtime::TextEvent("window_reveal_end", {{"reason", revealed ? L"revealed" : reason}}, {
            {"window", reinterpret_cast<uintptr_t>(window)}, {"window_valid", after.valid},
            {"visible_after", after.visible}, {"minimized_after", after.minimized},
            {"foreground_after", after.foreground}, {"reveal_ok", revealed},
            {"operation_started", operation_started}, {"via_tray", via_tray},
            {"target_changed", target_changed}, {"elapsed_ms", GetTickCount64() - started}},
            revealed ? diagnostics::runtime::Level::Info : diagnostics::runtime::Level::Warning);
    };
    diagnostics::runtime::Event("window_reveal_begin", {
        {"window", reinterpret_cast<uintptr_t>(window)}, {"window_valid", before.valid},
        {"visible_before", before.visible}, {"minimized_before", before.minimized},
        {"hidden_to_tray", was_hidden_to_tray}});
    if (!before.valid) { finish(L"invalid_window"); return; }
    s.core.tray_hibernated = false;
    s.core.all_windows_hidden_since_tick = 0;
    diagnostics::runtime::Event("window_reveal_wake_begin", {{"window", reinterpret_cast<uintptr_t>(window)}});
    s.compositor.EnsureAwake();
    diagnostics::runtime::Event("window_reveal_wake_end", {
        {"window", reinterpret_cast<uintptr_t>(window)}, {"elapsed_ms", GetTickCount64() - started}});
    if (s.hwnd != window) { finish(L"target_changed"); return; }
    if (!IsWindow(window)) { finish(L"window_destroyed"); return; }
    const HWND tray_window = s.tray_controller.Window();
    // An unbound tray controller must not suppress this window's direct show path.
    via_tray = tray_window == window;
    operation_started = true;
    if (via_tray) {
        s.tray_controller.RestoreWindow();
    } else {
        const int requested_show = IsIconic(window) ? SW_RESTORE : SW_SHOW;
        const bool previous_visible = ShowWindow(window, requested_show) != FALSE;
        const bool foreground_attempted = s.hwnd == window && app::InspectWindowRevealState(window).Restored();
        const bool foreground_set = foreground_attempted && SetForegroundWindow(window);
        diagnostics::runtime::Event("window_reveal_show", {
            {"window", reinterpret_cast<uintptr_t>(window)}, {"requested_show", static_cast<uint64_t>(requested_show)},
            {"show_previous_visible", previous_visible}, {"foreground_attempted", foreground_attempted},
            {"foreground_set", foreground_set}});
    }
    if (s.hwnd != window) { finish(L"target_changed"); return; }
    const auto after = app::InspectWindowRevealState(window);
    if (!after.valid) { finish(L"window_destroyed"); return; }
    s.hidden_to_tray = after.Restored() ? false : was_hidden_to_tray;
    SyncWindowIdentities(s);
    finish(L"not_visible");
}

bool WantsTrayIcon(const AppState& s, bool window_hidden) {
    return app::TrayIconWanted(
        s.appPrefs.KeepsRunningInBackground() || extensions::native::BackgroundPackageEnabled(),
        s.appPrefs.notify_icon_mode, window_hidden);
}

bool TakeFreshStart(AppState& s) {
    const bool hidden = s.hidden_to_tray;
    s.hidden_to_tray = false;
    return hidden && !app::RestoresLastTabs(s.appPrefs);
}

void StartFreshAt(AppState& s, const std::wstring& path) {
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    if (s.addressEditing) HideAddressEditor(s, false);
    OpenTabAt(s, path);
    // Highest index first; WindowTabs::CloseTab keeps pinned tabs and fixes
    // up the active index when a tab before it goes away.
    for (size_t i = s.window_tabs.items.size(); i-- > 0;) {
        if (i != s.window_tabs.active) CloseLayoutTab(s, i);
    }
}

void InstallTrayRevealHook(AppState& s) {
    s.tray_controller.SetBeforeRestore([&s] {
        s.core.tray_hibernated = false;
        s.core.all_windows_hidden_since_tick = 0;
        s.compositor.EnsureAwake();
        if (TakeFreshStart(s)) StartFreshAt(s, app::DefaultLocation(s.appPrefs));
        SyncWindowIdentities(s);  // counts as an open window again
        // A sign-in launch into the tray (startup_launch.h) shows the icon even
        // when neither setting keeps it; back to the usual rule once revealed.
        s.tray_controller.SetVisible(WantsTrayIcon(s, false));
    });
}

} // namespace pulse
