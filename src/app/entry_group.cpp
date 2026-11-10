#include "entry_group.h"
#include "entry_sort.h"
#include "../common/json_utils.h"
#include "../common/path_utils.h"
#include <shlobj.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace pulse::app {

namespace {

uint64_t Ticks(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// Local midnight of `st` (date part) as UTC FILETIME ticks.
uint64_t LocalMidnight(SYSTEMTIME st) {
    st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0;
    FILETIME local{}, utc{};
    if (!SystemTimeToFileTime(&st, &local) || !LocalFileTimeToFileTime(&local, &utc)) return 0;
    return Ticks(utc);
}

constexpr uint64_t kDay = 864000000000ull; // 24 h in 100 ns ticks

// Parent part of full_path (prefix kept); empty for plain listings.
std::wstring_view ParentOf(const fs::DirEntry& e) {
    const std::wstring_view full(e.full_path);
    const size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? std::wstring_view{} : full.substr(0, slash);
}

int ParentCompare(const fs::DirEntry& a, const fs::DirEntry& b) {
    const std::wstring_view pa = ParentOf(a), pb = ParentOf(b);
    const int r = CompareStringOrdinal(pa.data(), static_cast<int>(pa.size()),
                                       pb.data(), static_cast<int>(pb.size()), TRUE);
    return r == CSTR_LESS_THAN ? -1 : (r == CSTR_GREATER_THAN ? 1 : 0);
}

bool IsFolder(const fs::DirEntry& e) {
    return e.is_dir || (!e.link_target.empty() && e.link_target_is_dir);
}

// Pinyin initial of a CJK ideograph via zh-CN collation boundaries: each
// boundary is the first syllable of its letter in pinyin order.
wchar_t PinyinInitial(wchar_t c) {
    static constexpr wchar_t kBoundary[] = L"吖八嚓咑妸发旮铪讥咔垃呒拏噢妑七呥仨他屲夕丫帀";
    static constexpr char kLetter[] = "ABCDEFGHJKLMNOPQRSTWXYZ";
    constexpr wchar_t kFirst = 0x4E00, kLast = 0x9FA5;
    if (c < kFirst || c > kLast) return 0;
    static std::array<std::atomic<char>, kLast - kFirst + 1> cache{};
    std::atomic<char>& slot = cache[static_cast<size_t>(c - kFirst)];
    if (const char hit = slot.load(std::memory_order_relaxed)) return static_cast<wchar_t>(hit);
    int lo = 0, hi = static_cast<int>(std::size(kBoundary)) - 2, found = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int r = CompareStringEx(L"zh-CN", 0, &c, 1, &kBoundary[mid], 1, nullptr, nullptr, 0);
        if (r == CSTR_GREATER_THAN || r == CSTR_EQUAL) {
            found = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    const char letter = found >= 0 ? kLetter[found] : '#';
    slot.store(letter, std::memory_order_relaxed);
    return static_cast<wchar_t>(letter);
}

std::wstring_view Extension(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return {};
    return std::wstring_view(name).substr(dot + 1);
}

int ExtCompare(const std::wstring& a, const std::wstring& b) {
    const std::wstring_view ea = Extension(a), eb = Extension(b);
    const size_t n = std::min(ea.size(), eb.size());
    for (size_t i = 0; i < n; ++i) {
        const wint_t ca = std::towlower(ea[i]), cb = std::towlower(eb[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (ea.size() == eb.size()) return 0;
    return ea.size() < eb.size() ? -1 : 1;
}

// Reverse the group order when the list is sorted by the grouped field in
// the "other" direction (dates default to newest first).
bool FlipGroups(GroupBy g, ui::SortColumn col, ui::SortDirection dir) {
    switch (g) {
    case GroupBy::Name: return col == ui::SortColumn::Name && dir == ui::SortDirection::Desc;
    case GroupBy::Size: return col == ui::SortColumn::Size && dir == ui::SortDirection::Desc;
    case GroupBy::Type: return col == ui::SortColumn::Type && dir == ui::SortDirection::Desc;
    case GroupBy::Date:
        return (col == ui::SortColumn::Mtime || col == ui::SortColumn::Created ||
                col == ui::SortColumn::Accessed) && dir == ui::SortDirection::Asc;
    // Newest month and the largest frames come first; names run A to Z.
    case GroupBy::MediaMonth: return col == ui::SortColumn::MediaTaken && dir == ui::SortDirection::Asc;
    case GroupBy::MediaTier: return col == ui::SortColumn::MediaDims && dir == ui::SortDirection::Asc;
    case GroupBy::MediaCamera: return col == ui::SortColumn::MediaCamera && dir == ui::SortDirection::Desc;
    case GroupBy::MediaAlbum: return col == ui::SortColumn::MediaAlbum && dir == ui::SortDirection::Desc;
    case GroupBy::MediaArtist: return col == ui::SortColumn::MediaArtist && dir == ui::SortDirection::Desc;
    default: return false;
    }
}

} // namespace

const FILETIME& GroupDateOf(const fs::DirEntry& e, ui::SortColumn sort_column) noexcept {
    if (sort_column == ui::SortColumn::Created) return e.ctime;
    if (sort_column == ui::SortColumn::Accessed) return e.atime;
    return e.mtime;
}

namespace {
int DateRank(uint64_t t, const GroupClock& c) {
    if (t >= c.tomorrow) return 0;
    if (t >= c.today) return 1;
    if (t >= c.yesterday) return 2;
    if (t >= c.week) return 3;
    if (t >= c.last_week) return 4;
    if (t >= c.month) return 5;
    if (t >= c.last_month) return 6;
    if (t >= c.year) return 7;
    return 8;
}
} // namespace

GroupClock MakeGroupClock(ui::SortColumn sort_column) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    GroupClock c;
    c.date_column = sort_column;
    c.today = LocalMidnight(now);
    c.tomorrow = c.today + kDay;
    c.yesterday = c.today - kDay;
    const int monday_offset = (now.wDayOfWeek + 6) % 7; // 0 = Monday
    c.week = c.today - static_cast<uint64_t>(monday_offset) * kDay;
    c.last_week = c.week - 7 * kDay;
    SYSTEMTIME m = now;
    m.wDay = 1;
    c.month = LocalMidnight(m);
    SYSTEMTIME lm = m;
    if (lm.wMonth == 1) { lm.wMonth = 12; --lm.wYear; } else { --lm.wMonth; }
    c.last_month = LocalMidnight(lm);
    SYSTEMTIME y = m;
    y.wMonth = 1;
    c.year = LocalMidnight(y);
    return c;
}

int GroupRank(const fs::DirEntry& e, GroupBy g, const GroupClock& c) {
    // A folder's tag determines its group; folder placement applies within it.
    if (c.folders != FolderSortMode::Mixed && g != GroupBy::None &&
        g != GroupBy::Tag && g != GroupBy::ThisPc && g != GroupBy::Network && g != GroupBy::AiDup && IsFolder(e))
        return kFolderGroupRank;
    switch (g) {
    case GroupBy::Date: return DateRank(Ticks(GroupDateOf(e, c.date_column)), c);
    case GroupBy::Name: {
        if (e.name.empty()) return 4;
        wchar_t ch = e.name[0];
        if (ch >= L'0' && ch <= L'9') return 0;
        if (const wchar_t py = PinyinInitial(ch)) ch = py;
        ch = static_cast<wchar_t>(std::towupper(ch));
        if (ch >= L'A' && ch <= L'H') return 1;
        if (ch >= L'I' && ch <= L'P') return 2;
        if (ch >= L'Q' && ch <= L'Z') return 3;
        return 4;
    }
    case GroupBy::Size: {
        if (IsFolder(e)) return 0;
        const uint64_t s = e.size;
        if (s == 0) return 1;
        if (s < 16ull * 1024) return 2;
        if (s < 1024ull * 1024) return 3;
        if (s < 128ull * 1024 * 1024) return 4;
        if (s < 1024ull * 1024 * 1024) return 5;
        if (s < 4096ull * 1024 * 1024) return 6;
        return 7;
    }
    case GroupBy::Type: return IsFolder(e) ? 0 : 1;
    case GroupBy::ThisPc: return e.pc_launcher ? 2 : e.pc_device ? 1 : 0;
    case GroupBy::Network:
    case GroupBy::AiDup: return e.net_group;
    case GroupBy::Tag: {
        if (!c.tags) return 0;
        if (c.tags->rank_by_name.empty()) return c.tags->untagged;
        std::wstring key = e.name;
        for (auto& ch : key) ch = static_cast<wchar_t>(std::towlower(ch));
        const auto it = c.tags->rank_by_name.find(key);
        return it != c.tags->rank_by_name.end() ? it->second : c.tags->untagged;
    }
    case GroupBy::MediaMonth:
    case GroupBy::MediaCamera:
    case GroupBy::MediaAlbum:
    case GroupBy::MediaArtist:
    case GroupBy::MediaTier: {
        if (IsFolder(e)) return -1;
        if (!c.media) return 0;
        const auto it = c.media->by_name.find(e.name);
        if (it == c.media->by_name.end() || it->second.mtime != Ticks(e.mtime) || it->second.size != e.size)
            return c.media->Unknown();
        return it->second.rank;
    }
    default: return 0;
    }
}

int GroupCompare(const fs::DirEntry& a, const fs::DirEntry& b, GroupBy g,
                 const GroupClock& clock, ui::SortColumn col, ui::SortDirection dir) {
    GroupClock active = clock;
    active.date_column = col;
    return GroupCompareRanks(a, b, g, col, dir, GroupRank(a, g, active), GroupRank(b, g, active), active);
}

int GroupCompareRanks(const fs::DirEntry& a, const fs::DirEntry& b, GroupBy g,
                      ui::SortColumn col, ui::SortDirection dir, int ra, int rb, const GroupClock& clock) {
    if (g == GroupBy::None) return 0;
    if (ra == kFolderGroupRank || rb == kFolderGroupRank) {
        if (ra == rb) return 0;
        return (ra == kFolderGroupRank) == (clock.folders == FolderSortMode::FoldersFirst) ? -1 : 1;
    }
    if (g == GroupBy::Location) return ParentCompare(a, b);
    const bool flip = FlipGroups(g, col, dir);
    if (IsMediaGroup(g) && ra != rb) {
        // Mixed retains the field's folder bucket; missing metadata stays last.
        if (ra < 0 || rb < 0) return ra < 0 ? -1 : 1;
        const int unknown = clock.media ? clock.media->Unknown() : 0;
        if (ra == unknown || rb == unknown) return ra == unknown ? 1 : -1;
    }
    const bool pinned = g == GroupBy::Size || g == GroupBy::Type;
    if (ra != rb) {
        if (pinned && (ra == 0 || rb == 0)) return ra == 0 ? -1 : 1;
        return (ra < rb) != flip ? -1 : 1;
    }
    if (g == GroupBy::Type && ra == 1) {
        const int ext = ExtCompare(a.name, b.name);
        return flip ? -ext : ext;
    }
    return 0;
}

std::wstring GroupKey(const fs::DirEntry& e, GroupBy g, const GroupClock& clock) {
    const int rank = GroupRank(e, g, clock);
    if (rank == kFolderGroupRank) return L"folders";
    if (g == GroupBy::Type && rank == 1) {
        std::wstring ext(Extension(e.name));
        for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
        return L"t:" + ext;
    }
    if (g == GroupBy::Location) {
        std::wstring key = L"l:";
        key += ParentOf(e);
        for (auto& ch : key) ch = static_cast<wchar_t>(std::towlower(ch));
        return key;
    }
    if (IsMediaGroup(g)) {
        // By title, not rank: ranks shift as values arrive, a collapsed group stays.
        std::wstring key = L"m" + std::to_wstring(static_cast<int>(g)) + L":";
        if (rank < 0) return key + L"/";
        std::wstring label = MediaGroupLabel(clock, rank);
        if (label.empty()) return key + L"?";
        for (auto& ch : label) ch = static_cast<wchar_t>(std::towlower(ch));
        return key + label;
    }
    static constexpr wchar_t kPrefix[] = L"-ndtsglpwk";
    std::wstring key(1, kPrefix[static_cast<int>(g)]);
    key += std::to_wstring(rank);
    return key;
}

std::wstring MediaGroupLabel(const GroupClock& clock, int rank) {
    if (!clock.media || rank < 0 || rank >= clock.media->Unknown()) return {};
    return clock.media->labels[static_cast<size_t>(rank)];
}

std::wstring GroupLocationLabel(const fs::DirEntry& e) {
    std::wstring parent(ParentOf(e));
    parent = path::StripExtendedPathPrefix(parent);
    if (parent.size() == 2 && parent[1] == L':') parent += L'\\';
    return parent;
}

void StableGroupOrder(std::vector<fs::DirEntry>& entries, std::vector<std::wstring>* parallel,
                      GroupBy g, ui::SortColumn col, ui::SortDirection dir, FolderSortMode folders) {
    if (g == GroupBy::None || entries.size() < 2) return;
    GroupClock clock = g == GroupBy::Date ? MakeGroupClock(col) : GroupClock{};
    clock.folders = folders;
    std::vector<size_t> order(entries.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return GroupCompare(entries[a], entries[b], g, clock, col, dir) < 0;
    });
    std::vector<fs::DirEntry> sorted;
    sorted.reserve(entries.size());
    for (size_t i : order) sorted.push_back(std::move(entries[i]));
    entries = std::move(sorted);
    if (parallel && parallel->size() == order.size()) {
        std::vector<std::wstring> moved;
        moved.reserve(order.size());
        for (size_t i : order) moved.push_back(std::move((*parallel)[i]));
        *parallel = std::move(moved);
    }
}

namespace {
thread_local const EntryGrouping* t_grouping = nullptr;

std::wstring GroupFolderKey(const std::wstring& path) {
    if (path.starts_with(L"pulse:")) {
        // One shared choice per virtual kind: every search remembers the same.
        std::wstring key = path;
        for (auto& ch : key) ch = static_cast<wchar_t>(std::towlower(ch));
        const size_t colon = key.find(L':', 6);
        const std::wstring kind = key.substr(6, colon == std::wstring::npos ? std::wstring::npos : colon - 6);
        if (kind == L"recent") return L"pulse:recent";
        if (kind == L"search" || kind == L"saved-search") return L"pulse:search";
        if (kind == L"tag" || kind == L"tags") return L"pulse:tag";
        if (kind == L"recycle") return L"pulse:recycle";
        return {};
    }
    std::wstring key = path::StripExtendedPathPrefix(path);
    std::replace(key.begin(), key.end(), L'/', L'\\');
    if (!(key.size() >= 3 && key[1] == L':' && key[2] == L'\\') &&
        !key.starts_with(L"\\\\")) return {};
    while (key.size() > 3 && key.back() == L'\\') key.pop_back();
    return key;
}

std::wstring GroupJsonKey(int by) {
    return L"folder_group_" + std::to_wstring(by);
}

const std::wstring& DownloadsFolder() {
    static const std::wstring folder = [] {
        std::wstring out;
        PWSTR raw = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &raw)) && raw)
            out = GroupFolderKey(raw);
        CoTaskMemFree(raw);
        return out;
    }();
    return folder;
}
} // namespace

const EntryGrouping* CurrentEntryGrouping() noexcept {
    return t_grouping && t_grouping->by != GroupBy::None ? t_grouping : nullptr;
}

namespace {
std::mutex g_tag_catalog_mutex;
std::shared_ptr<const TagCatalogSnapshot> g_tag_catalog;
} // namespace

void PublishTagCatalog(std::shared_ptr<const TagCatalogSnapshot> snapshot) {
    std::lock_guard lock(g_tag_catalog_mutex);
    g_tag_catalog = std::move(snapshot);
}

std::shared_ptr<const TagCatalogSnapshot> CurrentTagCatalog() {
    std::lock_guard lock(g_tag_catalog_mutex);
    return g_tag_catalog;
}

std::shared_ptr<const TagGroupMap> TagGroupsForFolder(const std::wstring& folder) {
    auto map = std::make_shared<TagGroupMap>();
    const auto catalog = CurrentTagCatalog();
    if (!catalog) return map;
    map->untagged = static_cast<int>(catalog->tags.size());
    // Callers pass tab / work paths, already normalized like the catalog keys.
    if (folder.empty() || folder.starts_with(L"pulse:")) return map;
    std::wstring parent = folder;
    for (auto& ch : parent) ch = static_cast<wchar_t>(std::towlower(ch));
    while (!parent.empty() && parent.back() == L'\\') parent.pop_back();
    for (const auto& [path, rank] : catalog->paths) {
        const size_t slash = path.find_last_of(L'\\');
        if (slash == std::wstring::npos || slash != parent.size() ||
            path.compare(0, slash, parent) != 0) continue;
        map->rank_by_name.emplace(path.substr(slash + 1), rank);
    }
    return map;
}

namespace {
std::atomic<MediaGroupSource> g_media_groups{nullptr};
}  // namespace

void SetMediaGroupSource(MediaGroupSource source) noexcept {
    g_media_groups.store(source, std::memory_order_release);
}

std::shared_ptr<const MediaGroupMap> MediaGroupsForFolder(const std::wstring& folder, GroupBy g) {
    const MediaGroupSource source = g_media_groups.load(std::memory_order_acquire);
    return source && IsMediaGroup(g) && !folder.empty() ? source(folder, g) : nullptr;
}

ScopedEntryGrouping::ScopedEntryGrouping(int group_by, const std::wstring& folder) : previous_(t_grouping) {
    state_.by = GroupByFromInt(group_by);
    if (state_.by == GroupBy::Date) state_.clock = MakeGroupClock();
    if (state_.by == GroupBy::Tag) state_.clock.tags = TagGroupsForFolder(folder);
    if (IsMediaGroup(state_.by)) state_.clock.media = MediaGroupsForFolder(folder, state_.by);
    t_grouping = &state_;
}

ScopedEntryGrouping::~ScopedEntryGrouping() {
    t_grouping = previous_;
}

bool FolderGroupPrefs::PathLess::operator()(const std::wstring& a, const std::wstring& b) const {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
}

std::optional<GroupBy> FolderGroupPrefs::Find(const std::wstring& path) const {
    const auto it = groups_.find(GroupFolderKey(path));
    return it == groups_.end() ? std::nullopt : std::optional(it->second);
}

bool FolderGroupPrefs::Set(const std::wstring& path, GroupBy by) {
    const auto key = GroupFolderKey(path);
    if (key.empty()) return false;
    // Media groups last for the session; the folder's saved choice stays.
    if (IsMediaGroup(by)) {
        session_[key] = by;
        return false;
    }
    session_.erase(key);
    const auto [it, inserted] = groups_.try_emplace(key, by);
    if (!inserted && it->second == by) return false;
    it->second = by;
    return true;
}

void FolderGroupPrefs::AppendJson(std::wstring& out) const {
    for (int by = 0; by <= 6; ++by) {
        out += L",\n  \"" + GroupJsonKey(by) + L"\":[";
        bool first = true;
        for (const auto& [path, saved] : groups_) {
            if (static_cast<int>(saved) != by) continue;
            if (!first) out += L",";
            first = false;
            out += L"\"";
            json::Escape(path, out);
            out += L"\"";
        }
        out += L"]";
    }
    if (default_) out += L",\n  \"folder_group_default\":" + std::to_wstring(static_cast<int>(*default_));
}

void FolderGroupPrefs::ReadJson(const std::wstring& input) {
    Clear();
    for (int by = 0; by <= 6; ++by)
        for (const auto& path : json::ExtractStringArray(input, GroupJsonKey(by)))
            Set(path, GroupByFromInt(by));
    const int fallback = json::ExtractInt(input, L"folder_group_default", -1);
    if (fallback >= 0 && fallback <= 5) default_ = GroupByFromInt(fallback);
}

GroupBy FolderGroupPrefs::Resolve(const std::wstring& path) const {
    const auto key = GroupFolderKey(path);
    if (!key.empty()) {
        if (const auto it = session_.find(key); it != session_.end()) return it->second;
        if (const auto it = groups_.find(key); it != groups_.end()) return it->second;
        if (default_ && !key.starts_with(L"pulse:")) return *default_;
    }
    return DefaultGroupFor(path);
}

void FolderGroupPrefs::ApplyToAll(GroupBy by) {
    default_ = by;
    std::erase_if(groups_, [](const auto& item) { return !item.first.starts_with(L"pulse:"); });
    std::erase_if(session_, [](const auto& item) { return !item.first.starts_with(L"pulse:"); });
}

GroupBy DefaultGroupFor(const std::wstring& path) {
    const std::wstring key = GroupFolderKey(path);
    if (key.empty()) return GroupBy::None;
    // Recycle bin: mtime is the deleted time, so this groups by deletion date.
    if (key == L"pulse:recent" || key == L"pulse:recycle") return GroupBy::Date;
    const std::wstring& downloads = DownloadsFolder();
    if (!downloads.empty() &&
        CompareStringOrdinal(key.c_str(), static_cast<int>(key.size()),
                             downloads.c_str(), static_cast<int>(downloads.size()), TRUE) == CSTR_EQUAL)
        return GroupBy::Date;
    return GroupBy::None;
}

} // namespace pulse::app
