#pragma once
#include <windows.h>
#include <string>
#include <vector>
namespace pulse::dialog_auto {
enum class Architecture { X64, X86 };
#ifdef _WIN64
inline constexpr Architecture CurrentArchitecture = Architecture::X64;
inline constexpr auto DllName = L"PulseDialogAuto64.dll";
inline constexpr auto StubDllName = L"PulseDialogStub64.dll";
inline constexpr auto ConfigKey = L"Software\\Pulse\\DialogAuto";
inline constexpr REGSAM RegistryView = KEY_WOW64_64KEY;
#else
inline constexpr Architecture CurrentArchitecture = Architecture::X86;
inline constexpr auto DllName = L"PulseDialogAuto32.dll";
inline constexpr auto StubDllName = L"PulseDialogStub32.dll";
inline constexpr auto ConfigKey = L"Software\\Pulse\\DialogAuto32";
inline constexpr REGSAM RegistryView = KEY_WOW64_32KEY;
#endif
struct Status { bool enabled = false; bool conflict = false; DWORD error = ERROR_SUCCESS; std::wstring dll_path; };
const wchar_t* DllNameFor(Architecture architecture) noexcept;
const wchar_t* StubDllNameFor(Architecture architecture) noexcept;
bool IsAllowedDialogDllName(Architecture architecture, const wchar_t* file_name) noexcept;
Status Inspect(Architecture architecture);
struct JournalRepairResult { DWORD error = ERROR_SUCCESS, rollback_error = ERROR_SUCCESS; unsigned repaired = 0; };
// Restore only a missing DllPath witnessed by two matching, strictly owned,
// retired COM registrations. All architectures are checked before any write.
JournalRepairResult RepairMissingRegistrationJournals(const std::vector<Architecture>& architectures);
DWORD Enable(Architecture architecture, const std::wstring& absolute_dll);
DWORD EnableIfUnclaimed(Architecture architecture, const std::wstring& absolute_dll);
// Rebind only an exact old journal that is fully disabled and has no CLSID roots.
// This does not relax the unclaimed-only endpoint or discard extra preferences.
DWORD EnableIfDisabledFor(Architecture architecture, const std::wstring& expected_old_dll, const std::wstring& new_dll);
// Cross-install retired journals additionally require the old component to remain missing.
// False mutation_attempted means every failed precondition left the journal untouched.
DWORD EnableIfRetiredDisabledFor(Architecture architecture, const std::wstring& expected_old_dll,
    const std::wstring& new_dll, bool* mutation_attempted = nullptr);
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
