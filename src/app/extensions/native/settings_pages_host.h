#pragma once
#include <windows.h>
namespace pulse { struct AppState; namespace ui { struct WindowViewModel; } }
namespace pulse::extensions::native {
void SyncSettingsPagesHost(AppState& owner, const ui::WindowViewModel& vm);
void CloseSettingsPagesHost(HWND owner);
}
