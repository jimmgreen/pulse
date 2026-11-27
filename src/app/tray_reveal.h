// tray_reveal.h — Closing to the tray and coming back.
//
// With "keep running after close" on (AppPrefs::KeepsRunningInBackground,
// the only switch that decides it), the close button only hides the
// window. When startup is set to open the default location, the
// window starts over there when it is shown again, as a fresh launch would;
// with "restore last tabs" it comes back unchanged. Pinned tabs stay.
#pragma once
#include <string>
#include <windows.h>

namespace pulse {
namespace app {
// Win32 visibility only, not a first-paint or current-virtual-desktop guarantee.
struct WindowRevealState {
    bool valid = false, visible = false, minimized = false, foreground = false;
    // Foreground activation can be denied by Windows; it is not a visibility failure.
    bool Restored() const noexcept { return valid && visible && !minimized; }
};
inline WindowRevealState InspectWindowRevealState(HWND window) noexcept {
    WindowRevealState state;
    state.valid = window && IsWindow(window);
    if (state.valid) {
        state.visible = IsWindowVisible(window) != FALSE;
        state.minimized = IsIconic(window) != FALSE;
        state.foreground = GetForegroundWindow() == window;
    }
    return state;
}
} // namespace app
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
// Shows and activates `s` (multi-window M4b). The tray controller hides and
// restores one window; when `s` is another one it is shown directly, so a
// request for one window never brings up a different one.
void RevealWindow(AppState& s);
// Whether the notification-area icon should show (#57: always, only while
// the window is closed to it, or never).
bool WantsTrayIcon(const AppState& s, bool window_hidden);

} // namespace pulse
