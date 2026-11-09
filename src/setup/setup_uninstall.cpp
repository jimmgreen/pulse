#include "../common/dialog_auto_registration.h"
#include "start_pin_registration.h"
#include "setup_uninstall.h"
#include "setup_dialog_versions.h"
#include "setup_dialog_migration.h"
#include "../common/dialog_bundle.h"

#include "setup_common.h"
#include "setup_engine.h"
#include "setup_log.h"
#include "setup_process.h"
#include "setup_registry.h"
#include "setup_service.h"

#include <windows.h>
#include <knownfolders.h>

#include <algorithm>

namespace pulse::setup {
namespace {

// Files of an installation that predates the manifest ([Files] in the iss).
constexpr const wchar_t* kKnownFiles[] = {
    L"pulse.exe", L"lumatext.dll", L"pdfium.dll", L"Pulse.Index.exe", L"Pulse.Document.exe",
    L"Pulse.Preview.exe", L"pulse_start_pin.dll", L"pulse_shell.exe", L"pulse_elevated.exe", L"pulse_integration.exe",
    L"msvcp140.dll", L"msvcp140_atomic_wait.dll", L"vcruntime140.dll", L"vcruntime140_1.dll",
    L"uninstall.exe",
};

bool DirExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

bool EndsWith(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// ---- data cleanup (CleanupPulseData and friends) -----------------------

bool IsPulseIndexArtifact(const std::wstring& name) {
    const std::wstring n = Lower(name);
    return n == L"pulse-index.bin" || n == L"pulse-index.bin.tmp" || n == L"pulse-index.dlt" ||
           (n.rfind(L"pulse-index-", 0) == 0 && EndsWith(n, L".dlt"));
}

void DeletePulseIndexArtifacts(const std::wstring& dir) {
    if (dir.empty() || !DirExists(dir)) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsPulseIndexArtifact(fd.cFileName)) {
                const std::wstring path = dir + L"\\" + fd.cFileName;
                if (!DeleteFileW(path.c_str())) Log(L"Could not delete Pulse index artifact: " + path);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());  // only succeeds when the custom directory is now empty
}

// ReadJsonBool from the iss: "key": true|false, anything else -> fallback.
bool ReadJsonBool(const std::string& json, const char* key, bool fallback) {
    const std::string marker = std::string("\"") + key + "\"";
    size_t i = json.find(marker);
    if (i == std::string::npos) return fallback;
    i = json.find(':', i + marker.size());
    if (i == std::string::npos) return fallback;
    ++i;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) ++i;
    if (json.compare(i, 4, "true") == 0) return true;
    if (json.compare(i, 5, "false") == 0) return false;
    return fallback;
}

std::wstring LocalPulseDir() { return KnownFolder(FOLDERID_LocalAppData) + L"\\Pulse"; }

bool RemovePreviewPacksOnUninstall() {
    std::string json;
    if (!ReadWholeFile(LocalPulseDir() + L"\\packs\\packs.json", json)) return true;
    return ReadJsonBool(json, "remove_on_uninstall", true);
}

void DeleteLocalDataKeepingPacks() {
    const std::wstring root = LocalPulseDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || EqualsNoCase(name, L"packs")) continue;
        const std::wstring path = root + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(path);
        else DeleteFileW(path.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void CleanupPulseData(const std::wstring& index_path) {
    const std::wstring program_data = KnownFolder(FOLDERID_ProgramData) + L"\\Pulse";
    if (!index_path.empty() && !SameDirectory(index_path, program_data + L"\\Index"))
        DeletePulseIndexArtifacts(NormalizeDir(index_path));
    if (RemovePreviewPacksOnUninstall()) RemoveTree(LocalPulseDir());
    else DeleteLocalDataKeepingPacks();
    RemoveTree(program_data);
    Log(L"Removed Pulse settings, caches and index data");
}

// File Explorer "Pulse tags" submenu (shell_tag_menu.cpp) and its dot icons.
void DeleteShellTagMenu() {
    for (const wchar_t* key : {L"Software\\Classes\\*\\shell\\PulseTags", L"Software\\Classes\\Directory\\shell\\PulseTags",
                               L"Software\\Classes\\CLSID\\{A2D41CF7-62E1-49F4-BCE9-3624D44FB779}"})
    {
        RegDeleteTreeW(HKEY_CURRENT_USER, key);
        RegDeleteKeyW(HKEY_CURRENT_USER, key);
    }
    RemoveTree(LocalPulseDir() + L"\\tagicons");
}

// ---- installed files ----------------------------------------------------

std::vector<std::wstring> ReadFileManifest(const std::wstring& app) {
    std::vector<std::wstring> files;
    std::string data;
    if (!ReadWholeFile(app + L"\\" + kFileManifestName, data)) return files;
    size_t start = 0;
    while (start < data.size()) {
        size_t end = data.find('\n', start);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::wstring rel = Utf8ToWide(line);
        // Never follow a manifest entry out of {app}.
        if (!rel.empty() && rel.find(L"..") == std::wstring::npos && rel.find(L':') == std::wstring::npos &&
            rel[0] != L'\\' && rel[0] != L'/')
            files.push_back(rel);
        start = end + 1;
    }
    return files;
}

void CollectTree(const std::wstring& root, const std::wstring& rel, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\" + rel + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) CollectTree(root, rel + L"\\" + name, out);
        else out.push_back(rel + L"\\" + name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool DeleteInstalledFile(const std::wstring& path, bool& reboot_needed) {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (DeleteFileW(path.c_str())) return true;
    const DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;
    // Locked (e.g. loaded by Explorer): Inno defers these to the next restart too.
    if (MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        reboot_needed = true;
        Log(L"In use, deleted at restart: " + path);
        return true;
    }
    Log(L"Could not delete " + path + L": " + Win32Error(e));
    return false;
}

void DeleteShortcut(const std::wstring& lnk) {
    if (DeleteFileW(lnk.c_str())) Log(L"Removed shortcut " + lnk);
}

}  // namespace

bool WriteFileManifest(const std::wstring& app_dir, const std::vector<std::wstring>& files) {
    std::string data;
    for (const auto& f : files) data += WideToUtf8(f) + "\r\n";
    return WriteWholeFile(app_dir + L"\\" + kFileManifestName, data.data(), data.size());
}

bool IsPulseInstallDir(const std::wstring& app_dir) {
    return FileExists(app_dir + L"\\pulse.exe") || FileExists(app_dir + L"\\" + kFileManifestName);
}

bool IndexServiceInstalled() { return ServiceExists(kServiceName); }

UninstallResult RunUninstall(const UninstallRequest& req, const UninstallCallbacks& cb) {
    const Options& o = req.options;
    const bool real = !o.test;
    const bool upgrade = o.pulse_upgrade;
    const std::wstring app = NormalizeDir(req.app_dir);
    UninstallResult result;
    auto progress = [&](int percent, const std::wstring& item) {
        if (cb.progress) cb.progress(percent, item);
    };
    Log(L"Uninstalling Pulse from " + app + (upgrade ? L" (upgrade, keeping data)" : L"") +
        (o.test ? L" (test mode)" : L"") + (req.cleanup_data ? L"; removing user data" : L""));
    if (app.size() < 4 || !IsPulseInstallDir(app)) {
        result.exit_code = kExitPrepareFailed;
        result.nothing_changed = true;
        result.error = L"This folder does not contain a Pulse installation: " + app;
        Log(result.error);
        return result;
    }
    // InitializeUninstall reads it before the service (and its config) goes away.
    const std::wstring index_path = real ? ConfiguredIndexPath() : std::wstring();

    // 1. Close Pulse with the same idle handshake as an upgrade.
    progress(3, L"pulse.exe");
    const std::vector<std::wstring> dirs{app};
    CloseState state = ClosePulseForUpdate(dirs, false);
    while (state == CloseState::Busy && cb.busy && cb.busy()) state = ClosePulseForUpdate(dirs, false);
    if (state != CloseState::Done) {
        result.exit_code = kExitPrepareFailed;
        result.nothing_changed = true;
        result.error = state == CloseState::Busy ? L"Pulse is still copying or moving files."
                                                 : L"Pulse could not be closed.";
        Log(result.error);
        return result;
    }

    // Restore before removing DLLs, including an install-directory migration.
    if(real) {
        DWORD restored=DisableOwnedDialogRegistration(app, dialog_auto::Inspect(), dialog_auto::DisableFor);
        if(restored==ERROR_SUCCESS) restored=dialog_auto::DisableFor(app+L"\\"+dialog_auto::DllName);
        if(restored==ERROR_SUCCESS) restored=dialog_auto::DisableFor(app+L"\\DialogBridge\\"+dialog_auto::DllName);
        if (restored == ERROR_SUCCESS && FileExists(app + L"\\dialog-current.json")) {
            dialog_bundle::BundleLease current;
            restored = dialog_bundle::ResolveInstalledBundle(app, current);
            if (!restored) restored = dialog_auto::DisableFor(current.Directory() + L"\\" + dialog_auto::DllName);
        }
        if(restored!=ERROR_SUCCESS) {
            result.exit_code=kExitPrepareFailed;
            result.error=L"Could not restore Pulse file dialogs. Installation files were kept. "+Win32Error(restored);
            Log(result.error);return result;
        }
    }

    // 2. [UninstallRun]: stop and remove the index service before files go.
    progress(10, kServiceName);
    if (real) {
        const DWORD stop = StopService(kServiceName, 15000);
        if (stop != ERROR_SUCCESS) Log(L"Stopping PulseIndex: " + Win32Error(stop));
        const std::wstring index_exe = InstalledRuntimeFile(app, L"Pulse.Index.exe");
        if (FileExists(index_exe)) {
            const auto r = RunProcess(index_exe, L"--uninstall", true);
            Log(L"Pulse.Index.exe --uninstall: " + std::to_wstring(r.exit_code));
        }
    }
    StopPulseHosts(dirs, real);
    WaitUntilImageGone(L"Pulse.Index.exe", dirs, 10000);

    // 3. usUninstall: give file associations back (not when only moving the install).
    if (real && !upgrade) {
        progress(18, L"pulse_integration.exe");
        const auto r = RunProcess(InstalledRuntimeFile(app, L"pulse_integration.exe"),
                                  L"--restore --exe " + Quote(app + L"\\pulse.exe"), true);
        result.restore_result = r.started ? static_cast<int>(r.exit_code) : 3;
        Log(L"Integration restore: " + std::to_wstring(result.restore_result));
        // Everything was given back: the backups of the original values are
        // done with. After a failure they stay, so a later restore can finish.
        if (result.restore_result == 0) {
            RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Pulse\\ShellIntegration");
            RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Pulse\\ShellIntegration");
            RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Pulse");  // only when nothing else is left in it
        }
    }

    if (real && !UnregisterStartPin(app, result.error)) {
        Log(result.error);
        result.exit_code = kExitFailed;
        return result;
    }

    // 4. Installed files, then the directories they leave empty.
    std::vector<std::wstring> files = ReadFileManifest(app);
    if (files.empty()) {
        Log(L"No file manifest; removing the known Pulse files");
        for (const wchar_t* f : kKnownFiles) files.push_back(f);
        CollectTree(app, L"licenses", files);
    }
    const bool dialog_clean = CleanupDialogVersions(app, true);
    std::erase_if(files, IsDialogVersionFile);
    if (!dialog_clean) {
        result.dialog_cleanup_pending = true;
        result.error = L"Some dialog versions remain in DialogBridge because they are in use or could not be safely verified. Close their host applications before a later cleanup.";
        Log(result.error);
    }
    const std::wstring self_name = L"uninstall.exe";
    size_t index = 0;
    for (const auto& rel : files) {
        ++index;
        if (EqualsNoCase(rel, self_name.c_str())) { result.schedule_self_delete = true; continue; }
        progress(20 + static_cast<int>(55 * index / files.size()), rel);
        DeleteInstalledFile(app + L"\\" + rel, result.reboot_needed);
    }
    DeleteFileW((app + L"\\" + kFileManifestName).c_str());
    // Deepest directories first.
    std::vector<std::wstring> subdirs;
    for (const auto& rel : files)
        for (size_t p = rel.find(L'\\'); p != std::wstring::npos; p = rel.find(L'\\', p + 1))
            subdirs.push_back(rel.substr(0, p));
    std::sort(subdirs.begin(), subdirs.end(), [](const std::wstring& a, const std::wstring& b) {
        return a.size() > b.size();
    });
    for (const auto& d : subdirs) RemoveDirectoryW((app + L"\\" + d).c_str());
    for (const wchar_t* work : {L"\\.pulse-setup", L"\\.pulse-staging"}) RemoveTree(app + work);
    if (!result.schedule_self_delete && FileExists(app + L"\\uninstall.exe")) result.schedule_self_delete = true;

    // 5. Shortcuts ([Icons]); the Start-menu group goes once it is empty.
    progress(80, L"Start menu");
    const std::wstring group = KnownFolder(real ? FOLDERID_CommonPrograms : FOLDERID_Programs) +
                               (real ? L"\\Pulse" : L"\\Pulse Test");
    for (const wchar_t* name : {L"\\Pulse.lnk", L"\\卸载 Pulse.lnk", L"\\解除安裝 Pulse.lnk", L"\\Uninstall Pulse.lnk"})
        DeleteShortcut(group + name);
    RemoveDirectoryW(group.c_str());
    DeleteShortcut(KnownFolder(real ? FOLDERID_PublicDesktop : FOLDERID_Desktop) +
                   (real ? L"\\Pulse.lnk" : L"\\Pulse Test.lnk"));

    // 6. Registry: uninstall entry, startup value, Explorer tag menu (usPostUninstall).
    progress(88, L"Registry");
    const wchar_t* key = real ? kUninstallKey : kTestUninstallKey;
    const RegLocation views[] = {{HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY}, {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY},
                                 {HKEY_CURRENT_USER, KEY_WOW64_64KEY}, {HKEY_CURRENT_USER, KEY_WOW64_32KEY}};
    for (const auto& where : views) {
        auto dir = ReadString(where.root, where.view, key, L"Inno Setup: App Path");
        if (!dir) dir = ReadString(where.root, where.view, key, L"InstallLocation");
        // Only the entry of this installation; another copy keeps its own.
        if (!dir || !SameDirectory(NormalizeDir(*dir), app)) continue;
        const LSTATUS s = RegDeleteKeyExW(where.root, key, where.view, 0);
        Log(L"Uninstall entry removed: " + std::to_wstring(s));
    }
    if (real) {
        DeleteValue(HKEY_CURRENT_USER, 0, kRunKey, L"Pulse");
        DeleteShellTagMenu();
        // 7. User data: explicit choice, otherwise only the preview-pack setting.
        progress(94, L"%LOCALAPPDATA%\\Pulse");
        if (req.cleanup_data) CleanupPulseData(index_path);
        else if (!upgrade && RemovePreviewPacksOnUninstall()) RemoveTree(LocalPulseDir() + L"\\packs");
    }
    progress(100, L"");
    result.exit_code = kExitOk;
    Log(L"Uninstall finished" + std::wstring(result.reboot_needed ? L" (restart needed for locked files)" : L""));
    return result;
}

void ScheduleSelfDelete(const std::wstring& self_path, const std::wstring& app_dir) {
    // cmd outlives this process: wait, delete the uninstaller, then remove the
    // directory if nothing else is left in it (rd without /s).
    const std::wstring system = KnownFolder(FOLDERID_System);
    const std::wstring cmd = system + L"\\cmd.exe";
    const std::wstring wait = L"ping -n 3 127.0.0.1 >nul";
    std::wstring args = L"/d /c \"" + wait + L" & del /f /q " + Quote(self_path) + L" & " + wait + L" & del /f /q " +
                        Quote(self_path) + L" & rd " + Quote(app_dir) + L"\"";
    std::wstring line = Quote(cmd) + L" " + args;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(cmd.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       system.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Log(L"Scheduled removal of " + self_path);
    } else {
        Log(L"Could not schedule removal of the uninstaller: " + Win32Error(GetLastError()));
    }
}

}  // namespace pulse::setup
