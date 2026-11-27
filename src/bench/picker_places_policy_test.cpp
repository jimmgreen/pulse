#include "../ui/picker_places_ui.h"
#include "../ui/picker_places_menu.h"
#include <cstdio>

int main() {
    using namespace pulse::ui;
    namespace places = pulse::picker_places;
    int failures = 0;
    auto check = [&](bool okay, const char* label) {
        std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", label);
        if (!okay) ++failures;
    };
    places::Snapshot data;
    data.shared_available = true;
    data.starred_groups = {{L"empty-first", L"Empty first"}, {L"second", L"Second"},
        {L"first", L"First"}, {L"empty-last", L"Empty last"}};
    data.starred_items = {{L"C:\\top.txt", 1, {}}, {L"C:\\one", 2, L"first"},
        {L"C:\\two", 2, L"second"}, {L"C:\\three", 2, L"first"}};
    data.networks = {{LR"(\\server\share)", L"NAS"}, {LR"(\\SERVER\share)", L"duplicate"}, {L"relative", L"bad"}};
    WindowViewModel networks;
    AppendPickerNetworkPlaces(networks, data, true, [](const auto& path) { return path; });
    check(networks.sidebar.size() == 1 && networks.sidebar[0].id == kPickerNetworksSection &&
        networks.sidebar[0].collapsed && networks.sidebar[0].items.size() == 1 &&
        networks.sidebar[0].items[0].path == LR"(\\server\share)" && networks.sidebar[0].items[0].label == L"NAS",
        "network sidebar preserves navigable UNC label, collapse, and deduplicates paths");
    WindowViewModel vm;
    AppendPickerSharedPlaces(vm, data, false, false, {}, [](const std::wstring& path) { return path; });
    const std::vector<std::wstring> expected{L"C:\\top.txt", std::wstring(kPickerGroupPrefix) + L"first",
        L"C:\\one", L"C:\\three", std::wstring(kPickerGroupPrefix) + L"second", L"C:\\two",
        std::wstring(kPickerGroupPrefix) + L"empty-first", std::wstring(kPickerGroupPrefix) + L"empty-last"};
    std::vector<std::wstring> actual;
    for (const auto& item : vm.sidebar[0].items) actual.push_back(item.path);
    check(actual == expected, "empty groups append in catalog order without moving occupied groups or members");
    check(vm.sidebar[0].items[6].expandable && vm.sidebar[0].items[6].expanded,
        "empty group has an expandable drop target heading");
    WindowViewModel folded;
    AppendPickerSharedPlaces(folded, data, false, false, {L"first", L"empty-first"},
        [](const std::wstring& path) { return path; });
    check(folded.sidebar[0].items.size() == 6 && !folded.sidebar[0].items[1].expanded &&
        !folded.sidebar[0].items[4].expanded, "folding keeps occupied and empty group headings");
    const std::vector<places::StarredItem> file{{L"C:\\source.txt", 1, {}}};
    const std::vector<places::StarredItem> folder{{L"C:\\source", 2, {}}};
    HitTestResult hit;
    hit.region = HitTestResult::SidebarItem;
    hit.path = L"C:\\top.txt";
    check(ResolvePickerPlacesDrop(vm, data, hit, file, 0).effect == DROPEFFECT_NONE,
        "a starred file cannot receive a file drop");
    hit.path = L"C:\\source\\child";
    check(ResolvePickerPlacesDrop(vm, data, hit, folder, MK_CONTROL).effect == DROPEFFECT_NONE,
        "folder cannot be copied into its own descendant");
    hit.path = L"C:\\source";
    check(ResolvePickerPlacesDrop(vm, data, hit, folder, 0).effect == DROPEFFECT_NONE,
        "folder cannot be moved into itself");
    hit.path = std::wstring(kPickerGroupPrefix) + L"empty-first";
    check(ResolvePickerPlacesDrop(vm, data, hit, file, 0).effect == DROPEFFECT_NONE,
        "favorite group rejects files");
    auto drop = ResolvePickerPlacesDrop(vm, data, hit, folder, 0);
    check(drop.favorite && drop.group == L"empty-first" && drop.effect == DROPEFFECT_LINK,
        "empty favorite group accepts folder references");
    auto mixed = folder; mixed.push_back(file[0]);
    check(ResolvePickerPlacesDrop(vm, data, hit, mixed, 0).effect == DROPEFFECT_NONE,
        "favorite group rejects a mixed file and folder selection");
    data.shared_available = false;
    check(ResolvePickerPlacesDrop(vm, data, hit, folder, 0).effect == DROPEFFECT_NONE,
        "offline favorite group rejects writes");
    hit.region = HitTestResult::SidebarHeader; hit.index = 0; hit.path.clear();
    check(ResolvePickerPlacesDrop(vm, data, hit, file, 0).effect == DROPEFFECT_NONE,
        "offline favorite section rejects writes");
    data.shared_available = true;
    check(ResolvePickerPlacesDrop(vm, data, hit, file, 0).effect == DROPEFFECT_LINK,
        "online favorite section accepts file references");
    hit.region = HitTestResult::SidebarItem; hit.path = L"C:\\destination";
    data.shared_available = false;
    check(ResolvePickerPlacesDrop(vm, data, hit, file, MK_CONTROL).effect == DROPEFFECT_COPY,
        "offline workspace still permits a filesystem copy");
    const auto contains = [](const WindowViewModel& model, const std::wstring& path) {
        for (const auto& section : model.sidebar)
            for (const auto& row : section.items) if (SamePickerPath(row.path, path)) return true;
        return false;
    };
    const auto first_group = std::wstring(kPickerGroupPrefix) + L"first";
    auto hidden_group = vm;
    FilterPickerHiddenPlaces(hidden_group, {first_group});
    check(!contains(hidden_group, first_group) && !contains(hidden_group, L"C:\\one") &&
        !contains(hidden_group, L"C:\\three") && contains(hidden_group, L"C:\\two"),
        "hidden group removes its members without hiding another group");
    std::vector<std::wstring> hidden{L"C:\\one", L"C:\\two"};
    hidden.erase(hidden.begin());
    auto restored = vm;
    FilterPickerHiddenPlaces(restored, hidden);
    check(contains(restored, L"C:\\one") && !contains(restored, L"C:\\two"),
        "restoring one hidden path leaves the other hidden");
    const auto find = [](const std::vector<FluentMenuItem>& rows, int command) -> const FluentMenuItem* {
        for (const auto& row : rows) {
            if (row.command == command) return &row;
            for (const auto& child : row.children) if (child.command == command) return &child;
        }
        return nullptr;
    };
    const auto file_menu = BuildPickerPlacesMenu(data, L"C:\\top.txt", {}, true);
    check(!find(file_menu, 7) && find(file_menu, 5) && find(file_menu, 3) && find(file_menu, 4),
        "starred file exposes open copy and unstar but no workspace pin");
    const auto plain_file = BuildPickerPlacesMenu(data, L"C:\\plain.txt", {}, true, false);
    check(find(plain_file, 6) && !find(plain_file, 7), "ordinary file can be starred but not pinned as workspace");
    const auto folder_menu = BuildPickerPlacesMenu(data, L"C:\\one", {}, true);
    check(find(folder_menu, 5) && find(folder_menu, 7) && find(folder_menu, 14) && find(folder_menu, 15) &&
        find(folder_menu, 100) && find(folder_menu, 103) && !find(folder_menu, 102)->enabled,
        "folder menu preserves group protocol indices and disables its current group");
    const auto group_menu = BuildPickerPlacesMenu(data, first_group, {}, true);
    check(find(group_menu, 12) && find(group_menu, 13) && !find(group_menu, 3) && !find(group_menu, 4),
        "virtual group has rename and ungroup without filesystem commands");
    data.workspaces = {{L"C:\\one", L"First workspace"}, {L"C:\\two", L"Second workspace"}};
    const auto first_workspace = BuildPickerPlacesMenu(data, L"C:\\one", {}, true);
    const auto last_workspace = BuildPickerPlacesMenu(data, L"C:\\two", {}, true);
    check(find(first_workspace, 8) && find(first_workspace, 9) && !find(first_workspace, 10)->enabled &&
        find(first_workspace, 11)->enabled && find(last_workspace, 10)->enabled && !find(last_workspace, 11)->enabled,
        "workspace commands preserve IDs and disable first/last movement boundaries");
    const std::vector<std::wstring> restore_paths{L"C:\\a\\same", L"D:\\b\\same", first_group, L"C:\\one"};
    const auto restore_menu = BuildPickerPlacesMenu(data, L"", restore_paths, false);
    check(restore_menu.size() == 1 && restore_menu[0].mnemonic == L'R' &&
        restore_menu[0].children.front().command == 200 && restore_menu[0].children.back().command == 2 &&
        restore_menu[0].children.back().mnemonic == L'A', "restore flyout starts with individual path and retains restore-all mnemonic");
    check(find(restore_menu, 200)->text == restore_paths[0] && find(restore_menu, 201)->text == restore_paths[1] &&
        find(restore_menu, 202)->text == L"First" && find(restore_menu, 203)->text == L"First workspace" &&
        find(restore_menu, 203)->tooltip == L"C:\\one", "restore labels disambiguate paths and use known group/workspace names");
    const auto unavailable = BuildPickerPlacesMenu(data, L"C:\\one", {}, false);
    check(unavailable.front().command == 1 && unavailable.front().mnemonic == L'H' &&
        find(unavailable, 3)->enabled && !find(unavailable, 5)->enabled && !find(unavailable, 8)->enabled,
        "offline menu keeps local navigation and hide while disabling shared writes");
    std::vector<std::wstring> many_hidden(65, L"C:\\hidden");
    data.starred_groups.resize(65);
    const auto bounded = BuildPickerPlacesMenu(data, L"C:\\one", many_hidden, true);
    check(find(bounded, 163) && !find(bounded, 164) && find(bounded, 263) && !find(bounded, 264) &&
        !find(bounded, 1)->enabled, "group and restore protocol ranges cap at 64 and hide respects its limit");
    // 常用: folders dropped on it pin to the program; its rows are removed, not hidden.
    {
        places::Snapshot online; online.shared_available = true;
        WindowViewModel common;
        SidebarGroup group; group.id = kPickerAppCommonSection; group.header = L"Weixin 常用";
        group.add_action = SidebarAddAction::PinToApp;
        SidebarItem row; row.path = L"C:\\print"; group.items.push_back(row);
        common.sidebar.push_back(group);
        HitTestResult header; header.region = HitTestResult::SidebarHeader; header.index = 0;
        auto pin = ResolvePickerPlacesDrop(common, online, header, folder, 0);
        check(pin.pin && pin.effect == DROPEFFECT_LINK, "folders dropped on the 常用 title pin to the program");
        check(ResolvePickerPlacesDrop(common, online, header, file, 0).effect == DROPEFFECT_NONE,
            "a file cannot be pinned as a program place");
        HitTestResult on_row; on_row.region = HitTestResult::SidebarItem; on_row.index = 0;
        on_row.sidebar_section = kPickerAppCommonSection; on_row.path = L"C:\\print";
        check(ResolvePickerPlacesDrop(common, online, on_row, folder, 0).pin, "a 常用 row is a pin target too");
        common.sidebar[0].add_action = SidebarAddAction::None;   // no program name: nothing to pin to
        check(!ResolvePickerPlacesDrop(common, online, header, folder, 0).pin, "without a program there is no pin target");
        const auto removable = BuildPickerPlacesMenu(online, L"C:\\print", {}, true, true, L"Weixin", false, true);
        check(find(removable, 18) && !find(removable, 1), "a 常用 row offers remove and no hide");
        const auto fixed = BuildPickerPlacesMenu(online, L"C:\\Users", {}, true, true, L"Weixin", false, false);
        check(!find(fixed, 18) && find(fixed, 1), "a fixed place keeps hide and has no remove");
    }
    // Favorites in the redesigned sidebar: workspaces first, what 常用 shows is skipped,
    // members of a favorite group stay; Pulse's quick-access pins stay out (此电脑 has them).
    {
        places::Snapshot fav;
        fav.workspaces = {{L"D:\\brand", L"Brand"}};
        fav.starred_groups = {{L"g", L"Finance"}};
        fav.starred_items = {{L"D:\\bills", 2, {}}, {L"D:\\design", 2, {}}, {L"D:\\bills\\2026", 2, L"g"}};
        fav.folders = {{L"D:\\pinned", L"", false, true}, {L"D:\\design", L"", false, true}, {L"D:\\recent", L"", false, false}};
        WindowViewModel merged;
        AppendPickerFavoritesMerged(merged, fav, false, {}, [](const std::wstring& path) { return path; },
            [](const std::wstring& path) { return SamePickerPath(path, L"D:\\bills"); });
        std::vector<std::wstring> paths;
        for (const auto& item : merged.sidebar[0].items) paths.push_back(item.path);
        const std::vector<std::wstring> want{L"D:\\brand", L"D:\\design", std::wstring(kPickerGroupPrefix) + L"g",
            L"D:\\bills\\2026"};
        check(paths == want, "favorites: workspace, starred (minus 常用), group with members, no quick-access pins");
        check(merged.sidebar[0].id == kPickerFavoritesSection, "the merged group keeps the favorites id (drop target, collapse bit)");
    }
    check(IsPickerFolderName(L"New folder") && !IsPickerFolderName(L"CON.txt") &&
        !IsPickerFolderName(L"../outside") && !IsPickerFolderName(L"trailing."),
        "new folder validation rejects reserved names and escaping paths");
    check(IsPickerDiskPath(L"C:\\" + std::wstring(1500, L'a')) &&
        IsPickerDiskPath(L"\\\\?\\UNC\\server\\share\\file") &&
        !IsPickerDiskPath(L"\\\\.\\PhysicalDrive0") && !IsPickerDiskPath(L"relative\\file") &&
        !IsPickerDiskPath(L"C:\\bad\npath"),
        "filesystem drag accepts long and UNC paths without accepting device or relative paths");
    return failures ? 1 : 0;
}
