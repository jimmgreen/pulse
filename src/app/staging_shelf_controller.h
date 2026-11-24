#pragma once
#include <windows.h>
#include <string>

struct IDataObject;

namespace pulse {
struct AppState;

void StartStagingShelf(AppState& s);
void TickStagingShelf(AppState& s);
void CloseStagingShelfOwner(AppState& s, AppState* next);
void SummonStagingShelf(AppState& s);
void ToggleStagingShelf(AppState& s);
void ShowStagingShelf(AppState& s, POINT point, bool expanded);
void TearOffStagingShelf(AppState& s, POINT point);
void DockStagingShelf(AppState& s);
void ShowStagingShelfMenu(AppState& s, POINT point);
bool StagingShelfVisible(const AppState& s);
bool StagingShelfDockHot(const AppState& s);
bool StageShelfDrop(AppState& s, IDataObject* object, int target, bool shift_move);
bool FloatingShelfDragActive(const AppState& s);
bool ReleaseFloatingShelfDrop(AppState& s, const std::wstring& destination);

} // namespace pulse
