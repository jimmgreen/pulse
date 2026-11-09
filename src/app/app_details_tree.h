// app_details_tree.h — Inline folder expansion in the details view (Finder
// style): a chevron on every folder row opens its children in place.
#pragma once
#include "app_runtime.h"

namespace pulse {

// Builds every pane's tree view for its current snapshot and starts listing
// newly opened folders. Cheap when nothing changed; called from BuildVm.
void SyncDetailsTrees(AppState& s);
void TryRenameDetailsTreeCreation(AppState& s);
// Called for every worker result before pane handling.
void NoteDetailsTreeResult(AppState& s, const app::WorkResult& res, bool again = false);
void AppendDetailsTreeWatchPaths(const app::Tab& tab, std::vector<std::wstring>& paths);
bool NoteDetailsTreeChange(AppState& s, const std::wstring& path, bool poll = false);

// Mouse-down on a list row: chevrons (Alt: the folder and its subfolders),
// the "more" row buttons, and placeholder rows (swallowed). True when consumed.
bool HandleDetailsTreeMouseDown(AppState& s, const ui::WindowViewModel& vm,
                                const ui::HitTestResult& hit, int x, int y, bool alt);
// Left / Right (collapse or parent / expand or first child), numpad *
// (expand with subfolders) and Enter on a "more" row. True when consumed.
bool HandleDetailsTreeKey(AppState& s, const ui::PaneViewModel& pane, WPARAM key);
// Moves a target view row off loading / empty / failed rows, stepping in
// `dir` first. Rows outside the tree are returned unchanged.
int SkipDetailsTreePlaceholders(const ui::PaneViewModel& pane, int view, int dir);
// Whether the source row is a placeholder (no hover card, tooltip or menu).
bool IsDetailsTreePlaceholder(const app::Tab& tab, int index);
// Whether `point` (client coordinates) is on a chevron in the pane slot
// (no hover card there: the chevron is a button).
bool PointOnDetailsTreeChevron(AppState& s, int pane_index, POINT point);
// Snack action: undoes the last recursive expansion.
void UndoDetailsTreeExpand(AppState& s);

} // namespace pulse
