#pragma once

#include "../common/dialog_bridge_paths.h"

#include <windows.h>
#include <cstdint>
#include <string>

namespace pulse::dialog_broker {

struct ResidualCleanupOptions {
    bool verify_protected = true;
    bool pass_test_flag = false;
    DWORD min_core_age_ms = 60000;
    DWORD uninstaller_timeout_ms = 60000;
};

struct ResidualCleanupResult {
    bool ran_bridge_prune = false;
    bool ran_install_cleanup = false;
    bool install_cleanup_launched = false;
    DWORD install_cleanup_exit_code = 0;
    uint32_t stale_files_removed = 0;
    uint32_t core_dirs_removed = 0;
    bool pending_after = false;
};

// Inspects both Common Files\Pulse\DialogBridge and the active install root to
// determine whether any retired DialogBridge, obsolete AppRuntime, stale .old/.new
// files, or obsolete Core version directories still await cleanup.
bool HasPendingResidualCleanup(const std::wstring& bridge_root,
                               const pulse::dialog_bridge_paths::ActiveBridgePointer& pointer) noexcept;

// Performs one non-intrusive cleanup pass:
// 1. Prunes unlocked *.old.* / *.new.* files and obsolete v\<16-hex>\ Core dirs
//    inside bridge_root (never disturbing active or locked versions or unknown dirs).
// 2. When the migrated install root has retired DialogBridge or obsolete AppRuntime
//    versions (and no active .pulse-setup transaction), runs `<install_root>\uninstall.exe
//    /CLEANUPVERSIONS` silently without killing or disturbing any user process.
ResidualCleanupResult RunResidualCleanupPass(const std::wstring& bridge_root,
                                             const pulse::dialog_bridge_paths::ActiveBridgePointer& pointer,
                                             const ResidualCleanupOptions& options = {},
                                             HANDLE stop_event = nullptr) noexcept;

} // namespace pulse::dialog_broker
