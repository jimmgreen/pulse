#pragma once

#include <string>
#include <vector>
#include <windows.h>

namespace pulse::app {

// Starts another Pulse window as its own process (`--new-window`). The new
// process skips the single-instance mutex, so it runs beside the window that
// asked for it instead of opening a tab there. `path` may be empty, in which
// case the new window picks its own starting folder.
bool LaunchNewWindow(const std::wstring& path);

// A tab torn off to the desktop keeps what the user had selected: the entries
// travel as `--entry`/`--focus` names, so the new window selects the same rows
// (and previews the same file) once its folder has loaded.
bool LaunchNewWindow(const std::wstring& path, const std::vector<std::wstring>& selected_names,
                     const std::wstring& focus_name);

// The Pulse window under a screen point, unless it is `exclude` - the window
// that is asking, such as a tab drag source. Each instance is one window, so
// this is also how a tab finds the window the user dropped it on.
HWND PulseWindowUnderPoint(POINT screen_point, HWND exclude);

// Every other Pulse window on the desktop (each instance is one window), so the
// primary can ask them to hand their tabs over.
std::vector<HWND> OtherPulseWindows(HWND exclude);

} // namespace pulse::app
