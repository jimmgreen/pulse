#pragma once
#include "app_runtime.h"
namespace pulse {
int WorkspaceIndexFromPath(const std::wstring& path);
bool UpdateWorkspaceDrag(AppState& s, int y);
void CommitWorkspaceDrag(AppState& s, const std::wstring& path, int gap);
void FillWorkspaceSidebarState(AppState& s, ui::WindowViewModel& vm);
const ui::SidebarItem* WorkspacePaneRow(const ui::WindowViewModel& vm, const ui::HitTestResult& hit);
void MoveWorkspacePane(AppState& s, int workspace, int from, int to);
void ActivateWorkspacePane(AppState& s, int workspace, int pane);
}
