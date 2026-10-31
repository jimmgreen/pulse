#pragma once
#include "app_runtime.h"
namespace pulse {
int WorkspaceIndexFromPath(const std::wstring& path);
bool UpdateWorkspaceDrag(AppState& s, int y);
void CommitWorkspaceDrag(AppState& s, const std::wstring& path, int gap);
void FillWorkspaceSidebarState(AppState& s, ui::WindowViewModel& vm);
}
