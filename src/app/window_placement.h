// window_placement.h — restoring the saved main-window rectangle.
#pragma once

#include <windows.h>

#include <vector>

namespace pulse::app {

// GetWindowPlacement reports rcNormalPosition in workspace coordinates: they
// are relative to the work area of the window's monitor, so a taskbar or
// another app bar docked at the top or left shifts them against screen
// coordinates. CreateWindowEx takes screen coordinates; handing it the saved
// rectangle directly moved the window up (or left) by the bar's size on every
// launch.
RECT WorkspaceToScreenRect(const RECT& workspace, const RECT& monitor, const RECT& work);

// Inverse of WorkspaceToScreenRect.
RECT ScreenToWorkspaceRect(const RECT& screen, const RECT& monitor, const RECT& work);

// Screen rectangle for a saved rcNormalPosition, using the monitor it lies on.
RECT RestoredWindowRect(const RECT& workspace);

// The rectangle to save for a window, in workspace coordinates like
// rcNormalPosition. A snapped or arranged window (Snap layouts, dragged to an
// edge, top border double-clicked) keeps its pre-snap size in
// rcNormalPosition, so the next launch reopened at the old size and place;
// such a window saves the rectangle it actually has.
RECT SavedNormalRect(HWND hwnd, const WINDOWPLACEMENT& placement);

// Where a new window opens next to its opener (multi-window M4b, plan §5 P1).
// `opener` is the opener's normal-state screen rectangle, `work` the work
// area of its monitor. `offset` pixels down and right; when that leaves the
// work area, the work area's top-left. The size is kept, shrunk to fit.
RECT CascadeWindowRect(const RECT& opener, const RECT& work, int offset);

// Where a folder opened from another monitor gets its new window (M7a, plan
// §6 rule 6): `rect` (the opener's normal rectangle, on `from_work`) keeps its
// place relative to the work area's top-left on `to_work`, its size shrunk to
// fit and its position pulled back inside. Without a target work area, `rect`.
RECT MoveToWorkArea(const RECT& rect, const RECT& from_work, const RECT& to_work);

// Where a window of a restored session opens (multi-window M4d, plan §7).
// `saved` is its screen rectangle (RestoredWindowRect), `on_monitor` whether
// that still overlaps a monitor, `work` the work area it opens in (its
// monitor's, or the primary monitor's when that monitor is gone) and `taken`
// the rectangles of the windows restored before it. A window whose monitor is
// gone opens at the work area's top-left, its size kept but shrunk to fit; a
// window with the same top-left as one in `taken` moves on by `offset`
// (CascadeWindowRect) until it no longer has.
RECT PlaceSessionWindow(const RECT& saved, bool on_monitor, const RECT& work,
                        const std::vector<RECT>& taken, int offset);

} // namespace pulse::app
