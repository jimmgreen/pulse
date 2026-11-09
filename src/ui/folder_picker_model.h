#pragma once

// Pure state of the Pulse folder / picture picker: paths, filtering, sorting
// and what the primary button would pick. No windows, no file system calls.

#include <windows.h>

#include "../fs/fs_snapshot.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::ui {

enum class PickerMode { Folder, Image, File, Save };
enum class PickerEntryKind { Folder, Image, Drive, File };

struct PickerFilter { std::wstring label; std::wstring pattern; };
enum class PickerSort { Name, Modified, Size, Type };
struct PickerOptions {
    uint32_t native_options = 0;
    std::vector<PickerFilter> filters;
    size_t filter_index = 0;
    bool show_hidden = false;
    std::wstring search;
    PickerSort sort = PickerSort::Name;
    bool descending = false;
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
};

// One folder's contents as read by the loader. path "" is This PC (drives).
struct PickerListing {
    uint64_t generation = 0;
    std::wstring path;
    std::vector<PickerEntry> entries;
    // The same rows in the same order as the main window's directory snapshot,
    // so the picker's list is drawn by the shared pane renderer.
    fs::SnapshotPtr snapshot;
    DWORD error = ERROR_SUCCESS;
};

bool IsPickerImageName(std::wstring_view name);
// Hidden and system items, "." and "..", and (in picture mode) other files
// stay out of the list.
bool PickerMatchesFilter(std::wstring_view name, PickerMode mode, const PickerOptions& options = {});
// read_next sets followed only when it replaces path with a shortcut target.
// Kept independent of COM so cycle, cancellation and hop bounds are testable.
DWORD FollowPickerShortcutChain(std::wstring& path,
    const std::function<DWORD(std::wstring&, bool& followed)>& read_next,
    const std::function<bool()>& cancelled = {});
bool ParsePickerNames(std::wstring_view text, std::vector<std::wstring>& names);
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
