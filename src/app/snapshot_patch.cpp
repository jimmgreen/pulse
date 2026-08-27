#include "snapshot_patch.h"
#include "entry_sort.h"
#include "../fs/fs_enum.h"
#include <algorithm>
#include <cwctype>

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

} // namespace pulse::app
