// app_commands.h — Menus, omnibar, view/split, tags, settings chrome.
#pragma once
#include "app_runtime.h"
#include "../ui/quick_preview_command.h"

namespace pulse::ui { struct ToolbarCustomizeSpec; struct ToolbarPickerTile; struct FluentMenuItem; }
namespace pulse::app { struct SelectionCaps; struct DriveVerbResult; }

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
// Alt+0: removes every tag from the files.
bool ClearTagsForSelection(AppState& s, const std::vector<std::wstring>& paths);
std::vector<uint32_t>& TagColorPalette(AppState& s);
void AppendCustomTagColor(AppState& s, uint32_t rgb);
void ShowTagPicker(AppState& s, POINT screen_pt,
                   const std::vector<std::wstring>* paths_override = nullptr);
// Staging tray batch menu (right-click on the tray).
void ShowTrayBatchMenu(AppState& s, POINT screen_pt);
// Shelf ▾ menu: copy / move mode, 送到 targets, choose a folder.
void ShowTrayReleaseMenu(AppState& s, POINT screen_pt);
// 送到: release into dir without opening it (its chip flashes).
void TraySendTo(AppState& s, const std::wstring& dir, bool move);
// Shelf ZIP button: the staged items into the current folder.
void TrayShelfZip(AppState& s);
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
// The rename prompt of a tag (a menu with a text field).
bool RenameTagInteractive(AppState& s, const app::TagId& tag_id, POINT screen_pt);
// Right-click on a sidebar tag group header: rename / delete the group.
void ShowTagGroupMenu(AppState& s, const std::wstring& group_id, POINT screen_pt);
// Asks for a group name (menu with a text field); empty when cancelled.
// Name for a smart view (a saved tag combination); empty when dismissed.
std::wstring PromptSmartViewName(AppState& s, POINT screen_pt, const std::wstring& suggested);
std::wstring CreateTagGroupInteractive(AppState& s, POINT screen_pt);
// Folds or unfolds a sidebar tag group header row.
void ToggleTagGroupRow(AppState& s, const std::wstring& path);
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
// Last-window shutdown also cancels and joins background menu-cache readers.
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
void ShowSidebarDriveMenu(AppState& s, std::wstring path, POINT screen_pt);
void ShowItemContextMenu(AppState& s, POINT screen_pt, bool from_chip = false);
// Shift+F10 / Apps key: the item menu under the focused item, else the background menu.
void ShowKeyboardContextMenu(AppState& s);
// 解散文件夹 of these folders (each into its own parent) under one undo
// group; returns the tasks (the assistant's file.dissolve waits for them).
std::vector<uint64_t> DissolveFolders(AppState& s, app::Tab& tab, const std::vector<std::wstring>& folders,
                                      uint64_t group, const std::wstring& label);
void ShowBackgroundContextMenu(AppState& s, POINT screen_pt);
void ShowBreadcrumbChildren(AppState& s, std::wstring path, POINT screen_pt);
void ShowBreadcrumbMenu(AppState& s, std::wstring path, POINT screen_pt);
void ShowNewDropdown(AppState& s);
void ShowSplitDropdown(AppState& s);
app::SidebarEntry* QuickAccessEntryForPath(AppState& s,
                                                   const std::wstring& path);
void ShowStarredBadgeEditor(AppState& s, const std::wstring& path,
                                    POINT screen_pt);
void ShowCuratedItemMenu(AppState& s, const std::wstring& path,
                                bool recent, POINT screen_pt);
// anchor: client point of a zoom gesture (Ctrl+wheel); the item under it keeps
// its place. Without one the focused item (if on screen) or the middle of the
// visible range is kept in place.
void SetViewMode(AppState& s, ui::ViewMode mode, const POINT* anchor = nullptr);
// Continuous icon size (DIP): shown in the icon mode the size belongs to and
// remembered with the folder's view.
void SetIconSize(AppState& s, float icon_dip, const POINT* anchor = nullptr);
// The header's icon button: the icon size used last in this tab.
void ShowLastIconSize(AppState& s, const POINT* anchor = nullptr);
// The header's icons / photo wall buttons: that size as a grid or as a photo
// wall, remembered with the folder.
void ShowIconWall(AppState& s, bool wall, const POINT* anchor = nullptr);
// A view picked from the view menu or with Ctrl+Shift+1..8. An icon size
// there means the plain grid, so it also leaves the photo wall; SetViewMode
// keeps the wall (the size button returns to it).
void ChooseViewMode(AppState& s, ui::ViewMode mode);
// Ctrl+wheel: `step` notches through the icon sizes (ui::ZoomIconDip). Out of
// the smallest goes to Details; in from other views returns to the last size.
// leave_icons = false (the header slider) stays within the icon sizes.
void ZoomIconSize(AppState& s, int step, const POINT* anchor = nullptr, bool leave_icons = true);
// "Apply to all folders" (#31) for the active real folder. Returns true when
// applied; confirm=false skips the dialog (self tests).
bool ApplyViewToAllFolders(AppState& s, bool confirm = true);
// Group menu / group picker: `group_by` becomes every real folder's grouping (#75).
bool ApplyGroupToAllFolders(AppState& s, int group_by, bool confirm = true);
void ShowViewDropdown(AppState& s, int pane_index);
void ShowSortDropdown(AppState& s);
// Toolbar "Group" button: the group-by choices on their own.
void ShowGroupDropdown(AppState& s);
void ShowToolbarPinMenu(AppState& s, int slot, POINT screen);
void ShowToolbarViewMenu(AppState& s, ui::HitTestResult::Region region, POINT screen);
// A pinned app (index into toolbar_apps): separator after it, unpin, 自定义工具栏.
void ShowToolbarAppPinMenu(AppState& s, size_t app, POINT screen);
std::vector<ui::FluentMenuItem> BuildToolbarMoreItems(AppState& s);
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
// The 自定义工具栏 popover; page 0 tools, 1 apps.
void ShowToolbarCustomize(AppState& s, int page = 0);
// Toolbar picker T3c: a toolbar pin pressed while the picker is open.
void BeginToolbarViewDrag(AppState& s, int view, POINT pt);
bool BeginToolbarSeparatorDrag(AppState& s, POINT pt);
void RestoreToolbarDrag(AppState& s);
void BeginToolbarMenuDrag(AppState& s, int tool, POINT screen);
bool ToolbarPinDragging();
void BeginToolbarPinDrag(AppState& s, int index, POINT pt);
void MoveToolbarPinDrag(AppState& s, POINT pt);
void EndToolbarPinDrag(AppState& s, bool cancel);
// Pinned apps (自定义工具栏 → 固定的软件), app_toolbar_apps.cpp.
// Toolbar entries in pin order, with their fit against the selection (or the
// dragged items in drop mode).
std::vector<ui::WindowViewModel::ToolbarAppPin> ToolbarAppPins(AppState& s);
// Click: opens the selected items the app takes, or just starts it.
void RunToolbarApp(AppState& s, size_t app);
// Its display name ("" past the last pin): the assistant's toolbar.open_with.
std::wstring ToolbarAppName(AppState& s, size_t app);
// Opens `items` with pinned app `app`: only the types it opens unless `all`.
void OpenWithToolbarApp(AppState& s, size_t app, const std::vector<std::wstring>& items, bool all);
// The same for files dropped on the app's pin: the pin gulps, a note follows.
void DropOnToolbarApp(AppState& s, size_t app, const std::vector<std::wstring>& items, bool all);
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
bool ApplyToolbarAppRows(AppState& s, const std::vector<ui::ToolbarPickerTile>& apps);
// Pulse's file picker for a program (.exe or shortcut), opening in the Start menu.
bool PickToolbarAppFile(AppState& s, HWND owner, std::wstring& path);
void ShowOmnibar(AppState& s, OmnibarMode mode);
// 管理别名: list, open and remove aliases; export / import them.
void ShowAliasManager(AppState& s);
void ShowAdvancedSearch(AppState& s, bool require_scope = false);
void ShowSearchFilterMenu(AppState& s, int chip, RECT control_rect);
void ShowRecyclePlaceMenu(AppState& s, POINT screen_pt);
// Rebuilds the sidebar model (quick access, OneDrive accounts, drives, saved
// searches). Used on startup, after a language switch, and when the volume set
// changes (a plugged-in stick, a mounted image).
void RefreshSidebarModel(AppState& s);
// "手机连上了却看不到？": the three things to check.
void ShowDeviceHelp(AppState& s);
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
// `history_key` names the entry point (#58-9): the picker opens where that
// entry point last picked, and remembers the new pick for it.
// A non-empty `initial` opens there instead of the entry point's history.
bool PickFolder(AppState& s, std::wstring& path, const wchar_t* title, std::wstring_view history_key = {},
                std::wstring_view initial = {});
// Windows' JSON file dialog; file_name is the save dialog's suggestion.
bool PickJsonFile(HWND owner, bool save, const wchar_t* file_name, std::wstring& out);
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
void HandleQuickPreviewCommand(AppState& s, ui::QuickPreviewWindow& preview,
                               const ui::QuickPreviewCommand& command);
// Quick Look on `item` (the focused tab's selected row); activate false: the
// press-and-hold peek, which leaves the focus and the mouse with Pulse.
void OpenQuickPreview(AppState& s, const ui::QuickPreviewItem& item, bool activate);
// 按住预览 (Settings > 快速预览): a press on a file row arms it; the preview
// opens if the button is still down after the delay and the pointer stayed
// within the drag threshold, and closes on release. Space pins it open.
void ArmHoldPreview(AppState& s, int index, POINT client);
void HoldPreviewMouseMove(AppState& s, POINT client);
void HoldPreviewButtonUp(AppState& s);
void HoldPreviewTimer(AppState& s);
bool HoldPreviewPin(AppState& s);
constexpr UINT_PTR kHoldPreviewTimer = 0x48504B;   // 'HPK'
// Settings > 快速预览 > 预览窗口, as Quick Look takes it (set before Show).
ui::QuickPreviewOptions QuickPreviewOptionsFromPrefs(const app::AppPrefs& prefs);
// Keeps an open quick preview in step with the focused listing after a
// snapshot refresh or directory notification: reloads a changed file,
// re-anchors after an in-preview delete, closes when the entry is gone.
void SyncQuickPreview(AppState& s);
// 下载并预览 (C4b, details well / Quick Look card): brings one cloud-only file
// onto this device on request; WM_CLOUD_DOWNLOAD reports progress and the end.
void StartCloudPreviewDownload(AppState& s, const std::wstring& path, uint64_t size);
void HandleCloudDownloadMessage(AppState& s, WPARAM event, LPARAM job);
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

namespace pulse { void SyncDetailsPreview(AppState& s); }
