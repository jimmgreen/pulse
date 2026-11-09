// tray_reveal.cpp — Closing to the tray and coming back (see tray_reveal.h).
#include "tray_reveal.h"
#include "app_internal.h"
#include "startup_location.h"
#include "window_identity_ui.h"

namespace pulse {

void HideMainWindowToTray(AppState& s) {
    s.hidden_to_tray = true;
    s.tray_controller.HideWindow(WantsTrayIcon(s, true));
    SyncWindowIdentities(s);  // it no longer counts as an open window
}

void RevealWindow(AppState& s) {
    const HWND tray_window = s.tray_controller.Window();
    if (!tray_window || tray_window == s.hwnd || !s.hwnd) {
        s.tray_controller.RestoreWindow();
    } else {
        ShowWindow(s.hwnd, IsIconic(s.hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(s.hwnd);
    }
    // Back on screen: it counts again (the restore hook cleared the flag).
    s.hidden_to_tray = false;
    SyncWindowIdentities(s);
}

bool WantsTrayIcon(const AppState& s, bool window_hidden) {
    return app::TrayIconWanted(s.appPrefs.KeepsRunningInBackground(),
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
        if (TakeFreshStart(s)) StartFreshAt(s, app::DefaultLocation(s.appPrefs));
        SyncWindowIdentities(s);  // counts as an open window again
        // A sign-in launch into the tray (startup_launch.h) shows the icon even
        // when neither setting keeps it; back to the usual rule once revealed.
        s.tray_controller.SetVisible(WantsTrayIcon(s, false));
    });
}

} // namespace pulse
