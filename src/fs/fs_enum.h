// fs_enum.h — Directory enumeration: NtQueryDirectoryFile primary,
// FindFirstFileExW+LARGE_FETCH fallback. No Shell COM, no reparse following.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::fs {

struct DirEntry {
    std::wstring name;
    uint64_t size = 0;
    FILETIME mtime{};
    DWORD attrs = 0;
    bool is_dir = false;
    bool is_reparse = false;
    bool cloud_recall = false;
    std::wstring full_path; // set for virtual views (search/tag); empty = parent+name
    std::wstring recycle_path; // $R payload when listing pulse:recycle; not a .lnk target
    bool change_record_only = false; // Historical deleted/moved-out item, never a file operation source.
    std::wstring change_type_text;
    std::wstring change_old_path;
    // Resolved .lnk target (empty = not a link or unresolvable). The fields
    // above always describe the .lnk file itself; these describe the target.
    std::wstring link_target;
    uint64_t link_target_size = 0;
    FILETIME link_target_mtime{};
    bool link_target_is_dir = false;
};

// pulse:tag: / pulse:search: / pulse:workspace: — not filesystem paths.
bool IsVirtualPath(const std::wstring& path);
bool IsUncPath(const std::wstring& path);

enum class NetStatus { Unknown = 0, Online, Slow, Offline };

// Canonical long-path prefix for Win32 APIs.
std::wstring NormalizePath(std::wstring path);
std::wstring ParentPath(const std::wstring& path);

// "计算图形.dwg.lnk" -> "计算图形.dwg" for display.
std::wstring StripLnkSuffix(const std::wstring& name);

// Enumerate a directory into out. Throws std::runtime_error on failure.
void EnumerateDirectory(const std::wstring& path, std::vector<DirEntry>& out);

} // namespace pulse::fs
