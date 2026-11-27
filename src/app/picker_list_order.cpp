#include "picker_list_order.h"
#include "app_prefs.h"
#include "entry_sort.h"
#include "../common/json_utils.h"
#include "../common/localization.h"
#include "../common/path_utils.h"

#include <algorithm>

namespace pulse::app {

ui::PickerListOrder PickerListOrderPrefs::Resolve(const std::wstring& folder) const {
    ui::PickerListOrder order;
    const FolderSort sort = sorts.Find(folder).value_or(sorts.Default());
    order.column = static_cast<int>(sort.column);
    order.descending = sort.direction == ui::SortDirection::Desc;
    order.folder_mode = sorts.FindFolderMode(folder).value_or(folder_sort_mode);
    if (order.folder_mode < 0 || order.folder_mode > 2) order.folder_mode = 0;
    order.group_by = PickerSupportedGroup(static_cast<int>(groups.Resolve(folder)));
    order.tags = tags;
    if (order.group_by == static_cast<int>(GroupBy::Tag) && !order.tags) order.group_by = 0;
    return order;
}

std::shared_ptr<const PickerListOrderPrefs> PickerListOrderPrefsFromPrefs(const AppPrefs& prefs) {
    auto order = std::make_shared<PickerListOrderPrefs>();
    order->sorts = prefs.folder_sorts;
    order->groups = prefs.folder_groups;
    order->folder_sort_mode = prefs.folder_sort_mode >= 0 && prefs.folder_sort_mode <= 2 ? prefs.folder_sort_mode : 0;
    order->tags = CurrentTagCatalog();
    return order;
}

std::shared_ptr<const PickerListOrderPrefs> ParsePickerListOrderPrefs(const std::wstring& json) {
    auto prefs = std::make_shared<PickerListOrderPrefs>();
    prefs->sorts.ReadJson(json);
    prefs->groups.ReadJson(json);
    prefs->folder_sort_mode = pulse::json::ExtractInt(json, L"folder_sort_mode", 0);
    if (prefs->folder_sort_mode < 0 || prefs->folder_sort_mode > 2) prefs->folder_sort_mode = 0;
    return prefs;
}

int PickerSupportedGroup(int group_by) noexcept {
    switch (GroupByFromInt(group_by)) {
    case GroupBy::Name: case GroupBy::Date: case GroupBy::Type: case GroupBy::Size:
    case GroupBy::Tag: case GroupBy::Location:
        return group_by;
    default:
        return 0;
    }
}

void OrderPickerEntries(std::vector<ui::PickerEntry>& entries, const std::vector<fs::DirEntry>& rows,
                        const std::wstring& folder, const ui::PickerListOrder& order) {
    const auto column = static_cast<ui::SortColumn>(order.column);
    const auto direction = order.descending ? ui::SortDirection::Desc : ui::SortDirection::Asc;
    ScopedFolderSortMode folders(FolderSortModeFromInt(order.folder_mode));
    EntryGrouping context;
    context.by = GroupByFromInt(PickerSupportedGroup(order.group_by));
    if (context.by == GroupBy::Date) context.clock = MakeGroupClock(column);
    if (context.by == GroupBy::Tag) context.clock.tags = TagGroupsForFolder(folder, order.tags);
    ScopedEntryGrouping grouping(std::move(context));
    const auto row = [&](const ui::PickerEntry& entry) -> const fs::DirEntry* {
        return entry.source < rows.size() ? &rows[entry.source] : nullptr;
    };
    std::stable_sort(entries.begin(), entries.end(), [&](const ui::PickerEntry& a, const ui::PickerEntry& b) {
        const fs::DirEntry* ra = row(a);
        const fs::DirEntry* rb = row(b);
        if (!ra || !rb) return ra != nullptr && rb == nullptr;
        return EntryLess(*ra, *rb, column, direction);
    });
}

std::shared_ptr<const ui::ListGroups> BuildPickerListGroups(const std::vector<fs::DirEntry>& rows,
                                                            const ui::PickerListOrder& order,
                                                            const std::set<std::wstring>& collapsed,
                                                            const std::wstring& folder) {
    const int group_by = PickerSupportedGroup(order.group_by);
    if (!group_by || rows.empty()) return nullptr;
    const GroupBy by = GroupByFromInt(group_by);
    const auto column = static_cast<ui::SortColumn>(order.column);
    const auto direction = order.descending ? ui::SortDirection::Desc : ui::SortDirection::Asc;
    GroupClock clock = by == GroupBy::Date ? MakeGroupClock(column) : GroupClock{};
    clock.folders = FolderSortModeFromInt(order.folder_mode);
    if (by == GroupBy::Tag) clock.tags = TagGroupsForFolder(folder, order.tags);
    auto groups = std::make_shared<ui::ListGroups>();
    std::set<std::wstring> seen;
    int prev_rank = 0;
    for (size_t index = 0; index < rows.size(); ++index) {
        const fs::DirEntry& entry = rows[index];
        const int rank = GroupRank(entry, by, clock);
        if (groups->empty() ||
            GroupCompareRanks(rows[index - 1], entry, by, column, direction, prev_rank, rank, clock) != 0) {
            ui::ListGroup group;
            group.first = static_cast<int>(index);
            group.rank = rank;
            group.sample = static_cast<int>(index);
            group.key = GroupKey(entry, by, clock);
            group.collapsed = collapsed.contains(group.key);
            if (by == GroupBy::Tag && order.tags && rank >= 0 &&
                static_cast<size_t>(rank) < order.tags->tags.size()) {
                group.label = order.tags->tags[static_cast<size_t>(rank)].name;
                group.color_rgb = order.tags->tags[static_cast<size_t>(rank)].rgb;
            }
            if (by == GroupBy::Location) {
                group.label = GroupLocationLabel(entry);
                if (group.label.empty()) group.label = path::StripExtendedPathPrefix(folder);
            }
            if (rank == kFolderGroupRank) group.label = l10n::Get(l10n::StringId::GroupFolders);
            // A key seen twice: the rows are not in grouped order; draw flat.
            if (!seen.insert(group.key).second) return nullptr;
            groups->push_back(std::move(group));
        }
        ui::ListGroup& group = groups->back();
        ++group.count;
        if (!entry.is_dir) group.bytes += entry.size;
        prev_rank = rank;
    }
    return groups;
}

} // namespace pulse::app
