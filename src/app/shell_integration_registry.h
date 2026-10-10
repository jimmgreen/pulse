#pragma once

#include <windows.h>
#include <string>

namespace pulse::app {
#ifdef PULSE_INTEGRATION_TEST
using IntegrationWriteHook = bool (*)(const std::wstring&, const std::wstring&);
void SetIntegrationWriteHookForTesting(IntegrationWriteHook hook);
#endif


enum class ShellIntegrationKind { Folders, WinE, ThisPc, Directory, Drive };

// Folder opens do not run Folder\shell\open\command's (Default) value: HKLM
// registers CLSID_ExecuteFolder as that key's DelegateExecute, and the shell
// activates the delegate instead. Intercepting a launcher's "open file
// location" therefore means becoming that delegate. This CLSID is Pulse's own,
// served by pulse.exe --folder-open-com (folder_open_com.cpp).
// {6E2F1A44-9C3B-4D7E-8A51-2B0C7D9E4F13}
inline constexpr wchar_t kFolderOpenDelegateClassId[] =
    L"{6E2F1A44-9C3B-4D7E-8A51-2B0C7D9E4F13}";

// Uses HKCU (redirectable with RegOverridePredefKey in isolated tests).
// A false restore result can mean that legacy registrations had no exact
// backup: matching Pulse overrides are removed, unrelated values stay put.
bool ApplyShellIntegration(ShellIntegrationKind kind, const std::wstring& exe, bool on);
bool ReadShellIntegration(ShellIntegrationKind kind, const std::wstring& exe);
bool HasShellIntegrationOwnership(ShellIntegrationKind kind, const std::wstring& exe);
// Exact pre-snapshot orphan signature, used by startup migration and explicit
// repair. Ordinary enable/disable must not infer ownership of arbitrary values.
bool HasLegacyShellIntegrationResidue();
bool RepairLegacyShellIntegrationResidue();
bool PrepareShellIntegrationUpgrade(ShellIntegrationKind kind, const std::wstring& exe);
// Installer only: the selected group was owned immediately before the old
// uninstaller ran. Missing values may have been removed by that old uninstaller.
bool UpgradeShellIntegration(ShellIntegrationKind kind, const std::wstring& previous_exe,
                             const std::wstring& exe);
bool ShellCommandTargetsExecutable(const std::wstring& command, const std::wstring& exe);

} // namespace pulse::app
