// app_commands.h — Menus, omnibar, view/split, tags, settings chrome.
#pragma once
#include "app_runtime.h"
#include "../ui/quick_preview_command.h"

namespace pulse::ui { struct ToolbarCustomizeSpec; }
namespace pulse::app { struct SelectionCaps; struct DriveVerbResult; }

namespace pulse {
bool EnsureMenu(AppState& s);
void CopySelectedPath(AppState& s);
// 复制名称 (#303): the selected file names, or the folder's name on blank space.
void CopySelectedName(AppState& s);
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
// What the active selection can do by what it is (selection_caps.h): the
// toolbar, the chip, the item menu and the shortcuts all ask this.
app::SelectionCaps SelectionCapsOf(AppState& s);
// F2 / 重命名 when the selection has a name to change (a drive: its label).
void RenameSelected(AppState& s);
// Network view: removes the selected Pulse pins. False when none was selected.
bool UnpinSelectedNetworks(AppState& s);
// WM_DRIVE_VERB: an eject / disconnect finished (drive_verbs.h).
void ApplyDriveVerbResult(AppState& s, const app::DriveVerbResult& result);
void ApplyWorkspacePinLabel(std::vector<ui::FluentMenuItem>& items, AppState& s);
std::vector<ui::FluentMenuItem> BuildFinderItemMenu(
        AppState& s, bool can_undo, const std::wstring& undo_label);
std::wstring CommonExtension(const app::Tab& tab,
                                    const std::vector<int>& indices);
// Registry/COM cache key for the selection: ".ext", ":folder", ":drive", or "".
std::wstring StaticVerbKey(const app::Tab& tab, const std::vector<int>& indices);
void PrefetchStaticVerbs(AppState& s, const std::wstring& ext);
void StartShellRegistryWatch(HWND hwnd);
void StopShellRegistryWatch();
void SeedShellVerbCache(AppState& s);
// WM_SHELL_VERB_SEED: merges the parsed machine cache unless a newer seed started.
void ApplyShellVerbSeed(AppState& s, ShellVerbSeed& seed);
// Persistent menu layout (ctx_layout_cache.h): read on a worker thread and
// merged on WM_CTX_LAYOUT_SEED; changed rows are written 1.5 s later off the
// UI thread. A registry flush discards the pending write and reads again.
void SeedCtxLayout(AppState& s);
void ApplyCtxLayoutSeed(AppState& s, CtxLayoutSeed& seed);
void SaveCtxLayoutIfDirty(AppState& s);
void DiscardCtxLayoutSave();
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
// from_chip: the selection chip's ⋯ (adds the pinned-app rows on top).
void ShowItemContextMenu(AppState& s, POINT screen_pt, bool from_chip = false);
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
// Toolbar selection chip, linked to the Pulse item menu: the first plain
// built-in commands of the item menu as the user configured it (order and
// visibility from Settings > Pulse menu), cached per selection.
std::vector<ui::SelectionChipAction> SelectionChipActions(AppState& s);
// 0-2 quick action, ui::kSelChipMore the full item menu, ui::kSelChipClear clear.
void RunToolbarSelectionAction(AppState& s, int action);
// One entry for the click chain: a pinned command (pin) or a chip button.
void RunToolbarAction(AppState& s, bool pin, int index);
// Pinned toolbar commands (自定义工具栏) in toolbar order, enabled per state.
std::vector<ui::SelectionChipAction> ToolbarPinActions(AppState& s);
// The New button's main part makes a folder, unless "remember the last
// type" last saw a text document.
bool ToolbarNewCreatesFolder(const AppState& s);
// The 自定义工具栏 sheet; Done saves app.json.
void ShowToolbarCustomize(AppState& s);
// Pinned apps (自定义工具栏 → 固定的软件), app_toolbar_apps.cpp.
// Toolbar entries in pin order, with their fit against the selection (or the
// dragged items in drop mode).
std::vector<ui::WindowViewModel::ToolbarAppPin> ToolbarAppPins(AppState& s);
// Click: opens the selected items the app takes, or just starts it.
void RunToolbarApp(AppState& s, size_t app);
// Opens `items` with pinned app `app`: only the types it opens unless `all`.
void OpenWithToolbarApp(AppState& s, size_t app, const std::vector<std::wstring>& items, bool all);
// Adds a program (.exe or a shortcut to one) to the pins and saves.
bool PinToolbarApp(AppState& s, const std::wstring& path);
std::wstring ToolbarAppTooltip(AppState& s, size_t app);
// Drag over the window: drop mode on/off for these sources (before BuildVm),
// then the drop on an app or on the label (after the hit test). Returns true
// when the toolbar took the point; `effect` is the DROPEFFECT to report.
void UpdateToolbarDropMode(AppState& s, const std::vector<std::wstring>& sources);
bool ResolveToolbarAppDrop(AppState& s, const ui::WindowViewModel& vm, const ui::HitTestResult& hit,
                           const std::vector<std::wstring>& sources, POINT pt, DWORD key_state,
                           DWORD allowed, DWORD& effect);
// Selection chip ⋯: 用 <pinned app> 打开 rows and a 固定到工具栏 ▸ flyout of
// suggested apps; RunToolbarAppMenuCommand handles their ids.
std::vector<ui::FluentMenuItem> ToolbarAppMenuItems(AppState& s);
bool RunToolbarAppMenuCommand(AppState& s, int command);
// The 自定义工具栏 sheet's 固定的软件 section: pinned apps, two suggestions
// for this folder and 添加软件…; Apply returns true when the pins changed.
void FillToolbarAppRows(AppState& s, ui::ToolbarCustomizeSpec& spec);
bool ApplyToolbarAppRows(AppState& s, const ui::ToolbarCustomizeSpec& spec);
// Pulse's file picker for a program (.exe or shortcut), opening in the Start menu.
bool PickToolbarAppFile(AppState& s, HWND owner, std::wstring& path);
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
// Pulse's own pickers (ui/folder_picker_dialog), owned by the main window.
bool PickImageFile(AppState& s, std::wstring& path);
bool PickFolder(AppState& s, std::wstring& path, const wchar_t* title);
D2D1_COLOR_F ResolveAccentColor(const app::AppPrefs& prefs, bool dark = false);
void ApplyAccentFromPrefs(AppState& s, bool snap_picker);
bool SelectedQuickPreviewItem(AppState& s, ui::QuickPreviewItem& item);
// The quick preview item for row `index` (source order) of `tab`; false for
// rows quick look does not open (drives, recycle bin and virtual folders).
bool QuickPreviewItemForRow(AppState& s, const app::Tab& tab, int index, ui::QuickPreviewItem& item);
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
// 系统右键菜单 live preview drag (settings_ui.cpp): true when it handled the message.
bool SysCtxPreviewDragMove(AppState& s, int x, int y, WPARAM buttons);
bool SysCtxPreviewDragEnd(AppState& s, int x, int y);
bool SysCtxPreviewDragCancel(AppState& s);
void ToggleTheme(AppState& s);
} // namespace pulse
