#pragma once

// Pure state of the Pulse folder / picture picker: paths, filtering, sorting
// and what the primary button would pick. No windows, no file system calls.

#include <windows.h>

#include "../fs/fs_snapshot.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app { struct TagCatalogSnapshot; }
namespace pulse::ui {

enum class PickerMode { Folder, Image, File, Save };
enum class PickerEntryKind { Folder, Image, Drive, File };

struct PickerFilter { std::wstring label; std::wstring pattern; };
enum class PickerSort { Name, Modified, Size, Type };
struct PickerEntry;
class PickerArchiveSession;
class PickerArchiveDelivery;
// The main window's order for one folder: its remembered sort and group-by
// (app::PickerListOrderPrefs resolves it from Pulse's preferences).
struct PickerListOrder {
    int column = 0;          // ui::SortColumn
    bool descending = false;
    int group_by = 0;        // app::GroupBy; only groups the picker can compute
    int folder_mode = 0;     // app::FolderSortMode
    std::shared_ptr<const app::TagCatalogSnapshot> tags; // one immutable catalog for sort and headers
    bool operator==(const PickerListOrder&) const = default;
};
struct PickerOptions {
    uint32_t native_options = 0;
    std::vector<PickerFilter> filters;
    size_t filter_index = 0;
    bool show_hidden = false;
    bool show_protected_os_files = false;
    std::wstring search;
    PickerSort sort = PickerSort::Name;
    bool descending = false;
    // Worker-side ordering with the main window's comparator (rows indexed by
    // PickerEntry::source); empty: SortPickerEntries with sort / descending.
    std::function<void(std::vector<PickerEntry>&, const std::vector<fs::DirEntry>&)> order_entries;
    // View filters of the open filter bar (files only; folders stay to navigate).
    // category: 0 all, else what category_of returns for the name.
    int category = 0;
    std::function<int(std::wstring_view name)> category_of;   // worker thread; 1..kPickerCategoryCount-1
    uint64_t modified_after = 0;  // FILETIME ticks (UTC); 0: any time
    uint64_t new_since = 0;       // inclusive FILETIME boundary for 新文件; 0: no history
    bool only_new = false;       // independent of modified_after; ignored without new_since
    bool save_same_format_only = false; // Save filter bar: hide non-matching files instead of dimming
    // File acceptance only; listing and Image format restrictions remain strict.
    bool advisory_file_filters = false;
    // Explicit Open contract only. The loader still checks existence; it does
    // not implement a read-access probe for either value of this field.
    bool skip_access_validation = false;
    bool allow_missing_open = false;
    bool require_existing_open_parent = false;
    bool validate_local_open_access_v1 = false;
    // Explicit Open setter evidence. Only typed submissions may try a default
    // extension after the exact path is missing; listing selections never do.
    std::wstring default_extension;
    bool extension_filter_changed = false;
    bool typed_names = false;
    bool use_default_extension = false;
    bool no_dereference_links = false;
    std::shared_ptr<PickerArchiveSession> archive;
};

struct PickerEntry {
    std::wstring name;   // display name ("Local Disk (C:)" for drives)
    std::wstring path;   // full path
    PickerEntryKind kind = PickerEntryKind::Folder;
    FILETIME modified{};
    uint64_t size = 0;   // file size; total bytes for drives
    uint64_t free = 0;   // drives only
    size_t source = 0;   // index in the enumeration, used while building the listing
    uint32_t shell_attributes = 0; // SFGAOF; path may be a Shell parsing name.
    bool is_new = false;        // files modified >= new_since in this listing's snapshot
    bool archive = false;       // physical outer archive; it remains a file, not a folder
};

// One folder's contents as read by the loader. path "" is This PC (drives).
inline constexpr size_t kPickerCategoryCount = 7;  // all, image, document, video, audio, archive, other
struct PickerListing {
    uint64_t generation = 0;
    std::wstring path;
    std::vector<PickerEntry> entries;
    // The same rows in the same order as the main window's directory snapshot,
    // so the picker's list is drawn by the shared pane renderer.
    fs::SnapshotPtr snapshot;
    DWORD error = ERROR_SUCCESS;
    // The whole folder before the view filters (disk listings only): files per
    // type (options.filters order), per category, and 新文件 of the current type.
    std::vector<uint32_t> filter_counts;
    std::array<uint32_t, kPickerCategoryCount> category_counts{};
    uint32_t new_count = 0;
    bool counted = false;
};

bool IsPickerImageName(std::wstring_view name);
// Visibility follows the main window: hidden is gated independently from
// protected (hidden + system); system-only entries remain visible.
bool PickerMatchesFilter(std::wstring_view name, PickerMode mode, const PickerOptions& options = {});
// The same test against options.filters[filter_index] instead of the selected type.
bool PickerMatchesFilterAt(std::wstring_view name, PickerMode mode, const PickerOptions& options, size_t filter_index);
// read_next sets followed only when it replaces path with a shortcut target.
// Kept independent of COM so cycle, cancellation and hop bounds are testable.
DWORD FollowPickerShortcutChain(std::wstring& path,
    const std::function<DWORD(std::wstring&, bool& followed)>& read_next,
    const std::function<bool()>& cancelled = {});
bool ParsePickerNames(std::wstring_view text, std::vector<std::wstring>& names);
bool PickerAllowsAttributes(DWORD attributes, const PickerOptions& options);
bool PickerShowsEntry(DWORD attributes, std::wstring_view name, PickerMode mode,
                      const PickerOptions& options = {});
// Folders first, then names in Explorer's numeric-aware order.
void SortPickerEntries(std::vector<PickerEntry>& entries, const PickerOptions& options = {});

// "C:\a\b" -> "C:\a"; a drive or share root -> "" (This PC).
std::wstring PickerParent(std::wstring_view path);
// Typed text to a path: trims spaces and quotes, expands %VARS%, accepts
// "/" and "C:", and drops trailing separators except on roots.
std::wstring NormalizePickerInput(std::wstring_view text);
bool SamePickerPath(std::wstring_view a, std::wstring_view b);
bool IsPickerFolderName(std::wstring_view name);
bool IsPickerDiskPath(std::wstring_view path);

// What the primary button returns: the selected folder or picture, else the
// current folder in folder mode. Empty = nothing to pick (button disabled).
std::wstring PickerChosenPath(PickerMode mode, std::wstring_view current,
                              const PickerEntry* selected);

// Next entry after `from` whose name starts with `ch` (wraps); -1 if none.
int PickerTypeAhead(const std::vector<PickerEntry>& entries, int from, wchar_t ch);

// Back stack: Navigate pushes the old location, Back pops it.
class PickerHistory {
public:
    void Navigate(std::wstring from);
    bool CanGoBack() const { return !back_.empty(); }
    std::wstring Back();
    bool CanGoForward() const { return !forward_.empty(); }
    std::wstring Back(std::wstring current);
    std::wstring Forward(std::wstring current);

private:
    std::vector<std::wstring> back_;
    std::vector<std::wstring> forward_;
};

} // namespace pulse::ui
