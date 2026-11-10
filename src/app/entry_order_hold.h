#pragma once
#include "../fs/fs_enum.h"
#include "../ui/ui_view_model.h"
#include <string>
#include <vector>

namespace pulse::app {

// Renamed rows retain their displayed position within their group. New rows
// and metadata updates that change the active sort key use the comparator.

// True when both entries have equivalent active sorting and grouping keys.
bool SameEntrySortPosition(const fs::DirEntry& a, const fs::DirEntry& b,
                           ui::SortColumn col, ui::SortDirection dir);

// True when `a` and `b` fall in the same group (always true when ungrouped).
bool SameEntryGroup(const fs::DirEntry& a, const fs::DirEntry& b,
                    ui::SortColumn col, ui::SortDirection dir);

// True when a's group is shown before b's (never when ungrouped).
bool GroupBefore(const fs::DirEntry& a, const fs::DirEntry& b,
                 ui::SortColumn col, ui::SortDirection dir);

// Inserts `entry` before the first row that sorts after it (EntryLess, so
// grouping included): its sort position. The name predates that.
void InsertAtGroupEnd(std::vector<fs::DirEntry>& entries, fs::DirEntry entry,
                      ui::SortColumn col, ui::SortDirection dir);

// Puts `entry` in row `slot` when that row is in the same group, otherwise
// drops the row (if any) and inserts `entry` at the end of its group.
void PlaceEntryHeld(std::vector<fs::DirEntry>& entries, int slot, fs::DirEntry entry,
                    ui::SortColumn col, ui::SortDirection dir);

// A rename Pulse issued and whose refresh may arrive before the watcher event.
struct EntryRename {
    std::wstring old_name;
    std::wstring new_name;
};

// Merges a freshly enumerated listing (`fresh`, in sort order) into the order
// on screen (`shown`): rows with unchanged sort keys keep their place with fresh
// metadata, a renamed row (`renames`) keeps its old row, removed rows drop
// out, and new rows merge in at their sort position.
std::vector<fs::DirEntry> KeepEntryOrder(const std::vector<fs::DirEntry>& shown,
                                         const std::vector<fs::DirEntry>& fresh,
                                         const std::vector<EntryRename>& renames,
                                         ui::SortColumn col, ui::SortDirection dir);

// True when `fresh` lists the rows shown in the same order and groups, so
// KeepEntryOrder would return `fresh` unchanged. A rescan of an unchanged
// folder takes this path instead of rebuilding the listing.
bool SameEntryOrder(const std::vector<fs::DirEntry>& shown,
                    const std::vector<fs::DirEntry>& fresh,
                    ui::SortColumn col, ui::SortDirection dir);

// Points selection names at their new names when `fresh` shows the rename
// landed (the old name is gone, the new one present), so a refresh that
// captured the selection before the rename keeps the renamed item selected.
void FollowHeldRenames(const std::vector<EntryRename>& renames,
                       const std::vector<fs::DirEntry>& fresh,
                       std::vector<std::wstring>& names, std::wstring& focus);

// Drops the renames `fresh` already reflects (or that failed and left the old
// name gone); the rest still wait for their refresh. Keeps the newest few.
void PruneHeldRenames(std::vector<EntryRename>& renames, const std::vector<fs::DirEntry>& fresh);

} // namespace pulse::app
