// fs_enum.h — Directory enumeration: NtQueryDirectoryFile primary,
// FindFirstFileExW+LARGE_FETCH fallback. No Shell COM, no reparse following.
#pragma once
#include <atomic>
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace pulse::fs {

enum class LinkKind { None, SymbolicLink, Junction };

inline LinkKind ClassifyLink(DWORD attrs, DWORD reparse_tag) {
    if (!(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return LinkKind::None;
    if (reparse_tag == IO_REPARSE_TAG_SYMLINK) return LinkKind::SymbolicLink;
    if (reparse_tag == IO_REPARSE_TAG_MOUNT_POINT) return LinkKind::Junction;
    return LinkKind::None;
}

// Fields that only virtual views, the recycle bin, change history, shortcuts
// and "This PC" rows set, held out of line so an ordinary row stays small.
struct DirEntryExtra {
    std::wstring full_path;
    std::wstring recycle_path;
    std::wstring change_type_text;
    std::wstring change_old_path;
    std::wstring link_target;
    std::wstring link_destination;
    uint64_t link_target_size = 0;
    FILETIME link_target_mtime{};
    uint64_t drive_total = 0;
    uint64_t drive_free = 0;
    std::wstring drive_fs;
    std::wstring shell_id;
};

inline const DirEntryExtra kNoDirEntryExtra{};

// Owns a DirEntryExtra and copies it with the entry.
class DirEntryExtraPtr {
public:
    DirEntryExtraPtr() = default;
    DirEntryExtraPtr(const DirEntryExtraPtr& other) : p_(Clone(other)) {}
    DirEntryExtraPtr(DirEntryExtraPtr&&) noexcept = default;
    DirEntryExtraPtr& operator=(const DirEntryExtraPtr& other) {
        if (this != &other) p_ = Clone(other);
        return *this;
    }
    DirEntryExtraPtr& operator=(DirEntryExtraPtr&&) noexcept = default;
    const DirEntryExtra& Get() const { return p_ ? *p_ : kNoDirEntryExtra; }
    DirEntryExtra& Mutable() {
        if (!p_) p_ = std::make_unique<DirEntryExtra>();
        return *p_;
    }
    explicit operator bool() const { return p_ != nullptr; }

private:
    static std::unique_ptr<DirEntryExtra> Clone(const DirEntryExtraPtr& other) {
        return other.p_ ? std::make_unique<DirEntryExtra>(*other.p_) : nullptr;
    }
    std::unique_ptr<DirEntryExtra> p_;
};

inline bool IsUnsetExtra(const std::wstring& value) { return value.empty(); }
inline bool IsUnsetExtra(uint64_t value) { return value == 0; }
inline bool IsUnsetExtra(const FILETIME& value) { return !value.dwLowDateTime && !value.dwHighDateTime; }

// An out-of-line field keeps its member syntax through an MSVC property:
// reading an unset one yields its default; writing a non-default value
// allocates the record. Mutate a field only by assigning it.
// A read is a call, so a reference into the record does not extend the life
// of a temporary entry: `const auto& p = tab->EntryAt(i).recycle_path;` was
// left pointing at freed memory (还原 found no payload and did nothing).
// Reading a temporary entry therefore yields a copy; a named one, a reference.
#define PULSE_DIR_ENTRY_EXTRA(Type, field)                                          \
    __declspec(property(get = field##_get, put = field##_put)) Type field;          \
    const Type& field##_get() const& { return extra_.Get().field; }                 \
    Type field##_get() const&& { return extra_.Get().field; }                       \
    void field##_put(Type value) {                                                   \
        if (extra_ || !IsUnsetExtra(value)) extra_.Mutable().field = std::move(value); \
    }

struct DirEntry {
    std::wstring name;
    uint64_t size = 0;
    FILETIME mtime{};
    FILETIME ctime{}; // creation time; zero when the source has none (index hits, recycle bin)
    FILETIME atime{}; // last access time; same rule
    DWORD attrs = 0;
    DWORD reparse_tag = 0; // Unknown tags, including cloud placeholders, are not links.
    bool is_dir = false;
    bool is_reparse = false;
    bool cloud_recall = false;
    uint8_t drive_type = 0; // GetDriveTypeW for "This PC" rows, 0 otherwise
    // "This PC" program icon (网盘程序入口, full_path "shell:::{CLSID}"):
    // 0 none, 1 other, 2 Baidu, 3 WPS. Never a file operation source.
    uint8_t pc_launcher = 0;
    // "This PC" phone (full_path pulse:device:<key>, see device_source.h
    // kPcDevice*): 0 none. Never a file operation source.
    uint8_t pc_device = 0;
    // pulse:networks row group (app::GroupBy::Network): 0 none, 1 pinned,
    // 2 mapped network drive, 3 device found on this network, 4 recent.
    uint8_t net_group = 0;
    // pulse:networks device rows (app::LanDeviceKind): 0 none, 1 PC, 2 NAS, 3 Mac.
    uint8_t net_kind = 0;
    bool change_record_only = false; // Historical deleted/moved-out item, never a file operation source.
    bool link_target_is_dir = false;
    bool drive_locked = false; // "This PC" rows: BitLocker volume not unlocked yet
    bool drive_remote_like = false; // "This PC" rows: mapped or user-mode (cloud) mount, see drive_kind.h
    DirEntryExtraPtr extra_;

    PULSE_DIR_ENTRY_EXTRA(std::wstring, full_path)    // set for virtual views (search/tag); empty = parent+name
    PULSE_DIR_ENTRY_EXTRA(std::wstring, recycle_path) // $R payload when listing pulse:recycle; not a .lnk target
    PULSE_DIR_ENTRY_EXTRA(std::wstring, change_type_text)
    PULSE_DIR_ENTRY_EXTRA(std::wstring, change_old_path)
    // Resolved .lnk target (empty = not a link or unresolvable). The fields
    // above always describe the .lnk file itself; these describe the target.
    PULSE_DIR_ENTRY_EXTRA(std::wstring, link_target)
    // Worker-read, immediate destination for display only; may no longer exist.
    // Does not change the entry's identity or shortcut penetration semantics.
    PULSE_DIR_ENTRY_EXTRA(std::wstring, link_destination)
    PULSE_DIR_ENTRY_EXTRA(uint64_t, link_target_size)
    PULSE_DIR_ENTRY_EXTRA(FILETIME, link_target_mtime)
    // "This PC" rows: capacity and bytes available to the user; 0 = unknown
    // (no media). Read on the worker thread with the rest of the listing.
    PULSE_DIR_ENTRY_EXTRA(uint64_t, drive_total)
    PULSE_DIR_ENTRY_EXTRA(uint64_t, drive_free)
    // "This PC" rows: the volume's file system (NTFS, exFAT, ...); empty = unknown.
    PULSE_DIR_ENTRY_EXTRA(std::wstring, drive_fs)
    // Phone rows (pulse:device:): the item's shell parsing name, so the
    // shell host can run Explorer's own verbs on it. Empty elsewhere.
    PULSE_DIR_ENTRY_EXTRA(std::wstring, shell_id)
};

#undef PULSE_DIR_ENTRY_EXTRA
// Listings hold one per row; keep rarely set fields in DirEntryExtra.
static_assert(sizeof(DirEntry) <= 96);

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
void EnumerateDirectory(const std::wstring& path, std::vector<DirEntry>& out, const std::atomic_bool* cancelled = nullptr);
// 显示隐藏共享 (Settings → 网络位置): a server's share list also lists the
// administrative disk shares whose names end in '$' (C$, ADMIN$). Off by default.
void SetShowHiddenShares(bool show);
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
