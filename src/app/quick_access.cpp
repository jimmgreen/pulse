#include "quick_access.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "../common/localization.h"
#include <algorithm>

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
        if (index < 0 || static_cast<size_t>(index) >= tab->snapshot->size() ||
            !(*tab->snapshot)[static_cast<size_t>(index)].is_dir) return {};
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
}
