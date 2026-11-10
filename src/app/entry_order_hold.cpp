#include "entry_order_hold.h"
#include "entry_group.h"
#include "entry_sort.h"
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string_view>
#include <unordered_map>

namespace pulse::app {

namespace {

// <0 when a's group is shown before b's; 0 in the same group or ungrouped.
int GroupOrder(const fs::DirEntry& a, const fs::DirEntry& b,
               ui::SortColumn col, ui::SortDirection dir) {
    const EntryGrouping* grouping = CurrentEntryGrouping();
    if (!grouping || grouping->by == GroupBy::None) return 0;
    return GroupCompare(a, b, grouping->by, grouping->clock, col, dir);
}

// towlower, without the CRT call for ASCII (most names are ASCII).
wchar_t FoldChar(wchar_t c) noexcept {
    if (c < 0x80) return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
    return static_cast<wchar_t>(std::towlower(c));
}

// Case-insensitive name key, folded per character like _wcsicmp.
struct FoldedNameHash {
    size_t operator()(std::wstring_view name) const noexcept {
        uint64_t hash = 1469598103934665603ull;
        for (wchar_t c : name) {
            hash ^= static_cast<uint64_t>(FoldChar(c));
            hash *= 1099511628211ull;
        }
        return static_cast<size_t>(hash);
    }
};

struct FoldedNameEqual {
    bool operator()(std::wstring_view a, std::wstring_view b) const noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (FoldChar(a[i]) != FoldChar(b[i])) return false;
        return true;
    }
};

} // namespace

bool SameEntrySortPosition(const fs::DirEntry& a, const fs::DirEntry& b,
                           ui::SortColumn col, ui::SortDirection dir) {
    return !EntryLess(a, b, col, dir) && !EntryLess(b, a, col, dir);
}

bool SameEntryGroup(const fs::DirEntry& a, const fs::DirEntry& b,
                    ui::SortColumn col, ui::SortDirection dir) {
    return GroupOrder(a, b, col, dir) == 0;
}

bool GroupBefore(const fs::DirEntry& a, const fs::DirEntry& b,
                 ui::SortColumn col, ui::SortDirection dir) {
    return GroupOrder(a, b, col, dir) < 0;
}

void InsertAtGroupEnd(std::vector<fs::DirEntry>& entries, fs::DirEntry entry,
                      ui::SortColumn col, ui::SortDirection dir) {
    // Before the first row that sorts after it: its sort position in a sorted
    // list, and a sensible one among rows held out of order.
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const fs::DirEntry& row) {
        return EntryLess(entry, row, col, dir);
    });
    entries.insert(it, std::move(entry));
}

void PlaceEntryHeld(std::vector<fs::DirEntry>& entries, int slot, fs::DirEntry entry,
                    ui::SortColumn col, ui::SortDirection dir) {
    if (slot >= 0 && slot < static_cast<int>(entries.size())) {
        auto& row = entries[static_cast<size_t>(slot)];
        if (SameEntryGroup(row, entry, col, dir)) {
            row = std::move(entry);
            return;
        }
        entries.erase(entries.begin() + slot);
    }
    InsertAtGroupEnd(entries, std::move(entry), col, dir);
}

std::vector<fs::DirEntry> KeepEntryOrder(const std::vector<fs::DirEntry>& shown,
                                         const std::vector<fs::DirEntry>& fresh,
                                         const std::vector<EntryRename>& renames,
                                         ui::SortColumn col, ui::SortDirection dir) {
    // Keys view the names in `fresh`, which outlives the map.
    std::unordered_map<std::wstring_view, size_t, FoldedNameHash, FoldedNameEqual> fresh_at;
    fresh_at.reserve(fresh.size());
    for (size_t i = 0; i < fresh.size(); ++i) fresh_at.emplace(fresh[i].name, i);
    std::unordered_map<std::wstring, std::wstring, FoldedNameHash, FoldedNameEqual> renamed_to;
    for (const auto& rename : renames) renamed_to[rename.old_name] = rename.new_name;

    // Rows that keep their place, in the order shown.
    std::vector<char> used(fresh.size(), 0);
    std::vector<size_t> kept;
    kept.reserve(std::min(shown.size(), fresh.size()));
    for (const auto& row : shown) {
        bool renamed = false;
        auto it = fresh_at.find(row.name);
        if (it == fresh_at.end()) {
            const auto rename = renamed_to.find(row.name);
            if (rename == renamed_to.end()) continue;
            renamed = true;
            it = fresh_at.find(rename->second);
            if (it == fresh_at.end()) continue;
        }
        const size_t at = it->second;
        if (used[at] || !SameEntryGroup(row, fresh[at], col, dir) ||
            (!renamed && !SameEntrySortPosition(row, fresh[at], col, dir))) continue;
        used[at] = 1;
        kept.push_back(at);
    }

    // Everything else is new to this view: fresh is in sort order, so the
    // newcomers merge in at their sort position among the kept rows.
    std::vector<fs::DirEntry> out;
    out.reserve(fresh.size());
    size_t next = 0;
    const auto skip_used = [&] { while (next < fresh.size() && used[next]) ++next; };
    skip_used();
    for (const size_t at : kept) {
        while (next < fresh.size() && EntryLess(fresh[next], fresh[at], col, dir)) {
            out.push_back(fresh[next++]);
            skip_used();
        }
        out.push_back(fresh[at]);
    }
    for (; next < fresh.size(); ++next)
        if (!used[next]) out.push_back(fresh[next]);
    return out;
}

bool SameEntryOrder(const std::vector<fs::DirEntry>& shown,
                    const std::vector<fs::DirEntry>& fresh,
                    ui::SortColumn col, ui::SortDirection dir) {
    if (shown.size() != fresh.size()) return false;
    for (size_t i = 0; i < fresh.size(); ++i) {
        if (shown[i].name != fresh[i].name || !SameEntryGroup(shown[i], fresh[i], col, dir)) return false;
    }
    return true;
}

void FollowHeldRenames(const std::vector<EntryRename>& renames,
                       const std::vector<fs::DirEntry>& fresh,
                       std::vector<std::wstring>& names, std::wstring& focus) {
    if (renames.empty()) return;
    std::unordered_map<std::wstring_view, char, FoldedNameHash, FoldedNameEqual> present;
    present.reserve(fresh.size());
    for (const auto& entry : fresh) present.emplace(entry.name, char{});
    const auto follow = [&](std::wstring& name) {
        if (name.empty() || present.find(name) != present.end()) return;
        // Newest rename first: a row renamed twice follows the latest name.
        for (auto it = renames.rbegin(); it != renames.rend(); ++it) {
            if (!FoldedNameEqual{}(it->old_name, name)) continue;
            if (present.find(it->new_name) != present.end()) name = it->new_name;
            return;
        }
    };
    for (auto& name : names) follow(name);
    follow(focus);
}

void PruneHeldRenames(std::vector<EntryRename>& renames, const std::vector<fs::DirEntry>& fresh) {
    if (renames.empty()) return;
    std::unordered_map<std::wstring_view, char, FoldedNameHash, FoldedNameEqual> present;
    present.reserve(fresh.size());
    for (const auto& entry : fresh) present.emplace(entry.name, char{});
    // A rename still pending leaves its old name on disk.
    renames.erase(std::remove_if(renames.begin(), renames.end(), [&](const EntryRename& rename) {
        return present.find(rename.old_name) == present.end() ||
               FoldedNameEqual{}(rename.old_name, rename.new_name);
    }), renames.end());
    constexpr size_t kMaxHeldRenames = 32;
    if (renames.size() > kMaxHeldRenames)
        renames.erase(renames.begin(), renames.end() - kMaxHeldRenames);
}

} // namespace pulse::app
