#pragma once
#include "../common/picker_places_snapshot.h"
#include "../common/picker_tray_consume.h"
#include "../common/localization.h"
#include "folder_picker_model.h"
#include "ui_view_model.h"
#include "drag_drop.h"
#include <algorithm>

namespace pulse::ui {
inline constexpr int kPickerAppCommonSection = 2;   // 「{程序} 常用」
inline constexpr int kPickerFavoritesSection = 7;
inline constexpr int kPickerWorkspacesSection = 8;
inline constexpr int kPickerNetworksSection = 9;
inline constexpr int kPickerTraySection = 50;
inline constexpr wchar_t kPickerGroupPrefix[] = L"pulse:star-group:";
inline constexpr wchar_t kPickerTrayTokenPrefix[] = L"pulse-picker-tray:";

inline void FilterPickerHiddenPlaces(WindowViewModel& vm, const std::vector<std::wstring>& hidden) {
    for (auto& group : vm.sidebar) {
        int hidden_indent = -1;
        std::erase_if(group.items, [&](const SidebarItem& item) {
            if (hidden_indent >= 0 && item.indent > hidden_indent) return true;
            hidden_indent = -1;
            if (std::any_of(hidden.begin(), hidden.end(), [&](const auto& path) { return SamePickerPath(path, item.path); })) {
                hidden_indent = item.indent;
                return true;
            }
            return false;
        });
    }
}

template<class LeafName>
inline void AppendPickerSharedPlaces(WindowViewModel& vm, const picker_places::Snapshot& data,
    bool favorites_collapsed, bool workspaces_collapsed, const std::unordered_set<std::wstring>& folded,
    LeafName leaf_name) {
    SidebarGroup favorites;
    favorites.id = kPickerFavoritesSection;
    favorites.header = l10n::Get(l10n::StringId::StarredItems);
    favorites.collapsed = favorites_collapsed;
    const auto append = [&](const picker_places::StarredItem& entry, int indent) {
        SidebarItem item;
        item.path = entry.path;
        item.label = leaf_name(entry.path);
        item.icon_glyph = entry.kind == 2 ? L"\xE8B7" : L"\xE8A5";
        item.indent = indent;
        item.starred_child = true;
        favorites.items.push_back(std::move(item));
    };
    std::unordered_set<std::wstring> emitted;
    const auto append_group = [&](const picker_places::StarredGroup& group) {
        if (group.id.empty() || !emitted.insert(group.id).second) return;
        SidebarItem heading;
        heading.path = std::wstring(kPickerGroupPrefix) + group.id;
        heading.label = group.name;
        heading.icon_glyph = L"\xE8B7";
        heading.expandable = true;
        heading.expanded = !folded.contains(group.id);
        favorites.items.push_back(std::move(heading));
        if (!folded.contains(group.id))
            for (const auto& member : data.starred_items) if (member.group == group.id) append(member, 1);
    };
    for (const auto& entry : data.starred_items) {
        const auto group = std::find_if(data.starred_groups.begin(), data.starred_groups.end(),
            [&](const auto& candidate) { return candidate.id == entry.group; });
        if (entry.group.empty() || group == data.starred_groups.end()) { append(entry, 0); continue; }
        append_group(*group);
    }
    // Keep occupied groups in their existing first-member position; expose empty
    // groups afterwards, in catalog order, so they remain valid drop targets.
    for (const auto& group : data.starred_groups) append_group(group);
    vm.sidebar.push_back(std::move(favorites));
    SidebarGroup workspaces;
    workspaces.id = kPickerWorkspacesSection;
    workspaces.header = l10n::Get(l10n::StringId::SidebarWorkspaces);
    workspaces.collapsed = workspaces_collapsed;
    for (const auto& workspace : data.workspaces) {
        SidebarItem item;
        item.path = workspace.root;
        item.label = workspace.name.empty() ? leaf_name(workspace.root) : workspace.name;
        item.icon_glyph = L"\xE8F1";
        workspaces.items.push_back(std::move(item));
    }
    vm.sidebar.push_back(std::move(workspaces));
}

// Favorites for the redesigned sidebar: workspaces first, then the starred items; what
// 常用 already shows is skipped (grouped members stay). Pulse's quick-access pins are
// 此电脑's everyday folders, not favorites.
template<class LeafName, class Skip>
inline void AppendPickerFavoritesMerged(WindowViewModel& vm, const picker_places::Snapshot& data,
    bool collapsed, const std::unordered_set<std::wstring>& folded, LeafName leaf_name, Skip skip) {
    SidebarGroup favorites;
    favorites.id = kPickerFavoritesSection;
    favorites.header = l10n::Get(l10n::StringId::StarredItems);
    favorites.rail_header = l10n::Get(l10n::StringId::PickerRailFavorites);
    favorites.collapsed = collapsed;
    const auto shown = [&](const std::wstring& path) {
        return std::any_of(favorites.items.begin(), favorites.items.end(), [&](const auto& item) { return SamePickerPath(item.path, path); });
    };
    for (const auto& workspace : data.workspaces) {
        if (workspace.root.empty() || skip(workspace.root) || shown(workspace.root)) continue;
        SidebarItem item;
        item.path = workspace.root;
        item.label = workspace.name.empty() ? leaf_name(workspace.root) : workspace.name;
        item.icon_glyph = L"\xE8F1";
        favorites.items.push_back(std::move(item));
    }
    const auto append = [&](const picker_places::StarredItem& entry, int indent) {
        SidebarItem item;
        item.path = entry.path;
        item.label = leaf_name(entry.path);
        item.icon_glyph = entry.kind == 2 ? L"\xE8B7" : L"\xE8A5";
        item.indent = indent;
        item.starred_child = true;
        favorites.items.push_back(std::move(item));
    };
    std::unordered_set<std::wstring> emitted;
    const auto append_group = [&](const picker_places::StarredGroup& group) {
        if (group.id.empty() || !emitted.insert(group.id).second) return;
        SidebarItem heading;
        heading.path = std::wstring(kPickerGroupPrefix) + group.id;
        heading.label = group.name;
        heading.icon_glyph = L"\xE8B7";
        heading.expandable = true;
        heading.expanded = !folded.contains(group.id);
        favorites.items.push_back(std::move(heading));
        if (!folded.contains(group.id))
            for (const auto& member : data.starred_items) if (member.group == group.id) append(member, 1);
    };
    for (const auto& entry : data.starred_items) {
        const auto group = std::find_if(data.starred_groups.begin(), data.starred_groups.end(),
            [&](const auto& candidate) { return candidate.id == entry.group; });
        if (entry.group.empty() || group == data.starred_groups.end()) {
            if (!skip(entry.path) && !shown(entry.path)) append(entry, 0);
            continue;
        }
        append_group(*group);
    }
    for (const auto& group : data.starred_groups) append_group(group);
    vm.sidebar.push_back(std::move(favorites));
}

template<class LeafName>
inline void AppendPickerNetworkPlaces(WindowViewModel& vm, const picker_places::Snapshot& data,
    bool collapsed, LeafName leaf_name) {
    SidebarGroup network;
    network.id = kPickerNetworksSection;
    network.header = l10n::Get(l10n::StringId::SidebarNetworkLocations);
    network.collapsed = collapsed;
    for (const auto& entry : data.networks) {
        if (!picker_places::detail::DiskPath(entry.path) ||
            std::any_of(network.items.begin(), network.items.end(), [&](const auto& item) {
                return SamePickerPath(item.path, entry.path);
            })) continue;
        SidebarItem item;
        item.path = entry.path;
        item.label = entry.label.empty() ? leaf_name(entry.path) : entry.label;
        item.icon_glyph = L"\xE968";
        network.items.push_back(std::move(item));
    }
    vm.sidebar.push_back(std::move(network));
}

struct PickerPlacesDrop {
    bool favorite = false;
    bool tray = false;
    bool pin = false;   // folders dropped on 「{程序} 常用」: pin them to the program
    DWORD effect = DROPEFFECT_NONE;
    std::wstring destination, group;
};

struct PickerPlacesDrag {
    bool pending = false, active = false, deferred_click = false, ctrl = false, shift = false;
    int row = -1;
    POINT start{};
    std::wstring filename_before;
    bool filename_from_selection = false;
    std::vector<picker_places::StarredItem> items;
    PickerPlacesDrop target;
};

inline PickerPlacesDrop ResolvePickerPlacesDrop(const WindowViewModel& vm,
    const picker_places::Snapshot& data, const HitTestResult& hit,
    const std::vector<picker_places::StarredItem>& items, DWORD keys) {
    PickerPlacesDrop result;
    if (items.empty()) return result;
    const bool header = hit.region == HitTestResult::SidebarHeader;
    const bool item = hit.region == HitTestResult::SidebarItem || hit.region == HitTestResult::SidebarItemExpand;
    const bool tray_header = header && hit.index >= 0 && hit.index < static_cast<int>(vm.sidebar.size()) &&
        vm.sidebar[hit.index].id == kPickerTraySection;
    const bool tray_item = item && (hit.sidebar_section == kPickerTraySection || hit.path.starts_with(kPickerTrayTokenPrefix));
    if (tray_header || tray_item) {
        result.tray = true;
        if (!data.shared_available || items.size() > picker_places::kMaxStageTrayItems) return result;
        result.effect = DROPEFFECT_LINK;
        return result;
    }
    const bool common_header = header && hit.index >= 0 && hit.index < static_cast<int>(vm.sidebar.size()) &&
        vm.sidebar[hit.index].id == kPickerAppCommonSection &&
        vm.sidebar[hit.index].add_action == SidebarAddAction::PinToApp;
    const bool common_item = item && hit.sidebar_section == kPickerAppCommonSection && std::any_of(vm.sidebar.begin(), vm.sidebar.end(),
        [](const SidebarGroup& g) { return g.id == kPickerAppCommonSection && g.add_action == SidebarAddAction::PinToApp; });
    if (common_header || common_item) {
        result.pin = true;
        if (items.size() > 64 || std::any_of(items.begin(), items.end(), [](const auto& entry) { return entry.kind != 2; })) return result;
        result.effect = DROPEFFECT_LINK;
        return result;
    }
    const bool favorite_header = header && hit.index >= 0 && hit.index < static_cast<int>(vm.sidebar.size()) &&
        vm.sidebar[hit.index].id == kPickerFavoritesSection;
    const bool favorite_group = item && hit.path.starts_with(kPickerGroupPrefix);
    if (favorite_header || favorite_group) {
        result.favorite = true;
        if (!data.shared_available || items.size() > 64) return result;
        if (favorite_group) {
            result.group = hit.path.substr(std::size(kPickerGroupPrefix) - 1);
            if (std::none_of(data.starred_groups.begin(), data.starred_groups.end(),
                [&](const auto& group) { return group.id == result.group; })) return {};
            if (std::any_of(items.begin(), items.end(), [](const auto& entry) { return entry.kind != 2; })) return {};
        }
        result.effect = DROPEFFECT_LINK;
        return result;
    }
    if (!item || !IsPickerDiskPath(hit.path)) return result;
    // A starred file is a selection shortcut, never a destination folder.
    for (const auto& favorite : data.starred_items)
        if (SamePickerPath(favorite.path, hit.path) && favorite.kind != 2) return result;
    std::vector<std::wstring> sources;
    for (const auto& source : items) sources.push_back(source.path);
    if (DropIntoOwnFolder(sources, hit.path)) return result;
    result.destination = hit.path;
    result.effect = ComputeDropEffect(keys, sources.front(), result.destination, DROPEFFECT_COPY | DROPEFFECT_MOVE);
    return result;
}
} // namespace pulse::ui
