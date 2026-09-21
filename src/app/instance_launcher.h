#pragma once

#include <string>
#include <windows.h>

namespace pulse::app {

// Starts another Pulse window as its own process (`--new-window`). The new
// process skips the single-instance mutex, so it runs beside the window that
// asked for it instead of opening a tab there. `path` may be empty, in which
// case the new window picks its own starting folder.
bool LaunchNewWindow(const std::wstring& path);

// The Pulse window under a screen point, unless it is `exclude` - the window
// that is asking, such as a tab drag source. Each instance is one window, so
// this is also how a tab finds the window the user dropped it on.
HWND PulseWindowUnderPoint(POINT screen_point, HWND exclude);

} // namespace pulse::app
