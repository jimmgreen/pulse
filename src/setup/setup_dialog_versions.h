#pragma once
#include "setup_file_locks.h"
#include <string>
#include <vector>

namespace pulse::setup {
struct DialogVersionPlan {
    bool versioned = false;
    bool reused = false;
    std::string digest;
    std::wstring relative_directory;
    std::vector<std::wstring> replacements;
};
bool IsDialogVersionDigest(const std::string& value);
bool IsDialogVersionFile(const std::wstring& relative);
// Old installations may keep a helper at root; a versioned helper takes precedence.
std::wstring InstalledRuntimeFile(const std::wstring& app, const wchar_t* name);
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
DialogCleanupReport CleanupDialogVersionsReport(const std::wstring& app, bool uninstall = false);
// Best effort, bounded housekeeping. Invalid, unknown, reparse and busy trees stay.
// Returns false when at least one managed candidate could not be removed.
bool CleanupDialogVersions(const std::wstring& app, bool uninstall = false);
}
