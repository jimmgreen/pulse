// window_placement.h — restoring the saved main-window rectangle.
#pragma once

#include <windows.h>

namespace pulse::app {

// GetWindowPlacement reports rcNormalPosition in workspace coordinates: they
// are relative to the work area of the window's monitor, so a taskbar or
// another app bar docked at the top or left shifts them against screen
// coordinates. CreateWindowEx takes screen coordinates; handing it the saved
// rectangle directly moved the window up (or left) by the bar's size on every
// launch.
RECT WorkspaceToScreenRect(const RECT& workspace, const RECT& monitor, const RECT& work);

// Screen rectangle for a saved rcNormalPosition, using the monitor it lies on.
RECT RestoredWindowRect(const RECT& workspace);

} // namespace pulse::app
