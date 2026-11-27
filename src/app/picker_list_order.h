#pragma once

// The open/save dialog lists a folder in the order the main window shows it:
// the folder's remembered sort column, direction and folder placement, and
// its group-by, compared and grouped by the main window's own code.

#include "entry_group.h"
#include "folder_sort_prefs.h"
#include "../ui/folder_picker_model.h"
#include "../ui/view_layout.h"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pulse::app {
struct AppPrefs;

// A read-only copy of the main window's per-folder sort and group choices.
struct PickerListOrderPrefs {
    FolderSortPrefs sorts;
    FolderGroupPrefs groups;
    int folder_sort_mode = 0;  // Settings > Folders first (app::FolderSortMode)
    std::shared_ptr<const TagCatalogSnapshot> tags;
    // What the main window would use when it opened `folder` now. A group-by
    // whose metadata is unavailable lists flat instead of inventing groups.
    ui::PickerListOrder Resolve(const std::wstring& folder) const;
};

std::shared_ptr<const PickerListOrderPrefs> PickerListOrderPrefsFromPrefs(const AppPrefs& prefs);
// Same keys AppPrefs::Load reads from app.json; unknown members are ignored.
std::shared_ptr<const PickerListOrderPrefs> ParsePickerListOrderPrefs(const std::wstring& json);

// Tag grouping additionally requires a complete immutable catalog snapshot.
int PickerSupportedGroup(int group_by) noexcept;

// Reorders `entries` (PickerEntry::source indexes `rows`) with EntryLess
// under the order's folder placement and grouping. Worker thread.
void OrderPickerEntries(std::vector<ui::PickerEntry>& entries, const std::vector<fs::DirEntry>& rows,
                        const std::wstring& folder, const ui::PickerListOrder& order);

// Group spans over `rows` (already in grouped order), as the main window
// builds them; empty when the order is not grouped or the rows are not in
// grouped order (then the list is drawn flat).
std::shared_ptr<const ui::ListGroups> BuildPickerListGroups(const std::vector<fs::DirEntry>& rows,
                                                            const ui::PickerListOrder& order,
                                                            const std::set<std::wstring>& collapsed,
                                                            const std::wstring& folder = {});

} // namespace pulse::app
