#pragma once

#include "index_config.h"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <windows.h>

namespace pulse::index {

enum class VolumeFsTag : uint8_t {
    UnknownOrNtfs = 0,
    Fat32 = 1,
    ExFat = 2,
    Fat = 3,
};

VolumeFsTag FsTagFromName(std::wstring_view file_system);
std::wstring_view FsNameFromTag(VolumeFsTag tag, VolumeBackend backend);
std::wstring ToWin32LongPath(std::wstring_view path);

uint16_t PackDiskVolKind(VolumeKind kind, VolumeBackend backend, VolumeFsTag fs_tag);
void UnpackDiskVolMeta(uint16_t raw_kind, uint64_t journal_id, int64_t next_usn,
                       VolumeKind& kind, VolumeBackend& backend,
                       VolumeFsTag& fs_tag, uint32_t& serial);

bool VolumeIdentityMatches(std::wstring_view drive_id, std::wstring_view drive_fs,
                           uint32_t drive_serial, std::wstring_view state_id,
                           VolumeBackend state_backend, VolumeFsTag state_fs_tag,
                           uint32_t state_serial);

struct WalkCallbacks {
    std::function<bool()> should_cancel;
    std::function<bool(bool parent_is_root, std::wstring_view name)> should_skip_name;
    std::function<bool(std::wstring_view full_path)> is_excluded_path;
    std::function<bool(std::wstring_view dir_path)> may_contain_exclusion;
    std::function<int32_t(int32_t parent_id, std::wstring_view name, bool is_dir,
                          bool hidden, uint64_t size, uint64_t mtime_ft)> emit_node;
    std::function<void(size_t total_added)> on_progress;
};

struct WalkStats {
    size_t added = 0;
    size_t skipped_dirs = 0;
    DWORD root_error = ERROR_SUCCESS;
    bool cancelled = false;
    bool hit_cap = false;
};

// Recursively scans `root_path` using NtQueryDirectoryFile (with FindFirstFileExW fallback)
// without a depth cap, skipping reparse-point subtrees and inaccessible subdirectories.
bool WalkDirectoryTree(const std::wstring& root_path, int32_t root_id,
                       bool root_is_volume_root, size_t max_items,
                       const WalkCallbacks& cb, WalkStats* out_stats = nullptr);

} // namespace pulse::index
