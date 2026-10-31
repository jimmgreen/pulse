#pragma once
#include <string>
#include <windows.h>

namespace pulse::dialog_admin {
DWORD VerifyProtectedTree(const std::wstring &root) noexcept;
DWORD VerifyProtectedPath(const std::wstring &path, bool directory) noexcept;
bool IsElevated() noexcept;
} // namespace pulse::dialog_admin
