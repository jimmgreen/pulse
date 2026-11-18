#include "setup_engine.h"
#include "setup_install_handoff.h"
#include "setup_dialog_migration.h"
#include "setup_dialog_versions.h"
#include "setup_runtime_transaction.h"
#include "start_pin_registration.h"

#include "setup_common.h"
#include "setup_log.h"
#include "setup_process.h"
#include "setup_registry.h"
#include "setup_service.h"
#include "setup_shortcut.h"
#include "setup_transaction.h"
#include "setup_uninstall.h"

#include <windows.h>
#include <knownfolders.h>

#include <optional>
#include <algorithm>

namespace pulse::setup {
namespace {

constexpr wchar_t kUninstallerName[] = L"uninstall.exe";
constexpr wchar_t kWinEVerbKey[] =
    L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\\shell\\opennewwindow\\command";
constexpr wchar_t kThisPcCommandKey[] =
    L"Software\\Classes\\CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\open\\command";
constexpr wchar_t kRecycleBinCommandKey[] =
    L"Software\\Classes\\CLSID\\{645FF040-5081-101B-9F08-00AA002F954E}\\shell\\open\\command";

}  // namespace

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

namespace {

const wchar_t* UninstallKey(const Options& o) { return o.test ? kTestUninstallKey : kUninstallKey; }

// ---- ConfiguredIndexPath ---------------------------------------------------

std::wstring ReadJsonString(const std::string& json, const char* key) {
    const std::string marker = std::string("\"") + key + "\"";
    size_t i = json.find(marker);
    if (i == std::string::npos) return {};
    i = json.find(':', i + marker.size());
    if (i == std::string::npos) return {};
    ++i;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) ++i;
    if (i >= json.size() || json[i] != '"') return {};
    std::string out;
    for (++i; i < json.size(); ++i) {
        char c = json[i];
        if (c == '"') {
            const int n = MultiByteToWideChar(CP_UTF8, 0, out.data(), static_cast<int>(out.size()), nullptr, 0);
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, out.data(), static_cast<int>(out.size()), w.data(), n);
            return w;
        }
        if (c == '\\') {
            if (++i >= json.size()) return {};
            c = json[i];
            c = c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c;
        }
        out += c;
    }
    return {};
}

}  // namespace

std::wstring ConfiguredIndexPath() {
    const std::wstring base = KnownFolder(FOLDERID_ProgramData) + L"\\Pulse";
    const std::wstring fallback = base + L"\\Index";
    std::wstring config = base + L"\\index-config.json";
    if (!FileExists(config)) config = base + L"\\Index\\config.json";
    std::string json;
    if (!ReadWholeFile(config, json)) return fallback;
    auto path = ReadJsonString(json, "index_path");
    return path.empty() ? fallback : path;
}

namespace {

// ---- Shell-integration preservation when the install directory moves ------

struct UpgradePrefs {
    bool captured = false;
    std::wstring previous_exe;
    std::optional<std::wstring> startup;
    bool directory = false, drive = false, win_e = false, this_pc = false, recycle_bin = false;

    std::wstring Flags() const {
        std::wstring p;
        if (directory) p += L" --directory";
        if (drive) p += L" --drive";
        if (win_e) p += L" --win-e";
        if (this_pc) p += L" --this-pc";
        if (recycle_bin) p += L" --recycle-bin";
        return p;
    }
};

bool CommandTargetsExe(std::wstring command, const std::wstring& exe) {
    while (!command.empty() && (command.front() == L' ' || command.front() == L'\t')) command.erase(0, 1);
    if (command.empty()) return false;
    std::wstring token;
    if (command[0] == L'"') {
        const auto end = command.find(L'"', 1);
        if (end == std::wstring::npos) return false;
        token = command.substr(1, end - 1);
    } else {
        token = command.substr(0, command.find_first_of(L" \t"));
    }
    return CompareStringOrdinal(token.c_str(), -1, exe.c_str(), -1, TRUE) == CSTR_EQUAL;
}

UpgradePrefs CaptureUpgradePrefs(const std::wstring& previous_exe) {
    UpgradePrefs p;
    p.captured = true;
    p.previous_exe = previous_exe;
    p.startup = ReadString(HKEY_CURRENT_USER, 0, kRunKey, L"Pulse");
    auto targets = [&](const wchar_t* key) {
        auto c = ReadString(HKEY_CURRENT_USER, 0, key, L"");
        return c && CommandTargetsExe(*c, previous_exe);
    };
    p.directory = targets(L"Software\\Classes\\Directory\\shell\\open\\command");
    p.drive = targets(L"Software\\Classes\\Drive\\shell\\open\\command");
    p.win_e = targets(kWinEVerbKey);
    p.this_pc = targets(kThisPcCommandKey);
    p.recycle_bin = targets(kRecycleBinCommandKey);
    return p;
}

std::wstring ReplaceExe(std::wstring command, const std::wstring& old_exe, const std::wstring& new_exe) {
    std::wstring lower = command, lower_old = old_exe;
    CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
    CharLowerBuffW(lower_old.data(), static_cast<DWORD>(lower_old.size()));
    const auto pos = lower.find(lower_old);
    if (pos != std::wstring::npos) command.replace(pos, old_exe.size(), new_exe);
    return command;
}

bool RestoreUpgradePrefs(const UpgradePrefs& p, const std::wstring& app, std::wstring& error) {
    if (!p.captured) return true;
    const std::wstring exe = app + L"\\pulse.exe";
    const auto flags = p.Flags();
    if (flags.empty()) return true;
    const auto r = RunProcess(InstalledRuntimeFile(app, L"pulse_integration.exe"),
                              flags.substr(1) + L" --upgrade-from " + Quote(p.previous_exe) + L" --exe " + Quote(exe),
                              true);
    if (!r.started || r.exit_code != 0) {
        error = L"Shell integration migration failed (" + std::to_wstring(r.exit_code) +
            L"). Both installation directories were kept; the previous version was not removed.";
        Log(error);
        return false;
    }
    const auto migrated = CaptureUpgradePrefs(exe);
    if ((p.directory && !migrated.directory) || (p.drive && !migrated.drive) ||
        (p.win_e && !migrated.win_e) || (p.this_pc && !migrated.this_pc) ||
        (p.recycle_bin && !migrated.recycle_bin)) {
        error = L"Cannot verify migrated shell commands. Both installation directories were kept; the previous version was not removed.";
        Log(error);
        return false;
    }
    return true;
}

// Splits a registered UninstallString into exe and arguments.
bool SplitCommandLine(const std::wstring& command, std::wstring& exe, std::wstring& args) {
    std::wstring c = command;
    while (!c.empty() && c.front() == L' ') c.erase(0, 1);
    if (c.empty()) return false;
    if (c[0] == L'"') {
        const auto end = c.find(L'"', 1);
        if (end == std::wstring::npos) return false;
        exe = c.substr(1, end - 1);
        args = c.substr(end + 1);
    } else {
        const auto sp = c.find(L' ');
        exe = c.substr(0, sp);
        args = sp == std::wstring::npos ? L"" : c.substr(sp + 1);
    }
    return !exe.empty();
}

// ---- Misc ------------------------------------------------------------------

bool WriteUninstaller(const std::wstring& self, uint64_t stub_size, const std::wstring& dest, std::wstring& error) {
    // The uninstaller is this bootstrapper without the payload.
    HANDLE in = CreateFileW(self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (in == INVALID_HANDLE_VALUE) { error = L"Cannot read setup image: " + Win32Error(GetLastError()); return false; }
    std::string image(static_cast<size_t>(stub_size), '\0');
    DWORD read = 0;
    const bool ok = ReadFile(in, image.data(), static_cast<DWORD>(image.size()), &read, nullptr) && read == image.size();
    CloseHandle(in);
    if (!ok) { error = L"Cannot read setup image"; return false; }
    if (!WriteWholeFile(dest, image.data(), image.size())) {
        error = L"Cannot write " + dest + L": " + Win32Error(GetLastError());
        return false;
    }
    return true;
}

// Staged files are flushed once before the commit so that a power loss after
// the journal is removed cannot leave truncated executables behind.
bool FlushTree(const std::wstring& root, const std::vector<std::wstring>& files, std::wstring& error) {
    for (const auto& rel : files) {
        const std::wstring p = root + L"\\" + rel;
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE || !FlushFileBuffers(h)) {
            error = L"Cannot flush " + p + L": " + Win32Error(GetLastError());
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            return false;
        }
        CloseHandle(h);
    }
    return true;
}

// Inno's uninstaller files of a taken-over installation (unins000.exe/.dat/.msg).
void RemoveInnoUninstaller(const std::wstring& app) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((app + L"\\unins???.*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        const auto dot = name.rfind(L'.');
        if (dot == std::wstring::npos) continue;
        const std::wstring ext = name.substr(dot);
        if (!EqualsNoCase(ext, L".exe") && !EqualsNoCase(ext, L".dat") && !EqualsNoCase(ext, L".msg")) continue;
        const std::wstring path = app + L"\\" + name;
        if (!DeleteFileW(path.c_str())) MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        Log(L"Removed Inno uninstaller file " + name);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

struct Ctx {
    const InstallRequest& req;
    const EngineCallbacks& cb;
    void Phase(Phase p, const std::wstring& s) const {
        Log(s);
        if (cb.phase) cb.phase(p, s);
    }
};

InstallResult Fail(int code, std::wstring error) {
    Log(L"FAILED (" + std::to_wstring(code) + L"): " + error);
    InstallResult r;
    r.exit_code = code;
    r.error = std::move(error);
    return r;
}

}  // namespace

InstallRequest ResolveDefaults(const Options& options) {
    InstallRequest r;
    r.options = options;
    const auto previous = FindPreviousInstall(UninstallKey(options));
    if (!options.dir.empty()) {
        r.app_dir = NormalizeDir(ExpandEnv(options.dir));
    } else if (previous && FileExists(previous->dir + L"\\pulse.exe")) {
        r.app_dir = previous->dir;
        Log(L"Reusing registered Pulse directory: " + r.app_dir);
    } else {
        r.app_dir = KnownFolder(FOLDERID_ProgramFiles) + (options.test ? L"\\Pulse Test" : L"\\Pulse");
    }
    if (previous && FileExists(previous->dir + L"\\pulse.exe")) {
        r.upgrade = true;
        r.previous_version = ReadString(previous->where.root, previous->where.view, UninstallKey(options),
                                        L"DisplayVersion").value_or(L"");
    }
    if (previous && previous->tasks) r.tasks = *previous->tasks;  // UsePreviousTasks
    if (options.tasks) ApplyTaskList(*options.tasks, r.tasks, true);
    if (options.merge_tasks) ApplyTaskList(*options.merge_tasks, r.tasks, false);
    if (options.test) r.tasks.index_service = r.tasks.startup = false;
    r.index_path = options.index_path.empty() ? ConfiguredIndexPath() : NormalizeDir(options.index_path);
    return r;
}

InstallResult RunInstall(Payload& payload, const std::wstring& self_path, const InstallRequest& req,
                         const EngineCallbacks& cb) {
    const Ctx ctx{req, cb};
    const Options& o = req.options;
    const std::wstring app = NormalizeDir(req.app_dir);
    const bool real = !o.test;
    ctx.Phase(Phase::Preparing, L"Installing Pulse " + std::wstring(payload.Info().version.begin(),
                                                                       payload.Info().version.end()) +
                                    L" to " + app + (o.test ? L" (test mode)" : L""));
    Log(L"Tasks: " + FormatTaskList(req.tasks) + L"; index path: " + req.index_path);
    if (app.size() < 4 || app[1] != L':') return Fail(kExitPrepareFailed, L"Invalid installation directory: " + app);

    const auto previous = FindPreviousInstall(UninstallKey(o));
    const bool in_place = previous && SameDirectory(previous->dir, app) && FileExists(previous->dir + L"\\pulse.exe");
    if (previous) Log(L"Previous install: " + previous->dir + (previous->inno ? L" (Inno)" : L"") +
                      (in_place ? L", upgrading in place" : L""));

    if (previous && real && NestedInstallDirectories(previous->dir, app))
        return Fail(kExitPrepareFailed, L"The old and new installation directories must not contain each other. Choose a separate directory.");

    // An unattended update may only replace the registered machine install in place.
    if (o.pulse_update && (!in_place || (real && !previous->where.machine())))
        return Fail(kExitPrepareFailed, L"Pulse update requires the existing installation directory and scope.");

    // 1. Extract and verify everything before any running Pulse is disturbed.
    CreateDirs(app);
    DialogLayoutMigration migration(app, real);
    RuntimeRegistrationTransaction runtime_registration(app, real);
    FileTransaction tx(app, [&](std::wstring& failure) { return migration.Recover(failure) && runtime_registration.Recover(failure); });
    std::wstring error;
    if (!tx.Begin(error)) return Fail(kExitPrepareFailed, error);
    ctx.Phase(Phase::Extracting, L"Extracting files");
    const ULONGLONG t0 = GetTickCount64();
    bool cancelled = false;
    const ProgressCallback progress = [&](const ExtractProgress& p) {
        if (cb.progress && !cb.progress(p)) { cancelled = true; return false; }
        return true;
    };
    if (!payload.ExtractTo(tx.StagingDir(), progress, error)) {
        tx.Abort();
        if (cancelled) return Fail(kExitCancelledDuring, L"Cancelled.");
        return Fail(kExitPrepareFailed, L"Extraction failed: " + error);
    }
    std::vector<std::wstring> files;
    uint64_t total_bytes = 0;
    for (const auto& f : payload.Info().files) { files.push_back(f.path); total_bytes += f.size; }
    if (!WriteUninstaller(self_path, payload.Info().payload_offset, tx.StagingDir() + L"\\" + kUninstallerName,
                          error)) {
        tx.Abort();
        return Fail(kExitPrepareFailed, error);
    }
    files.push_back(kUninstallerName);
    total_bytes += payload.Info().payload_offset;
    const std::string defaults = std::string("[features]\r\nglobal_search=") +
        (req.tasks.global_search ? "1" : "0") + "\r\nai_assistant=" +
        (req.tasks.ai_assistant ? "1" : "0") + "\r\n";
    const std::wstring defaults_source = (previous ? previous->dir : app) + L"\\setup-defaults.ini";
    const std::wstring defaults_staged = tx.StagingDir() + L"\\setup-defaults.ini";
    const bool defaults_written = FileExists(defaults_source)
        ? CopyFileW(defaults_source.c_str(), defaults_staged.c_str(), FALSE) != FALSE
        : WriteWholeFile(defaults_staged, defaults.data(), defaults.size());
    if (!defaults_written) { tx.Abort(); return Fail(kExitPrepareFailed, L"Cannot write setup defaults"); }
    files.push_back(L"setup-defaults.ini");
    DialogVersionPlan dialog_version;
    if (!PrepareDialogVersion(app, tx.StagingDir(), files, dialog_version, error)) { tx.Abort(); return Fail(kExitPrepareFailed, error); }
    if (dialog_version.versioned) migration.SetVersionDirectory(dialog_version.relative_directory);
    if (!WriteFileManifest(tx.StagingDir(), files)) { tx.Abort(); return Fail(kExitPrepareFailed, L"Cannot stage installed-file list."); }
    files.push_back(kFileManifestName);
    if (dialog_version.versioned) dialog_version.replacements.insert(dialog_version.replacements.end()-1, kFileManifestName);
    else dialog_version.replacements.push_back(kFileManifestName);
    if (!FlushTree(tx.StagingDir(), files, error)) { tx.Abort(); return Fail(kExitPrepareFailed, error); }
    Log(L"Extracted and flushed " + std::to_wstring(files.size()) + L" files in " +
        std::to_wstring(GetTickCount64() - t0) + L" ms");

    if (!PublishDialogVersion(app, tx.StagingDir(), dialog_version, error)) { tx.Abort(); return Fail(kExitPrepareFailed, error); }

    if (!runtime_registration.Prepare(error)) { tx.Abort(); return Fail(kExitPrepareFailed, error); }
    // 2. Close Pulse and its hosts (PrepareToInstall).
    std::vector<std::wstring> dirs{app};
    if (previous && !SameDirectory(previous->dir, app)) dirs.push_back(previous->dir);
    ctx.Phase(Phase::Closing, L"Closing Pulse");
    auto close_all = [&]() -> bool {
        CloseState state = ClosePulseForUpdate(dirs, o.pulse_update);
        while (state == CloseState::Busy && cb.busy && cb.busy()) state = ClosePulseForUpdate(dirs, o.pulse_update);
        if (state != CloseState::Done) {
            error = state == CloseState::Busy ? L"Pulse is still copying or moving files."
                                              : L"Pulse could not be closed.";
            return false;
        }
        StopPulseHosts(dirs, real);
        WaitUntilImageGone(L"Pulse.Index.exe", dirs, 10000);
        return true;
    };
    if (!close_all()) { tx.Abort(); return Fail(kExitPrepareFailed, error); }

    const bool shared_dialog_layout = std::any_of(files.begin(), files.end(), [](const auto& path) {
        return EqualsNoCase(path, L"PulseDialogAuto64.dll") || EqualsNoCase(path, L"PulseDialogAuto32.dll");
    });
    auto obsolete_dialog_files = (shared_dialog_layout || dialog_version.versioned) ? LegacyDialogFiles(app, files) : std::vector<std::wstring>{};
    std::erase_if(obsolete_dialog_files, IsDialogVersionFile);
    auto obsolete_files = obsolete_dialog_files;
    const auto retired_runtimes = RetiredRuntimeFiles(files);
    obsolete_files.insert(obsolete_files.end(), retired_runtimes.begin(), retired_runtimes.end());
    auto replacement_files = dialog_version.replacements;
    replacement_files.insert(replacement_files.end(), obsolete_files.begin(), obsolete_files.end());
    for (;;) {
        const auto locks = InspectReplacementLocks(app, replacement_files);
        if (locks.Ready()) break;
        Log(L"Replacement preflight:\n" + DescribeReplacementLocks(locks));
        if (locks.error != ERROR_SUCCESS) {
            tx.Abort();
            return Fail(kExitPrepareFailed, L"Installation file preflight failed (Win32 " + std::to_wstring(locks.error) + L").\n" + DescribeReplacementLocks(locks));
        }
        if (!cb.locked || !cb.locked(locks)) {
            tx.Abort();
            return Fail(kExitPrepareFailed, L"Installation files are unavailable. Close the programs using them and retry.\n" + DescribeReplacementLocks(locks));
        }
    }
    if (cancelled) { tx.Abort(); return Fail(kExitCancelledDuring, L"Cancelled."); }

    // Keep the old executable available until the new files and associations are verified.
    UpgradePrefs prefs;
    std::wstring previous_directory;
    if (previous && !in_place && real) {
        std::wstring exe, args;
        if (!previous->uninstall_string.empty()) {
            if (!SplitCommandLine(previous->uninstall_string, exe, args)) {
                tx.Abort();
                return Fail(kExitPrepareFailed, L"Cannot parse the previous uninstaller command.");
            }
            if (!FileExists(exe)) {
                tx.Abort();
                return Fail(kExitPrepareFailed, L"The previous uninstaller is missing: " + exe);
            }
            prefs = CaptureUpgradePrefs(ParentDir(exe) + L"\\pulse.exe");
            previous_directory = ParentDir(exe);
            if (!prefs.Flags().empty()) {
                const auto r = RunProcess((dialog_version.versioned ? app + L"\\" + dialog_version.relative_directory : tx.StagingDir()) + L"\\pulse_integration.exe",
                                          L"--prepare-upgrade --exe " + Quote(prefs.previous_exe) + prefs.Flags(),
                                          true);
                if (!r.started || r.exit_code != 0) {
                    tx.Abort();
                    return Fail(kExitPrepareFailed, L"Cannot preserve the shell integration backup.");
                }
            }

        }
    }

    // 3. Replace files atomically.
    ctx.Phase(Phase::Replacing, L"Replacing files");
    if ((shared_dialog_layout || dialog_version.versioned) && !migration.Prepare(error)) { tx.Abort(); return Fail(kExitPrepareFailed, error); }
    auto register_runtime = [&](std::wstring& failure) {
        if (!real) return true;
        if (!RegisterStartPin(app, failure)) return false;
        if (req.tasks.index_service) {
            const auto index = InstalledRuntimeFile(app, L"Pulse.Index.exe");
            bool exists = true;
            const DWORD switched = SwitchServiceRuntime(kServiceName, index, exists);
            if (switched) { failure = L"Index service runtime switch failed: " + Win32Error(switched); return false; }
            const auto command = exists ? L"--ensure-service-state" : L"--install";
            const auto r = RunProcess(index, command, true);
            SERVICE_STATUS_PROCESS status{};
            const DWORD query = ReadServiceStatus(kServiceName, status);
            Log(L"Index service configuration: existing=" + std::to_wstring(exists) +
                L" command=" + command + L" launched=" + std::to_wstring(r.started) +
                L" result=" + std::to_wstring(r.exit_code) + L" status_error=" + std::to_wstring(query) +
                L" state=" + std::to_wstring(status.dwCurrentState) +
                L" win32_exit=" + std::to_wstring(status.dwWin32ExitCode) +
                L" service_exit=" + std::to_wstring(status.dwServiceSpecificExitCode));
            if (!r.started || r.exit_code != 0) {
                failure = L"Index service configuration failed (" + std::wstring(command) + L"): " +
                    std::to_wstring(r.exit_code);
                return false;
            }
            if (query) { failure = L"Cannot verify index service: " + Win32Error(query); return false; }
        }
        if (!req.tasks.index_service && in_place) {
            const DWORD disabled = DisableService(kServiceName);
            if (disabled) { failure = L"Cannot disable index service: " + Win32Error(disabled); return false; }
        }
        return true;
    };
    if (!tx.Commit(dialog_version.replacements, error, obsolete_files, [&](std::wstring& failure) {
        if (!migration.Apply(failure)) return false;
        return !previous_directory.empty() || register_runtime(failure);
    })) return Fail(kExitFatalInstall, error);
    tx.Finish();
    if (!previous_directory.empty()) {
        // After this point neither directory may be rolled back: a failed multi-entry
        // migration can legitimately leave some commands pointing to each executable.
        if (!CompleteDirectoryMove(
            [&](std::wstring& failure) { return RestoreUpgradePrefs(prefs, app, failure); },
            [&](std::wstring& failure) {
                Log(L"Removing previous version: " + previous->uninstall_string);
                UninstallRequest removal;
                removal.app_dir = previous_directory;
                removal.options = o;
                removal.options.pulse_upgrade = true;
                // Use this engine, not an older uninstaller that may ignore upgrade mode.
                const auto removed = RunUninstall(removal, {});
                if (removed.exit_code != 0) {
                    failure = L"The new files and shell associations are installed, but the previous version could not be removed (" +
                        std::to_wstring(removed.exit_code) + L"). The new installation files were kept. " + removed.error;
                    return false;
                }
                if (removed.schedule_self_delete) DeleteFileW((previous_directory + L"\\uninstall.exe").c_str());
                if (previous->inno) RemoveInnoUninstaller(previous_directory);
                RemoveDirectoryW(previous_directory.c_str());
                if (prefs.startup) WriteString(HKEY_CURRENT_USER, 0, kRunKey, L"Pulse",
                    ReplaceExe(*prefs.startup, prefs.previous_exe, app + L"\\pulse.exe"));
                else DeleteValue(HKEY_CURRENT_USER, 0, kRunKey, L"Pulse");
                if (!close_all()) { failure = error; return false; }
                WaitUntilServiceGone(kServiceName, 10000);
                return true;
            }, register_runtime, error)) return Fail(kExitFatalInstall, error);
    }
    DialogCleanupReport cleanup_report;
    size_t versions_removed = 0;
    if (dialog_version.versioned) {
        do {
            cleanup_report = CleanupDialogVersionsReport(app);
            versions_removed += cleanup_report.deleted_directories.size();
            Log(L"Dialog cleanup: removed=" + std::to_wstring(versions_removed) +
                L" protected=" + std::to_wstring(cleanup_report.protected_directories.size()) +
                L" blocked=" + std::to_wstring(cleanup_report.blocked_directories.size()) +
                L" retained=" + std::to_wstring(cleanup_report.retained_directories.size()));
            if (!cleanup_report.Retryable() || !cb.cleanup_locked ||
                !cb.cleanup_locked(cleanup_report)) break;
        } while (true);
        if (!cleanup_report.Complete()) Log(L"Old dialog versions retained; retry cleanup after their owning programs exit.");
    }
    // Remove empty legacy directories only; unregistered user files remain untouched.
    for (auto it = obsolete_dialog_files.rbegin(); it != obsolete_dialog_files.rend(); ++it) {
        auto directory = ParentDir(app + L"\\" + *it);
        while (!SameDirectory(directory, app)) {
            if (!RemoveDirectoryW(directory.c_str())) break;
            directory = ParentDir(directory);
        }
    }
    RemoveDirectoryW((app + L"\\DialogBridge").c_str());

    // 4. Register: uninstall entry, shortcuts, startup.
    ctx.Phase(Phase::Registering, L"Registering Pulse");
    InstallResult result;
    result.app_dir = app;
    result.dialog_cleanup_pending = dialog_version.versioned && !cleanup_report.Complete();
    result.dialog_versions_removed = versions_removed;
    result.defaults_written = defaults_written;
    bool post_failed = false;
    UninstallEntry entry;
    entry.dir = app;
    entry.version.assign(payload.Info().version.begin(), payload.Info().version.end());
    if (payload.Info().channel == "win81") entry.display_suffix = L" (Windows 8.1 x64)";
    entry.uninstaller = app + L"\\" + kUninstallerName;
    if (o.test) entry.uninstall_args = L" /PULSETEST";  // the test uninstaller must stay on the test key
    entry.tasks = FormatTaskList(req.tasks);
    entry.estimated_kb = total_bytes / 1024;
    const RegLocation where = real ? RegLocation{HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY}
                                   : RegLocation{HKEY_CURRENT_USER, KEY_WOW64_64KEY};
    if (!WriteUninstallEntry(where, UninstallKey(o), entry, error)) return Fail(kExitFatalInstall, error);
    if (previous && previous->inno && in_place) RemoveInnoUninstaller(app);

    const std::wstring group = KnownFolder(real ? FOLDERID_CommonPrograms : FOLDERID_Programs) +
                               (real ? L"\\Pulse" : L"\\Pulse Test");
    const bool chinese = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
    if (!EnsureShortcut(group + L"\\Pulse.lnk", app + L"\\pulse.exe", L"", app, L"Pulse", error))
        Log(L"Shortcut: " + error);
    // Inno named it "{cm:UninstallProgram,Pulse}"; replace the Inno one in place.
    const std::wstring uninstall_lnk = group + (chinese ? L"\\卸载 Pulse.lnk" : L"\\Uninstall Pulse.lnk");
    DeleteFileW(uninstall_lnk.c_str());
    if (!EnsureShortcut(uninstall_lnk, entry.uninstaller, L"", app, L"", error)) Log(L"Shortcut: " + error);
    const std::wstring desktop = KnownFolder(real ? FOLDERID_PublicDesktop : FOLDERID_Desktop);
    const std::wstring desktop_link = desktop + (real ? L"\\Pulse.lnk" : L"\\Pulse Test.lnk");
    // Existing shortcuts also need the activation entry when an upgrade does
    // not request creation of a desktop shortcut.
    if (req.tasks.desktop_icon || GetFileAttributesW(desktop_link.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!EnsureShortcut(desktop_link, app + L"\\pulse.exe", L"", app,
                            L"Pulse", error))
            Log(L"Shortcut: " + error);
    }
    if (real && req.tasks.startup && !o.pulse_update)
        WriteString(HKEY_CURRENT_USER, 0, kRunKey, L"Pulse", Quote(app + L"\\pulse.exe"));

    // 5. Post-install configuration (CurStepChanged ssPostInstall + [Run]).
    result.startup = real && req.tasks.startup;
    if (real) {
        if (req.tasks.index_service) ctx.Phase(Phase::IndexService, L"Configuring the index service");
        result.index_configured = req.tasks.index_service;
        if (req.tasks.index_service) {
            const auto configured = RunProcess(InstalledRuntimeFile(app, L"Pulse.Index.exe"),
                L"--set-index-path " + Quote(NormalizeDir(req.index_path)), true);
            if (!configured.started || configured.exit_code != 0) {
                post_failed = true;
                result.error = L"Index location migration failed: " + std::to_wstring(configured.exit_code);
                Log(result.error);
            }
            SERVICE_STATUS_PROCESS status{};
            const DWORD query = ReadServiceStatus(kServiceName, status);
            result.index_running = !query && status.dwCurrentState == SERVICE_RUNNING;
            Log(L"Index service final state: query=" + std::to_wstring(query) +
                L" state=" + std::to_wstring(status.dwCurrentState) +
                L" win32_exit=" + std::to_wstring(status.dwWin32ExitCode));
            if (query) {
                post_failed = true;
                result.index_configured = false;
                result.error = L"Cannot query installed index service: " + Win32Error(query);
                Log(result.error);
            }
        } else if (in_place && ServiceExists(kServiceName)) {
            const auto removed = RunProcess(InstalledRuntimeFile(app, L"Pulse.Index.exe"), L"--uninstall", true);
            if (!removed.started || removed.exit_code != 0) {
                post_failed = true;
                result.error = L"Disabled index service remains pending removal: " + std::to_wstring(removed.exit_code);
                Log(result.error);
            }
        }
        ctx.Phase(Phase::SeedVerbs, L"Caching context-menu verbs");
        const auto seed = RunProcess(app + L"\\pulse.exe", L"--seed-shell-verbs", true, app);
        Log(L"Seed shell verbs: " + std::to_wstring(seed.exit_code));
        result.verbs_seeded = seed.started && seed.exit_code == 0;
    }

    // GetCustomSetupExitCode: only unattended updates report post-install failures.
    result.exit_code = (o.pulse_update && post_failed) ? kExitFailed : kExitOk;
    ctx.Phase(Phase::Done, L"Done, exit code " + std::to_wstring(result.exit_code));
    return result;
}

}  // namespace pulse::setup
