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
std::atomic<MediaSortKeySource> g_media_sort_keys{nullptr};
thread_local const std::wstring* t_media_sort_folder = nullptr;

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

void SetMediaSortKeySource(MediaSortKeySource source) noexcept {
    g_media_sort_keys.store(source, std::memory_order_release);
}

ScopedMediaSortFolder::ScopedMediaSortFolder(const std::wstring& folder) noexcept
    : previous_(t_media_sort_folder) {
    t_media_sort_folder = &folder;
}

ScopedMediaSortFolder::~ScopedMediaSortFolder() {
    t_media_sort_folder = previous_;
}

bool EntryLess(const fs::DirEntry& a, const fs::DirEntry& b,
               ui::SortColumn col, ui::SortDirection dir) {
    if (const EntryGrouping* grouping = CurrentEntryGrouping()) {
        GroupClock clock = grouping->clock;
        clock.folders = CurrentFolderSortMode();
        if (const int c = GroupCompare(a, b, grouping->by, clock, col, dir))
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
          const std::string* na = nullptr, const std::string* nb = nullptr,
          const MediaSortKey* ma = nullptr, const MediaSortKey* mb = nullptr) {
    const bool a_folder = a.is_dir || (!a.link_target.empty() && a.link_target_is_dir);
    const bool b_folder = b.is_dir || (!b.link_target.empty() && b.link_target_is_dir);
    // Folder placement is independent of the sort direction.
    if (a_folder != b_folder) {
        if (folders == FolderSortMode::FoldersFirst) return a_folder;
        if (folders == FolderSortMode::FoldersLast)
            return !a_folder;
    }
    int cmp = 0;
    // This PC rows: its details view heads the date column 使用情况 and the
    // size column 可用. Drives without a known size follow in either direction.
    if (a.drive_type != 0 && b.drive_type != 0 &&
        (col == ui::SortColumn::Mtime || col == ui::SortColumn::Size)) {
        const bool ka_known = a.drive_total > 0, kb_known = b.drive_total > 0;
        if (ka_known != kb_known) return ka_known;
        if (ka_known) {
            const auto key = [col](const fs::DirEntry& e) {
                const uint64_t free = std::min(e.drive_free, e.drive_total);
                return col == ui::SortColumn::Size
                    ? static_cast<double>(free)
                    : static_cast<double>(e.drive_total - free) / static_cast<double>(e.drive_total);
            };
            const double va = key(a), vb = key(b);
            cmp = va < vb ? -1 : va > vb ? 1 : 0;
        }
        if (cmp == 0) cmp = ItemNameCompare(a, b, na, nb);
        if (dir == ui::SortDirection::Desc) cmp = -cmp;
        return cmp < 0;
    }
    if (col >= ui::SortColumn::MediaTaken) {
        if (ma && mb) {
            if (ma->known != mb->known) return ma->known;
            if (ma->known) {
                if (!ma->text.empty() || !mb->text.empty()) cmp = NameCompare(ma->text, mb->text);
                if (cmp == 0) cmp = ma->number < mb->number ? -1 : ma->number > mb->number ? 1 : 0;
            }
        }
        if (cmp == 0) cmp = ItemNameCompare(a, b, na, nb);
        if (dir == ui::SortDirection::Desc) cmp = -cmp;
        return cmp < 0;
    }
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
    default:  // Media*: above
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
              const FolderSizeLookup* sizes, const std::function<void()>& tick, size_t keep = SIZE_MAX) {
    const size_t n = entries.size();
    if (keep == 0) { entries.clear(); return; }
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
    std::vector<MediaSortKey> media_keys;
    if (col >= ui::SortColumn::MediaTaken && t_media_sort_folder) {
        if (const MediaSortKeySource source = g_media_sort_keys.load(std::memory_order_acquire))
            source(*t_media_sort_folder, entries, col, media_keys);
        if (media_keys.size() != n) media_keys.clear();
    }
    const bool media = !media_keys.empty();
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), size_t{0});
    const EntryGrouping* grouping = CurrentEntryGrouping();
    const FolderSortMode mode = CurrentFolderSortMode();
    GroupClock clock = grouping ? grouping->clock : GroupClock{};
    clock.date_column = col;
    clock.folders = mode;
    std::vector<int> group_ranks;
    if (grouping) {
        group_ranks.reserve(n);
        for (const auto& entry : entries) {
            if (tick) tick();
            group_ranks.push_back(GroupRank(entry, grouping->by, clock));
        }
    }
    const auto less = [&](size_t x, size_t y) {
        if (tick) tick();
        const fs::DirEntry& a = entries[x];
        const fs::DirEntry& b = entries[y];
        if (grouping) {
            if (const int c = GroupCompareRanks(a, b, grouping->by, col, dir,
                    group_ranks.empty() ? 0 : group_ranks[x], group_ranks.empty() ? 0 : group_ranks[y], clock))
                return c < 0;
        }
        return Less(a, b, col, dir, mode,
                    sizes ? &size_keys[x] : nullptr, sizes ? &size_keys[y] : nullptr,
                    keyed ? &name_keys[x] : nullptr, keyed ? &name_keys[y] : nullptr,
                    media ? &media_keys[x] : nullptr, media ? &media_keys[y] : nullptr);
    };
    keep = (std::min)(keep, n);
    if (keep < n) {
        std::partial_sort(order.begin(), order.begin() + keep, order.end(), less);
        order.resize(keep);
    } else std::sort(order.begin(), order.end(), less);
    std::vector<fs::DirEntry> sorted;
    sorted.reserve(keep);
    for (const size_t i : order) sorted.push_back(std::move(entries[i]));
    entries.swap(sorted);
}

} // namespace

void SortEntries(std::vector<fs::DirEntry>& entries, ui::SortColumn col, ui::SortDirection dir,
                 const std::function<void()>& tick) {
    SortRows(entries, col, dir, nullptr, tick);
}

void SortFirstEntries(std::vector<fs::DirEntry>& entries, ui::SortColumn col, ui::SortDirection dir,
                      size_t keep, const std::function<void()>& tick) {
    SortRows(entries, col, dir, nullptr, tick, keep);
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
