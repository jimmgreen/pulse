#include "snapshot_patch.h"
#include "entry_sort.h"
#include "../fs/fs_enum.h"
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <unordered_map>

namespace pulse::app {

namespace {

std::wstring ChildPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L'\\' + name;
}

int FindName(const std::vector<fs::DirEntry>& entries, const std::wstring& name) {
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        if (_wcsicmp(entries[static_cast<size_t>(i)].name.c_str(), name.c_str()) == 0)
            return i;
    }
    return -1;
}

void InsertSorted(std::vector<fs::DirEntry>& entries, fs::DirEntry entry,
                  ui::SortColumn col, ui::SortDirection sort_dir) {
    auto it = std::lower_bound(entries.begin(), entries.end(), entry,
        [col, sort_dir](const fs::DirEntry& a, const fs::DirEntry& b) {
            return EntryLess(a, b, col, sort_dir);
        });
    entries.insert(it, std::move(entry));
}

} // namespace

bool FillDirEntry(const std::wstring& dir, const std::wstring& name, fs::DirEntry& out) {
    if (name.empty() || name.find_first_of(L"\\/") != std::wstring::npos) return false;
    const std::wstring full = fs::NormalizePath(ChildPath(dir, name));
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &data))
        return false;
    out = fs::DirEntry{};
    out.name = name;
    out.attrs = data.dwFileAttributes;
    out.is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out.is_reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    out.cloud_recall = (data.dwFileAttributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0;
    out.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    out.mtime = data.ftLastWriteTime;
    return true;
}

NotifyPatch ApplyDirNotify(std::vector<fs::DirEntry>& entries, const std::wstring& folder,
                           const fs::DirNotifyEvent& event,
                           ui::SortColumn col, ui::SortDirection sort_dir) {
    if (event.name.empty() || event.name.find_first_of(L"\\/") != std::wstring::npos)
        return NotifyPatch::NeedFullEnum;

    if (event.action == FILE_ACTION_REMOVED) {
        const int at = FindName(entries, event.name);
        if (at >= 0) entries.erase(entries.begin() + at);
        return NotifyPatch::Applied;
    }

    if (event.action == FILE_ACTION_RENAMED_NEW_NAME) {
        const int at = FindName(entries, event.old_name.empty() ? event.name : event.old_name);
        fs::DirEntry entry;
        if (!FillDirEntry(folder, event.name, entry)) {
            if (at >= 0) entries.erase(entries.begin() + at);
            return NotifyPatch::Applied;
        }
        if (at >= 0) entries.erase(entries.begin() + at);
        const int dup = FindName(entries, event.name);
        if (dup >= 0) entries.erase(entries.begin() + dup);
        InsertSorted(entries, std::move(entry), col, sort_dir);
        return NotifyPatch::Applied;
    }

    if (event.action == FILE_ACTION_ADDED || event.action == FILE_ACTION_MODIFIED) {
        fs::DirEntry entry;
        if (!FillDirEntry(folder, event.name, entry)) {
            const int at = FindName(entries, event.name);
            if (at >= 0) entries.erase(entries.begin() + at);
            return NotifyPatch::Applied;
        }
        const int at = FindName(entries, event.name);
        if (at >= 0) entries.erase(entries.begin() + at);
        InsertSorted(entries, std::move(entry), col, sort_dir);
        return NotifyPatch::Applied;
    }

    return NotifyPatch::NeedFullEnum;
}

namespace {

// Case-insensitive name key, folded per character like _wcsicmp/FindName.
struct FoldedNameHash {
    size_t operator()(const std::wstring& name) const noexcept {
        uint64_t hash = 1469598103934665603ull;
        for (wchar_t c : name) {
            hash ^= static_cast<uint64_t>(std::towlower(c));
            hash *= 1099511628211ull;
        }
        return static_cast<size_t>(hash);
    }
};

struct FoldedNameEqual {
    bool operator()(const std::wstring& a, const std::wstring& b) const noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::towlower(a[i]) != std::towlower(b[i])) return false;
        return true;
    }
};

} // namespace

NotifyPatch ApplyDirNotifyBatch(std::vector<fs::DirEntry>& entries, const std::wstring& folder,
                                const std::vector<fs::DirNotifyEvent>& events,
                                ui::SortColumn col, ui::SortDirection sort_dir) {
    constexpr size_t kSequentialLimit = 4;
    if (events.size() <= kSequentialLimit) {
        for (const auto& event : events) {
            if (ApplyDirNotify(entries, folder, event, col, sort_dir) == NotifyPatch::NeedFullEnum)
                return NotifyPatch::NeedFullEnum;
        }
        return NotifyPatch::Applied;
    }

    // Final state of every touched name: the on-disk name to stat, or empty
    // when the last event removed it. Later events win, as they would in order.
    std::unordered_map<std::wstring, std::wstring, FoldedNameHash, FoldedNameEqual> touched;
    touched.reserve(events.size() * 2);
    for (const auto& event : events) {
        if (event.name.empty() || event.name.find_first_of(L"\\/") != std::wstring::npos)
            return NotifyPatch::NeedFullEnum;
        switch (event.action) {
        case FILE_ACTION_REMOVED:
            touched[event.name].clear();
            break;
        case FILE_ACTION_RENAMED_NEW_NAME:
            if (!event.old_name.empty()) touched[event.old_name].clear();
            touched[event.name] = event.name;
            break;
        case FILE_ACTION_ADDED:
        case FILE_ACTION_MODIFIED:
            touched[event.name] = event.name;
            break;
        default:
            return NotifyPatch::NeedFullEnum;
        }
    }

    const auto less = [col, sort_dir](const fs::DirEntry& a, const fs::DirEntry& b) {
        return EntryLess(a, b, col, sort_dir);
    };
    std::vector<fs::DirEntry> fresh;
    fresh.reserve(touched.size());
    for (const auto& item : touched) {
        if (item.second.empty()) continue;
        fs::DirEntry entry;
        if (FillDirEntry(folder, item.second, entry)) fresh.push_back(std::move(entry));
    }
    std::sort(fresh.begin(), fresh.end(), less);

    std::vector<fs::DirEntry> merged;
    merged.reserve(entries.size() + fresh.size());
    auto next = fresh.begin();
    for (auto& entry : entries) {
        if (touched.find(entry.name) != touched.end()) continue;
        // InsertSorted places a new entry before the first one not less than it.
        while (next != fresh.end() && less(*next, entry)) merged.push_back(std::move(*next++));
        merged.push_back(std::move(entry));
    }
    for (; next != fresh.end(); ++next) merged.push_back(std::move(*next));
    entries = std::move(merged);
    return NotifyPatch::Applied;
}

} // namespace pulse::app
