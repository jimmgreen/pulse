#pragma once
#include <windows.h>
namespace pulse { struct AppState; }
namespace pulse::ui { struct WindowViewModel; }
namespace pulse {
void SyncStoreEdits(AppState& s, const ui::WindowViewModel& vm);
void DestroyStoreEdits(AppState& s);
HBRUSH StoreEditsCtlColor(AppState& s, HDC hdc, HWND edit);
}
