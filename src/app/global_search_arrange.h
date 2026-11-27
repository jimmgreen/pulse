// global_search_arrange.h — Sort and group order of the quick search results.
// Pure: takes the merged rows (relevance order, with their class) and returns
// the shown order as spans. The window owns collapse state and drawing.
#pragma once
#include "../common/localization.h"
#include "../index/search_kinds.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

struct GroupClock;

// Values are persisted (app_prefs.json); append only.
enum class GlobalSearchSort : uint8_t { Relevance, Name, Modified, Size, Type, Path };
enum class GlobalSearchFolders : uint8_t { Top, Mixed, Bottom };
enum class GlobalSearchGroup : uint8_t { Match, Type, Date, Folder, Size, None };
// Where a merged row came from; Match grouping shows these as its groups.
enum class GlobalSearchRowClass : uint8_t { Jump, Best, App, Folder, File, Content };

inline constexpr int kGlobalSearchSortCount = 6, kGlobalSearchFoldersCount = 3, kGlobalSearchGroupCount = 6;
constexpr GlobalSearchSort GlobalSearchSortFromInt(int v) noexcept {
    return v >= 0 && v < kGlobalSearchSortCount ? static_cast<GlobalSearchSort>(v) : GlobalSearchSort::Relevance;
}
constexpr GlobalSearchFolders GlobalSearchFoldersFromInt(int v) noexcept {
    return v >= 0 && v < kGlobalSearchFoldersCount ? static_cast<GlobalSearchFolders>(v) : GlobalSearchFolders::Top;
}
constexpr GlobalSearchGroup GlobalSearchGroupFromInt(int v) noexcept {
    return v >= 0 && v < kGlobalSearchGroupCount ? static_cast<GlobalSearchGroup>(v) : GlobalSearchGroup::Match;
}
// The direction a newly chosen order starts in: newest and largest first, names A to Z.
constexpr bool GlobalSearchSortStartsDescending(GlobalSearchSort sort) noexcept {
    return sort == GlobalSearchSort::Modified || sort == GlobalSearchSort::Size;
}

struct GlobalSearchArrangeItem {
    std::wstring_view name, path;
    bool directory = false;
    uint64_t size = 0, mtime = 0;  // bytes, FILETIME ticks (UTC)
    GlobalSearchRowClass cls = GlobalSearchRowClass::File;
};
struct GlobalSearchArrangeOptions {
    GlobalSearchSort sort = GlobalSearchSort::Relevance;
    bool descending = false;
    GlobalSearchFolders folders = GlobalSearchFolders::Top;  // non-relevance orders only
    GlobalSearchGroup group = GlobalSearchGroup::Match;
    bool apps_first = false;
};
struct GlobalSearchSpan {
    std::wstring key;                 // stable collapse key ("t:image", "d:3", "f:<folder>")
    l10n::StringId title{};           // used when label is empty
    std::wstring label, detail;       // folder groups: the folder's name and where it is
    index::SearchKind kind = index::SearchKind::Any;  // type groups: colour dot
    bool app = false;                 // the 应用 group
    bool header = true;               // false: one flat list without a title
    bool foldable = false;
    std::vector<size_t> items;        // indexes into the input
};

// Kind of a file by its extension (the type: chips' lists); Folder for folders,
// Any when no kind lists the extension.
index::SearchKind GlobalSearchKindOf(std::wstring_view name, bool directory);
// Lower-case extension without the dot; empty for none.
std::wstring GlobalSearchExtensionOf(std::wstring_view name);
// Shown order. Match grouping by relevance keeps the merged class order
// (跳转, 最佳匹配, 应用, 文件夹, 文件); any other order lists everything once.
// clock: date buckets; null makes one for now.
std::vector<GlobalSearchSpan> ArrangeGlobalSearch(const std::vector<GlobalSearchArrangeItem>& items,
                                                  const GlobalSearchArrangeOptions& options,
                                                  const GroupClock* clock = nullptr);

} // namespace pulse::app
