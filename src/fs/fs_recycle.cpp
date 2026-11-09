// fs_recycle.cpp
#include "fs_recycle.h"
#include "drive_kind.h"
#include "../common/recycle_index.h"
#include "../common/current_user_security.h"
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

namespace pulse::fs {
namespace {

std::wstring FileNameOf(const std::wstring& path) {
    std::wstring_view view = path;
    while (view.size() > 1 && (view.back() == L'\\' || view.back() == L'/'))
        view.remove_suffix(1);
    const size_t slash = view.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? std::wstring(view)
                                            : std::wstring(view.substr(slash + 1));
}

DirEntry ToDirEntry(const RecycleItem& item) {
    DirEntry entry;
    entry.name = item.name;
    entry.size = item.size;
    entry.mtime = item.deleted;
    entry.is_dir = item.is_dir;
    entry.attrs = item.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    entry.full_path = NormalizePath(item.original_path);
    entry.recycle_path = item.content_path;
    return entry;
}

} // namespace

bool QueryRecycleBinInfo(RecycleBinInfo& out) {
    out = {};
    SHQUERYRBINFO info{};
    info.cbSize = sizeof(info);
    if (FAILED(SHQueryRecycleBinW(nullptr, &info))) return false;
    out.bytes = static_cast<uint64_t>(info.i64Size);
    out.items = static_cast<uint64_t>(info.i64NumItems);
    out.valid = true;
    return true;
}

std::wstring RecycleIndexPath(const std::wstring& content_path) {
    const size_t slash = content_path.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash + 2 >= content_path.size()) return {};
    std::wstring name = content_path.substr(slash + 1);
    if (name.size() < 3 || name[0] != L'$' || (name[1] != L'R' && name[1] != L'r')) return {};
    name[1] = (name[1] == L'R') ? L'I' : L'i';
    return content_path.substr(0, slash + 1) + name;
}

// A folder whose $I record holds no total (some tools leave it 0): add up
// what is inside, bounded so a huge tree cannot stall the listing.
uint64_t RecycledFolderBytes(const std::wstring& dir, int& budget, const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) return 0;
    uint64_t total = 0;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (--budget < 0 || (cancelled && cancelled())) break;
        const std::wstring_view name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                total += RecycledFolderBytes(dir + L"\\" + data.cFileName, budget, cancelled);
        } else {
            total += (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return total;
}

static bool ReadRecycleIndexCancellable(const std::wstring& index_path, RecycleItem& out,
    const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) return false;
    out = {};
    const std::wstring name = FileNameOf(index_path);
    if (name.size() < 3 || name[0] != L'$' || (name[1] != L'I' && name[1] != L'i'))
        return false;
    out.index_path = index_path;
    pulse::recycle::IndexRecord record;
    if (!pulse::recycle::ReadIndex(index_path, record)) return false;
    out.original_path = std::move(record.original_path);
    out.size = record.size;
    out.deleted = record.deleted;
    out.name = FileNameOf(out.original_path);
    if (out.name.empty()) return false;

    std::wstring r_name = FileNameOf(index_path);
    if (r_name.size() >= 2) r_name[1] = (r_name[1] == L'I') ? L'R' : L'r';
    const size_t slash = index_path.find_last_of(L'\\');
    out.content_path = (slash == std::wstring::npos)
        ? r_name : index_path.substr(0, slash + 1) + r_name;
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (!GetFileAttributesExW(out.content_path.c_str(), GetFileExInfoStandard, &attrs))
        return false; // An orphan $I record is not a restorable recycle item.
    out.is_dir = (attrs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!out.is_dir && out.size == 0) {
        ULARGE_INTEGER bytes;
        bytes.HighPart = attrs.nFileSizeHigh;
        bytes.LowPart = attrs.nFileSizeLow;
        out.size = bytes.QuadPart;
    } else if (out.is_dir && out.size == 0) {
        int budget = 20000;
        out.size = RecycledFolderBytes(out.content_path, budget, cancelled);
    }
    return true;
}

bool ReadRecycleIndex(const std::wstring& index_path, RecycleItem& out) {
    return ReadRecycleIndexCancellable(index_path, out, {});
}

bool EnumerateRecycleBinAtRoot(const std::wstring& recycle_root, std::vector<DirEntry>& out,
    const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) return false;
    const std::wstring sid = CurrentUserSidString();
    if (sid.empty()) return false; // Never fall back to scanning other users.
    const std::wstring normalized = NormalizePath(recycle_root);
    if (normalized.empty()) return false;
    const std::wstring sid_dir = normalized + L"\\" + sid;
    WIN32_FIND_DATAW index{};
    HANDLE find = FindFirstFileW((sid_dir + L"\\$I*").c_str(), &index);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    do {
        if (cancelled && cancelled()) { FindClose(find); return false; }
        if (index.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        RecycleItem item;
        if (ReadRecycleIndexCancellable(sid_dir + L"\\" + index.cFileName, item, cancelled))
            out.push_back(ToDirEntry(item));
    } while (FindNextFileW(find, &index));
    const DWORD error = GetLastError();
    FindClose(find);
    return error == ERROR_NO_MORE_FILES;
}

bool EnumerateRecycleBinAtRoot(const std::wstring& recycle_root, std::vector<DirEntry>& out) {
    return EnumerateRecycleBinAtRoot(recycle_root, out, {});
}

void EnumerateRecycleBin(std::vector<DirEntry>& out, RecycleBinInfo* info,
    const std::function<bool()>& cancelled) {
    out.clear();
    if (cancelled && cancelled()) { if (info) *info = {}; return; }
    if (info) QueryRecycleBinInfo(*info);
    const DWORD drives = GetLogicalDrives();
    bool complete = drives != 0;
    for (int i = 0; i < 26; ++i) {
        if (cancelled && cancelled()) { out.clear(); if (info) *info = {}; return; }
        if ((drives & (1u << i)) == 0) continue;
        wchar_t root[4] = { static_cast<wchar_t>(L'A' + i), L':', L'\\', 0 };
        const UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        if (IsRemoteLikeDrive(root)) continue;  // cloud mount: no local bin to scan
        const std::wstring bin = NormalizePath(std::wstring(root) + L"$Recycle.Bin");
        if (!EnumerateRecycleBinAtRoot(bin, out, cancelled)) complete = false;
    }
    if (cancelled && cancelled()) { out.clear(); if (info) *info = {}; return; }
    if (!info) return;
    uint64_t enum_bytes = 0;
    for (const auto& entry : out) enum_bytes += entry.size;
    const uint64_t enum_items = out.size();
    // Use the same live, current-user items as the list, including after clear.
    // If a volume could not be read, retain Shell's occupancy instead.
    if (complete) {
        info->valid = true;
        info->items = enum_items;
        info->bytes = enum_bytes;
        return;
    }
}

void EnumerateRecycleBin(std::vector<DirEntry>& out, RecycleBinInfo* info) {
    EnumerateRecycleBin(out, info, {});
}

} // namespace pulse::fs
