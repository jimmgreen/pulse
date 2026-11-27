#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>
namespace pulse { struct AppState; namespace ui { struct WindowViewModel; } }
namespace pulse::extensions::native {
void SyncSettingsPagesHost(AppState& owner, const ui::WindowViewModel& vm);
void CloseSettingsPagesHost(HWND owner);
// The store's 设置 button: show this runtime package's page the next time the
// plugin settings page is synced (ignored if the package has no page).
void RequestSettingsPagesPackage(HWND owner, size_t package);
// Bind a package-specific settings URI before the shared detail header is built.
void BindSettingsPagesPath(HWND owner, std::wstring_view path);
void RequestSettingsPagesContextMenu(HWND owner, size_t package);
// The package the page was opened for (its name heads the page), or empty when it lists them all.
std::wstring FocusedSettingsTitle(HWND owner);
// The package the page was opened for (empty id when it lists them all), its tab and whether it has a 右键菜单 tab.
struct FocusedSettingsTab { uint64_t page = 0; std::string id; std::wstring title; };
struct FocusedSettingsInfo {
    std::string package_id;
    int mode = 0;
    bool has_menu = false;
    std::vector<FocusedSettingsTab> plugin_tabs;
    int active_plugin_tab = 0;
    uint32_t status_kind = 0;
    std::wstring status_text;
};
FocusedSettingsInfo FocusedSettings(HWND owner);
// The titles of a package's own extra settings tabs (支持格式, ...), after its first page; empty
// until the package has created them. The store detail page lists them like the settings page does.
std::vector<std::wstring> PackageSettingsExtraTabTitles(HWND owner, size_t package);
// The header's tabs: 0 常用设置, 1 右键菜单.
void SetFocusedSettingsMode(HWND owner, int mode);
void SetFocusedSettingsPluginTab(HWND owner, int tab_index);
}