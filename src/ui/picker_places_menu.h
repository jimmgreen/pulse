#pragma once

#include "fluent_menu.h"
#include "folder_picker_model.h"
#include "../common/localization.h"
#include "../common/picker_places_snapshot.h"
#include <algorithm>

namespace pulse::ui {

// Commands 100+i and 200+i retain the original snapshot/hidden-path indices.
inline std::vector<FluentMenuItem> BuildPickerPlacesMenu(
    const picker_places::Snapshot& snapshot, const std::wstring& path,
    const std::vector<std::wstring>& hidden_paths, bool mutations_available,
    bool is_folder = true, const std::wstring& host_app_name = {}, bool pinned_to_app = false,
    bool removable_common = false) {
    using Id = l10n::StringId;
    constexpr wchar_t group_prefix[] = L"pulse:star-group:";
    const auto item = [](int command, Id text, bool enabled = true) {
        FluentMenuItem row;
        row.command = command;
        row.text = l10n::Get(text);
        row.enabled = enabled;
        return row;
    };
    const auto favorite = std::find_if(snapshot.starred_items.begin(), snapshot.starred_items.end(),
        [&](const auto& entry) { return SamePickerPath(entry.path, path); });
    const auto workspace = std::find_if(snapshot.workspaces.begin(), snapshot.workspaces.end(),
        [&](const auto& entry) { return SamePickerPath(entry.root, path); });
    const bool group_path = path.starts_with(group_prefix);
    const std::wstring group_id = group_path ? path.substr(std::size(group_prefix) - 1) : std::wstring();
    const auto group = std::find_if(snapshot.starred_groups.begin(), snapshot.starred_groups.end(),
        [&](const auto& entry) { return entry.id == group_id; });
    std::vector<FluentMenuItem> rows;
    // A 常用 row is removed (command 18), not hidden: hiding is for fixed places.
    if (!path.empty() && !removable_common) {
        rows.push_back(item(1, Id::PickerHidePlace, hidden_paths.size() < 64));
        rows.back().mnemonic = L'H';
    }
    auto restore = item(0, Id::PickerRestorePlaces, !hidden_paths.empty());
    restore.mnemonic = L'R';
    for (size_t i = 0; i < std::min<size_t>(64, hidden_paths.size()); ++i) {
        FluentMenuItem row;
        row.command = 200 + static_cast<int>(i);
        row.text = hidden_paths[i];
        row.tooltip = hidden_paths[i];
        if (hidden_paths[i].starts_with(group_prefix)) {
            const auto id = hidden_paths[i].substr(std::size(group_prefix) - 1);
            const auto known = std::find_if(snapshot.starred_groups.begin(), snapshot.starred_groups.end(),
                [&](const auto& entry) { return entry.id == id; });
            if (known != snapshot.starred_groups.end() && !known->name.empty()) row.text = known->name;
        } else {
            const auto known = std::find_if(snapshot.workspaces.begin(), snapshot.workspaces.end(),
                [&](const auto& entry) { return SamePickerPath(entry.root, hidden_paths[i]); });
            if (known != snapshot.workspaces.end() && !known->name.empty()) row.text = known->name;
        }
        restore.children.push_back(std::move(row));
    }
    if (!restore.children.empty()) restore.children.back().separator_after = true;
    restore.children.push_back(item(2, Id::PickerRestoreAllPlaces, !hidden_paths.empty()));
    restore.children.back().mnemonic = L'A';
    rows.push_back(std::move(restore));
    if (path.empty()) return rows;
    rows.back().separator_after = true;
    if (group_path) {
        const bool editable = mutations_available && group != snapshot.starred_groups.end();
        rows.push_back(item(12, Id::FavoritesGroupRename, editable));
        rows.push_back(item(13, Id::FavoritesGroupUngroup, editable));
        return rows;
    }
    rows.push_back(item(3, Id::Open));
    rows.push_back(item(4, Id::CopyPath));
    rows.back().separator_after = true;
    const bool starred = favorite != snapshot.starred_items.end();
    rows.push_back(item(starred ? 5 : 6, starred ? Id::Unstar : Id::Star, mutations_available));
    const bool folder = starred ? favorite->kind == 2 : is_folder;
    if (starred && folder) {
        auto move = item(0, Id::FavoritesGroupMoveTo, mutations_available);
        for (size_t i = 0; i < std::min<size_t>(64, snapshot.starred_groups.size()); ++i) {
            FluentMenuItem target;
            target.command = 100 + static_cast<int>(i);
            target.text = snapshot.starred_groups[i].name;
            target.enabled = mutations_available && !snapshot.starred_groups[i].id.empty() &&
                snapshot.starred_groups[i].id != favorite->group;
            target.checked = snapshot.starred_groups[i].id == favorite->group;
            move.children.push_back(std::move(target));
        }
        if (!move.children.empty()) rows.push_back(std::move(move));
        rows.push_back(item(14, Id::FavoritesGroupNew, mutations_available));
        if (!favorite->group.empty()) rows.push_back(item(15, Id::FavoritesGroupLeave, mutations_available));
    }
    if (workspace != snapshot.workspaces.end()) {
        rows.back().separator_after = true;
        rows.push_back(item(8, Id::UnpinWorkspace, mutations_available));
        rows.push_back(item(9, Id::WorkspaceRenameTitle, mutations_available));
        rows.push_back(item(10, Id::WorkspaceMoveUp, mutations_available && workspace != snapshot.workspaces.begin()));
        rows.push_back(item(11, Id::WorkspaceMoveDown, mutations_available && workspace + 1 != snapshot.workspaces.end()));
    } else if (folder) {
        rows.push_back(item(7, Id::PinWorkspace, mutations_available));
    }
    if (folder && !host_app_name.empty()) {
        std::wstring text = l10n::Get(pinned_to_app ? Id::PickerUnpinFromApp : Id::PickerPinToApp);
        if (const auto pos = text.find(L"{name}"); pos != std::wstring::npos) text.replace(pos, 6, host_app_name);
        FluentMenuItem pin_row;
        pin_row.command = pinned_to_app ? 17 : 16;
        pin_row.text = std::move(text);
        pin_row.glyph = pinned_to_app ? L"\xE77A" : L"\xE718";
        rows.push_back(std::move(pin_row));
    }
    if (removable_common) {
        auto remove = item(18, Id::PickerCommonRemove);
        remove.glyph = L"\xE711";
        rows.push_back(std::move(remove));
    }
    return rows;
}

} // namespace pulse::ui
