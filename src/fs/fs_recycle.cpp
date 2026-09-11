// fs_recycle.cpp
#include "fs_recycle.h"
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

bool ParseIndexBytes(const BYTE* data, size_t size, RecycleItem& out) {
    if (!data || size < 24) return false;
    uint64_t ver = 0;
    memcpy(&ver, data, 8);
    if (ver == 2) {
        if (size < 28) return false;
        uint64_t file_size = 0;
        memcpy(&file_size, data + 8, 8);
        FILETIME deleted{};
        memcpy(&deleted, data + 16, 8);
        uint32_t nchars = 0;
        memcpy(&nchars, data + 24, 4);
        if (nchars == 0 || nchars > 32768) return false;
        const size_t need = 28ull + static_cast<size_t>(nchars) * 2ull;
        size_t bytes = static_cast<size_t>(nchars) * 2ull;
        if (need > size) {
            if (size <= 28) return false;
            bytes = size - 28;
            nchars = static_cast<uint32_t>(bytes / 2);
        }
        out.original_path.assign(reinterpret_cast<const wchar_t*>(data + 28), nchars);
        while (!out.original_path.empty() && out.original_path.back() == L'\0')
            out.original_path.pop_back();
        out.size = file_size;
        out.deleted = deleted;
        out.name = FileNameOf(out.original_path);
        return !out.original_path.empty() && !out.name.empty();
    }
    if (ver == 1) {
        const size_t maxn = (std::min)((size - 24) / 2, static_cast<size_t>(260));
        const wchar_t* p = reinterpret_cast<const wchar_t*>(data + 24);
        out.original_path.assign(p, wcsnlen(p, maxn));
        uint64_t file_size = 0;
        memcpy(&file_size, data + 8, 8);
        memcpy(&out.deleted, data + 16, 8);
        out.size = file_size;
        out.name = FileNameOf(out.original_path);
        return !out.original_path.empty() && !out.name.empty();
    }
    return false;
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

bool ReadRecycleIndex(const std::wstring& index_path, RecycleItem& out) {
    out = {};
    const std::wstring name = FileNameOf(index_path);
    if (name.size() < 3 || name[0] != L'$' || (name[1] != L'I' && name[1] != L'i'))
        return false;
    out.index_path = index_path;
    HANDLE handle = CreateFileW(index_path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 24 || size.QuadPart > 64 * 1024) {
        CloseHandle(handle);
        return false;
    }
    std::vector<BYTE> buf(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(handle, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr);
    CloseHandle(handle);
    if (!ok || read < 24) return false;
    if (!ParseIndexBytes(buf.data(), read, out)) return false;

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
    }
    return true;
}

bool EnumerateRecycleBinAtRoot(const std::wstring& recycle_root, std::vector<DirEntry>& out) {
    const std::wstring sid = CurrentUserSidString();
    if (sid.empty()) return false; // Never fall back to scanning other users.
    const std::wstring sid_dir = NormalizePath(recycle_root) + L"\\" + sid;
    WIN32_FIND_DATAW index{};
    HANDLE find = FindFirstFileW((sid_dir + L"\\$I*").c_str(), &index);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    do {
        if (index.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        RecycleItem item;
        if (ReadRecycleIndex(sid_dir + L"\\" + index.cFileName, item))
            out.push_back(ToDirEntry(item));
    } while (FindNextFileW(find, &index));
    const DWORD error = GetLastError();
    FindClose(find);
    return error == ERROR_NO_MORE_FILES;
}

void EnumerateRecycleBin(std::vector<DirEntry>& out, RecycleBinInfo* info) {
    out.clear();
    if (info) QueryRecycleBinInfo(*info);
    const DWORD drives = GetLogicalDrives();
    bool complete = drives != 0;
    for (int i = 0; i < 26; ++i) {
        if ((drives & (1u << i)) == 0) continue;
        wchar_t root[4] = { static_cast<wchar_t>(L'A' + i), L':', L'\\', 0 };
        const UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        const std::wstring bin = NormalizePath(std::wstring(root) + L"$Recycle.Bin");
        if (!EnumerateRecycleBinAtRoot(bin, out)) complete = false;
    }
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

} // namespace pulse::fs
