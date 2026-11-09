#pragma once
// Uninstaller (P3): port of [UninstallRun], InitializeUninstall and
// CurUninstallStepChanged in installer/PulseSetup.iss. {app}\uninstall.exe is
// the setup bootstrapper without its payload; it runs this engine.

#include "setup_options.h"

#include <functional>
#include <string>
#include <vector>

namespace pulse::setup {

inline constexpr wchar_t kFileManifestName[] = L".pulse-files";

struct UninstallRequest {
    Options options;
    std::wstring app_dir;       // directory of uninstall.exe
    bool cleanup_data = false;  // CleanupUserData: settings, caches and index data
};

struct UninstallCallbacks {
    std::function<void(int percent, const std::wstring& item)> progress;
    // Pulse reported a running copy/move: true = ask again, false = give up.
    std::function<bool()> busy;
};

struct UninstallResult {
    int exit_code = 0;
    std::wstring error;
    // pulse_integration.exe --restore result (0 ok, 1 incomplete, other failed);
    // -1 when it did not run (upgrade, test mode).
    int restore_result = -1;
    bool reboot_needed = false;     // a locked file is deleted at the next restart
    bool dialog_cleanup_pending = false; // occupied/unverified version directories retained, no reboot promise
    bool schedule_self_delete = false;
    bool nothing_changed = false;   // failed or gave up before anything was removed
};

// True when {app_dir} looks like a Pulse installation this uninstaller may remove.
bool IsPulseInstallDir(const std::wstring& app_dir);
bool IndexServiceInstalled();
UninstallResult RunUninstall(const UninstallRequest& request, const UninstallCallbacks& callbacks);
// After the process exits: deletes uninstall.exe and the then-empty {app}.
void ScheduleSelfDelete(const std::wstring& self_path, const std::wstring& app_dir);

// Installed-file list written by the installer (relative paths, UTF-8 lines).
bool WriteFileManifest(const std::wstring& app_dir, const std::vector<std::wstring>& files);

}  // namespace pulse::setup
