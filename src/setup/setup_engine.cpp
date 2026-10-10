#include "setup_engine.h"

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

void RestoreUpgradePrefs(const UpgradePrefs& p, const std::wstring& app) {
    if (!p.captured) return;
    const std::wstring exe = app + L"\\pulse.exe";
    if (p.startup) WriteString(HKEY_CURRENT_USER, 0, kRunKey, L"Pulse", ReplaceExe(*p.startup, p.previous_exe, exe));
    else DeleteValue(HKEY_CURRENT_USER, 0, kRunKey, L"Pulse");
    const auto flags = p.Flags();
    if (flags.empty()) return;
    const auto r = RunProcess(app + L"\\pulse_integration.exe",
                              flags.substr(1) + L" --upgrade-from " + Quote(p.previous_exe) + L" --exe " + Quote(exe),
                              true);
    if (!r.started) Log(L"Could not start integration migration");
    else if (r.exit_code != 0) Log(L"Integration migration incomplete; historical backup retained");
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

    // An unattended update may only replace the registered machine install in place.
    if (o.pulse_update && (!in_place || (real && !previous->where.machine())))
        return Fail(kExitPrepareFailed, L"Pulse update requires the existing installation directory and scope.");

    // 1. Extract and verify everything before any running Pulse is disturbed.
    CreateDirs(app);
    FileTransaction tx(app);
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
    if (!FlushTree(tx.StagingDir(), files, error)) { tx.Abort(); return Fail(kExitPrepareFailed, error); }
    Log(L"Extracted and flushed " + std::to_wstring(files.size()) + L" files in " +
        std::to_wstring(GetTickCount64() - t0) + L" ms");

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
    if (cancelled) { tx.Abort(); return Fail(kExitCancelledDuring, L"Cancelled."); }

    // Moving to another directory: preserve integration, then remove the old copy.
    UpgradePrefs prefs;
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
            if (!prefs.Flags().empty()) {
                const auto r = RunProcess(tx.StagingDir() + L"\\pulse_integration.exe",
                                          L"--prepare-upgrade --exe " + Quote(prefs.previous_exe) + prefs.Flags(),
                                          true);
                if (!r.started || r.exit_code != 0) {
                    tx.Abort();
                    return Fail(kExitPrepareFailed, L"Cannot preserve the shell integration backup.");
                }
            }
            Log(L"Removing previous version: " + previous->uninstall_string);
            const auto r = RunProcess(exe, args + L" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /PULSEUPGRADE=1", true);
            if (!r.started || r.exit_code != 0) {
                tx.Abort();
                return Fail(kExitPrepareFailed, L"The previous version could not be removed (" +
                                                    std::to_wstring(r.exit_code) + L").");
            }
            if (!close_all()) { tx.Abort(); return Fail(kExitPrepareFailed, error); }
            WaitUntilServiceGone(kServiceName, 10000);
        }
    }

    // 3. Replace files atomically.
    ctx.Phase(Phase::Replacing, L"Replacing files");
    if (!tx.Commit(files, error)) return Fail(kExitFatalInstall, error);
    tx.Finish();
    if (!WriteFileManifest(app, files)) Log(L"Could not write the installed-file list");
    for (const wchar_t* stale : {L"msvcp140.dll", L"msvcp140_atomic_wait.dll", L"vcruntime140.dll",
                                 L"vcruntime140_1.dll"}) {
        bool shipped = false;
        for (const auto& f : files) shipped |= EqualsNoCase(f, stale);
        if (!shipped) DeleteFileW((app + L"\\" + stale).c_str());  // [InstallDelete]
    }

    // 4. Register: uninstall entry, shortcuts, startup.
    ctx.Phase(Phase::Registering, L"Registering Pulse");
    InstallResult result;
    result.app_dir = app;
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
    if (req.tasks.desktop_icon) {
        const std::wstring desktop = KnownFolder(real ? FOLDERID_PublicDesktop : FOLDERID_Desktop);
        if (!EnsureShortcut(desktop + (real ? L"\\Pulse.lnk" : L"\\Pulse Test.lnk"), app + L"\\pulse.exe", L"", app,
                            L"Pulse", error))
            Log(L"Shortcut: " + error);
    }
    if (real && req.tasks.startup && !o.pulse_update)
        WriteString(HKEY_CURRENT_USER, 0, kRunKey, L"Pulse", Quote(app + L"\\pulse.exe"));

    // 5. Post-install configuration (CurStepChanged ssPostInstall + [Run]).
    result.startup = real && req.tasks.startup;
    if (real) {
        RestoreUpgradePrefs(prefs, app);
        if (req.tasks.index_service) ctx.Phase(Phase::IndexService, L"Configuring the index service");
        const std::wstring index_exe = app + L"\\Pulse.Index.exe";
        if (!req.tasks.index_service) {
            if (in_place && ServiceExists(kServiceName)) {
                const auto r = RunProcess(index_exe, L"--uninstall", true);
                if (!r.started || r.exit_code != 0) {
                    post_failed = true;
                    Log(L"Index service removal failed: " + std::to_wstring(r.exit_code));
                }
            }
        } else {
            auto r = RunProcess(index_exe, L"--set-index-path " + Quote(NormalizeDir(req.index_path)), true);
            if (!r.started || r.exit_code != 0) {
                post_failed = true;
                result.error = L"Index location configuration failed: " + std::to_wstring(r.exit_code);
                Log(result.error);
            } else {
                // Configure the location first: an upgrade must not start a scan
                // only to stop it again for the same index path.
                r = RunProcess(index_exe, L"--install", true);
                if (!r.started || r.exit_code != 0) {
                    post_failed = true;
                    result.error = L"Index service setup failed: " + std::to_wstring(r.exit_code);
                    Log(result.error);
                } else {
                    result.index_running = true;
                }
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
