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
// DWORD "Host" under ConfigKey: 0 shows every intercepted dialog in-process
// (the PulsePickerUiProof provider); missing or nonzero shows basic dialogs in
// pulse.exe (docs/dialog-out-of-process-plan-20261107.md).
bool PreferOutOfProcess() noexcept;
// DWORD "RemoteHost" under ConfigKey: nonzero shows dialogs with events, custom
// controls or properties in pulse.exe --picker-remote-host as well (instead of
// loading PulsePickerUiProof.dll in-process). Off until the remote host passes
// its real-UI checks (docs/dialog-out-of-process-plan-20261107.md, stage 5d).
bool PreferRemoteHost() noexcept;
}
