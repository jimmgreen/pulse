#pragma once
#include <windows.h>
#include <string>
namespace pulse::dialog_auto {
enum class Architecture { X64, X86 };
#ifdef _WIN64
inline constexpr Architecture CurrentArchitecture = Architecture::X64;
inline constexpr auto DllName = L"PulseDialogAuto64.dll";
inline constexpr auto ConfigKey = L"Software\\Pulse\\DialogAuto";
inline constexpr REGSAM RegistryView = KEY_WOW64_64KEY;
#else
inline constexpr Architecture CurrentArchitecture = Architecture::X86;
inline constexpr auto DllName = L"PulseDialogAuto32.dll";
inline constexpr auto ConfigKey = L"Software\\Pulse\\DialogAuto32";
inline constexpr REGSAM RegistryView = KEY_WOW64_32KEY;
#endif
struct Status { bool enabled = false; bool conflict = false; DWORD error = ERROR_SUCCESS; std::wstring dll_path; };
const wchar_t* DllNameFor(Architecture architecture) noexcept;
Status Inspect(Architecture architecture);
DWORD Enable(Architecture architecture, const std::wstring& absolute_dll);
DWORD EnableIfUnclaimed(Architecture architecture, const std::wstring& absolute_dll);
// Rebind only an exact old journal that is fully disabled and has no CLSID roots.
// This does not relax the unclaimed-only endpoint or discard extra preferences.
DWORD EnableIfDisabledFor(Architecture architecture, const std::wstring& expected_old_dll, const std::wstring& new_dll);
DWORD Disable(Architecture architecture);
DWORD DisableFor(Architecture architecture, const std::wstring& absolute_dll);
DWORD Enable(const std::wstring& absolute_dll);
// Transactional multi-architecture callers must not replace another bundle's
// journal during recovery. The identity check shares Enable's mutation lock.
DWORD EnableIfUnclaimed(const std::wstring& absolute_dll);
DWORD EnableIfDisabledFor(const std::wstring& expected_old_dll, const std::wstring& new_dll);
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
