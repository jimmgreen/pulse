// action_registry.cpp - The action table and what each action does now.
//
// P1 wires the actions the palette offers first (navigation, tabs, windows,
// view, selection, the common file commands, the tray, settings); the rest of
// the inventory joins in P4. Rows only call what their menu row or key calls.
#include "action_registry.h"
#include "../window_arrange_ui.h"

#include "action_args.h"
#include "action_inventory.h"
#include "action_state.h"
#include "action_settings.h"
#include "../app_internal.h"
#include "../app_commands.h"
#include "../cloud_roots.h"
#include "../drive_verbs.h"
#include "../git_ui.h"
#include "../app_messages.h"
#include "../ai_assistant_chat.h"
#include "../context_menu.h"
#include "../favorites_bar.h"
#include "../keymap.h"
#include "../places.h"
#include "../app_hosted_edit.h"
#include "../app_ops_ui.h"
#include "../app_runtime.h"
#include "../folder_alias.h"
#include "../global_search_controller.h"
#include "../tab_controller.h"
#include "../app_navigation.h"
#include "../reopen_closed.h"
#include "../tab_transfer.h"
#include "../window_factory.h"
#include "../window_registry.h"
#include "../tag_query.h"
#include "../tag_view.h"
#include "../vertical_tabs.h"
#include "../window_overview_ui.h"
#include "../../common/localization.h"
#include "../../fs/bitlocker_volume.h"
#include "../../fs/cloud_state.h"
#include "../../fs/fs_enum.h"
#include "../../ui/command_icons.h"
#include "../../ui/ui_renderer.h"

#include <iterator>
#include <string_view>
#include <utility>

namespace pulse::app::actions {

namespace {

using I = l10n::StringId;
using P = ui::fluent::MenuPictogram;

std::wstring Text(I id) { return l10n::Get(id).c_str(); }

// ---- What an action needs; each marks the view unavailable with a reason --

bool Off(ActionView& v, I why) {
    v.enabled = false;
    v.why = Text(why);
    return false;
}

bool NeedSelection(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    return tab && tab->SelectedCount() > 0 ? true : Off(v, I::ActWhySelect);
}

bool NeedOne(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    return tab && tab->SelectedCount() == 1 ? true : Off(v, I::ActWhySelectOne);
}

bool InFolder(AppState& s) {
    const Tab* tab = ActiveTab(s);
    return tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path);
}

bool NeedFolder(AppState& s, ActionView& v) {
    return InFolder(s) ? true : Off(v, I::ActWhyNotFolder);
}

// Task 46: 查找重复文件 names the folder it compares; AI 整理此文件夹… needs a
// model. Both are grey outside a real folder (此电脑, 回收站, 搜索结果).
void FillFindDuplicates(AppState& s, ActionView& v) {
    if (!InFolder(s)) {
        Off(v, I::ActWhyOpenFolder);
        return;
    }
    std::wstring value = Text(I::ActFindDupValue);
    const std::wstring folder = TabTitle(ActiveTab(s)->current_path);
    if (const size_t at = value.find(L"{folder}"); at != std::wstring::npos) value.replace(at, 8, folder);
    v.value = std::move(value);
}

void FillOrganizeHere(AppState& s, ActionView& v) {
    if (!InFolder(s)) {
        Off(v, I::ActWhyOpenFolder);
        return;
    }
    if (!AiAgentConnected()) {
        Off(v, I::ActNeedAi);
        return;
    }
    v.value = Text(I::ActOrganizeValue);
}

bool NeedPanes(AppState& s, ActionView& v) {
    return LiveLayout(s).VisiblePanes().size() > 1 ? true : Off(v, I::ActWhyOnePane);
}

std::wstring OnOff(bool on) { return Text(on ? I::AiSetOn : I::AiSetOff); }

// The radio row's label of a choice list ("" when none is current).
std::wstring Current(const std::vector<ui::FluentMenuItem>& rows) {
    for (const auto& row : rows)
        if (row.radio) return row.text;
    return {};
}

BackgroundViewOptions ViewOptions(AppState&, const Tab& tab) {
    std::wstring kind;
    ParsePulsePath(tab.current_path, &kind, nullptr);
    const bool search = kind == L"search" || kind == L"saved-search";
    const bool real = !tab.current_path.empty() && !fs::IsVirtualPath(tab.current_path);
    BackgroundViewOptions o;
    o.view_mode = tab.view_mode;
    o.sort_column = tab.sort_column;
    o.sort_direction = tab.sort_direction;
    o.folder_sort = static_cast<int>(tab.EffectiveFolderSort());
    o.can_sort = kind != L"recent";
    o.indexed_search = search && !tab.content_results;
    o.show_path = search || kind == L"recycle";
    o.group_virtual = search || IsTagViewKind(kind) || kind == L"recycle";
    o.can_group = !tab.content_results && (real || kind == L"recent" || o.group_virtual);
    o.group_by = tab.group_by;
    o.can_apply_group_all = !tab.content_results && real;
    return o;
}

// ---- The table -------------------------------------------------------------

using Run = void (*)(AppState&);
using Fill = void (*)(AppState&, ActionView&);
using Pick = void (*)(AppState&, int);

struct Def {
    std::string_view id;
    I title;
    I domain;
    const wchar_t* glyph;
    P pictogram;
    const wchar_t* chord;  // default key; shown as the user's key now
    int command;           // the MenuCmd it runs, or 0 for `run`
    Run run;
    Fill fill;             // enabled / why / value / choices; nullptr: always on
    Pick pick = nullptr;   // runs a choice without a menu command (its position)
    bool hidden = false;   // the assistant's only (ActionView::palette)
};

void Post(AppState& s, UINT message) {
    // Posted like the keys do: windows are not made or closed under the palette.
    PostMessageW(s.hwnd, message, 0, 0);
}

// Where a palette row opens its menu: below the address bar's left end.
POINT BelowAddress(AppState& s) {
    const D2D1_RECT_F addr = s.renderer.AddressBarRect(static_cast<float>(s.compositor.Width()));
    POINT pt{static_cast<LONG>(addr.left), static_cast<LONG>(addr.bottom)};
    ClientToScreen(s.hwnd, &pt);
    return pt;
}

void ShowTagPickerBelowAddress(AppState& s) { ShowTagPicker(s, BelowAddress(s)); }

void FillViewMode(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    if (!tab) { Off(v, I::ActWhyNotFolder); return; }
    auto rows = BuildViewMenu(tab->view_mode, false, false, tab->wall);
    // The view modes and 照片墙; the panel and apply-to-all rows have their own actions.
    while (!rows.empty() && rows.back().command != CmdViewWall) rows.pop_back();
    if (!rows.empty()) rows.back().separator_after = false;
    v.value = Current(rows);
    v.choices = std::move(rows);
}

void FillSort(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    if (!tab) { Off(v, I::ActWhyNotFolder); return; }
    const BackgroundViewOptions o = ViewOptions(s, *tab);
    if (!o.can_sort) { Off(v, I::ActWhyNoSort); return; }
    v.choices = BuildSortMenu(o);
    v.value = Current(v.choices);
    if (!v.value.empty())
        v.value += tab->sort_direction == ui::SortDirection::Asc ? L" \x2191" : L" \x2193";
}

void FillSortDirection(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    if (!tab) { Off(v, I::ActWhyNotFolder); return; }
    if (!ViewOptions(s, *tab).can_sort) { Off(v, I::ActWhyNoSort); return; }
    const bool asc = tab->sort_direction == ui::SortDirection::Asc;
    v.value = Text(asc ? I::SortAscending : I::SortDescending) + L" \x2192 " +
              Text(asc ? I::SortDescending : I::SortAscending);
}

void RunSortDirection(AppState& s) {
    const Tab* tab = ActiveTab(s);
    if (!tab) return;
    DispatchMenuCommand(s, tab->sort_direction == ui::SortDirection::Asc ? CmdSortDescending
                                                                          : CmdSortAscending);
}

void FillGroup(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    if (!tab) { Off(v, I::ActWhyNotFolder); return; }
    const BackgroundViewOptions o = ViewOptions(s, *tab);
    if (!o.can_group) { Off(v, I::ActWhyNoGroup); return; }
    v.choices = BuildGroupMenu(o).children;
    v.value = Current(v.choices);
}

void FillLayout(AppState& s, ActionView& v) {
    v.choices = BuildSplitMenu(static_cast<int>(LayoutOf(s)));
    v.value = Current(v.choices);
    for (const auto& row : v.choices)
        if (row.radio) v.pictogram = row.pictogram;
}

void FillPaste(AppState& s, ActionView& v) {
    if (!NeedFolder(s, v)) return;
    if (!ClipboardHasFiles()) Off(v, I::ActWhyNoPaste);
}

void FillTargetCopy(AppState& s, ActionView& v) {
    if (NeedPanes(s, v)) NeedSelection(s, v);
}

void FillUndo(AppState& s, ActionView& v) {
    if (!s.ops.CanUndo()) Off(v, I::ActWhyNoUndo);
}

void FillCompare(AppState& s, ActionView& v) {
    if (!NeedPanes(s, v)) return;
    const LayoutTab* lt = s.window_tabs.Active();
    v.value = OnOff(lt && lt->compare);
}

void FillDetails(AppState& s, ActionView& v) { v.value = OnOff(s.showDetailsPanel); }
void FillFolderSizes(AppState& s, ActionView& v) { v.value = OnOff(s.appPrefs.folder_size_auto); }
void FillSelection(AppState& s, ActionView& v) { NeedSelection(s, v); }
void FillOne(AppState& s, ActionView& v) { NeedOne(s, v); }
void FillFolder(AppState& s, ActionView& v) { NeedFolder(s, v); }
void FillPanes(AppState& s, ActionView& v) { NeedPanes(s, v); }

// 还原: items selected in the Recycle Bin.
void FillRestore(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    if (!tab || !IsRecycleTab(tab)) { Off(v, I::AiFoNotBin); return; }
    NeedSelection(s, v);
}

void FillSelectionOrFolder(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    if (!(tab && tab->SelectedCount() > 0) && !InFolder(s)) Off(v, I::ActWhyNotFolder);
}

// 添加或去掉标签 ›: one row per tag, in the sidebar's order.
void FillTags(AppState& s, ActionView& v) {
    if (!NeedSelection(s, v)) return;
    if (s.places.tags.empty()) { Off(v, I::ActWhyNoTags); return; }
    for (const auto& tag : s.places.tags) {
        ui::FluentMenuItem row;
        row.text = tag.name;
        v.choices.push_back(std::move(row));
    }
}

void PickTag(AppState& s, int choice) {
    const Tab* tab = ActiveTab(s);
    if (!tab || choice < 0 || static_cast<size_t>(choice) >= s.places.tags.size()) return;
    ToggleTagWithUndo(s, s.places.tags[static_cast<size_t>(choice)].id, SelectedFullPaths(*tab));
}

// The selected items, or the open folder, join the favorites (never leave them).
void AddFavorites(AppState& s) {
    const Tab* tab = ActiveTab(s);
    if (!tab) return;
    std::vector<std::wstring> paths;
    if (tab->SelectedCount() > 0) paths = SelectedFullPaths(*tab);
    else if (InFolder(s)) paths.push_back(tab->current_path);
    for (const auto& p : paths) {
        if (p.empty() || fs::IsVirtualPath(p) || s.places.IsStarred(p)) continue;
        const DWORD attrs = GetFileAttributesW(p.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) continue;
        ToggleStarred(s, p, (attrs & FILE_ATTRIBUTE_DIRECTORY) ? PlaceItemKind::Folder : PlaceItemKind::File);
    }
}

// The focused pane leaves the split for a window of its own (the split it
// left stays on screen here).
void PaneToWindow(AppState& s) {
    Pane* pane = s.pane;
    if (!pane || LiveLayout(s).VisiblePanes().size() < 2) return;
    LayoutTab* into = s.window_tabs.Active();
    PaneOrigin origin;
    const std::optional<size_t> tab_index = SplitPaneIntoTab(s, pane, origin);
    if (!tab_index) return;
    WindowInit init;
    if (pane->ActiveTab()) init.path = pane->ActiveTab()->current_path;
    init.adopt_from = &s;
    init.adopt_index = *tab_index;
    OpenPulseWindow(s, init);
    for (size_t i = 0; i < s.window_tabs.items.size(); ++i)
        if (s.window_tabs.items[i].get() == into) {
            if (s.window_tabs.active != i) SwitchTab(s, i);
            break;
        }
}

void SelectNone(AppState& s) {
    Tab* tab = ActiveTab(s);
    if (!tab) return;
    tab->ClearSelection();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// ---- P4: the rest of the inventory -------------------------------------------

ui::FluentMenuItem ChoiceRow(std::wstring text, bool radio = false) {
    ui::FluentMenuItem row;
    row.text = std::move(text);
    row.radio_group = true;
    row.radio = radio;
    return row;
}

int ActiveTabIndex(AppState& s) {
    return s.window_tabs.Active() ? static_cast<int>(s.window_tabs.active) : -1;
}

void RunTab(AppState& s, int command) { s.tabs.RunTabCommand(s.window_tabs, ActiveTabIndex(s), command); }

void FillTabPin(AppState& s, ActionView& v) {
    const LayoutTab* lt = s.window_tabs.Active();
    v.value = OnOff(lt && lt->pinned);
}

void FillTabColor(AppState& s, ActionView& v) {
    const LayoutTab* lt = s.window_tabs.Active();
    const uint32_t* palette = TabController::Palette();
    for (int i = 0; i < static_cast<int>(TabController::PaletteSize()); ++i)
        v.choices.push_back(ChoiceRow(ColorName(i), lt && lt->marker_rgb == palette[i]));
    v.choices.push_back(ChoiceRow(ColorName(-1), lt && lt->marker_rgb == 0));
    v.value = Current(v.choices);
}

void PickTabColor(AppState& s, int choice) {
    RunTab(s, choice < static_cast<int>(TabController::PaletteSize()) ? CmdTabColorBase + choice : CmdTabColorNone);
}

// 新组, the groups to join, 移出分组.
void FillTabGroup(AppState& s, ActionView& v) {
    const LayoutTab* lt = s.window_tabs.Active();
    if (!lt || lt->pinned) { Off(v, I::ActWhyNotFolder); return; }
    v.choices.push_back(ChoiceRow(Text(s.window_tabs.tab_groups.empty() ? I::TabCreateGroup : I::TabNewGroup)));
    for (const auto& g : s.window_tabs.tab_groups)
        v.choices.push_back(ChoiceRow(g.name.empty() ? Text(I::TabUnnamedGroup) : g.name, lt->tab_group == g.id));
    if (lt->tab_group != 0) v.choices.push_back(ChoiceRow(Text(I::TabRemoveGroup)));
}

void PickTabGroup(AppState& s, int choice) {
    const int groups = static_cast<int>(s.window_tabs.tab_groups.size());
    if (choice == 0) {
        if (!s.tabs.RunTabCommand(s.window_tabs, ActiveTabIndex(s), CmdTabAddToNewGroup) ||
            s.window_tabs.tab_groups.empty())
            return;
        s.tabs.ShowGroupMenu(s.window_tabs, s.window_tabs.tab_groups.back().id, BelowAddress(s), *s.menu);
    } else if (choice <= groups) {
        RunTab(s, CmdTabJoinGroupBase + choice - 1);
    } else {
        RunTab(s, CmdTabRemoveFromGroup);
    }
}

void FillManyTabs(AppState& s, ActionView& v) {
    if (s.window_tabs.items.size() < 2) Off(v, I::ActWhyOneTab);
}

void FillTabList(AppState& s, ActionView& v) {
    FillManyTabs(s, v);
    for (size_t i = 0; i < s.window_tabs.items.size(); ++i)
        v.choices.push_back(ChoiceRow(TabStripTitle(*s.window_tabs.items[i]), i == s.window_tabs.active));
}

void PickTab(AppState& s, int choice) {
    if (choice >= 0 && static_cast<size_t>(choice) < s.window_tabs.items.size()) SwitchTab(s, static_cast<size_t>(choice));
}

void FillPaneList(AppState& s, ActionView& v) {
    if (!NeedPanes(s, v)) return;
    const auto panes = PanesOnScreen(LiveLayout(s));
    for (size_t i = 0; i < panes.size(); ++i) {
        const Tab* t = panes[i]->ActiveTab();
        std::wstring name = t ? TabTitle(t->current_path) : std::wstring();
        v.choices.push_back(ChoiceRow(std::to_wstring(i + 1) + L" \u00B7 " + name, panes[i] == s.pane));
    }
}

void PickPane(AppState& s, int choice) {
    const auto panes = PanesOnScreen(LiveLayout(s));
    if (choice >= 0 && static_cast<size_t>(choice) < panes.size()) FocusPane(s, panes[static_cast<size_t>(choice)]);
}

std::vector<HWND> OtherWindows(AppState& s, std::vector<std::wstring>* titles = nullptr) {
    std::vector<HWND> out;
    ForEachWindowSharingCore(s, [&](AppState& w) {
        if (&w == &s || !w.hwnd) return;
        out.push_back(w.hwnd);
        if (titles) titles->push_back(w.window_tabs.Active() ? TabStripTitle(*w.window_tabs.Active()) : std::wstring());
    });
    return out;
}

void FillWindowList(AppState& s, ActionView& v) {
    std::vector<std::wstring> titles;
    if (OtherWindows(s, &titles).empty()) { Off(v, I::AiActWhyOneWindow); return; }
    for (auto& t : titles) v.choices.push_back(ChoiceRow(std::move(t)));
}

void PickWindow(AppState& s, int choice) {
    const auto windows = OtherWindows(s);
    if (choice < 0 || static_cast<size_t>(choice) >= windows.size()) return;
    HWND h = windows[static_cast<size_t>(choice)];
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    SetForegroundWindow(h);
}

constexpr std::pair<SidebarSectionId, I> kSectionRows[] = {
    {SidebarSectionId::Workspaces, I::SidebarWorkspaces}, {SidebarSectionId::QuickAccess, I::SidebarQuickAccess},
    {SidebarSectionId::SavedSearches, I::SidebarSavedSearches}, {SidebarSectionId::Drives, I::SidebarDrives},
    {SidebarSectionId::Tags, I::SidebarTags}, {SidebarSectionId::Networks, I::SidebarNetworkLocations},
};

void FillSections(AppState& s, ActionView& v) {
    for (const auto& [id, name] : kSectionRows) {
        ui::FluentMenuItem row;
        row.text = Text(name);
        row.checked = ((s.sidebarCollapsedMask >> static_cast<int>(id)) & 1u) == 0;
        v.choices.push_back(std::move(row));
    }
}

void PickSection(AppState& s, int choice) {
    if (choice >= 0 && static_cast<size_t>(choice) < std::size(kSectionRows))
        ToggleSidebarSection(s, static_cast<int>(kSectionRows[static_cast<size_t>(choice)].first));
}

void FillIconSize(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    if (!tab) { Off(v, I::ActWhyNotFolder); return; }
    v.choices.push_back(ChoiceRow(Text(I::ActIconBigger)));
    v.choices.push_back(ChoiceRow(Text(I::ActIconSmaller)));
}

void PickIconSize(AppState& s, int choice) { ZoomIconSize(s, choice == 0 ? 1 : -1); }

void FillWall(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    if (!tab) { Off(v, I::ActWhyNotFolder); return; }
    v.value = OnOff(tab->wall);
}

// 始终置顶 / 跟随方向 / 不优先: the sort menu's own rows.
void FillFoldersFirst(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    if (!tab) { Off(v, I::ActWhyNotFolder); return; }
    const BackgroundViewOptions o = ViewOptions(s, *tab);
    if (!o.can_sort) { Off(v, I::ActWhyNoSort); return; }
    for (auto& row : BuildSortMenu(o))
        if (row.command >= CmdFolderSortTop && row.command <= CmdFolderSortMixed) {
            row.separator_after = false;
            v.choices.push_back(std::move(row));
        }
    v.value = Current(v.choices);
}

bool InRecent(AppState& s) {
    const Tab* tab = ActiveTab(s);
    std::wstring kind;
    if (tab) ParsePulsePath(tab->current_path, &kind, nullptr);
    return kind == L"recent";
}

void FillRecentRemove(AppState& s, ActionView& v) {
    if (!InRecent(s)) { Off(v, I::ActWhyNotRecent); return; }
    NeedSelection(s, v);
}

void RunRecentRemove(AppState& s) { RunWithArgs(s, "recent.remove", json::Value::Object()); }

void FillAliasDisplay(AppState&, ActionView& v) {
    if (AliasStore::Instance().Empty()) { Off(v, I::ActWhyNoAliases); return; }
    constexpr I labels[] = {I::AliasDisplayBoth, I::AliasDisplayOnly, I::AliasDisplayReal};
    const int current = static_cast<int>(AliasStore::Instance().Display());
    for (int i = 0; i < 3; ++i) {
        ui::FluentMenuItem row = ChoiceRow(Text(labels[i]), current == i);
        row.command = CmdAliasDisplayBase + i;
        v.choices.push_back(std::move(row));
    }
    v.value = Current(v.choices);
}

void FillAliases(AppState&, ActionView& v) {
    if (AliasStore::Instance().Empty()) Off(v, I::ActWhyNoAliases);
}

void ExportAliases(AppState& s) {
    std::wstring file;
    if (PickJsonFile(s.hwnd, true, L"aliases.json", file)) AliasStore::Instance().ExportTo(file);
}

bool TrayEmpty(AppState& s) {
    for (const auto& b : s.tray.batches())
        if (!b.items.empty()) return false;
    return true;
}

void FillTray(AppState& s, ActionView& v) {
    if (TrayEmpty(s)) Off(v, I::ActWhyNoTray);
}

void FillComputeSize(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    if (!tab || tab->SelectedCount() != 1) { Off(v, I::ActWhySelectOne); return; }
}

void ComputeSize(AppState& s) {
    const Tab* tab = ActiveTab(s);
    if (!tab) return;
    const auto paths = SelectedFullPaths(*tab);
    if (paths.size() == 1) StartDetailsSizeWalk(s, paths[0]);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void FillIndexStatus(AppState& s, ActionView& v) { v.value = ContentIndexStatusText(s); }

void FillPreviewStep(AppState&, ActionView& v) {
    v.choices.push_back(ChoiceRow(Text(I::ActPrevious)));
    v.choices.push_back(ChoiceRow(Text(I::ActNext)));
}

void PickPreviewStep(AppState& s, int choice) { NavigateQuickPreview(s, choice == 0 ? -1 : 1); }

constexpr const wchar_t* kNoKey = nullptr;
// ---- P4b: Git, cloud files, drives, the toolbar's apps -----------------------

void FillGit(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    if (!tab || tab->git_root.empty()) Off(v, I::AiGitNotRepo);
}

void FillCloudPin(AppState& s, ActionView& v) {
    if (!NeedSelection(s, v)) return;
    if (!IsUnderCloudFilesRoot(ActiveTab(s)->current_path)) Off(v, I::AiCloudNotCloud);
}

// 下载并预览: one selected file that is online-only now.
void FillCloudFile(AppState& s, ActionView& v) {
    if (!NeedOne(s, v)) return;
    const std::vector<std::wstring> paths = SelectedFullPaths(*ActiveTab(s));
    const DWORD attrs = paths.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(paths[0].c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) || !fs::IsCloudPlaceholder(attrs))
        Off(v, I::AiCloudDlLocal);
}

// The one drive selected in This PC (eject / disconnect / unlock act on it).
bool OneDrive(AppState& s, fs::DirEntry& e, std::wstring& root) {
    const Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot) return false;
    const std::vector<int> sel = tab->SelectedIndices();
    if (sel.size() != 1 || sel[0] < 0 || static_cast<size_t>(sel[0]) >= tab->EntryCount()) return false;
    e = tab->EntryAt(static_cast<size_t>(sel[0]));
    if (!e.drive_type) return false;
    root = fs::DriveRootOf(EntryFullPath(*tab, sel[0]));
    return !root.empty();
}

void FillEject(AppState& s, ActionView& v) {
    fs::DirEntry e;
    std::wstring root;
    if (!OneDrive(s, e, root) || !DriveEjectable(root, e.drive_type)) Off(v, I::AiDriveWhich);
}

void FillDisconnect(AppState& s, ActionView& v) {
    fs::DirEntry e;
    std::wstring root;
    if (!OneDrive(s, e, root) || e.drive_type != DRIVE_REMOTE) Off(v, I::AiDriveWhich);
}

void FillUnlock(AppState& s, ActionView& v) {
    fs::DirEntry e;
    std::wstring root;
    if (!OneDrive(s, e, root) || !e.drive_locked) Off(v, I::AiDriveWhich);
}

void FillToolbarOpen(AppState& s, ActionView& v) {
    if (!NeedSelection(s, v)) return;
    if (s.appPrefs.toolbar_apps.empty()) Off(v, I::AiOpenWithEmpty);
}

void RunGitCommand(AppState& s, int command) {
    if (const Tab* tab = ActiveTab(s)) HandleGitCommand(s, command, tab->git_root);
}

// 用其他程序打开: the toolbar's 用 … 打开 rows for the selection, to pick from.
void ShowToolbarAppMenu(AppState& s) {
    std::vector<ui::FluentMenuItem> items = ToolbarAppMenuItems(s);
    if (items.empty() || !EnsureMenu(s)) return;
    const int cmd = s.menu->TrackPopup(BelowAddress(s), std::move(items));
    if (cmd) RunToolbarAppMenuCommand(s, cmd);
}

constexpr Run kNoRun = nullptr;
constexpr Fill kAlways = nullptr;

// Inventory actions with no row in kDefs, and why (check_actions.py).
constexpr std::pair<std::string_view, std::string_view> kNoRow[] = {
    {"search.files", "the search_files tool runs it"},
    {"settings.set", "one row per setting, from action_settings (SettingView)"},
    {"file.organize", "AI apps only: it needs moves[{item, dest}]; the assistant uses organize_files"},
};

constexpr bool NoRowKnown() {
    for (const auto& [id, why] : kNoRow)
        if (!FindAction(id) || why.empty()) return false;
    return true;
}
static_assert(NoRowKnown(), "kNoRow: an id is missing from action_inventory.h, or has no reason");

constexpr Def kDefs[] = {
    // 导航
    {"nav.back", I::Back, I::ActDomNav, L"\xE72B", P::None, L"Alt+Left", 0,
     +[](AppState& s) { GoBack(s); }, kAlways},
    {"nav.forward", I::Forward, I::ActDomNav, L"\xE72A", P::None, L"Alt+Right", 0,
     +[](AppState& s) { GoForward(s); }, kAlways},
    {"nav.up", I::Up, I::ActDomNav, L"\xE74A", P::None, L"Alt+Up", 0,
     +[](AppState& s) { GoUp(s); }, kAlways},
    {"nav.refresh", I::Refresh, I::ActDomNav, L"\xE72C", P::None, L"F5", CmdRefresh, kNoRun, kAlways},
    {"nav.recent_changes", I::ActRecentChanges, I::ActDomNav, L"\xE81C", P::None, kNoKey,
     CmdViewRecentChanges, kNoRun, FillFolder},
    {"nav.go", I::ActGoAddress, I::ActDomNav, L"\xE8AD", P::None, L"Ctrl+L", 0,
     +[](AppState& s) { ShowAddressEditor(s); }, kAlways},
    {"nav.open", I::ActOpenSelected, I::ActDomNav, L"\xE8E5", P::None, L"Enter", 0,
     +[](AppState& s) { OpenSelected(s); }, FillSelection},
    {"nav.recycle_bin", I::ActOpenRecycle, I::ActDomNav, L"\xE75C", P::None, kNoKey,
     CmdOpenRecycle, kNoRun, kAlways},
    // 标签页
    {"tab.new", I::TooltipNewTab, I::ActDomTabs, L"\xE710", P::None, L"Ctrl+T", 0,
     +[](AppState& s) { OpenNewTab(s); }, kAlways},
    {"tab.close", I::TabClose, I::ActDomTabs, L"\xE711", P::None, L"Ctrl+W", 0,
     +[](AppState& s) { CloseActiveTab(s); }, kAlways},
    {"window.reopen", I::ActReopenClosed, I::ActDomTabs, L"\xE7A7", P::None, L"Ctrl+Shift+T", 0,
     +[](AppState& s) { ReopenLastClosed(s); }, kAlways},
    {"tab.to_window", I::ActTabToWindow, I::ActDomTabs, L"\xE8A7", P::None, L"Ctrl+Shift+M", 0,
     +[](AppState& s) { Post(s, WM_MOVE_TAB_TO_NEW_WINDOW); }, kAlways},
    // 窗口与分栏
    {"window.layout", I::ActLayout, I::ActDomWindow, nullptr, P::LayoutSideBySide, kNoKey, 0,
     kNoRun, FillLayout},
    {"pane.mark_target", I::ActMarkTarget, I::ActDomWindow, L"\xE7C1", P::None, L"Ctrl+Alt+D", 0,
     +[](AppState& s) { MarkTargetPane(s); }, FillPanes},
    {"pane.to_window", I::ActPaneToWindow, I::ActDomWindow, L"\xE8A7", P::None, kNoKey, 0, PaneToWindow,
     FillPanes},
    {"window.new", I::ActNewWindow, I::ActDomWindow, L"\xE78B", P::None, L"Ctrl+N", 0,
     +[](AppState& s) { Post(s, WM_NEW_WINDOW); }, kAlways},
    {"window.close", I::ActCloseWindow, I::ActDomWindow, L"\xE8BB", P::None, L"Ctrl+Shift+W", 0,
     +[](AppState& s) { Post(s, WM_CLOSE); }, kAlways},
    {"window.overview", I::WindowOverviewTitle, I::ActDomWindow, L"\xE7C4", P::None, L"Ctrl+Shift+O", 0,
     +[](AppState& s) { ToggleWindowOverview(s); }, kAlways},
    {"window.arrange", I::ArrangeSideBySide, I::ActDomWindow, L"\xE8A9", P::None, kNoKey, 0,
     +[](AppState& s) { ArrangeWindows(s, WindowArrangeLayout::SideBySide); }, kAlways},
    {"window.arrange", I::ArrangeStacked, I::ActDomWindow, L"\xE8A9", P::None, kNoKey, 0,
     +[](AppState& s) { ArrangeWindows(s, WindowArrangeLayout::Stacked); }, kAlways},
    {"window.arrange", I::ArrangeColumns, I::ActDomWindow, L"\xE8A9", P::None, kNoKey, 0,
     +[](AppState& s) { ArrangeWindows(s, WindowArrangeLayout::Columns); }, kAlways},
    {"window.arrange", I::ArrangeTile, I::ActDomWindow, L"\xE8A9", P::None, kNoKey, 0,
     +[](AppState& s) { ArrangeWindows(s, WindowArrangeLayout::Tile); }, kAlways},
    {"window.arrange", I::ArrangeMain, I::ActDomWindow, L"\xE8A9", P::None, kNoKey, 0,
     +[](AppState& s) { ArrangeWindows(s, WindowArrangeLayout::Main); }, kAlways},
    {"window.arrange", I::ArrangeCascade, I::ActDomWindow, L"\xE8A9", P::None, kNoKey, 0,
     +[](AppState& s) { ArrangeWindows(s, WindowArrangeLayout::Cascade); }, kAlways},
    {"window.merge", I::ActMergeWindows, I::ActDomWindow, L"\xE8C8", P::None, L"Ctrl+Shift+J", 0,
     +[](AppState& s) { MergeAllWindows(s); }, kAlways},
    {"window.maximize", I::ActMaximize, I::ActDomWindow, L"\xE922", P::None, L"F11", 0,
     +[](AppState& s) { ShowWindow(s.hwnd, IsZoomed(s.hwnd) ? SW_RESTORE : SW_MAXIMIZE); }, kAlways},
    {"sidebar.toggle", I::ActSidebar, I::ActDomWindow, L"\xE700", P::None, L"Ctrl+B", 0,
     +[](AppState& s) { ToggleSidebarCollapsed(s); }, kAlways},
    {"favbar.toggle", I::ActFavoritesBar, I::ActDomWindow, L"\xE734", P::None, L"Ctrl+Shift+B", 0,
     +[](AppState& s) { ToggleFavoritesBar(s); }, kAlways},
    {"details.toggle", I::DetailsPane, I::ActDomWindow, L"\xE946", P::None, kNoKey, CmdDetailsPanel,
     kNoRun, FillDetails},
    // 视图
    {"view.mode", I::ActViewMode, I::ActDomView, L"\xE8A9", P::None, kNoKey, 0, kNoRun, FillViewMode},
    {"view.sort", I::SortBy, I::ActDomView, L"\xE8CB", P::None, kNoKey, 0, kNoRun, FillSort},
    {"view.sort", I::ActSortDirection, I::ActDomView, L"\xE8CB", P::None, kNoKey, 0,
     RunSortDirection, FillSortDirection},
    {"view.group", I::GroupBy, I::ActDomView, L"\xF168", P::None, kNoKey, 0, kNoRun, FillGroup},
    {"view.folder_sizes", I::ActFolderSizes, I::ActDomView, L"\xE8B7", P::None, kNoKey,
     CmdToggleFolderSizes, kNoRun, FillFolderSizes},
    {"view.columns", I::ColumnLayout, I::ActDomView, L"\xF5C1", P::None, kNoKey, CmdColumnLayout,
     kNoRun, kAlways},
    {"view.apply_all", I::ApplyViewAllFolders, I::ActDomView, L"\xF5B6", P::None, kNoKey,
     CmdApplyViewToAllFolders, kNoRun, FillFolder},
    {"view.filter", I::HelpFilter, I::ActDomView, L"\xE71C", P::None, L"Ctrl+F", 0,
     +[](AppState& s) { ShowFilterEditor(s); }, kAlways},
    {"view.compare", I::ActCompare, I::ActDomView, L"\xF5B5", P::None, kNoKey, CmdCompareToggle,
     kNoRun, FillCompare},
    // 选择
    {"select.all", I::SelectAll, I::ActDomSelect, L"\xE8B3", P::None, L"Ctrl+A", CmdSelectAll, kNoRun, kAlways},
    {"select.invert", I::InvertSelection, I::ActDomSelect, L"\xE7A1", P::None, L"Ctrl+I",
     CmdInvertSelection, kNoRun, kAlways},
    {"select.pattern", I::ActSelectPattern, I::ActDomSelect, L"\xF5B4", P::None, L"Ctrl+Shift+A",
     CmdSelectWildcard, kNoRun, kAlways},
    {"select.clear", I::ActSelectNone, I::ActDomSelect, L"\xE8E6", P::None, kNoKey, 0, SelectNone, FillSelection},
    // The assistant's selector (action_state.h); by hand it is 按模式选择.
    {"select.items", I::ActSelectWhere, I::ActDomSelect, L"\xF5B4", P::None, kNoKey, CmdSelectWildcard, kNoRun,
     kAlways, nullptr, true},
    {"settings.read", I::ActReadSettings, I::ActDomApp, L"\xE713", P::None, kNoKey, CmdSettings, kNoRun, kAlways,
     nullptr, true},
    // 文件
    {"edit.undo", I::Undo, I::ActDomFiles, L"\xE7A7", P::None, L"Ctrl+Z", CmdUndo, kNoRun, FillUndo},
    {"file.copy", I::ActCopy, I::ActDomFiles, L"\xE8C8", P::None, L"Ctrl+C", CmdCopy, kNoRun, FillSelection},
    {"file.cut", I::Cut, I::ActDomFiles, L"\xE8C6", P::None, L"Ctrl+X", CmdCut, kNoRun, FillSelection},
    {"file.paste", I::Paste, I::ActDomFiles, L"\xE77F", P::None, L"Ctrl+V", CmdPaste, kNoRun, FillPaste},
    {"file.copy_path", I::CopyPath, I::ActDomFiles, L"\xF5B2", P::None, L"Ctrl+Shift+C", CmdCopyPath,
     kNoRun, kAlways},
    {"file.copy_to", I::CopyToTarget, I::ActDomFiles, L"\xF5B8", P::None, L"Ctrl+Alt+C", CmdCopyToTarget,
     kNoRun, FillTargetCopy},
    {"file.move_to", I::MoveToTarget, I::ActDomFiles, L"\xF5B9", P::None, L"Ctrl+Alt+X", CmdMoveToTarget,
     kNoRun, FillTargetCopy},
    {"file.new_folder", I::NewFolder, I::ActDomFiles, L"\xE8F4", P::None, L"Ctrl+Shift+N", CmdNewFolder,
     kNoRun, FillFolder},
    {"file.new_file", I::NewTextDocument, I::ActDomFiles, L"\xE8A5", P::None, kNoKey, CmdNewTextFile,
     kNoRun, FillFolder},
    {"file.rename", I::Rename, I::ActDomFiles, L"\xE8AC", P::None, L"F2", CmdRename, kNoRun, FillOne},
    {"file.batch_rename", I::BatchRename, I::ActDomFiles, L"\xF5B3", P::None, L"Ctrl+Shift+R",
     CmdBatchRename, kNoRun, kAlways},
    {"file.group_into_folder", I::ActGroupIntoFolder, I::ActDomFiles, L"\xF5BB", P::None, L"Ctrl+G",
     CmdGroupIntoFolder, kNoRun, FillSelection},
    {"file.dissolve", I::ActDissolveFolder, I::ActDomFiles, L"\xF5BB", P::None, L"Ctrl+Shift+G",
     CmdDissolveFolder, kNoRun, FillSelection},
    {"file.find_duplicates", I::ActFindDuplicates, I::ActDomFiles, L"\xE8C8", P::None, kNoKey, CmdFindDuplicates,
     kNoRun, FillFindDuplicates},
    {"file.zip", I::TrayMenuZip, I::ActDomFiles, L"\xF012", P::None, kNoKey, CmdCompressZip, kNoRun,
     FillSelection},
    {"file.recycle", I::Delete, I::ActDomFiles, L"\xE74D", P::None, L"Delete", CmdDelete, kNoRun,
     FillSelection},
    // Shift+Delete's dialog by hand; the assistant's card asks twice, in red.
    {"file.delete_permanent", I::PermanentDelete, I::ActDomFiles, L"\xE74D", P::None, L"Shift+Delete", 0,
     +[](AppState& s) { DeleteSelected(s, true); }, FillSelection},
    {"recycle.restore", I::Restore, I::ActDomFiles, L"\xE7A7", P::None, kNoKey, CmdRestoreRecycle, kNoRun,
     FillRestore},
    {"recycle.empty", I::EmptyRecycleBin, I::ActDomFiles, L"\xE74D", P::None, kNoKey, CmdEmptyRecycle, kNoRun,
     kAlways},
    {"file.properties", I::Properties, I::ActDomFiles, L"\xE946", P::None, L"Alt+Enter", CmdProperties,
     kNoRun, kAlways},
    {"file.terminal", I::OpenInTerminal, I::ActDomFiles, L"\xE756", P::None, kNoKey, CmdOpenTerminal,
     kNoRun, kAlways},
    // 标签与收藏
    {"tag.panel", I::ActTagPanel, I::ActDomTags, L"\xE8EC", P::None, L"Alt+T", 0,
     ShowTagPickerBelowAddress, FillSelection},
    {"tag.clear", I::ActClearTags, I::ActDomTags, L"\xE8EC", P::None, L"Alt+0", 0,
     +[](AppState& s) { if (const Tab* tab = ActiveTab(s)) ClearTagsWithUndo(s, SelectedFullPaths(*tab)); },
     FillSelection},
    {"tag.toggle", I::ActToggleTag, I::ActDomTags, L"\xE8EC", P::None, kNoKey, 0, kNoRun, FillTags, PickTag},
    {"fav.add", I::ActAddFavorite, I::ActDomTags, L"\xE734", P::None, kNoKey, 0, AddFavorites,
     FillSelectionOrFolder},
    {"workspace.pin", I::PinWorkspace, I::ActDomTags, L"\xF5BE", P::None, kNoKey, CmdPinWorkspace,
     kNoRun, kAlways},
    {"quick_access.pin", I::PinQuickAccess, I::ActDomTags, L"\xF5BD", P::None, kNoKey, CmdPinQuickAccess,
     kNoRun, FillFolder},
    {"alias.manage", I::ManageAliasesEllipsis, I::ActDomTags, L"\xE8EC", P::None, kNoKey,
     CmdManageAliases, kNoRun, kAlways},
    // 暂存区
    {"tray.add", I::ActStageToTray, I::ActDomTray, L"\xE7B8", P::None, L"Ctrl+Shift+S", CmdStageToTray,
     kNoRun, FillSelection},
    // 搜索
    {"search.advanced", I::AdvancedSearch, I::ActDomSearch, L"\xF5B7", P::None, L"Ctrl+Shift+F",
     CmdAdvancedSearch, kNoRun, kAlways},
    {"search.project", I::ActProject, I::ActDomSearch, L"\xE8B7", P::None, L"Ctrl+P", 0,
     +[](AppState& s) { ShowOmnibar(s, OmnibarMode::Project); }, kAlways},
    {"index.install_full", I::EnableFullIndex, I::ActDomSearch, L"\xE721", P::None, kNoKey,
     CmdInstallFullIndex, kNoRun, kAlways},
    // 预览
    {"preview.toggle", I::ActQuickPreview, I::ActDomPreview, L"\xE8FF", P::None, L"Space", 0,
     +[](AppState& s) { ToggleQuickPreview(s); }, kAlways},
    // 网络
    {"net.connect_server", I::ConnectServerMenu, I::ActDomNetwork, L"\xE968", P::None, kNoKey,
     CmdConnectServer, kNoRun, kAlways},
    {"net.map_drive", I::ActMapDrive, I::ActDomNetwork, L"\xE8CE", P::None, kNoKey, CmdMapNetworkDrive,
     kNoRun, kAlways},
    // P4: 标签页与窗口
    {"tab.pin", I::TabPin, I::ActDomTabs, L"\xE718", P::None, kNoKey, 0,
     +[](AppState& s) { RunTab(s, CmdTabPin); }, FillTabPin},
    {"tab.color", I::TabColor, I::ActDomTabs, L"\xE790", P::None, kNoKey, 0, kNoRun, FillTabColor, PickTabColor},
    {"tab.rename", I::TabRename, I::ActDomTabs, L"\xE8AC", P::None, kNoKey, 0,
     +[](AppState& s) { s.tabs.ShowTabMenu(s.window_tabs, ActiveTabIndex(s), BelowAddress(s), *s.menu); }, kAlways},
    {"tab.group", I::ActTabGroup, I::ActDomTabs, L"\xF168", P::None, kNoKey, 0, kNoRun, FillTabGroup, PickTabGroup},
    {"tab.duplicate", I::TabDuplicate, I::ActDomTabs, L"\xE8C8", P::None, kNoKey, 0,
     +[](AppState& s) { RunTab(s, CmdTabDuplicate); }, kAlways},
    {"tab.close_others", I::TabCloseOthers, I::ActDomTabs, L"\xE711", P::None, kNoKey, 0,
     +[](AppState& s) { RunTab(s, CmdTabCloseOthers); }, FillManyTabs},
    {"tab.activate", I::ActTabActivate, I::ActDomTabs, L"\xE7C4", P::None, L"Ctrl+Tab", 0, kNoRun, FillTabList, PickTab},
    {"pane.focus", I::ActPaneFocus, I::ActDomWindow, L"\xE7C4", P::None, L"Tab", 0, kNoRun, FillPaneList, PickPane},
    {"window.activate", I::ActWindowActivate, I::ActDomWindow, L"\xE78B", P::None, kNoKey, 0, kNoRun, FillWindowList,
     PickWindow},
    {"sidebar.section", I::ActSidebarSection, I::ActDomWindow, L"\xE700", P::None, kNoKey, 0, kNoRun, FillSections,
     PickSection},
    {"nav.open_in_new_tab", I::ActOpenInNewTab, I::ActDomNav, L"\xE8A7", P::None, kNoKey, CmdOpenInNewTab, kNoRun,
     FillSelection},
    {"nav.reveal", I::HintActOpenPath, I::ActDomNav, L"\xE8DA", P::None, kNoKey, CmdOpenPath, kNoRun, FillOne},
    // P4: 视图
    {"view.icon_size", I::ActIconSize, I::ActDomView, L"\xE71E", P::None, kNoKey, 0, kNoRun, FillIconSize, PickIconSize},
    {"view.wall", I::PhotoWall, I::ActDomView, L"\xEB9F", P::None, kNoKey, CmdViewWall, kNoRun, FillWall},
    {"view.folders_first", I::ActFoldersFirst, I::ActDomView, L"\xE8B7", P::None, kNoKey, 0, kNoRun, FillFoldersFirst},
    // P4: 标签、别名与最近
    {"tag.create", I::ActTagCreate, I::ActDomTags, L"\xE8EC", P::None, kNoKey, 0,
     +[](AppState& s) { ShowCreateTagPicker(s, BelowAddress(s)); }, kAlways},
    {"tag.edit", I::ActTagEdit, I::ActDomTags, L"\xE8EC", P::None, kNoKey, 0, kNoRun, kAlways, nullptr, true},
    {"tag.delete", I::TagDelete, I::ActDomTags, L"\xE8EC", P::None, kNoKey, 0, kNoRun, kAlways, nullptr, true},
    {"tag.group", I::ActTagGroup, I::ActDomTags, L"\xE8EC", P::None, kNoKey, 0, kNoRun, kAlways, nullptr, true},
    {"smartview.save", I::ActSmartViewSave, I::ActDomTags, L"\xE8EC", P::None, kNoKey, 0, kNoRun, kAlways, nullptr,
     true},
    {"recent.remove", I::RemoveRecent, I::ActDomTags, L"\xE81C", P::None, kNoKey, 0, RunRecentRemove,
     FillRecentRemove},
    {"alias.set", I::SetAliasEllipsis, I::ActDomTags, L"\xE8AC", P::None, kNoKey, CmdSetAlias, kNoRun, FillOne},
    {"alias.display", I::ActAliasDisplay, I::ActDomTags, L"\xE8EC", P::None, kNoKey, 0, kNoRun, FillAliasDisplay},
    {"alias.export", I::ExportAliasesEllipsis, I::ActDomTags, L"\xEDE1", P::None, kNoKey, 0, ExportAliases,
     FillAliases},
    // P4: 暂存区
    {"tray.remove", I::ActTrayRemove, I::ActDomTray, L"\xE711", P::None, kNoKey, 0, kNoRun, FillTray, nullptr, true},
    {"tray.find_stale", I::ActTrayFindStale, I::ActDomTray, L"\xE721", P::None, kNoKey, 0,
     +[](AppState& s) { TrayFindStale(s); }, FillTray},
    {"tray.compare", I::ActTrayCompare, I::ActDomTray, L"\xF5B5", P::None, kNoKey, 0,
     +[](AppState& s) { TrayToggleCompare(s); }, FillTray},
    {"tray.text_diff", I::ActTrayTextDiff, I::ActDomTray, L"\xE8A5", P::None, kNoKey, 0,
     +[](AppState& s) { TrayOpenTextDiff(s); }, FillTray},
    {"tray.zip", I::ActTrayZip, I::ActDomTray, L"\xF012", P::None, kNoKey, 0, +[](AppState& s) { TrayShelfZip(s); },
     FillTray},
    {"tray.send_to", I::ActTraySendTo, I::ActDomTray, L"\xE8DE", P::None, kNoKey, 0,
     +[](AppState& s) { ShowTrayReleaseMenu(s, BelowAddress(s)); }, FillTray},
    // P4: 搜索与预览
    {"search.open", I::ActSearchOpen, I::ActDomSearch, L"\xE721", P::None, kNoKey, 0, kNoRun, kAlways, nullptr,
     true},
    {"search.global", I::GlobalSearch, I::ActDomSearch, L"\xE721", P::None, kNoKey, 0,
     +[](AppState& s) { ToggleGlobalSearch(s); }, kAlways},
    {"index.status", I::ActIndexStatus, I::ActDomSearch, L"\xE9D9", P::None, kNoKey, 0, kNoRun, FillIndexStatus,
     nullptr, true},
    {"file.compute_size", I::ComputeSize, I::ActDomFiles, L"\xE8B7", P::None, kNoKey, 0, ComputeSize,
     FillComputeSize},
    {"preview.step", I::ActPreviewStep, I::ActDomPreview, L"\xE8FF", P::None, kNoKey, 0, kNoRun, FillPreviewStep,
     PickPreviewStep},
    // P4: 网络
    {"net.place", I::ActNetPlace, I::ActDomNetwork, L"\xE968", P::None, kNoKey, 0, kNoRun, kAlways, nullptr, true},
    {"net.recent", I::NetworkClearRecent, I::ActDomNetwork, L"\xE81C", P::None, kNoKey, CmdNetworkClearRecent,
     kNoRun, kAlways},
    {"net.rescan", I::NetworkRescan, I::ActDomNetwork, L"\xE72C", P::None, kNoKey, CmdLanRescan, kNoRun, kAlways},
    {"net.system_settings", I::NetworkSharingSettings, I::ActDomNetwork, L"\xE713", P::None, kNoKey,
     CmdNetworkSharingSettings, kNoRun, kAlways},
    // P4: 快捷键
    {"keymap.read", I::ActKeymapRead, I::ActDomApp, L"\xE765", P::None, kNoKey, 0, kNoRun, kAlways, nullptr, true},
    {"keymap.set", I::ActKeymapSet, I::ActDomApp, L"\xE765", P::None, kNoKey, 0, kNoRun, kAlways, nullptr, true},
    // P4b: 右键菜单与其他程序
    {"file.context_menu", I::ContextMenu, I::ActDomFiles, L"\xE712", P::None, L"Shift+F10", 0,
     +[](AppState& s) { ShowKeyboardContextMenu(s); }, kAlways},
    {"shell.verbs", I::ActShellVerbs, I::ActDomFiles, L"\xE712", P::None, kNoKey, 0, kNoRun, kAlways, nullptr, true},
    {"shell.run_verb", I::ActShellRunVerb, I::ActDomFiles, L"\xE768", P::None, kNoKey, 0, kNoRun, kAlways, nullptr,
     true},
    {"toolbar.open_with", I::ActOpenWithOther, I::ActDomFiles, L"\xE7AC", P::None, kNoKey, 0, ShowToolbarAppMenu,
     FillToolbarOpen},
    {"toolbar.customize", I::CustomizeToolbarMenu, I::ActDomApp, L"\xE9E9", P::None, kNoKey, CmdCustomizeToolbar,
     kNoRun, kAlways},
    {"toolbar.pin_app", I::PinToToolbar, I::ActDomApp, L"\xE718", P::None, kNoKey, CmdCustomizeToolbar, kNoRun,
     kAlways, nullptr, true},
    {"toolbar.tools", I::ActToolbarTools, I::ActDomApp, L"\xE9E9", P::None, kNoKey, CmdCustomizeToolbar, kNoRun,
     kAlways, nullptr, true},
    // P4b: Git
    {"git.refresh", I::GitRefresh, I::ActDomFiles, L"\xE72C", P::None, kNoKey, 0,
     +[](AppState& s) { RunGitCommand(s, kCmdGitRefresh); }, FillGit},
    {"git.pull", I::GitPull, I::ActDomFiles, L"\xE896", P::None, kNoKey, 0,
     +[](AppState& s) { RunGitCommand(s, kCmdGitPull); }, FillGit},
    {"git.push", I::GitPush, I::ActDomFiles, L"\xE898", P::None, kNoKey, 0,
     +[](AppState& s) { RunGitCommand(s, kCmdGitPush); }, FillGit},
    // P4b: 云盘与驱动器
    {"cloud.keep", I::CloudKeepOnDevice, I::ActDomFiles, L"\xE718", P::None, kNoKey, CmdCloudKeep, kNoRun,
     FillCloudPin},
    {"cloud.free", I::CloudFreeUpSpace, I::ActDomFiles, L"\xE753", P::None, kNoKey, CmdCloudFree, kNoRun,
     FillCloudPin},
    {"preview.cloud_download", I::CloudDownloadPreview, I::ActDomPreview, L"\xE753", P::None, kNoKey, 0,
     +[](AppState& s) { RunWithArgs(s, "preview.cloud_download", json::Value::Object()); }, FillCloudFile},
    {"drive.eject", I::Eject, I::ActDomFiles, L"\xE88E", P::None, kNoKey, CmdEjectDrive, kNoRun, FillEject},
    {"drive.disconnect", I::DisconnectNetworkDrive, I::ActDomFiles, L"\xE8CD", P::None, kNoKey, CmdDisconnectDrive, kNoRun,
     FillDisconnect},
    {"drive.unlock", I::UnlockDrive, I::ActDomFiles, L"\xE785", P::None, kNoKey, CmdUnlockDrive, kNoRun, FillUnlock},
    {"assistant.organize_here", I::ActOrganizeHere, I::ActDomAssistant, ui::command_icons::kGlyphSparkle, P::None,
     kNoKey, CmdAiOrganizeHere, kNoRun, FillOrganizeHere},
    {"assistant.prefs", I::ActAssistantPrefs, I::ActDomApp, L"\xE713", P::None, kNoKey, 0,
     +[](AppState& s) { OpenSettingsTab(s, 13); }, kAlways},
    // 应用
    {"app.settings_page", I::ActOpenSettings, I::ActDomApp, L"\xE713", P::None, kNoKey, CmdSettings,
     kNoRun, kAlways},
    {"app.shortcut_help", I::ShortcutHints, I::ActDomApp, L"\xE765", P::None, L"Ctrl+/", CmdShortcutHelp,
     kNoRun, kAlways},
    {"app.exit", I::ExitPulse, I::ActDomApp, L"\xE7E8", P::None, kNoKey, CmdExitPulse, kNoRun, kAlways},
};

constexpr int kDefCount = static_cast<int>(std::size(kDefs));

constexpr bool AllIdsKnown() {
    for (const Def& d : kDefs)
        if (!FindAction(d.id)) return false;
    return true;
}
static_assert(AllIdsKnown(), "kDefs: an id is missing from action_inventory.h");

std::wstring SettingsDomain(int page) {
    std::wstring text = Text(I::ActDomSettings);
    const size_t at = text.find(L"%s");
    if (at != std::wstring::npos) text.replace(at, 2, SettingsPageTitle(page));
    return text;
}

// The settings page's own icon for each row (ui_settings_core.cpp).
constexpr std::pair<std::string_view, const wchar_t*> kSettingGlyphs[] = {
    {"theme", L"\xE706"}, {"window_effect", L"\xE81E"}, {"text_rendering", L"\xE8D2"},
    {"font_size", L"\xE8E9"}, {"language", L"\xE774"}, {"row_height", L"\xECE7"},
    {"click_to_open", L"\xE8B0"}, {"name_highlight", L"\xE7E6"}, {"blank_double_click", L"\xE72B"},
    {"folders_first", L"\xE8CB"}, {"alias_mark", L"\xE8AC"}, {"tree_sticky", L"\xE840"}, {"mtime_marks", L"\xE916"},
    {"mtime_range", L"\xEC92"}, {"startup_open", L"\xE81C"}, {"new_tab_open", L"\xECCD"},
    {"external_open", L"\xE8AD"}, {"tray_icon", L"\xECAA"}, {"open_folders_with_pulse", L"\xEC50"},
    {"staging_icon_size", L"\xED1E"}, {"change_days", L"\xE9D5"},
    {"menu_acrylic", L"\xE700"}, {"hover_select", L"\xE815"}, {"item_checkboxes", L"\xE73A"},
    {"show_hidden", L"\xED1A"}, {"show_protected", L"\xEA18"}, {"smart_dates", L"\xE787"},
    {"zebra_rows", L"\xE7BC"}, {"size_bars", L"\xE9D2"}, {"tag_name_color", L"\xE8EC"},
    {"selection_outline", L"\xE8B3"}, {"thumbnail_app_badges", L"\xE8B9"}, {"mtime_dim_older", L"\xE793"},
    {"mtime_bold_today", L"\xE8DD"}, {"mtime_folders", L"\xE8B7"}, {"mtime_other_views", L"\xE8A9"},
    {"mtime_flash", L"\xE945"}, {"onedrive_status", L"\xE753"}, {"svn_status", L"\xE8AB"},
    {"vertical_tabs", L"\xE8A0"}, {"pinned_tab_names", L"\xE718"}, {"double_click_closes_tab", L"\xEA39"},
    {"close_window_with_last_tab", L"\xE89F"}, {"launch_at_signin", L"\xEC57"}, {"start_in_tray", L"\xE896"},
    {"keep_running", L"\xE9F5"}, {"explorer_tag_menu", L"\xE838"}, {"staging_hide_when_empty", L"\xE8F6"},
    {"change_tracking", L"\xE823"}, {"lan_discovery", L"\xE701"}, {"network_recent", L"\xE81C"},
    {"hidden_shares", L"\xED1A"}, {"confirm_delete", L"\xE897"}, {"global_search", L"\xE721"},
    {"pinyin_search", L"\xE721"},
};

const wchar_t* SettingGlyph(std::string_view key) {
    for (const auto& [k, glyph] : kSettingGlyphs)
        if (k == key) return glyph;
    return L"\xE713";
}

ActionView SettingView(AppState& s, const SetDef& d, int index) {
    ActionView v;
    v.index = index;
    v.id = "settings.set";
    v.key = d.id;
    v.title = Text(d.label);
    v.domain = SettingsDomain(d.page);
    v.glyph = SettingGlyph(d.id);
    const int now = d.get(s);
    v.value = SettingText(d, now);
    if (!IsSwitch(d)) {
        const int current = ChoiceIndex(d, now);
        for (size_t i = 0; i < d.choices.size() && d.choices[i].key; ++i) {
            ui::FluentMenuItem row;
            row.text = Text(d.choices[i].label);
            row.radio_group = true;
            row.radio = static_cast<int>(i) == current;
            v.choices.push_back(std::move(row));
        }
    }
    return v;
}

} // namespace

std::vector<ActionView> ListActions(AppState& s) {
    const auto settings = SettingDefs();
    std::vector<ActionView> out;
    out.reserve(static_cast<size_t>(kDefCount) + settings.size());
    for (int i = 0; i < kDefCount; ++i) {
        const Def& d = kDefs[i];
        ActionView v;
        v.index = i;
        v.id = d.id;
        v.title = Text(d.title);
        v.domain = Text(d.domain);
        if (d.glyph) v.glyph = d.glyph;
        v.pictogram = d.pictogram;
        if (d.chord) v.shortcut = ShownShortcut(d.chord);
        if (d.id == "window.arrange") {
            constexpr I labels[]={I::ArrangeSideBySide,I::ArrangeStacked,I::ArrangeColumns,I::ArrangeTile,I::ArrangeMain,I::ArrangeCascade};
            for (int layout=0;layout<6;++layout) if(d.title==labels[layout]) {
                const auto key=static_cast<KeyAction>(static_cast<int>(KeyAction::ArrangeSideBySide)+layout);
                const auto& bindings=ActiveKeymap().Bindings(key);
                if (!bindings.empty()) v.shortcut=ChordDisplay(bindings.front());
            }
        }
        v.palette = !d.hidden;
        if (d.fill) d.fill(s, v);
        out.push_back(std::move(v));
    }
    for (size_t i = 0; i < settings.size(); ++i)
        out.push_back(SettingView(s, settings[i], kDefCount + static_cast<int>(i)));
    return out;
}

bool RunAction(AppState& s, int index, int choice) {
    if (index < 0) return false;
    if (index >= kDefCount) {
        const auto settings = SettingDefs();
        const size_t at = static_cast<size_t>(index - kDefCount);
        if (at >= settings.size()) return false;
        const SetDef& d = settings[at];
        if (IsSwitch(d)) {
            ApplySetting(s, d, d.get(s) != 0 ? 0 : 1);
            return true;
        }
        if (choice < 0 || choice >= static_cast<int>(d.choices.size()) || !d.choices[static_cast<size_t>(choice)].key)
            return false;
        ApplySetting(s, d, choice);
        return true;
    }
    const Def& d = kDefs[index];
    ActionView v;
    if (d.fill) d.fill(s, v);
    if (!v.enabled) return false;
    if (choice >= 0) {
        if (choice >= static_cast<int>(v.choices.size())) return false;
        const auto& row = v.choices[static_cast<size_t>(choice)];
        if (!row.enabled) return false;
        if (row.command == CmdNone) {
            if (!d.pick) return false;
            d.pick(s, choice);
            return true;
        }
        DispatchMenuCommand(s, row.command);
        return true;
    }
    if (d.command != 0) DispatchMenuCommand(s, d.command);
    else if (d.run) d.run(s);
    else return false;
    return true;
}

int ActionOfMenuCommand(AppState& s, int command) {
    if (command == CmdNone) return -1;
    for (int i = 0; i < kDefCount; ++i) {
        const Def& d = kDefs[i];
        if (d.command == command) return i;
        if (!d.fill || d.command != 0 || d.run) continue;  // only choice actions remain
        ActionView v;
        d.fill(s, v);
        for (const auto& row : v.choices)
            if (row.command == command) return i;
    }
    return -1;
}

} // namespace pulse::app::actions
