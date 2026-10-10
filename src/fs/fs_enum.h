// fs_enum.h — Directory enumeration: NtQueryDirectoryFile primary,
// FindFirstFileExW+LARGE_FETCH fallback. No Shell COM, no reparse following.
#pragma once
#include <atomic>
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace pulse::fs {

enum class LinkKind { None, SymbolicLink, Junction };

inline LinkKind ClassifyLink(DWORD attrs, DWORD reparse_tag) {
    if (!(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return LinkKind::None;
    if (reparse_tag == IO_REPARSE_TAG_SYMLINK) return LinkKind::SymbolicLink;
    if (reparse_tag == IO_REPARSE_TAG_MOUNT_POINT) return LinkKind::Junction;
    return LinkKind::None;
}

struct DirEntry {
    std::wstring name;
    uint64_t size = 0;
    FILETIME mtime{};
    FILETIME ctime{}; // creation time; zero when the source has none (index hits, recycle bin)
    FILETIME atime{}; // last access time; same rule
    DWORD attrs = 0;
    bool is_dir = false;
    bool is_reparse = false;
    DWORD reparse_tag = 0; // Unknown tags, including cloud placeholders, are not links.
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
    // Worker-read, immediate destination for display only; may no longer exist.
    // Does not change the entry's identity or shortcut penetration semantics.
    std::wstring link_destination;
    uint64_t link_target_size = 0;
    FILETIME link_target_mtime{};
    bool link_target_is_dir = false;
    // "This PC" rows: capacity and bytes available to the user; 0 = unknown
    // (no media). Read on the worker thread with the rest of the listing.
    uint64_t drive_total = 0;
    uint64_t drive_free = 0;
    bool drive_locked = false; // "This PC" rows: BitLocker volume not unlocked yet
};

// pulse:tag: / pulse:search: / pulse:workspace: — not filesystem paths.
bool IsVirtualPath(const std::wstring& path);
// ::{GUID} parsing names and shell: links (the desktop's Recycle Bin, a pinned
// Explorer button) are shell namespaces, not folders; never normalize them.
bool IsShellNamespacePath(const std::wstring& path);
bool IsUncPath(const std::wstring& path);

enum class NetStatus { Unknown = 0, Online, Slow, Offline };

// Canonical long-path prefix for Win32 APIs.
std::wstring NormalizePath(std::wstring path);
std::wstring ParentPath(const std::wstring& path);

// "计算图形.dwg.lnk" -> "计算图形.dwg" for display.
std::wstring StripLnkSuffix(const std::wstring& name);

// Enumerate a directory into out. Throws std::runtime_error on failure.
void EnumerateDirectory(const std::wstring& path, std::vector<DirEntry>& out, const std::atomic_bool* cancelled = nullptr);
struct EnumerationOptions {
    std::function<bool()> cancelled;
    DWORD timeout_ms = 15000;
};
// Bounded caller wait. Retired I/O retains its own buffers until the provider
// completes; callbacks are consulted only by the caller, never by retired work.
void EnumerateDirectory(const std::wstring& path, std::vector<DirEntry>& out, const EnumerationOptions& options);

// Metadata-only lookup for worker-side updates; never follows the target.
DWORD ReadReparseTag(const std::wstring& path);

} // namespace pulse::fs
