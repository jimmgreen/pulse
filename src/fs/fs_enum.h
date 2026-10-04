// fs_enum.h — Directory enumeration: NtQueryDirectoryFile primary,
// FindFirstFileExW+LARGE_FETCH fallback. No Shell COM, no reparse following.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::fs {

// FILE_ATTRIBUTE_REPARSE_POINT alone cannot tell a symlink from a junction, or
// from a cloud placeholder / dedup / AppExecLink entry that must keep the plain
// folder or file presentation. Only the reparse tag distinguishes them, and
// only the tag read gives Symlink or Junction; everything else is Other.
enum class ReparseKind : uint8_t { None = 0, Symlink, Junction, Other };

// Symlinks and junctions are displayed like .lnk shortcuts: type label, link
// icon overlay, and the resolved target as the penetrated path.
inline bool IsLinkReparse(ReparseKind kind) {
    return kind == ReparseKind::Symlink || kind == ReparseKind::Junction;
}

// IO_REPARSE_TAG_SYMLINK -> Symlink, IO_REPARSE_TAG_MOUNT_POINT -> Junction,
// anything else -> Other.
ReparseKind KindFromTag(uint32_t tag);

struct DirEntry {
    std::wstring name;
    uint64_t size = 0;
    FILETIME mtime{};
    FILETIME ctime{}; // creation time; zero when the source has none (index hits, recycle bin)
    FILETIME atime{}; // last access time; same rule
    DWORD attrs = 0;
    bool is_dir = false;
    bool is_reparse = false;
    // Filled by the worker's reparse pass (link_resolve.cpp), not by
    // enumeration: the tag needs a handle open with FSCTL_GET_REPARSE_POINT.
    ReparseKind reparse_kind = ReparseKind::None;
    bool cloud_recall = false;
    uint8_t drive_type = 0; // GetDriveTypeW for "This PC" rows, 0 otherwise
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
    // "This PC" rows: capacity and bytes available to the user; 0 = unknown
    // (no media). Read on the worker thread with the rest of the listing.
    uint64_t drive_total = 0;
    uint64_t drive_free = 0;
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
