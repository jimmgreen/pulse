#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../../../sdk/native/include/pulse/vfs.h"
#include "vfs_path.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulse::extensions::vfs {

struct VfsEntryData {
    std::wstring name;
    uint64_t size_bytes = 0;
    int64_t modified_unix_ms = 0;
    uint32_t flags = 0;
    std::wstring type_text;
};

struct VfsMountInfo {
    std::string package_id;
    std::string mount_id;
    std::wstring display_name;
    std::wstring icon_glyph;
    uint32_t capabilities = 0;
};

struct VfsMountRegistration {
    std::string package_id;
    std::string mount_id;
    std::wstring display_name;
    std::wstring icon_glyph;
    uint32_t capabilities = 0;
    PulseVfsListDirFn list_dir = nullptr;
    PulseVfsExtractFileFn extract_file = nullptr;
    PulseVfsStatFn stat = nullptr;
    PulseVfsExtractItemsFn extract_items = nullptr;
    PulseVfsModifyFn modify = nullptr;
    void* user = nullptr;
    std::shared_ptr<std::atomic<uint32_t>> session_active_calls;
    std::function<void()> on_call_finished;
};

struct VfsProgressInfo {
    uint64_t completed_bytes = 0;
    uint64_t total_bytes = 0;
    uint64_t completed_items = 0;
    uint64_t total_items = 0;
    std::wstring current;
};

// Callbacks and outcome of a long provider operation (extract_items / modify).
struct VfsOperationHooks {
    std::function<bool()> is_cancelled;
    std::function<void(const VfsProgressInfo&)> progress;  // called on the provider's worker thread
    std::wstring message;                                   // out: provider's reason on failure
};

struct VfsChangeRequest {
    uint32_t kind = PULSE_VFS_CHANGE_PUT;
    std::wstring vfs_path;      // item inside the mount (the mount root for RESTORE)
    std::wstring source_path;   // PUT
    std::wstring new_vfs_path;  // RENAME, same mount
};

FILETIME UnixMsToFileTime(int64_t unix_ms) noexcept;

class VfsRegistry {
public:
    static VfsRegistry& Instance();

    VfsRegistry() = default;
    ~VfsRegistry() = default;
    VfsRegistry(const VfsRegistry&) = delete;
    VfsRegistry& operator=(const VfsRegistry&) = delete;

    PulseStatus RegisterMount(VfsMountRegistration reg, PulseVfsMountHandle& out_handle);
    bool UnregisterMount(PulseVfsMountHandle handle);
    bool ResolveHandle(PulseVfsMountHandle handle,
                       std::string& out_package_id,
                       std::string& out_mount_id) const;
    bool DescribeMount(std::string_view package_id,
                       std::string_view mount_id,
                       VfsMountInfo& out_info) const;
    std::vector<VfsMountInfo> ListMounts() const;

    PulseStatus ListDirectory(std::wstring_view vfs_path,
                              std::vector<VfsEntryData>& out_entries,
                              const std::function<bool()>& is_cancelled = {});
    PulseStatus ExtractFile(std::wstring_view vfs_path,
                            std::wstring_view dest_path,
                            const std::function<bool()>& is_cancelled = {});
    PulseStatus Stat(std::wstring_view vfs_path,
                     VfsEntryData& out_entry,
                     const std::function<bool()>& is_cancelled = {});

    // vfs_paths: items of one folder of one mount; written into the empty directory dest_dir.
    PulseStatus ExtractItems(const std::vector<std::wstring>& vfs_paths,
                             std::wstring_view dest_dir,
                             VfsOperationHooks& hooks);
    // All changes target one mount and land together (or not at all).
    PulseStatus Modify(const std::vector<VfsChangeRequest>& changes, VfsOperationHooks& hooks);
    // PULSE_VFS_CAP_* of the mount holding vfs_path; 0 when there is none.
    uint32_t CapabilitiesOf(std::wstring_view vfs_path) const;
    bool WaitUntilIdle(PulseVfsMountHandle handle, uint32_t timeout_ms = 5000);

private:
    struct MountRecord;
    struct ActiveCallGuard;
    std::shared_ptr<MountRecord> FindRecord(std::string_view package_id,
                                            std::string_view mount_id) const;

    mutable std::mutex mutex_;
    PulseVfsMountHandle next_handle_ = 1;
    std::map<PulseVfsMountHandle, std::shared_ptr<MountRecord>> by_handle_;
    std::map<std::pair<std::string, std::string>, PulseVfsMountHandle> by_key_;
};

} // namespace pulse::extensions::vfs