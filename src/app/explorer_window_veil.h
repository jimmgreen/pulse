// explorer_window_veil.h — keeps a just-created File Explorer window invisible
// while the takeover decides whether Pulse shows it instead.
//
// The veil is a layered, fully transparent style applied from outside the
// Explorer process; the window keeps running and is restored exactly when the
// takeover leaves it, fails, or runs out of time. A window property marks
// veiled windows so a later Pulse restores any left behind by a crash.
#pragma once
#include <windows.h>

namespace pulse::app {

// Veils `hwnd` unless it is already layered or veiled. Blocks while Explorer's
// UI thread handles the style change; call only off the UI thread.
bool VeilExplorerWindow(HWND hwnd) noexcept;
// Restores a window veiled by VeilExplorerWindow; no-op for any other window.
void UnveilExplorerWindow(HWND hwnd) noexcept;
bool IsExplorerWindowVeiled(HWND hwnd) noexcept;
// Restores every top-level File Explorer window still carrying the marker.
void UnveilAbandonedExplorerWindows() noexcept;

} // namespace pulse::app
