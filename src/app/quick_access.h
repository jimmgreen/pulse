#pragma once
#include "app_runtime.h"

namespace pulse {
std::vector<std::wstring> QuickAccessTargets(const app::Tab* tab, bool background);
void AppendQuickAccessCommand(AppState& s, std::vector<ui::FluentMenuItem>& items,
                              const std::vector<std::wstring>& paths);
bool HandleQuickAccessCommand(AppState& s, int command,
                              const std::vector<std::wstring>& paths);
void ShowQuickAccessMenu(AppState& s, const std::wstring& path, POINT point);
// Header "+" of Quick access: pin the current folder or a picked one.
void ShowQuickAccessAddMenu(AppState& s, POINT point);
// 编辑快速访问: the panel that shows or hides the built-in links, reorders them
// together with the pinned folders, removes pins and adds folders.
void ShowQuickAccessEditor(AppState& s, POINT point);
// Layout key of each row of the quick-access sidebar group (see
// MergeQuickAccessOrder); empty for rows that do not move (the Git project row).
std::vector<std::wstring> QuickAccessGroupRowKeys(const AppState& s, const ui::SidebarGroup& group);
// Key of row `item` of sidebar section `section`; empty unless it is a movable
// quick-access row.
std::wstring QuickAccessRowKey(const AppState& s, const ui::WindowViewModel& vm,
                               int section, int item);
// Drops the dragged quick-access row `key` into gap `gap` of the visible rows.
void CommitQuickAccessDrag(AppState& s, const std::wstring& key, size_t gap);
// Right-click on a workspace row: open, refresh its saved layout, unpin.
void MoveWorkspace(AppState& s, int index, int to_index);
void UpdateWorkspaceFromCurrent(AppState& s, int index);
inline constexpr UINT kUndoWorkspaceRemovalMessage = WM_APP + 0x3B1;
void ClearWorkspaceActions(AppState& s);
void RenameWorkspace(AppState& s, int index);
void RemoveWorkspaceWithUndo(AppState& s, int index);
void UndoWorkspaceRemoval(AppState& s);
std::vector<ui::FluentMenuItem> BuildWorkspaceMenuItems(AppState& s, int index);
std::vector<ui::FluentMenuItem> BuildWorkspacePaneMenuItems(const AppState& s, int workspace, int pane);
void ShowWorkspacePaneMenu(AppState& s, int workspace, int pane, POINT point);
void ShowWorkspaceMenu(AppState& s, int index, POINT point);
// Right-click on a pinned network location row: open, unpin (#236).
void ShowNetworkPlaceMenu(AppState& s, const std::wstring& unc, POINT point);
}
