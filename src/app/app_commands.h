// app_commands.h — Menus, omnibar, view/split, tags, settings chrome.
#pragma once
#include "app_runtime.h"

namespace pulse {
bool EnsureMenu(AppState& s);
void CopySelectedPath(AppState& s);
void CreateNewItem(AppState& s, bool folder);
void QueueTagAds(AppState& s, std::vector<app::TagAdsUpdate> updates);
std::vector<app::TagAdsUpdate> BuildTagAdsUpdates(
        const app::PlacesCatalog& places, const std::vector<std::wstring>& paths,
        bool include_descendants = false);
bool ToggleTagForSelection(AppState& s, const app::TagId& tag_id,
                                  const std::vector<std::wstring>& paths);
std::vector<uint32_t>& TagColorPalette(AppState& s);
void AppendCustomTagColor(AppState& s, uint32_t rgb);
void ShowTagPicker(AppState& s, POINT screen_pt);
void ShowCreateTagPicker(AppState& s, POINT screen_pt);
void ShowTagSidebarMenu(AppState& s, const app::TagId& tag_id, POINT screen_pt);
void DispatchMenuCommand(AppState& s, int cmd);
bool ClipboardHasFiles();
void ApplyWorkspacePinLabel(std::vector<ui::FluentMenuItem>& items, AppState& s);
std::vector<ui::FluentMenuItem> BuildFinderItemMenu(
        AppState& s, bool can_undo, const std::wstring& undo_label);
std::wstring CommonExtension(const app::Tab& tab,
                                    const std::vector<int>& indices);
void PrefetchStaticVerbs(AppState& s, const std::wstring& ext);
DWORD WINAPI ShellRegistryWatch(LPVOID param);
void StartShellRegistryWatch(HWND hwnd);
void StopShellRegistryWatch();
void SeedShellVerbCache(AppState& s);
void StartCtxQuery(AppState& s, std::vector<std::wstring> paths,
                          bool background, const std::wstring& ext);
void MaybePrefetchHoverCtxMenu(AppState& s);
void PumpShellMenuMessages(AppState& s);
void WaitForShellMenuReady(AppState& s);
void ScheduleFolderRefresh(AppState& s);
std::vector<ui::FluentMenuItem> ExplorerMenu(
    AppState& s, const std::vector<ui::FluentMenuItem>& base);
void RefreshOpenCtxMenu(AppState& s);
bool HandleShellMenuCommand(AppState& s, int cmd);
void ShowItemContextMenu(AppState& s, POINT screen_pt);
void ShowBackgroundContextMenu(AppState& s, POINT screen_pt);
void ShowNewDropdown(AppState& s);
void ShowSplitDropdown(AppState& s);
app::SidebarEntry* QuickAccessEntryForPath(AppState& s,
                                                   const std::wstring& path);
void ShowStarredBadgeEditor(AppState& s, const std::wstring& path,
                                    POINT screen_pt);
void ShowCuratedItemMenu(AppState& s, const std::wstring& path,
                                bool recent, POINT screen_pt);
void SetViewMode(AppState& s, ui::ViewMode mode);
void ShowViewDropdown(AppState& s, int pane_index);
void ShowOmnibar(AppState& s, OmnibarMode mode);
void ShowRecyclePlaceMenu(AppState& s, POINT screen_pt);
void ApplyAppWindowChrome(AppState& s);
bool PickImageFile(HWND owner, std::wstring& path);
bool PickFolder(HWND owner, std::wstring& path, const wchar_t* title);
D2D1_COLOR_F ResolveAccentColor(const app::AppPrefs& prefs);
void ApplyAccentFromPrefs(AppState& s, bool snap_picker);
bool SelectedQuickPreviewItem(AppState& s, ui::QuickPreviewItem& item);
void ToggleQuickPreview(AppState& s);
void NavigateQuickPreview(AppState& s, int direction);
void ApplySettingsEffects(AppState& s, app::SettingsEffect effects);
app::SettingsTaskCompletion SettingsCompletion(HWND hwnd);
void ToggleTheme(AppState& s);
} // namespace pulse
