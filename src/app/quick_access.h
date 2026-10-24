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
// Right-click on a saved layout: open, rename, organize, update, or delete.
void ShowWorkspaceMenu(AppState& s, int index, POINT point);
void SaveCurrentWorkspace(AppState& s);
bool ToggleWorkspaceGroup(AppState& s, const std::wstring& path);
bool ToggleWorkspaceDirectories(AppState& s, const std::wstring& path);
// Right-click on a pinned network location row: open, unpin (#236).
void ShowNetworkPlaceMenu(AppState& s, const std::wstring& unc, POINT point);
}
