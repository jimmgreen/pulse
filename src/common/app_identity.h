#pragma once
#include <windows.h>
#include <propsys.h>
#include <string>

namespace pulse::app_identity {
inline constexpr wchar_t kId[] = L"Pulse.FileManager";
HRESULT SetProcess() noexcept;
HRESULT SetShortcut(IPropertyStore* properties) noexcept;
HRESULT SetWindow(HWND window, const std::wstring& stable_entry) noexcept;
void ClearWindow(HWND window) noexcept;
}
