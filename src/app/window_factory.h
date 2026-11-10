// window_factory.h - Opens further Pulse windows on the shared core
// (multi-window M4a; plan §2, §8).
//
// The first window is created by WinMain and owns the AppCore (its AppState
// lives until the process exits). Every later window gets an AppState bound to
// that core, owned here: WM_CREATE skips the process-level setup for it, and
// after WM_NCDESTROY the state is retired and freed on the host's next message,
// never inside the window's own message handling.
#pragma once
#include <windows.h>

#include "closed_windows.h"

#include <optional>
#include <string>

namespace pulse {

struct AppState;

// Style of every Pulse main window (see WinMain for why WS_CAPTION without
// WS_SYSMENU).
constexpr DWORD kMainWindowStyle = WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN;

struct WindowInit {
    std::wstring path;  // folder of the first tab ("" = This PC)
    // A reopened window (M4c): its tabs, and its place unless that monitor
    // is gone. `path` is not used then.
    std::optional<app::ClosedWindow> restore;
    // Ctrl+Shift+M (M5c): tab `adopt_index` of this window moves in alive
    // instead of a first tab opening; `path` is the fallback if it cannot.
    AppState* adopt_from = nullptr;
    size_t adopt_index = 0;
    // A tab dragged off the strip (M6a): the window opens here, unmaximized.
    std::optional<RECT> rect;
    // A folder opened from outside Pulse (M7a, plan §6 rule 6): the monitor
    // the request came from (its foreground window's). On another monitor
    // than `from`'s the window keeps its spot relative to the work area there.
    HMONITOR monitor = nullptr;
};

// Creates and shows a window on `from`'s core at `init.path`, next to `from`
// and with its per-window layout (sidebar, details pane; a restored window's
// own when it has one). Returns the new window, or nullptr when it could not
// be created.
HWND OpenPulseWindow(AppState& from, const WindowInit& init);
// A window's sidebar and details pane, for the session and closed windows (M4d).
app::WindowLayoutSnapshot CaptureWindowLayout(const AppState& s);
void ApplyWindowLayout(AppState& s, const app::WindowLayoutSnapshot& layout);
// Session v8 (M4d, plan §7): opens core.pending_session_windows next to the
// first window, back to front so the stacking order comes back, then brings
// `first` (the focused window of the session) to the front. A window whose
// monitor is gone opens on the primary monitor; windows at the same spot are
// cascaded (PlaceSessionWindow).
void OpenSessionWindows(AppState& first);

// WM_NCDESTROY: hands the state of a window opened by OpenPulseWindow over for
// release. False for the first window, whose state WinMain owns.
bool RetireWindowState(AppState& s);
// Frees retired states (the host's WM_RELEASE_WINDOW_STATES, and WinMain
// before the core goes away).
void ReleaseRetiredWindowStates();
// WinMain after the message loop: destroys windows still open (an abnormal
// quit) and frees every state before the core goes away.
void ReleaseWindowStatesAtExit();

}  // namespace pulse
