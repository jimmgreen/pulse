#include "quick_access.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "app_network_locations.h"
#include "../common/localization.h"
#include <algorithm>
#include <cwchar>

namespace pulse {
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
    ui::FluentMenuItem remove;
    remove.command = app::CmdUnpinQuickAccess;
    remove.text = l10n::Get(l10n::StringId::UnpinQuickAccess);
    remove.glyph = L"\xE77A";
    const int command = s.menu->TrackPopup(point, {open, remove});
    if (command == app::CmdOpen) NavigateTo(s, path);
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
    choose.separator_after = true;
    // Files belong in Starred; say so where people look for it.
    ui::FluentMenuItem hint;
    hint.command = kFilesHint;
    hint.text = l10n::Get(l10n::StringId::QuickAccessFilesHint);
    hint.glyph = L"\xE734";
    hint.enabled = false;
    hint.secondary = true;
    const int command = s.menu->TrackPopup(point, {current, choose, hint});
    if (command == kAddCurrent && !here.empty()) {
        s.places.SetQuickAccessPinned({here}, true);
    } else if (command == kChoose) {
        std::wstring picked;
        if (!PickFolder(s, picked,
                        l10n::Get(l10n::StringId::QuickAccessChooseFolder).c_str()) ||
            picked.empty())
            return;
        s.places.SetQuickAccessPinned({picked}, true);
    } else {
        return;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

namespace {
bool EditWorkspaceText(AppState& s, POINT point, l10n::StringId hint,
                       std::wstring& text, bool allow_empty = false) {
    if (!EnsureMenu(s)) return false;
    constexpr int kSave = 31001;
    s.menu->SetFilterPlaceholder(l10n::Get(hint));
    s.menu->SetInitialFilterText(text);
    s.menu->SetFilterMinWidth(320.0f);
    const auto build = [&](const std::wstring& query) {
        text = query;
        ui::FluentMenuItem save;
        save.command = kSave;
        save.text = l10n::Get(l10n::StringId::TabNameSave);
        save.enabled = allow_empty || query.find_first_not_of(L" \t\r\n") != std::wstring::npos;
        return std::vector<ui::FluentMenuItem>{save};
    };
    const int command = s.menu->TrackPopup(point, build(text), build);
    s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::TabMenuSearch));
    s.menu->SetFilterMinWidth(0.0f);
    return command == kSave;
}

bool CanSaveWorkspace(const std::vector<std::wstring>& paths) {
    return !paths.empty() && paths.size() <= 4 &&
        std::all_of(paths.begin(), paths.end(), [](const auto& path) {
            return !path.empty() && !fs::IsVirtualPath(path);
        });
}
}

void SaveCurrentWorkspace(AppState& s) {
    const auto paths = CollectPanePaths(s);
    if (!CanSaveWorkspace(paths)) return;
    std::wstring name = app::LayoutTabTitle(LiveLayout(s));
    POINT point{};
    GetCursorPos(&point);
    if (!EditWorkspaceText(s, point, l10n::StringId::WorkspaceNameHint, name)) return;
    const int index = s.places.PinWorkspace(paths.front(), name,
        static_cast<int>(WorkspaceLayoutOf(s)), paths, CollectPaneViews(s));
    if (index >= 0) {
        s.places.RenameWorkspace(index, name);
        LiveLayout(s).workspace_id = s.places.workspaces[static_cast<size_t>(index)].id;
        LiveLayout(s).title = s.places.workspaces[static_cast<size_t>(index)].name;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool ToggleWorkspaceGroup(AppState& s, const std::wstring& path) {
    constexpr std::wstring_view prefix = L"pulse:workspace-group:";
    if (!path.starts_with(prefix)) return false;
    const auto group = path.substr(prefix.size());
    if (!s.places.collapsed_workspace_groups.erase(group))
        s.places.collapsed_workspace_groups.insert(group);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

bool ToggleWorkspaceDirectories(AppState& s, const std::wstring& path) {
    std::wstring kind, rest;
    if (!app::ParsePulsePath(path, &kind, &rest) || kind != L"workspace" || rest.empty() ||
        rest.find_first_not_of(L"0123456789") != std::wstring::npos) return false;
    const auto index = std::wcstoull(rest.c_str(), nullptr, 10);
    if (index >= s.places.workspaces.size()) return false;
    const auto& id = s.places.workspaces[static_cast<size_t>(index)].id;
    if (!s.places.expanded_workspace_ids.erase(id)) s.places.expanded_workspace_ids.insert(id);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

void ShowWorkspaceMenu(AppState& s, int index, POINT point) {
    if (index < 0 || index >= static_cast<int>(s.places.workspaces.size())) return;
    if (!EnsureMenu(s)) return;
    constexpr int kUpdate = 31002, kUnpin = 31003, kRename = 31004, kGroup = 31005;
    ui::FluentMenuItem open;
    open.command = app::CmdOpen;
    open.text = l10n::Get(l10n::StringId::Open);
    open.glyph = L"\xE8B7";
    ui::FluentMenuItem update;
    update.command = kUpdate;
    update.text = l10n::Get(l10n::StringId::WorkspaceUpdateLayout);
    update.glyph = L"\xE72C";
    update.enabled = CanSaveWorkspace(CollectPanePaths(s));
    update.separator_after = true;
    ui::FluentMenuItem unpin;
    unpin.command = kUnpin;
    unpin.text = l10n::Get(l10n::StringId::UnpinWorkspace);
    unpin.glyph = L"\xE77A";
    ui::FluentMenuItem rename;
    rename.command = kRename;
    rename.text = l10n::Get(l10n::StringId::Rename);
    rename.glyph = L"\xE8AC";
    ui::FluentMenuItem group;
    group.command = kGroup;
    group.text = l10n::Get(l10n::StringId::WorkspaceGroup);
    group.glyph = L"\xE8B7";
    const int command = s.menu->TrackPopup(point, {open, rename, group, update, unpin});
    if (command == app::CmdOpen) {
        OpenWorkspace(s, index);
    } else if (command == kRename || command == kGroup) {
        const auto& w = s.places.workspaces[static_cast<size_t>(index)];
        const auto id = w.id;
        std::wstring text = command == kRename ? w.name : w.group;
        if (!EditWorkspaceText(s, point, command == kRename ? l10n::StringId::WorkspaceNameHint
                : l10n::StringId::WorkspaceGroupHint, text, command == kGroup)) return;
        if (command == kRename) {
            s.places.RenameWorkspace(index, text);
            for (auto& tab : s.window_tabs.items)
                if (tab->workspace_id == id) tab->title = s.places.workspaces[static_cast<size_t>(index)].name;
        } else s.places.SetWorkspaceGroup(index, text);
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
    ui::FluentMenuItem unpin;
    unpin.command = app::CmdUnpinQuickAccess;
    unpin.text = l10n::Get(l10n::StringId::Unpin);
    unpin.glyph = L"\xE77A";
    const int command = s.menu->TrackPopup(point, {open, unpin});
    if (command == app::CmdOpen) {
        NavigateTo(s, unc);
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
