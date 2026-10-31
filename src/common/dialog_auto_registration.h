#pragma once
#include <windows.h>
#include <string>
namespace pulse::dialog_auto {
#ifdef _WIN64
inline constexpr auto DllName = L"PulseDialogAuto64.dll";
inline constexpr auto ConfigKey = L"Software\\Pulse\\DialogAuto";
inline constexpr REGSAM RegistryView = KEY_WOW64_64KEY;
#else
inline constexpr auto DllName = L"PulseDialogAuto32.dll";
inline constexpr auto ConfigKey = L"Software\\Pulse\\DialogAuto32";
inline constexpr REGSAM RegistryView = KEY_WOW64_32KEY;
#endif
struct Status { bool enabled = false; bool conflict = false; DWORD error = ERROR_SUCCESS; std::wstring dll_path; };
DWORD Enable(const std::wstring& absolute_dll);
DWORD Disable();
// Uninstall: leave other installations untouched; ownership is checked under the mutation lock.
DWORD DisableFor(const std::wstring& absolute_dll);
Status Inspect();
bool IsEnabledFor(const std::wstring& absolute_dll) noexcept;
}
