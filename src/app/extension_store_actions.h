#pragma once
namespace pulse {
struct AppState;
bool ExtensionStoreDetailAction(AppState& state, int action);
bool ExtensionStoreLevelDragging(const AppState& state);
void ExtensionStoreLevelDrag(AppState& state, int x);
bool ExtensionStoreLevelDragEnd(AppState& state);
}
