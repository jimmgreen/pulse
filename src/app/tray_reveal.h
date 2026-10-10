// tray_reveal.h — Closing to the tray and coming back.
//
// With "keep running after close", global search or Explorer window takeover
// on (AppPrefs::KeepsRunningInBackground), the close button only hides the
// window. When startup is set to open the default location, the
// window starts over there when it is shown again, as a fresh launch would;
// with "restore last tabs" it comes back unchanged. Pinned tabs stay.
#pragma once
#include <string>

namespace pulse {
struct AppState;

// The close button / WM_CLOSE path: hide to the tray and remember why.
void HideMainWindowToTray(AppState& s);
// True once after a close-to-tray when startup opens the default location.
// Clears the close-to-tray mark either way.
bool TakeFreshStart(AppState& s);
// Opens `path` (empty = This PC) in a new tab and closes the unpinned others.
void StartFreshAt(AppState& s, const std::wstring& path);
// Lets TrayController::RestoreWindow start over before showing the window.
void InstallTrayRevealHook(AppState& s);
// Whether the notification-area icon should show (#57: always, only while
// the window is closed to it, or never).
bool WantsTrayIcon(const AppState& s, bool window_hidden);

} // namespace pulse
