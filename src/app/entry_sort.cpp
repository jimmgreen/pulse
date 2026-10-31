#include "entry_sort.h"
#include "entry_group.h"
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <numeric>
#include <string_view>

namespace pulse::app {

namespace {

std::wstring_view ExtensionView(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return {};
    return std::wstring_view(name).substr(dot + 1);
}

int ExtensionCompare(const std::wstring& a, const std::wstring& b) {
    const std::wstring_view ea = ExtensionView(a);
    const std::wstring_view eb = ExtensionView(b);
    const size_t n = std::min(ea.size(), eb.size());
    for (size_t i = 0; i < n; ++i) {
        const wint_t ca = std::towlower(ea[i]);
        const wint_t cb = std::towlower(eb[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (ea.size() == eb.size()) return 0;
    return ea.size() < eb.size() ? -1 : 1;
}

int NameCompare(const std::wstring& a, const std::wstring& b) {
    int cmp = StrCmpLogicalW(a.c_str(), b.c_str());
    if (cmp == 0) cmp = wcscmp(a.c_str(), b.c_str());
    return cmp;
}

// StrCmpLogicalW costs about a microsecond per call and dominated large
// sorts. For names made only of these characters, an LCMapStringEx sort key
// (digits as numbers, case ignored) orders exactly as NameCompare does; this
// was checked against StrCmpLogicalW on fuzzed and real directory names.
// Other characters (enclosed or non-ASCII digits, fractions, combining marks,
// fullwidth letters) can order differently, so one such name keeps the whole
// list on NameCompare.
bool KeySafeChar(wchar_t ch) {
    if (ch >= 0x20 && ch < 0x7F) return true;                         // ASCII
    if (ch >= 0x4E00 && ch <= 0x9FFF) return true;                    // CJK unified ideographs
    if (ch >= 0x3400 && ch <= 0x4DBF) return true;                    // CJK extension A
    if (ch >= 0xC0 && ch <= 0x17F && ch != 0xD7 && ch != 0xF7) return true; // Latin letters
    if (ch >= 0x3041 && ch <= 0x3096) return true;                    // hiragana
    if (ch >= 0x30A1 && ch <= 0x30FA) return true;                    // katakana
    if (ch >= 0xAC00 && ch <= 0xD7A3) return true;                    // hangul syllables
    switch (ch) {
    case 0x3000: case 0x3001: case 0x3002: case 0x300A: case 0x300B: case 0x300C: case 0x300D:
    case 0x300E: case 0x300F: case 0x3010: case 0x3011: case 0xFF01: case 0xFF08: case 0xFF09:
    case 0xFF0C: case 0xFF1A: case 0xFF1B: case 0xFF1F: case 0xFF5E: case 0x00B7: case 0x2014:
    case 0x2018: case 0x2019: case 0x201C: case 0x201D: case 0x2026:
        return true;
    default:
        return false;
    }
}

bool NameSortKey(const std::wstring& name, std::string& key) {
    if (name.empty()) { key.clear(); return true; }
    for (const wchar_t ch : name)
        if (!KeySafeChar(ch)) return false;
    constexpr DWORD kFlags = LCMAP_SORTKEY | SORT_DIGITSASNUMBERS | NORM_IGNORECASE;
    const int length = static_cast<int>(name.size());
    char buffer[512];
    // LCMAP_SORTKEY sizes are in bytes.
    int bytes = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, kFlags, name.c_str(), length,
                              reinterpret_cast<LPWSTR>(buffer), sizeof(buffer), nullptr, nullptr, 0);
    if (bytes > 0) { key.assign(buffer, static_cast<size_t>(bytes)); return true; }
    bytes = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, kFlags, name.c_str(), length,
                          nullptr, 0, nullptr, nullptr, 0);
    if (bytes <= 0) return false;
    key.assign(static_cast<size_t>(bytes), '\0');
    return LCMapStringEx(LOCALE_NAME_USER_DEFAULT, kFlags, name.c_str(), length,
                         reinterpret_cast<LPWSTR>(key.data()), bytes, nullptr, nullptr, 0) == bytes;
}

// This PC rows read "Label (C:)"; like File Explorer they stay in drive-letter
// order instead of following the volume labels. With both keys present the
// names compare by key, else through NameCompare.
int ItemNameCompare(const fs::DirEntry& a, const fs::DirEntry& b,
                    const std::string* key_a = nullptr, const std::string* key_b = nullptr) {
    if (a.drive_type != 0 && b.drive_type != 0) {
        const int cmp = _wcsicmp(a.full_path.c_str(), b.full_path.c_str());
        if (cmp != 0) return cmp;
    }
    if (key_a && key_b) {
        const int cmp = key_a->compare(*key_b);
        return cmp != 0 ? cmp : wcscmp(a.name.c_str(), b.name.c_str());
    }
    return NameCompare(a.name, b.name);
}

std::atomic<FolderSortMode> g_folder_sort_mode{FolderSortMode::FoldersFirst};
// -1: no ScopedFolderSortMode on this thread.
thread_local int t_folder_sort_mode = -1;

} // namespace

void SetFolderSortMode(FolderSortMode mode) noexcept {
    g_folder_sort_mode.store(mode, std::memory_order_relaxed);
}

FolderSortMode DefaultFolderSortMode() noexcept {
    return g_folder_sort_mode.load(std::memory_order_relaxed);
}

FolderSortMode CurrentFolderSortMode() noexcept {
    return t_folder_sort_mode >= 0 ? static_cast<FolderSortMode>(t_folder_sort_mode) : DefaultFolderSortMode();
}

ScopedFolderSortMode::ScopedFolderSortMode(FolderSortMode mode) noexcept : previous_(t_folder_sort_mode) {
    t_folder_sort_mode = static_cast<int>(mode);
}

ScopedFolderSortMode::~ScopedFolderSortMode() {
    t_folder_sort_mode = previous_;
}

bool EntryLess(const fs::DirEntry& a, const fs::DirEntry& b,
               ui::SortColumn col, ui::SortDirection dir) {
    if (const EntryGrouping* grouping = CurrentEntryGrouping()) {
        if (const int c = GroupCompare(a, b, grouping->by, grouping->clock, col, dir))
            return c < 0;
    }
    return EntryLess(a, b, col, dir, CurrentFolderSortMode());
}

namespace {

struct SizeKey {
    uint64_t bytes = 0;
    bool known = true;
};

bool Less(const fs::DirEntry& a, const fs::DirEntry& b, ui::SortColumn col,
          ui::SortDirection dir, FolderSortMode folders, const SizeKey* ka, const SizeKey* kb,
          const std::string* na = nullptr, const std::string* nb = nullptr) {
    const bool a_folder = a.is_dir || (!a.link_target.empty() && a.link_target_is_dir);
    const bool b_folder = b.is_dir || (!b.link_target.empty() && b.link_target_is_dir);
    // FoldersFirst pins folders above the direction flip below; FollowDirection
    // feeds the group through it so descending order sends folders down.
    if (a_folder != b_folder) {
        if (folders == FolderSortMode::FoldersFirst) return a_folder;
        if (folders == FolderSortMode::FollowDirection)
            return (dir == ui::SortDirection::Desc) ? !a_folder : a_folder;
    }
    int cmp = 0;
    switch (col) {
    case ui::SortColumn::Name:
        cmp = ItemNameCompare(a, b, na, nb);
        break;
    case ui::SortColumn::Size: {
        // Folder totals come from the size cache (#58); a folder without one
        // follows the rest in either direction.
        if (ka && kb && ka->known != kb->known) return ka->known;
        const uint64_t sa = ka ? ka->bytes : a.size;
        const uint64_t sb = kb ? kb->bytes : b.size;
        if (sa < sb) cmp = -1;
        else if (sa > sb) cmp = 1;
        else cmp = ItemNameCompare(a, b, na, nb);
        break;
    }
    case ui::SortColumn::Mtime:
        cmp = CompareFileTime(&a.mtime, &b.mtime);
        if (cmp == 0) cmp = ItemNameCompare(a, b, na, nb);
        break;
    case ui::SortColumn::Created:
        cmp = CompareFileTime(&a.ctime, &b.ctime);
        if (cmp == 0) cmp = ItemNameCompare(a, b, na, nb);
        break;
    case ui::SortColumn::Accessed:
        cmp = CompareFileTime(&a.atime, &b.atime);
        if (cmp == 0) cmp = ItemNameCompare(a, b, na, nb);
        break;
    case ui::SortColumn::Type: {
        // Drive rows have no extension; their type is the drive kind.
        cmp = a.drive_type != 0 && b.drive_type != 0
            ? static_cast<int>(a.drive_type) - static_cast<int>(b.drive_type)
            : ExtensionCompare(a.name, b.name);
        if (cmp == 0) cmp = ItemNameCompare(a, b, na, nb);
        break;
    }
    case ui::SortColumn::Path:
        cmp = _wcsicmp(a.full_path.c_str(), b.full_path.c_str());
        if (cmp == 0) cmp = ItemNameCompare(a, b, na, nb);
        break;
    }
    if (dir == ui::SortDirection::Desc) cmp = -cmp;
    return cmp < 0;
}

} // namespace

bool EntryLess(const fs::DirEntry& a, const fs::DirEntry& b,
               ui::SortColumn col, ui::SortDirection dir, FolderSortMode folders) {
    return Less(a, b, col, dir, folders, nullptr, nullptr);
}

namespace {

// Shared by SortEntries and SortEntriesBySize: the order EntryLess gives
// (grouping and folder mode included), computed over positions so that a
// throwing tick leaves the rows as they were.
void SortRows(std::vector<fs::DirEntry>& entries, ui::SortColumn col, ui::SortDirection dir,
              const FolderSizeLookup* sizes, const std::function<void()>& tick) {
    const size_t n = entries.size();
    if (n < 2) return;
    std::vector<SizeKey> size_keys;
    if (sizes) {
        size_keys.resize(n);
        std::wstring lower;
        for (size_t i = 0; i < n; ++i) {
            if (tick) tick();
            const fs::DirEntry& e = entries[i];
            if (!e.is_dir || e.drive_type != 0) {
                size_keys[i].bytes = e.size;
                continue;
            }
            lower = e.name;
            for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
            const auto it = sizes->find(lower);
            if (it != sizes->end()) size_keys[i].bytes = it->second;
            else size_keys[i].known = false;
        }
    }
    // All rows compare by name key or none do, so the order stays consistent.
    std::vector<std::string> name_keys(n);
    for (size_t i = 0; i < n; ++i) {
        if (tick) tick();
        if (!NameSortKey(entries[i].name, name_keys[i])) {
            std::vector<std::string>().swap(name_keys);
            break;
        }
    }
    const bool keyed = !name_keys.empty();
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), size_t{0});
    const EntryGrouping* grouping = CurrentEntryGrouping();
    std::vector<int> group_ranks;
    if (grouping && grouping->by != GroupBy::Location) {
        GroupClock clock = grouping->clock;
        clock.date_column = col;
        group_ranks.reserve(n);
        for (const auto& entry : entries) {
            if (tick) tick();
            group_ranks.push_back(GroupRank(entry, grouping->by, clock));
        }
    }
    const FolderSortMode mode = CurrentFolderSortMode();
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        if (tick) tick();
        const fs::DirEntry& a = entries[x];
        const fs::DirEntry& b = entries[y];
        if (grouping) {
            if (const int c = GroupCompareRanks(a, b, grouping->by, col, dir,
                    group_ranks.empty() ? 0 : group_ranks[x], group_ranks.empty() ? 0 : group_ranks[y]))
                return c < 0;
        }
        return Less(a, b, col, dir, mode,
                    sizes ? &size_keys[x] : nullptr, sizes ? &size_keys[y] : nullptr,
                    keyed ? &name_keys[x] : nullptr, keyed ? &name_keys[y] : nullptr);
    });
    std::vector<fs::DirEntry> sorted;
    sorted.reserve(n);
    for (const size_t i : order) sorted.push_back(std::move(entries[i]));
    entries.swap(sorted);
}

} // namespace

void SortEntries(std::vector<fs::DirEntry>& entries, ui::SortColumn col, ui::SortDirection dir,
                 const std::function<void()>& tick) {
    SortRows(entries, col, dir, nullptr, tick);
}

void SortEntriesBySize(std::vector<fs::DirEntry>& entries, ui::SortDirection dir,
                       const FolderSizeLookup& sizes, const std::function<void()>& tick) {
    SortRows(entries, ui::SortColumn::Size, dir, &sizes, tick);
}

uint64_t FolderSizeSignature(const FolderSizeLookup& sizes) {
    uint64_t signature = sizes.size();
    for (const auto& [name, bytes] : sizes) {
        uint64_t x = std::hash<std::wstring>{}(name) ^ (bytes + 0x9E3779B97F4A7C15ull);
        x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 27; x *= 0x94D049BB133111EBull;
        x ^= x >> 31;
        signature += x;
    }
    return signature;
}

} // namespace pulse::app