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

// This PC rows read "Label (C:)"; like File Explorer they stay in drive-letter
// order instead of following the volume labels.
int ItemNameCompare(const fs::DirEntry& a, const fs::DirEntry& b) {
    if (a.drive_type != 0 && b.drive_type != 0) {
        const int cmp = _wcsicmp(a.full_path.c_str(), b.full_path.c_str());
        if (cmp != 0) return cmp;
    }
    return NameCompare(a.name, b.name);
}

std::atomic<FolderSortMode> g_folder_sort_mode{FolderSortMode::FoldersFirst};

} // namespace

void SetFolderSortMode(FolderSortMode mode) noexcept {
    g_folder_sort_mode.store(mode, std::memory_order_relaxed);
}

FolderSortMode CurrentFolderSortMode() noexcept {
    return g_folder_sort_mode.load(std::memory_order_relaxed);
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

struct AudioKey {
    std::wstring value;
    bool known = false;
};

// Which of title / artist / album a sort column reads, or -1 for the rest.
int AudioFieldOf(ui::SortColumn col) noexcept {
    return col == ui::SortColumn::Title ? 0
         : col == ui::SortColumn::Artist ? 1
         : col == ui::SortColumn::Album ? 2 : -1;
}

bool Less(const fs::DirEntry& a, const fs::DirEntry& b, ui::SortColumn col,
          ui::SortDirection dir, FolderSortMode folders, const SizeKey* ka, const SizeKey* kb,
          const AudioKey* aa = nullptr, const AudioKey* ab = nullptr) {
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
        cmp = ItemNameCompare(a, b);
        break;
    case ui::SortColumn::Size: {
        // Folder totals come from the size cache (#58); a folder without one
        // follows the rest in either direction.
        if (ka && kb && ka->known != kb->known) return ka->known;
        const uint64_t sa = ka ? ka->bytes : a.size;
        const uint64_t sb = kb ? kb->bytes : b.size;
        if (sa < sb) cmp = -1;
        else if (sa > sb) cmp = 1;
        else cmp = ItemNameCompare(a, b);
        break;
    }
    case ui::SortColumn::Mtime:
        cmp = CompareFileTime(&a.mtime, &b.mtime);
        if (cmp == 0) cmp = ItemNameCompare(a, b);
        break;
    case ui::SortColumn::Created:
        cmp = CompareFileTime(&a.ctime, &b.ctime);
        if (cmp == 0) cmp = ItemNameCompare(a, b);
        break;
    case ui::SortColumn::Accessed:
        cmp = CompareFileTime(&a.atime, &b.atime);
        if (cmp == 0) cmp = ItemNameCompare(a, b);
        break;
    case ui::SortColumn::Type: {
        // Drive rows have no extension; their type is the drive kind.
        cmp = a.drive_type != 0 && b.drive_type != 0
            ? static_cast<int>(a.drive_type) - static_cast<int>(b.drive_type)
            : ExtensionCompare(a.name, b.name);
        if (cmp == 0) cmp = ItemNameCompare(a, b);
        break;
    }
    case ui::SortColumn::Path:
        cmp = _wcsicmp(a.full_path.c_str(), b.full_path.c_str());
        if (cmp == 0) cmp = ItemNameCompare(a, b);
        break;
    case ui::SortColumn::Title:
    case ui::SortColumn::Artist:
    case ui::SortColumn::Album:
        // A row without that tag has nothing to order by, so it sinks below the
        // tagged ones in either direction and orders among them by name.
        if (aa && ab && aa->known != ab->known) return aa->known;
        if (aa && ab && !aa->value.empty()) {
            cmp = _wcsicmp(aa->value.c_str(), ab->value.c_str());
            if (cmp == 0) cmp = ItemNameCompare(a, b);
        } else {
            cmp = ItemNameCompare(a, b);
        }
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

void SortEntriesBySize(std::vector<fs::DirEntry>& entries, ui::SortDirection dir,
                       const FolderSizeLookup& sizes, const std::function<void()>& tick) {
    const size_t n = entries.size();
    std::vector<SizeKey> keys(n);
    std::wstring lower;
    for (size_t i = 0; i < n; ++i) {
        const fs::DirEntry& e = entries[i];
        if (!e.is_dir || e.drive_type != 0) {
            keys[i].bytes = e.size;
            continue;
        }
        lower = e.name;
        for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
        const auto it = sizes.find(lower);
        if (it != sizes.end()) keys[i].bytes = it->second;
        else keys[i].known = false;
    }
    // Sort positions, not rows: a throwing tick leaves the rows as they were.
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), size_t{0});
    const EntryGrouping* grouping = CurrentEntryGrouping();
    const FolderSortMode mode = CurrentFolderSortMode();
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        if (tick) tick();
        const fs::DirEntry& a = entries[x];
        const fs::DirEntry& b = entries[y];
        if (grouping) {
            if (const int c = GroupCompare(a, b, grouping->by, grouping->clock, ui::SortColumn::Size, dir))
                return c < 0;
        }
        return Less(a, b, ui::SortColumn::Size, dir, mode, &keys[x], &keys[y]);
    });
    std::vector<fs::DirEntry> sorted;
    sorted.reserve(n);
    for (const size_t i : order) sorted.push_back(std::move(entries[i]));
    entries.swap(sorted);
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

void SortEntriesByAudioMeta(std::vector<fs::DirEntry>& entries, ui::SortColumn col,
                            ui::SortDirection dir, const AudioMetaLookup& meta,
                            const std::function<void()>& tick) {
    const int field = AudioFieldOf(col);
    if (field < 0) return;
    const size_t n = entries.size();
    std::vector<AudioKey> keys(n);
    std::wstring lower;
    for (size_t i = 0; i < n; ++i) {
        const fs::DirEntry& e = entries[i];
        if (e.is_dir || e.drive_type != 0) continue;
        // Keyed by name, the same way a folder's known totals are: a listing
        // that sorts by tags is one folder, and DirEntry::full_path is only
        // filled for virtual views.
        lower = e.name;
        for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
        const auto it = meta.find(lower);
        if (it == meta.end()) continue;
        keys[i].value = it->second[static_cast<size_t>(field)];
        keys[i].known = !keys[i].value.empty();
    }
    // Sort positions, not rows, so a throwing tick leaves the rows as they were.
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), size_t{0});
    const EntryGrouping* grouping = CurrentEntryGrouping();
    const FolderSortMode mode = CurrentFolderSortMode();
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        if (tick) tick();
        const fs::DirEntry& a = entries[x];
        const fs::DirEntry& b = entries[y];
        if (grouping) {
            if (const int c = GroupCompare(a, b, grouping->by, grouping->clock, col, dir))
                return c < 0;
        }
        return Less(a, b, col, dir, mode, nullptr, nullptr, &keys[x], &keys[y]);
    });
    std::vector<fs::DirEntry> sorted;
    sorted.reserve(n);
    for (const size_t i : order) sorted.push_back(std::move(entries[i]));
    entries.swap(sorted);
}

uint64_t AudioMetaSignature(const AudioMetaLookup& meta) {
    uint64_t signature = meta.size();
    for (const auto& [name, values] : meta) {
        uint64_t x = std::hash<std::wstring>{}(name);
        for (const auto& value : values) {
            x ^= std::hash<std::wstring>{}(value) + 0x9E3779B97F4A7C15ull + (x << 6) + (x >> 2);
        }
        x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 27; x *= 0x94D049BB133111EBull;
        x ^= x >> 31;
        signature += x;
    }
    return signature;
}

} // namespace pulse::app
