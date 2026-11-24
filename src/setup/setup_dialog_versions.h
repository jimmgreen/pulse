#pragma once
#include "setup_file_locks.h"
#include <string>
#include <vector>

namespace pulse::setup {
struct AppRuntimeVersionPlan {
    bool versioned = false;
    unsigned machine = 0;
    std::string digest;
    std::wstring relative_directory;
};
struct DialogVersionPlan {
    bool versioned = false;
    bool reused = false;
    unsigned machine = 0;
    std::string digest;
    std::string version;
    std::wstring relative_directory;
    std::vector<std::wstring> replacements;
    std::vector<DialogVersionPlan> companions;
    AppRuntimeVersionPlan app_runtime;
};
bool IsDialogVersionDigest(const std::string& value);
bool IsDialogVersionFile(const std::wstring& relative);
bool IsAppRuntimeVersionFile(const std::wstring& relative);
bool IsDialogBridgeStageFile(const std::wstring& relative);
// Old installations may keep a helper at root; a versioned helper takes precedence.
std::wstring InstalledRuntimeFile(const std::wstring& app, const wchar_t* name);
// Best-effort helper lookup for uninstall cleanup when the current bundle is polluted or damaged.
std::wstring UninstallRuntimeFile(const std::wstring& app, const wchar_t* name);
// Validates both the staged bundle and any existing immutable destination.
// All verification leases are released before the file transaction starts.
bool PrepareDialogVersion(const std::wstring& app, const std::wstring& staging,
    std::vector<std::wstring>& installed_files, DialogVersionPlan& plan, std::wstring& error);
bool PublishDialogVersion(const std::wstring& app, const std::wstring& staging,
    const DialogVersionPlan& plan, std::wstring& error);
struct DialogCleanupEntry {
    std::wstring directory;
    std::wstring reason;
    DWORD error = ERROR_SUCCESS;
};
struct DialogCleanupReport {
    std::vector<std::wstring> deleted_directories;
    std::vector<std::wstring> protected_directories;
    std::vector<DialogCleanupEntry> retained_directories;
    std::vector<DialogCleanupEntry> blocked_directories;
    FileLockReport locks;
    bool scan_complete = true;
    bool Complete() const { return scan_complete && retained_directories.empty() && blocked_directories.empty(); }
    bool Retryable() const { return !blocked_directories.empty(); }
};
// Never terminates an owner. Calling again after owners exit retries retained locks.
// identify_owners asks Restart Manager which programs hold blocked versions; it
// costs ~0.2 s per file (minutes for several busy bundles), so pass it only when
// the owners will be shown to the user.
DialogCleanupReport CleanupDialogVersionsReport(const std::wstring& app, bool uninstall = false,
                                                bool identify_owners = true);
DialogCleanupReport CleanupAppRuntimeVersionsReport(const std::wstring& app, bool uninstall = false,
                                                    bool identify_owners = true);
// Best effort, bounded housekeeping. Invalid, unknown, reparse and busy trees stay.
// Returns false when at least one managed candidate could not be removed.
bool CleanupDialogVersions(const std::wstring& app, bool uninstall = false);
// Serializes version publication with deferred cleanup: a version published but
// not yet named by its pointer would otherwise look retired. The lock file sits
// in the admin-only install root and disappears with its last handle.
class VersionLock {
public:
    VersionLock() = default;
    VersionLock(const VersionLock&) = delete;
    VersionLock& operator=(const VersionLock&) = delete;
    ~VersionLock();
    // ERROR_SUCCESS, ERROR_SHARING_VIOLATION after wait_ms, or the open failure.
    DWORD Acquire(const std::wstring& app, DWORD wait_ms);
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
// Retries retired dialog and application runtime versions after an install left
// them behind. Never waits for an install and never runs during recovery.
// Returns kExitOk, kExitCleanupRetained or kExitCleanupBusy.
int RunDeferredVersionCleanup(const std::wstring& app);
}
