#pragma once

#include <windows.h>
#include <string>

namespace pulse::dialog_com_hklm {

inline constexpr wchar_t kClsidOpen[] = L"{DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7}";
inline constexpr wchar_t kClsidSave[] = L"{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}";
inline constexpr wchar_t kDefaultClassesPrefix[] = L"SOFTWARE\\Classes\\CLSID";
inline constexpr wchar_t kDefaultBackupPrefix[] = L"SOFTWARE\\Pulse\\DialogBackup";

// Isolated root override allows full transactional unit testing under HKCU\Software\PulseTest\...
// without touching live machine-wide HKLM\SOFTWARE\Classes.
struct RegistryScope {
    HKEY root = HKEY_LOCAL_MACHINE;
    std::wstring classes_prefix = kDefaultClassesPrefix;
    std::wstring backup_prefix = kDefaultBackupPrefix;
    bool use_wow64_views = true;
};

struct ArchInspection {
    bool open_matches = false;
    bool save_matches = false;
    bool backup_present = false;
    std::wstring open_path;
    std::wstring save_path;
    DWORD error = ERROR_SUCCESS;
};

// Atomically backs up original InprocServer32 values (default string, type, ThreadingModel)
// into `backup_prefix` (if not already backed up) and points both Open & Save CLSIDs to `stub_dll_path`.
// Requires SYSTEM / elevated privileges with ownership/ACL permission when operating on live HKLM.
DWORD RegisterStub(const std::wstring& stub_dll_path, bool wow64_x86,
                   const RegistryScope& scope = {}) noexcept;

// Restores original backed-up InprocServer32 values for Open & Save CLSIDs and removes
// backup entries on clean completion. Safe to call repeatedly (idempotent).
DWORD RestoreBackup(bool wow64_x86, const RegistryScope& scope = {}) noexcept;

// Inspects current registration state against `expected_stub_path`.
ArchInspection Inspect(const std::wstring& expected_stub_path, bool wow64_x86,
                       const RegistryScope& scope = {}) noexcept;

} // namespace pulse::dialog_com_hklm
