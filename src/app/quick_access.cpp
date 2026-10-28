#include "quick_access.h"
#include "context_menu.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "app_network_locations.h"
#include "app_network_page.h"
#include "../common/localization.h"
#include "../ui/quick_access_editor.h"
#include <algorithm>

namespace pulse {
namespace {

// Local command of the quick-access menus; outside the MenuCmd range.
constexpr int kEditQuickAccessCommand = 0x7E00;

std::wstring QuickAccessKeyPath(const AppState& s, const std::wstring& key) {
    const int builtin = app::BuiltinQuickAccessFromKey(key);
    if (builtin < 0) return key;
    for (const auto& entry : s.sidebar.quick_access) {
        if (entry.builtin == builtin) return entry.path;
    }
    return {};
}

ui::QuickAccessEditorRow PinEditorRow(const std::wstring& path) {
    ui::QuickAccessEditorRow row;
    row.key = path;
    row.label = app::TabTitle(path);
    row.detail = path;
    row.glyph = L"\xE8B7";
    row.color = ui::HexColor(0xFBBF24);
    return row;
}

std::vector<ui::QuickAccessEditorRow> QuickAccessEditorRows(const AppState& s) {
    const auto builtin_keys = app::BuiltinQuickAccessDefaultKeys(s.sidebar.quick_access);
    std::vector<ui::QuickAccessEditorRow> rows;
    for (const auto& key : app::QuickAccessDisplayKeys(s.sidebar.quick_access, &s.places,
                                                      s.sidebarQuickAccessHiddenMask, true)) {
        const int builtin = app::BuiltinQuickAccessFromKey(key);
        if (builtin < 0) {
            rows.push_back(PinEditorRow(key));
            continue;
        }
        const auto entry = std::find_if(s.sidebar.quick_access.begin(), s.sidebar.quick_access.end(),
            [&](const auto& candidate) { return candidate.builtin == builtin; });
        if (entry == s.sidebar.quick_access.end()) continue;
        ui::QuickAccessEditorRow row;
        row.key = key;
        row.label = entry->label;
        row.glyph = entry->glyph;
        row.color = entry->color;
        row.builtin = true;
        row.visible = ((s.sidebarQuickAccessHiddenMask >> builtin) & 1u) == 0;
        row.default_visible = ((app::kDefaultQuickAccessHiddenMask >> builtin) & 1u) == 0;
        row.default_index = static_cast<int>(
            std::find(builtin_keys.begin(), builtin_keys.end(), key) - builtin_keys.begin());
        rows.push_back(std::move(row));
    }
    return rows;
}

void ApplyQuickAccessEditorRows(AppState& s, const std::vector<ui::QuickAccessEditorRow>& rows) {
    std::vector<std::wstring> keys;
    keys.reserve(rows.size());
    uint32_t mask = s.sidebarQuickAccessHiddenMask;
    for (const auto& row : rows) {
        keys.push_back(row.key);
        const int builtin = app::BuiltinQuickAccessFromKey(row.key);
        if (builtin < 0) continue;
        if (row.visible) mask &= ~(1u << builtin);
        else mask |= 1u << builtin;
    }
    s.sidebarQuickAccessHiddenMask = mask;
    s.places.SetQuickAccessLayout(keys, app::BuiltinQuickAccessDefaultKeys(s.sidebar.quick_access));
}

ui::FluentMenuItem EditQuickAccessMenuItem() {
    ui::FluentMenuItem item;
    item.command = kEditQuickAccessCommand;
    item.text = l10n::Get(l10n::StringId::QuickAccessEdit);
    item.glyph = L"\xE70F";
    return item;
}

} // namespace

void ShowQuickAccessEditor(AppState& s, POINT point) {
    ui::QuickAccessEditorText text;
    text.title = l10n::Get(l10n::StringId::QuickAccessEditTitle);
    text.hint = l10n::Get(l10n::StringId::QuickAccessEditHint);
    text.reset = l10n::Get(l10n::StringId::RestoreDefaults);
    text.add_folder = l10n::Get(l10n::StringId::QuickAccessEditAddFolder);
    text.done = l10n::Get(l10n::StringId::QuickAccessEditDone);
    std::vector<ui::QuickAccessEditorRow> rows = QuickAccessEditorRows(s);
    for (;;) {
        const auto result = ui::QuickAccessEditorPopup::Edit(s.hwnd, &s.compositor, s.scale,
                                                             point, s.darkMode, s.accentColor, text, rows);
        if (result == ui::QuickAccessEditorResult::Cancelled) return;
        if (result == ui::QuickAccessEditorResult::Applied) break;
        // 添加文件夹…: pick, add the row, and reopen with the edits so far.
        std::wstring picked;
        if (PickFolder(s, picked, l10n::Get(l10n::StringId::QuickAccessChooseFolder).c_str(), L"quick_access") &&
            !picked.empty() && !fs::IsVirtualPath(picked)) {
            const std::wstring path = fs::NormalizePath(picked);
            const bool listed = std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
                return !row.builtin && _wcsicmp(row.key.c_str(), path.c_str()) == 0;
            });
            if (!path.empty() && !listed) rows.push_back(PinEditorRow(path));
        }
    }
    ApplyQuickAccessEditorRows(s, rows);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

std::vector<std::wstring> QuickAccessGroupRowKeys(const AppState& s, const ui::SidebarGroup& group) {
    // The group lists the display keys in order, with the project row mixed
    // in; walk both and match by path.
    const auto keys = app::QuickAccessDisplayKeys(s.sidebar.quick_access, &s.places,
                                                  s.sidebarQuickAccessHiddenMask, false);
    std::vector<std::wstring> out(group.items.size());
    size_t next = 0;
    for (size_t i = 0; i < group.items.size() && next < keys.size(); ++i) {
        if (_wcsicmp(group.items[i].path.c_str(), QuickAccessKeyPath(s, keys[next]).c_str()) != 0)
            continue;
        out[i] = keys[next++];
    }
    return out;
}

std::wstring QuickAccessRowKey(const AppState& s, const ui::WindowViewModel& vm,
                               int section, int item) {
    if (section != static_cast<int>(app::SidebarSectionId::QuickAccess) || item < 0) return {};
    const int group = app::SidebarSectionIndex(vm, section);
    if (group < 0) return {};
    const auto keys = QuickAccessGroupRowKeys(s, vm.sidebar[static_cast<size_t>(group)]);
    return static_cast<size_t>(item) < keys.size() ? keys[static_cast<size_t>(item)] : std::wstring{};
}

void CommitQuickAccessDrag(AppState& s, const std::wstring& key, size_t gap) {
    const auto all = app::QuickAccessDisplayKeys(s.sidebar.quick_access, &s.places,
                                                 s.sidebarQuickAccessHiddenMask, true);
    const auto visible = app::QuickAccessDisplayKeys(s.sidebar.quick_access, &s.places,
                                                     s.sidebarQuickAccessHiddenMask, false);
    s.places.SetQuickAccessLayout(app::MoveQuickAccessKey(all, visible, key, gap),
                                  app::BuiltinQuickAccessDefaultKeys(s.sidebar.quick_access));
}

std::vector<std::wstring> QuickAccessTargets(const app::Tab* tab, bool background) {
    if (!tab || IsRecycleTab(tab)) return {};
    if (background) {
        if (tab->current_path.empty() || fs::IsVirtualPath(tab->current_path)) return {};
        return {tab->current_path};
    }
    if (!tab->snapshot) return {};
    std::vector<std::wstring> paths;
    for (int index : tab->SelectedIndices()) {
        if (index < 0 || static_cast<size_t>(index) >= tab->EntryCount() ||
            !tab->EntryAt(static_cast<size_t>(index)).is_dir) return {};
        auto path = EntryFullPath(*tab, index);
        if (path.empty() || fs::IsVirtualPath(path)) return {};
        paths.push_back(std::move(path));
    }
    return paths;
}

void AppendQuickAccessCommand(AppState& s, std::vector<ui::FluentMenuItem>& items,
                              const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    const bool all_pinned = std::all_of(paths.begin(), paths.end(),
        [&](const auto& path) { return s.places.IsQuickAccessPinned(path); });
    const std::wstring key = L"pulse:quick-access";
    if (s.ctxMenuPrefs.RecordSeen(key, l10n::Get(l10n::StringId::PinQuickAccess),
                                false, ipc::CtxMenuCategory::Software, false))
        s.ctxMenuPrefs.Save();
    const auto pref = s.ctxMenuPrefs.item_enabled.find(key);
    if (pref != s.ctxMenuPrefs.item_enabled.end() && !pref->second) return;
    ui::FluentMenuItem item;
    item.command = all_pinned ? app::CmdUnpinQuickAccess : app::CmdPinQuickAccess;
    item.text = l10n::Get(all_pinned ? l10n::StringId::UnpinQuickAccess
                                    : l10n::StringId::PinQuickAccess);
    item.glyph = L"\xE718";
    auto at = std::find_if(items.begin(), items.end(), [](const auto& value) {
        return value.command == app::CmdPinWorkspace;
    });
    items.insert(at, std::move(item));
}

bool HandleQuickAccessCommand(AppState& s, int command,
                              const std::vector<std::wstring>& paths) {
    if (command != app::CmdPinQuickAccess && command != app::CmdUnpinQuickAccess) return false;
    s.places.SetQuickAccessPinned(paths, command == app::CmdPinQuickAccess);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

void ShowQuickAccessMenu(AppState& s, const std::wstring& path, POINT point) {
    if (!EnsureMenu(s)) return;
    ui::FluentMenuItem open;
    open.command = app::CmdOpen;
    open.text = l10n::Get(l10n::StringId::Open);
    open.glyph = L"\xE8B7";
    // #58-2: a plain click navigates the focused pane; a new tab is one
    // gesture away, and the menu says which.
    ui::FluentMenuItem new_tab;
    new_tab.command = app::CmdOpenInNewTab;
    new_tab.text = l10n::Get(l10n::StringId::OpenNewTab);
    new_tab.shortcut = l10n::Get(l10n::StringId::QuickAccessNewTabHint);
    new_tab.glyph = L"\xE8A7";
    ui::FluentMenuItem remove;
    remove.command = app::CmdUnpinQuickAccess;
    remove.text = l10n::Get(l10n::StringId::UnpinQuickAccess);
    remove.glyph = L"\xE77A";
    remove.separator_after = true;
    std::vector<ui::FluentMenuItem> items{open, new_tab, remove, EditQuickAccessMenuItem()};
    app::ApplyBuiltinMenuPrefs(items, s.ctxMenuPrefs, app::BuiltinMenuSurface::Item);
    const int command = s.menu->TrackPopup(point, std::move(items));
    if (command == app::CmdOpen) NavigateTo(s, path);
    else if (command == app::CmdOpenInNewTab) OpenFolderTab(s, path);
    else if (command == kEditQuickAccessCommand) ShowQuickAccessEditor(s, point);
    else HandleQuickAccessCommand(s, command, {path});
}

void ShowQuickAccessAddMenu(AppState& s, POINT point) {
    if (!EnsureMenu(s)) return;
    constexpr int kAddCurrent = 1, kChoose = 2, kFilesHint = 3;
    const app::Tab* tab = ActiveTab(s);
    std::wstring here;
    if (tab && !IsRecycleTab(tab) && !tab->current_path.empty() &&
        !fs::IsVirtualPath(tab->current_path))
        here = tab->current_path;
    ui::FluentMenuItem current;
    current.command = kAddCurrent;
    current.text = l10n::Get(l10n::StringId::QuickAccessAddCurrent);
    current.glyph = L"\xE8B7";
    current.enabled = !here.empty() && !s.places.IsQuickAccessPinned(here);
    if (!here.empty()) {
        const size_t slash = here.find_last_of(L"\\/", here.size() >= 2 ? here.size() - 2 : 0);
        current.shortcut = here.size() > 3 && slash != std::wstring::npos
            ? here.substr(slash + 1) : here;
        while (current.shortcut.size() > 1 &&
               (current.shortcut.back() == L'\\' || current.shortcut.back() == L'/'))
            current.shortcut.pop_back();
    }
    ui::FluentMenuItem choose;
    choose.command = kChoose;
    choose.text = l10n::Get(l10n::StringId::QuickAccessChooseFolder);
    choose.glyph = L"\xED25";
    ui::FluentMenuItem edit = EditQuickAccessMenuItem();
    edit.separator_after = true;
    // Files belong in Starred; say so where people look for it.
    ui::FluentMenuItem hint;
    hint.command = kFilesHint;
    hint.text = l10n::Get(l10n::StringId::QuickAccessFilesHint);
    hint.glyph = L"\xE734";
    hint.enabled = false;
    hint.secondary = true;
    const int command = s.menu->TrackPopup(point, {current, choose, edit, hint});
    if (command == kEditQuickAccessCommand) {
        ShowQuickAccessEditor(s, point);
        return;
    }
    if (command == kAddCurrent && !here.empty()) {
        s.places.SetQuickAccessPinned({here}, true);
    } else if (command == kChoose) {
        std::wstring picked;
        if (!PickFolder(s, picked,
                        l10n::Get(l10n::StringId::QuickAccessChooseFolder).c_str(), L"quick_access") ||
            picked.empty())
            return;
        s.places.SetQuickAccessPinned({picked}, true);
    } else {
        return;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ShowWorkspaceMenu(AppState& s, int index, POINT point) {
    if (index < 0 || index >= static_cast<int>(s.places.workspaces.size())) return;
    if (!EnsureMenu(s)) return;
    constexpr int kUpdate = 2, kUnpin = 3;
    ui::FluentMenuItem open;
    open.command = app::CmdOpen;
    open.text = l10n::Get(l10n::StringId::Open);
    open.glyph = L"\xE8B7";
    ui::FluentMenuItem update;
    update.command = kUpdate;
    update.text = l10n::Get(l10n::StringId::WorkspaceUpdateLayout);
    update.glyph = L"\xE72C";
    update.separator_after = true;
    ui::FluentMenuItem unpin;
    unpin.command = kUnpin;
    unpin.text = l10n::Get(l10n::StringId::UnpinWorkspace);
    unpin.glyph = L"\xE77A";
    const int command = s.menu->TrackPopup(point, {open, update, unpin});
    if (command == app::CmdOpen) {
        OpenWorkspace(s, index);
    } else if (command == kUpdate) {
        // The snapshot was only taken when pinning; this is the one way to
        // refresh it without unpinning (which would drop frequent folders).
        s.places.UpdateWorkspaceSnapshot(index, static_cast<int>(WorkspaceLayoutOf(s)),
                                         CollectPanePaths(s), CollectPaneViews(s));
        s.places.active_workspace = index;
        s.places.Save();
        const auto& w = s.places.workspaces[static_cast<size_t>(index)];
        wchar_t message[512]{};
        swprintf_s(message, l10n::Get(l10n::StringId::WorkspaceLayoutUpdated).c_str(),
                   w.name.c_str());
        s.notification_toast.Show(s.hwnd, l10n::Get(l10n::StringId::SidebarWorkspaces),
                                  message, false);
    } else if (command == kUnpin) {
        s.places.UnpinWorkspace(index);
    } else {
        return;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ShowNetworkPlaceMenu(AppState& s, const std::wstring& unc, POINT point) {
    if (!EnsureMenu(s)) return;
    ui::FluentMenuItem open;
    open.command = app::CmdOpen;
    open.text = l10n::Get(l10n::StringId::Open);
    open.glyph = L"\xE8B7";
    ui::FluentMenuItem rename;
    rename.command = app::CmdRenameNetwork;
    rename.text = l10n::Get(l10n::StringId::Rename);
    rename.glyph = L"\xE8AC";
    ui::FluentMenuItem unpin;
    unpin.command = app::CmdUnpinQuickAccess;
    unpin.text = l10n::Get(l10n::StringId::Unpin);
    unpin.glyph = L"\xE77A";
    const int command = s.menu->TrackPopup(point, {open, rename, unpin});
    if (command == app::CmdOpen) {
        NavigateTo(s, unc);
    } else if (command == app::CmdRenameNetwork) {
        RenameNetworkPin(s, unc);  // 重命名: Pulse's name only
    } else if (command == app::CmdUnpinQuickAccess && s.places.UnpinNetwork(unc)) {
        // The Network view lists the pins too.
        ForEachPane(s, [&](app::Pane& pane) {
            if (auto* tab = pane.ActiveTab(); tab && tab->current_path == kNetworkLocationsPath)
                FillNetworkLocationsView(s, *tab);
        });
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}
}