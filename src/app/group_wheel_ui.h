// group_wheel_ui.h — App side of the drum "Group by" picker (ui/group_wheel.h):
// builds the options with a live preview of the active folder and routes
// window input to the picker while it is open.
#pragma once
#include <windows.h>

namespace pulse {

struct AppState;

void OpenGroupWheel(AppState& s);
// Same drum for the Sort button: column on the drum, direction and folder
// placement as segments, preview = the folder's first entries in that order.
void OpenSortWheel(AppState& s);
bool ApplySortToAllFolders(AppState& s, int column, bool desc, int folder, bool confirm = true);
// WndProc hook: true when the message was consumed by the open picker.
bool GroupWheelMessage(AppState& s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

} // namespace pulse
