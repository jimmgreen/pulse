// app_commands.h — Menus, omnibar, view/split, tags, settings chrome.
#pragma once
#include "app_runtime.h"
#include "../ui/quick_preview_command.h"

namespace pulse {
bool EnsureMenu(AppState& s);
void CopySelectedPath(AppState& s);
void CreateNewItem(AppState& s, bool folder);
void QueueTagAds(AppState& s, std::vector<app::TagAdsUpdate> updates);
bool DeleteTagAndRefreshViews(AppState& s, const app::TagId& tag_id);
std::vector<app::TagAdsUpdate> BuildTagAdsUpdates(
        const app::PlacesCatalog& places, const std::vector<std::wstring>& paths,
        bool include_descendants = false);
bool ToggleTagForSelection(AppState& s, const app::TagId& tag_id,
                                  const std::vector<std::wstring>& paths);
std::vector<uint32_t>& TagColorPalette(AppState& s);
void AppendCustomTagColor(AppState& s, uint32_t rgb);
void ShowTagPicker(AppState& s, POINT screen_pt,
                   const std::vector<std::wstring>* paths_override = nullptr);
// Staging tray batch menu (right-click on the tray).
void ShowTrayBatchMenu(AppState& s, POINT screen_pt);
// Stale tray items: search everywhere for the first missing one / drop them all.
void TrayFindStale(AppState& s);
void TrayRemoveStale(AppState& s);
// Two staged files: toggle the compare table / start the byte comparison.
void TrayToggleCompare(AppState& s);
void TrayStartContentCompare(AppState& s);
// Opens the stand-alone text compare window for the two staged files.
void TrayOpenTextDiff(AppState& s);
void ShowCreateTagPicker(AppState& s, POINT screen_pt);
#ifdef PULSE_WITH_SELFTEST
struct CreateTagPickerChoice { int command = 0; std::wstring name; bool committed = false; };
struct CreateTagPickerTestIo {
    std::function<CreateTagPickerChoice(const std::vector<ui::FluentMenuItem>&,
        std::function<std::vector<ui::FluentMenuItem>(const std::wstring&)>)> menu;
    std::function<bool(uint32_t&)> color;
};
CreateTagPickerTestIo* SetCreateTagPickerTestIo(CreateTagPickerTestIo* io);
#endif
void ShowTagSidebarMenu(AppState& s, const app::TagId& tag_id, POINT screen_pt);
void DispatchMenuCommand(AppState& s, int cmd);
bool ClipboardHasFiles();
void ApplyWorkspacePinLabel(std::vector<ui::FluentMenuItem>& items, AppState& s);
std::vector<ui::FluentMenuItem> BuildFinderItemMenu(
        AppState& s, bool can_undo, const std::wstring& undo_label);
std::wstring CommonExtension(const app::Tab& tab,
                                    const std::vector<int>& indices);
// Registry/COM cache key for the selection: ".ext", ":folder", ":drive", or "".
std::wstring StaticVerbKey(const app::Tab& tab, const std::vector<int>& indices);
void PrefetchStaticVerbs(AppState& s, const std::wstring& ext);
DWORD WINAPI ShellRegistryWatch(LPVOID param);
void StartShellRegistryWatch(HWND hwnd);
void StopShellRegistryWatch();
void SeedShellVerbCache(AppState& s);
// WM_SHELL_VERB_SEED: merges the parsed machine cache unless a newer seed started.
void ApplyShellVerbSeed(AppState& s, ShellVerbSeed& seed);
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
void ShowBreadcrumbMenu(AppState& s, std::wstring path, POINT screen_pt);
void ShowNewDropdown(AppState& s);
void ShowSplitDropdown(AppState& s);
app::SidebarEntry* QuickAccessEntryForPath(AppState& s,
                                                   const std::wstring& path);
void ShowStarredBadgeEditor(AppState& s, const std::wstring& path,
                                    POINT screen_pt);
void ShowCuratedItemMenu(AppState& s, const std::wstring& path,
                                bool recent, POINT screen_pt);
void SetViewMode(AppState& s, ui::ViewMode mode);
// "Apply to all folders" (#31) for the active real folder. Returns true when
// applied; confirm=false skips the dialog (self tests).
bool ApplyViewToAllFolders(AppState& s, bool confirm = true);
// Group menu / group picker: `group_by` becomes every real folder's grouping (#75).
bool ApplyGroupToAllFolders(AppState& s, int group_by, bool confirm = true);
void ShowViewDropdown(AppState& s, int pane_index);
void ShowSortDropdown(AppState& s);
// Toolbar "Group" button: the group-by choices on their own.
void ShowGroupDropdown(AppState& s);
void ShowToolbarMore(AppState& s);
void ShowOmnibar(AppState& s, OmnibarMode mode);
void ShowAdvancedSearch(AppState& s, bool require_scope = false);
void ShowSearchFilterMenu(AppState& s, int chip, RECT control_rect);
void ShowRecyclePlaceMenu(AppState& s, POINT screen_pt);
// Rebuilds the sidebar model (quick access, OneDrive accounts, drives, saved
// searches). Used on startup, after a language switch, and when the volume set
// changes (a plugged-in stick, a mounted image).
void RefreshSidebarModel(AppState& s);
// Navigation-pane background menu: show or hide sections, expand or collapse them all.
void ShowSidebarSectionsMenu(AppState& s, POINT screen_pt);
// Menu for one section (header or empty space inside it): toggle its built-in
// quick-access links, fold it, or hide it. Both take a SidebarSectionId value.
void ShowSidebarSectionMenu(AppState& s, int section, POINT screen_pt);
// Right-click on a OneDrive row (#80): the folder commands plus "hide OneDrive".
void ShowCloudPlaceMenu(AppState& s, const std::wstring& path, POINT screen_pt);
void ToggleSidebarSection(AppState& s, int group);
void SetEverySidebarSectionCollapsed(AppState& s, bool collapsed);
void ApplyAppWindowChrome(AppState& s);
// Re-reads the shared preference files and rebuilds everything derived from
// them. Called when another window changed them (WM_PULSE_PREFS_CHANGED).
void ApplySharedPreferenceRefresh(AppState& s);
// Pulse's own pickers (ui/folder_picker_dialog), owned by the main window.
bool PickImageFile(AppState& s, std::wstring& path);
bool PickFolder(AppState& s, std::wstring& path, const wchar_t* title);
D2D1_COLOR_F ResolveAccentColor(const app::AppPrefs& prefs, bool dark = false);
void ApplyAccentFromPrefs(AppState& s, bool snap_picker);
bool SelectedQuickPreviewItem(AppState& s, ui::QuickPreviewItem& item);
void ToggleQuickPreview(AppState& s);
void NavigateQuickPreview(AppState& s, int direction);
// File verb requested from inside the quick preview (WM_QUICK_PREVIEW_COMMAND).
void HandleQuickPreviewCommand(AppState& s, const ui::QuickPreviewCommand& command);
// Keeps an open quick preview in step with the focused listing after a
// snapshot refresh or directory notification: reloads a changed file,
// re-anchors after an in-preview delete, closes when the entry is gone.
void SyncQuickPreview(AppState& s);
void ApplySettingsEffects(AppState& s, app::SettingsEffect effects);
app::SettingsTaskCompletion SettingsCompletion(HWND hwnd);
void SetThemeMode(AppState& s, int mode);
bool HandleSettingsControl(AppState& s, const ui::HitTestResult& hit);
void ToggleTheme(AppState& s);
} // namespace pulse
