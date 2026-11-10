#pragma once
#include <windows.h>
#include <string>

namespace pulse::setup {
// Launch through the interactive desktop, never inherit the setup token.
HRESULT LaunchFromDesktop(const std::wstring& executable, const std::wstring& arguments,
                          const std::wstring& directory);
}
