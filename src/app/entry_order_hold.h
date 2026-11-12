#pragma once
#include "../fs/fs_enum.h"
#include "../fs/fs_watch.h"
#include "../ui/ui_view_model.h"
#include <string>
#include <vector>

namespace pulse::app {

// A modified row (same name, new size or date) retains its displayed position
// within its group until F5, a sort change, or reopening the folder, and so
// does a file saved through a temp file or a backup rename. New and renamed
// rows, and rows whose group changes, take their sort position.

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
// on screen (`shown`): existing rows in the same group keep their place with
// fresh metadata, removed rows drop out, and new rows (renamed ones included,
// or rows whose group changed) merge in at their sort position.
std::vector<fs::DirEntry> KeepEntryOrder(const std::vector<fs::DirEntry>& shown,
                                         const std::vector<fs::DirEntry>& fresh,
                                         ui::SortColumn col, ui::SortDirection dir);

// True when `fresh` lists the rows shown in the same order and groups, so
// KeepEntryOrder would return `fresh` unchanged. A rescan of an unchanged
// folder takes this path instead of rebuilding the listing.
bool SameEntryOrder(const std::vector<fs::DirEntry>& shown,
                    const std::vector<fs::DirEntry>& fresh,
                    ui::SortColumn col, ui::SortDirection dir);

// Updates selection names across a directory notification batch: names still
// present in `after` (including atomic replace saves and case-only renames)
// stay on that row; names no longer present follow FILE_ACTION_RENAMED_NEW_NAME
// chains in `events`.
void FollowNotifyRenames(const std::vector<fs::DirNotifyEvent>& events,
                         const std::vector<fs::DirEntry>& after,
                         std::vector<std::wstring>& names, std::wstring& focus);

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
