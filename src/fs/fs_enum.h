// fs_enum.h — Directory enumeration: NtQueryDirectoryFile primary,
// FindFirstFileExW+LARGE_FETCH fallback. No Shell COM, no reparse following.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

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
};

// pulse:tag: / pulse:search: / pulse:workspace: — not filesystem paths.
bool IsVirtualPath(const std::wstring& path);
bool IsUncPath(const std::wstring& path);

// ---------------------------------------------------------------------------
// Shell namespace paths (portable devices, cloud-drive shell folders).
//
// "This PC" in Explorer is a shell namespace, while Pulse built its drive list
// from GetLogicalDrives(), which only returns letters. Anything the shell adds
// without a letter - a phone over MTP, a cloud drive's folder - was invisible.
//
// Such an item is identified by its Desktop-absolute parsing name, wrapped in a
// virtual scheme: pulse:shell:::{GUID}\... or, for the ones that also have one,
// pulse:shell:C:\path. Parsing names round-trip through SHParseDisplayName, so a
// plain string stays a usable identity and DirEntry, tabs and sessions need no
// binary PIDL.
//
// The "pulse:shell:" prefix is deliberate: bare "shell:" is already a
// command-line form (shell:::{20D04FE0-...}, shell:MyComputerFolder) that
// IsThisPcArgument parses, and the two must not be confused.
// ---------------------------------------------------------------------------
constexpr std::wstring_view kShellPathPrefix = L"pulse:shell:";

bool IsShellPath(const std::wstring& path);
std::wstring MakeShellPath(std::wstring_view parsing_name);

// Parsing name behind a pulse:shell: path; empty when the path is not one.
std::wstring ShellParsingName(const std::wstring& path);

enum class NetStatus { Unknown = 0, Online, Slow, Offline };

// Canonical long-path prefix for Win32 APIs.
std::wstring NormalizePath(std::wstring path);
std::wstring ParentPath(const std::wstring& path);

// "计算图形.dwg.lnk" -> "计算图形.dwg" for display.
std::wstring StripLnkSuffix(const std::wstring& name);

// Enumerate a directory into out. Throws std::runtime_error on failure.
void EnumerateDirectory(const std::wstring& path, std::vector<DirEntry>& out);

// Metadata-only lookup for worker-side updates; never follows the target.
DWORD ReadReparseTag(const std::wstring& path);

} // namespace pulse::fs
