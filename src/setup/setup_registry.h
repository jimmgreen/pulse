#pragma once
// Uninstall-entry and preference registry access.

#include "setup_options.h"

#include <windows.h>

#include <optional>
#include <string>

namespace pulse::setup {

struct RegLocation {
    HKEY root = nullptr;
    REGSAM view = 0;  // KEY_WOW64_64KEY / KEY_WOW64_32KEY
    bool machine() const { return root == HKEY_LOCAL_MACHINE; }
};

struct PreviousInstall {
    RegLocation where;
    std::wstring dir;               // 'Inno Setup: App Path' or InstallLocation
    std::wstring uninstall_string;  // as registered
    std::optional<Tasks> tasks;     // remembered task selection
    std::optional<std::wstring> index_path;
    bool inno = false;              // written by Inno Setup (not by this setup)
};

// Looks in HKLM64, HKLM32, HKCU64, HKCU32 like PreviousPulseRoot in the iss.
std::optional<PreviousInstall> FindPreviousInstall(const wchar_t* key);

struct UninstallEntry {
    std::wstring dir, version, uninstaller, tasks;
    std::wstring uninstall_args;
    std::wstring display_suffix;  // " (Windows 8.1 x64)" for the win81 channel, as AppVerName in the iss  // appended to UninstallString (test installs)
    std::wstring index_path;
    uint64_t estimated_kb = 0;
};
bool WriteUninstallEntry(const RegLocation& where, const wchar_t* key, const UninstallEntry& e, std::wstring& error);

std::optional<std::wstring> ReadString(HKEY root, REGSAM view, const wchar_t* key, const wchar_t* value);
bool WriteString(HKEY root, REGSAM view, const wchar_t* key, const wchar_t* value, const std::wstring& data);
bool DeleteValue(HKEY root, REGSAM view, const wchar_t* key, const wchar_t* value);

}  // namespace pulse::setup
