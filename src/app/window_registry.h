#pragma once
// Multi-window support: Pulse runs several top-level windows in one process.
//
// Every window owns a heap-allocated AppState stored in its GWLP_USERDATA slot
// (see GetAppState), so the renderer, compositor, tabs and panes of one window
// never touch another's. This registry is the single owner of those AppStates
// and answers the two questions the rest of the app asks:
//
//   * how many windows are alive (the process exits only when the last closes);
//   * which window receives program-level requests (tray icon, single-instance
//     endpoint, updater, hotkeys) - that is always the first window created.
//
// The registry does not know how to create a window; app_main.cpp installs a
// creator callback at startup so the window procedure and its CreateWindowExW
// call stay in one place.

#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace pulse {

struct AppState;

// Creates the window for `state` and returns its handle, or null on failure.
// Implemented in app_main.cpp and installed below.
using PulseWindowCreator = std::function<HWND(AppState* state, int show_cmd)>;

void SetPulseWindowCreator(PulseWindowCreator creator);

// Creates the primary window for `state`. The caller keeps ownership of
// `state` (it lives on wWinMain's stack), so the registry never frees it.
HWND CreatePrimaryPulseWindow(AppState* state, int show_cmd);

// Creates an additional top-level window with a heap-allocated AppState aimed
// at `path`. The registry owns that AppState and frees it on close. `path` may
// be empty, in which case the new window falls back to the default location.
HWND OpenPulseWindow(const std::wstring& path);

// Window that owns process-level resources: tray icon, single-instance
// endpoint, global hotkeys, update checks. Null once it has closed.
HWND PrimaryPulseWindow();
AppState* PrimaryPulseState();

// Every live window, in creation order. Used to broadcast preference and
// theme changes and to save per-window sessions on exit.
std::vector<HWND> PulseWindows();

// True when only `hwnd` is left; closing it ends the process.
bool IsLastPulseWindow(HWND hwnd);

// True when `hwnd` currently owns process-level resources.
bool IsPrimaryPulseWindow(HWND hwnd);

// Unregisters `hwnd` and marks its AppState for deletion once the owning
// window procedure has returned (WM_DESTROY runs while AppState is still on
// the stack, so it must not be freed there). Returns true when this was the
// last window, which is the caller's signal to quit the message loop.
bool ReleasePulseWindow(HWND hwnd);

// Frees the AppStates retired by ReleasePulseWindow. Call after the message
// loop drains, before the process exits.
void ReclaimRetiredPulseStates();

// A window other than `hwnd`, used as the fallback owner when the primary
// window closes but others remain. Null when `hwnd` is the only window.
HWND AnyOtherPulseWindow(HWND hwnd);

// Assigns a new primary window (called when the old primary closes).
void ElectPrimaryPulseWindow(HWND hwnd);

} // namespace pulse
