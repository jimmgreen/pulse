// app_column_view.h — Column (Miller) view: listing sync and input handling.
#pragma once
#include "app_runtime.h"

namespace pulse {

// Keeps every visible pane's column listings in step with its folder and
// selection. Cheap when nothing changed; starts async loads otherwise.
void SyncColumnStrips(AppState& s);
// Called for every worker result before pane handling.
void NoteColumnStripResult(AppState& s, const app::WorkResult& res);

// Mouse-down on column-view regions (toggle button, rows, dividers).
// Returns true when consumed.
bool HandleColumnStripMouseDown(AppState& s, const ui::WindowViewModel& vm,
                                const ui::HitTestResult& hit, int x);
bool UpdateColumnStripResize(AppState& s, int x);
// Double-click on a divider restores that column's default width.
void ResetColumnStripWidth(AppState& s, const ui::HitTestResult& hit);
void EndColumnStripResize(AppState& s);
// Double-click right after a column click changed the folder: swallow it so
// the second click does not open whatever moved under the cursor.
bool ColumnStripSwallowDoubleClick(const AppState& s);
// Right-click on a column row: selects that item the way a click does (the
// list moves to its folder). Returns true when the item is now selected.
bool SelectColumnStripRow(AppState& s, const ui::HitTestResult& hit);
// Folder owning a column's blank space; empty for stale/invalid hit regions.
// The path is resolved before any navigation or context-menu action.
std::wstring ColumnStripBackgroundPath(AppState& s, const ui::HitTestResult& hit);
// Real destination of a blank Miller column or its folder row, never a guessed
// fallback to the active directory. Empty means the drop must be rejected.
std::wstring ColumnStripDropPath(AppState& s, const ui::HitTestResult& hit);
bool HandleColumnStripWheel(AppState& s, const ui::WindowViewModel& vm,
                            const ui::HitTestResult& hit, int wheel_delta);
// Left/Right in the column view: go to the parent / enter the selected folder.
bool HandleColumnArrowKey(AppState& s, bool right);
void ToggleColumnLayout(AppState& s, int pane_index);

} // namespace pulse
