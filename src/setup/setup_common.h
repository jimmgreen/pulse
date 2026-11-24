#pragma once
// Shared helpers for the Pulse setup bootstrapper.

#include <windows.h>
#include <shlobj.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::setup {

// Same AppId as installer/PulseSetup.iss so upgrades from Inno builds find
// the previous installation and keep a single "Apps & features" entry.
inline constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{A3F47C2E-9D1B-4E58-8C6A-2B5D0F9E1734}_is1";
// Test installs (/PULSETEST) use their own per-user entry and never touch the
// machine-wide installation, the PulseIndex service or user preferences.
inline constexpr wchar_t kTestUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PulseSetupTest";
inline constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t kServiceName[] = L"PulseIndex";
inline constexpr wchar_t kDialogBrokerServiceName[] = L"PulseDialogBroker";

// Exit codes follow Inno Setup so UpdateInstallErrorFromExitCode keeps working.
enum ExitCode : int {
    kExitOk = 0,
    kExitFailed = 1,            // install finished but a post-install step failed
    kExitCancelledBefore = 2,   // user/UAC cancelled before any change
    kExitFatalInstall = 4,      // error while replacing files (rolled back)
    kExitCancelledDuring = 5,
    kExitPrepareFailed = 7,     // PrepareToInstall equivalent failed
    // /CLEANUPVERSIONS only: versions remain (owners still running, or unverifiable),
    // or an install holds the version lock / awaits recovery.
    kExitCleanupRetained = 8,
    kExitCleanupBusy = 9,
};

std::wstring Win32Error(DWORD code);
std::wstring NormalizeDir(std::wstring path);  // trims blanks and trailing backslash
bool EqualsNoCase(const std::wstring& a, const wchar_t* b);
bool SameDirectory(const std::wstring& a, const std::wstring& b);
bool PathInDirectories(const std::wstring& path, const std::vector<std::wstring>& dirs);
std::wstring KnownFolder(REFKNOWNFOLDERID id);
std::wstring ExpandEnv(const std::wstring& text);
std::wstring Quote(const std::wstring& s);

struct RunResult {
    bool started = false;
    DWORD exit_code = 0xFFFFFFFF;
};
// Starts `exe` hidden; waits when `wait` is true.
RunResult RunProcess(const std::wstring& exe, const std::wstring& args, bool wait,
                     const std::wstring& cwd = {});

bool IsProcessElevated();

// Small Win32 file helpers (std::filesystem/iostream add ~250 KB to the stub).
std::wstring ParentDir(const std::wstring& path);
bool CreateDirs(const std::wstring& dir);
bool ReadWholeFile(const std::wstring& path, std::string& data);
bool WriteWholeFile(const std::wstring& path, const void* data, size_t size);
void RemoveTree(const std::wstring& dir);  // locked leftovers are removed at reboot
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

bool DirectoryHasPulseIndexArtifacts(const std::wstring& dir);
bool IsNonEmptyNonIndexDirectory(const std::wstring& dir);
std::wstring DefaultIndexPathForApp(const std::wstring& app_dir, bool test_mode = false);
std::wstring NormalizeIndexSelection(const std::wstring& chosen_dir, const std::wstring& app_dir,
                                     const std::wstring& current_configured_index = {},
                                     bool from_browse = false);
void DeletePulseIndexDirectoryArtifacts(const std::wstring& dir);

}  // namespace pulse::setup
