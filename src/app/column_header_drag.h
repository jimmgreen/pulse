// column_header_drag.h — Details header: click sorts, drag reorders columns.
//
// A press on a reorderable column header (the optional columns after Name,
// and Path in search views) is held until release: a release in place sorts
// as before, moving past the drag threshold picks the column up and shows
// where it would land; release commits AppPrefs::details_column_order, Esc
// or a lost capture cancels. Name, This PC and media columns sort on press.
#pragma once
#include "../ui/ui_view_model.h"
#include <windows.h>

namespace pulse {
struct AppState;

// Arms the gesture for a ColumnHeader hit. False: not reorderable, the
// caller handles the press as before.
bool BeginColumnHeaderPress(AppState& s, const ui::WindowViewModel& vm, const ui::HitTestResult& hit,
                            int mx, int my);
bool ColumnHeaderPressTracking(const AppState& s);
// WM_MOUSEMOVE while tracking.
void UpdateColumnHeaderDrag(AppState& s, int mx, int my);
// WM_LBUTTONUP: commit the move, or sort when the pointer never left.
void EndColumnHeaderPress(AppState& s);
// Esc / lost capture. Returns whether a gesture was cancelled.
bool CancelColumnHeaderPress(AppState& s);

} // namespace pulse
