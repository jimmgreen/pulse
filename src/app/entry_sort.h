#pragma once
#include "../fs/fs_enum.h"
#include "../ui/ui_view_model.h"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::app {

// How folders are ordered against files in the details list.
enum class FolderSortMode : uint8_t {
    FoldersFirst = 0,  // folders stay on top, reversed order included (File Explorer)
    FoldersLast,   // folders always follow files, independent of direction
    Mixed,             // one order for folders and files
};

constexpr FolderSortMode FolderSortModeFromInt(int value) noexcept {
    return value == 1 ? FolderSortMode::FoldersLast
         : value == 2 ? FolderSortMode::Mixed
                      : FolderSortMode::FoldersFirst;
}

// The settings default (Settings > Folders first), for folders without their
// own choice. One atomic slot, readable from any thread.
void SetFolderSortMode(FolderSortMode mode) noexcept;
FolderSortMode DefaultFolderSortMode() noexcept;
// The mode EntryLess and SortEntries use on this thread: the innermost
// ScopedFolderSortMode, else the settings default.
FolderSortMode CurrentFolderSortMode() noexcept;

// A folder's own mode (remembered per folder like its sort column) for the
// sorts on this thread. The worker and the notify patcher wrap their sorts in
// one, next to ScopedEntryGrouping. Nestable; restores the previous mode.
class ScopedFolderSortMode {
public:
    explicit ScopedFolderSortMode(FolderSortMode mode) noexcept;
    ~ScopedFolderSortMode();
    ScopedFolderSortMode(const ScopedFolderSortMode&) = delete;
    ScopedFolderSortMode& operator=(const ScopedFolderSortMode&) = delete;

private:
    int previous_;
};

bool EntryLess(const fs::DirEntry& a, const fs::DirEntry& b,
               ui::SortColumn col, ui::SortDirection dir);
// Explicit mode for tests and callers that must not read the global pref.
bool EntryLess(const fs::DirEntry& a, const fs::DirEntry& b,
               ui::SortColumn col, ui::SortDirection dir, FolderSortMode folders);

// Sorts `entries` in EntryLess order (grouping and folder mode included).
// Faster than std::sort with EntryLess on large lists; `tick` runs during key preparation and once per
// comparison and may throw to abandon the sort, which leaves `entries` untouched.
void SortEntries(std::vector<fs::DirEntry>& entries, ui::SortColumn col, ui::SortDirection dir,
                 const std::function<void()>& tick = {});
void SortFirstEntries(std::vector<fs::DirEntry>& entries, ui::SortColumn col, ui::SortDirection dir,
                      size_t keep, const std::function<void()>& tick = {});

// Folder totals for Size order (#58), keyed by lower-cased folder name.
using FolderSizeLookup = std::unordered_map<std::wstring, uint64_t>;
// Size order as EntryLess sorts it (grouping and folder mode included), except
// that folders compare by their totals in `sizes`; folders without one follow
// the rest in either direction. `tick` runs during key preparation and once per comparison and may throw
// to abandon the sort, which leaves `entries` untouched.
void SortEntriesBySize(std::vector<fs::DirEntry>& entries, ui::SortDirection dir,
                       const FolderSizeLookup& sizes, const std::function<void()>& tick = {});
// Order-independent fingerprint of `sizes`, to notice changed totals. Empty is 0.
uint64_t FolderSizeSignature(const FolderSizeLookup& sizes);

// 媒体文件夹专用列: SortColumn::Media* orders by values read off the window
// thread. media_columns_ui.cpp registers where they come from; without a
// source (tests) or a ScopedMediaSortFolder, and in EntryLess, such sorts
// fall back to names. Rows whose value is unknown follow in either direction.
struct MediaSortKey {
    bool known = false;
    std::wstring text;    // compared first, like names
    uint64_t number = 0;  // then this (an album's track number, a duration)
};
using MediaSortKeySource = void (*)(const std::wstring& folder, const std::vector<fs::DirEntry>& entries,
                                    ui::SortColumn column, std::vector<MediaSortKey>& keys);
void SetMediaSortKeySource(MediaSortKeySource source) noexcept;
// The folder SortEntries' rows on this thread live in, for a media sort.
// Nestable; restores the previous folder.
class ScopedMediaSortFolder {
public:
    explicit ScopedMediaSortFolder(const std::wstring& folder) noexcept;
    ~ScopedMediaSortFolder();
    ScopedMediaSortFolder(const ScopedMediaSortFolder&) = delete;
    ScopedMediaSortFolder& operator=(const ScopedMediaSortFolder&) = delete;

private:
    const std::wstring* previous_;
};

} // namespace pulse::app
