#pragma once
#include "entry_sort.h"
#include "../fs/fs_enum.h"
#include "../ui/ui_view_model.h"
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::app {

// "Group by" for real folders (details view). Values are persisted.
// Location groups by parent folder and only applies to multi-folder views
// (search results, tag views).
// ThisPc is This PC's fixed grouping (磁盘 / 设备 / 网盘程序入口): never offered
// in menus and never saved (the prefs only read 0..6).
// Network is the network page's fixed grouping (已固定 / 网络驱动器 / 局域网设备 /
// 最近访问, DirEntry::net_group); like ThisPc it is never offered or saved.
// AiDup is the assistant's 重复文件 list: one group per set of identical
// files (DirEntry::net_group = its ordinal, change_old_path = its title).
// Media* are the 媒体文件夹 groups (拍摄月份 / 相机 / 专辑 / 艺术家 / 分辨率档),
// grouped by values the media property reader found: offered only in the
// group wheel of a picture / music / video folder and kept for the session
// only (FolderGroupPrefs never writes them).
enum class GroupBy : uint8_t { None = 0, Name = 1, Date = 2, Type = 3, Size = 4, Tag = 5, Location = 6, ThisPc = 7,
                               Network = 8, AiDup = 9, MediaMonth = 10, MediaCamera = 11, MediaAlbum = 12,
                               MediaArtist = 13, MediaTier = 14 };

constexpr GroupBy GroupByFromInt(int v) noexcept {
    return v >= 1 && v <= 14 ? static_cast<GroupBy>(v) : GroupBy::None;
}

constexpr bool IsMediaGroup(GroupBy g) noexcept {
    return g >= GroupBy::MediaMonth && g <= GroupBy::MediaTier;
}

// Parent folder of a virtual-view entry (full_path), without the \\?\ prefix.
// Empty for plain folder listings.
std::wstring GroupLocationLabel(const fs::DirEntry& e);

// Tag grouping for one folder: each entry falls in its primary tag's group
// (first tag in sidebar order); untagged entries come last. Read-only, so
// the worker and the UI share it.
struct TagGroupMap {
    std::unordered_map<std::wstring, int> rank_by_name; // lower-case leaf -> tag index
    int untagged = 0;                                   // == tag count
};

// Catalog snapshot published by the UI thread whenever tags change.
struct TagCatalogSnapshot {
    struct Tag { std::wstring name; uint32_t rgb = 0; };
    std::vector<Tag> tags;                              // sidebar order
    std::vector<std::pair<std::wstring, int>> paths;    // lower-case normalized path, primary tag
    uint64_t revision = 0;
};
void PublishTagCatalog(std::shared_ptr<const TagCatalogSnapshot> snapshot);
std::shared_ptr<const TagCatalogSnapshot> CurrentTagCatalog();
std::shared_ptr<const TagGroupMap> TagGroupsForFolder(const std::wstring& folder);

// Media grouping for one folder: the group of each file whose values are
// known, for that exact file version. Files not listed (pending, unreadable,
// no such value) form the unknown group, shown last; folders come first.
struct MediaGroupMap {
    struct Item {
        uint64_t mtime = 0, size = 0;   // FILETIME ticks, bytes
        int rank = 0;                   // index into labels
    };
    std::unordered_map<std::wstring, Item> by_name;   // leaf name as listed
    std::vector<std::wstring> labels;                  // group order
    int Unknown() const noexcept { return static_cast<int>(labels.size()); }
};
// Registered by the media columns (app start); tests have none, so every
// file reads as unknown.
using MediaGroupSource = std::shared_ptr<const MediaGroupMap> (*)(const std::wstring& folder, GroupBy g);
void SetMediaGroupSource(MediaGroupSource source) noexcept;
std::shared_ptr<const MediaGroupMap> MediaGroupsForFolder(const std::wstring& folder, GroupBy g);

// Local-time day boundaries (FILETIME ticks, UTC) used by the date buckets.
// Built once per sort / span pass so every comparison sees the same "today".
struct GroupClock {
    uint64_t today = 0, yesterday = 0, week = 0, last_week = 0;
    uint64_t month = 0, last_month = 0, year = 0, tomorrow = 0;
    // Which time GroupBy::Date buckets for GroupRank/GroupKey: the list's sort
    // column when it is a date (created / accessed), otherwise modified.
    // GroupCompare takes the column from its own argument.
    ui::SortColumn date_column = ui::SortColumn::Mtime;
    FolderSortMode folders = FolderSortMode::Mixed;
    std::shared_ptr<const TagGroupMap> tags;  // GroupBy::Tag only
    std::shared_ptr<const MediaGroupMap> media;  // Media* only
};
GroupClock MakeGroupClock(ui::SortColumn sort_column = ui::SortColumn::Mtime);

// The time GroupBy::Date uses for a list sorted by `sort_column`.
const FILETIME& GroupDateOf(const fs::DirEntry& e, ui::SortColumn sort_column) noexcept;

// A separate directory group is shared by sorting, headers and collapse keys.
// Fixed virtual groups retain their own hierarchy; Mixed uses the chosen field.
inline constexpr int kFolderGroupRank = -2;

// Date ranks: 0 future, 1 today, 2 yesterday, 3 earlier this week, 4 last week,
// 5 earlier this month, 6 last month, 7 earlier this year, 8 a long time ago.
// Tag ranks: primary tag index; TagGroupMap::untagged when none.
// Name ranks: 0 digits, 1 A-H, 2 I-P, 3 Q-Z, 4 other (CJK by pinyin initial).
// Size ranks: 0 folders, 1 empty, 2 tiny <16K, 3 small <1M, 4 medium <128M,
//             5 large <1G, 6 huge <4G, 7 gigantic.
// Type ranks: 0 folders, 1 files (files then split by extension).
// ThisPc ranks: 0 drives, 1 devices (phones), 2 program icons.
// Network ranks: DirEntry::net_group (1 pinned .. 4 recent).
// Media ranks: -1 folders, MediaGroupMap label index, Unknown() when not known.
int GroupRank(const fs::DirEntry& e, GroupBy g, const GroupClock& clock);

// <0 when a's group is shown before b's, 0 same group.
int GroupCompare(const fs::DirEntry& a, const fs::DirEntry& b, GroupBy g,
                 const GroupClock& clock, ui::SortColumn col, ui::SortDirection dir);

// Same comparison using ranks prepared once per row (except Location, which
// still compares parent paths). Date ranks must use the active sort column.
int GroupCompareRanks(const fs::DirEntry& a, const fs::DirEntry& b, GroupBy g,
                      ui::SortColumn col, ui::SortDirection dir, int ra, int rb,
                      const GroupClock& clock = {});

// Stable key for collapse state ("d3", "t:pdf", ...).
std::wstring GroupKey(const fs::DirEntry& e, GroupBy g, const GroupClock& clock);

// A media group's title for `rank`; empty for folders (-1) and unknown values,
// which the callers name.
std::wstring MediaGroupLabel(const GroupClock& clock, int rank);

// Stable in-place reorder of `entries` (and `parallel`, if the same size) into
// group order; order inside a group is kept. For views whose order comes from
// elsewhere (indexed search pages) rather than the sorting worker.
void StableGroupOrder(std::vector<fs::DirEntry>& entries, std::vector<std::wstring>* parallel,
                      GroupBy g, ui::SortColumn col, ui::SortDirection dir, FolderSortMode folders);

// Active grouping for EntryLess on the current thread. The worker and the
// notify patcher wrap their sorts in a scope instead of threading the group
// through every comparator signature. Nestable; restores the previous state.
struct EntryGrouping {
    GroupBy by = GroupBy::None;
    GroupClock clock;
};
const EntryGrouping* CurrentEntryGrouping() noexcept;

class ScopedEntryGrouping {
public:
    // `folder` resolves tag groups (GroupBy::Tag) and media groups.
    explicit ScopedEntryGrouping(int group_by, const std::wstring& folder = {});
    ~ScopedEntryGrouping();
    ScopedEntryGrouping(const ScopedEntryGrouping&) = delete;
    ScopedEntryGrouping& operator=(const ScopedEntryGrouping&) = delete;

private:
    EntryGrouping state_;
    const EntryGrouping* previous_ = nullptr;
};

// Per-folder "group by" memory. Unlike view modes, an explicit None is stored
// so turning grouping off in Downloads survives the Date default.
class FolderGroupPrefs {
public:
    std::optional<GroupBy> Find(const std::wstring& path) const;
    bool Set(const std::wstring& path, GroupBy by);
    // Grouping a view uses: its saved choice, else the "apply to all" default
    // for real folders, else DefaultGroupFor (Downloads, Recent, recycle bin).
    GroupBy Resolve(const std::wstring& path) const;
    // "Apply to all folders" (#75): `by` becomes every real folder's grouping and
    // per-folder choices go. Virtual views (Recent, search, tags, recycle bin) keep theirs.
    void ApplyToAll(GroupBy by);
    std::optional<GroupBy> Default() const { return default_; }
    void Clear() { groups_.clear(); default_.reset(); }
    void AppendJson(std::wstring& out) const;
    void ReadJson(const std::wstring& json);

private:
    struct PathLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const;
    };
    std::map<std::wstring, GroupBy, PathLess> groups_;
    std::map<std::wstring, GroupBy, PathLess> session_;   // media groups: not saved
    std::optional<GroupBy> default_;
};

// Default when a folder has no saved choice: Date for Downloads, Recent and
// the recycle bin (deleted date).
GroupBy DefaultGroupFor(const std::wstring& path);

} // namespace pulse::app
