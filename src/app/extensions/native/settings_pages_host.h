#pragma once
#include <windows.h>
namespace pulse { struct AppState; namespace ui { struct WindowViewModel; } }
namespace pulse::extensions::native {
void SyncSettingsPagesHost(AppState& owner, const ui::WindowViewModel& vm);
void CloseSettingsPagesHost(HWND owner);
// The store's 设置 button: show this runtime package's page the next time the
// plugin settings page is synced (ignored if the package has no page).
void RequestSettingsPagesPackage(HWND owner, size_t package);
void RequestSettingsPagesContextMenu(HWND owner, size_t package);
}
