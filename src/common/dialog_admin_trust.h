#pragma once
#include <string>
#include <windows.h>

namespace pulse::dialog_admin {
DWORD VerifyProtectedTree(const std::wstring &root) noexcept;
DWORD VerifyProtectedPath(const std::wstring &path, bool directory) noexcept;
// Read-only installed-location checks; callers separately verify bundle hashes.
DWORD VerifyAdminLaunchLocation(const std::wstring& install_root, const std::wstring& bundle_root,
                               const std::wstring& host_path, WORD machine) noexcept;
// Only the elevated launcher may create this fixed child of a protected root.
// Existing directories are validated, never repaired or assigned weaker ACLs.
DWORD AdminControlRoot(const std::wstring& install_root, bool create, std::wstring& result) noexcept;
bool IsElevated() noexcept;
} // namespace pulse::dialog_admin
