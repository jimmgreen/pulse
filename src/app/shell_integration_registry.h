#pragma once

#include <windows.h>
#include <string>

namespace pulse::app {
#ifdef PULSE_INTEGRATION_TEST
using IntegrationWriteHook = bool (*)(const std::wstring&, const std::wstring&);
void SetIntegrationWriteHookForTesting(IntegrationWriteHook hook);
#endif


enum class ShellIntegrationKind { Folders, WinE, ThisPc, Directory, Drive, RecycleBin };

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
// Historical builds registered the folder association under Folder\shell, a
// group nothing here manages any more. A machine that ran one of them keeps
// opening folders in Pulse whatever the settings say, so release those values
// while the command still points at `exe` (empty means this process's module).
// Returns true when something was released (the caller refreshes the shell;
// a missing or foreign key is a no-op).
bool ReleaseStaleFolderAssociation(const std::wstring& exe);
bool PrepareShellIntegrationUpgrade(ShellIntegrationKind kind, const std::wstring& exe);
// Installer only: the selected group was owned immediately before the old
// uninstaller ran. Missing values may have been removed by that old uninstaller.
bool UpgradeShellIntegration(ShellIntegrationKind kind, const std::wstring& previous_exe,
                             const std::wstring& exe);
bool ShellCommandTargetsExecutable(const std::wstring& command, const std::wstring& exe);

// Why the last ApplyShellIntegration call on this thread returned false (the
// first problem it hit). kind is None after a successful call.
enum class ShellIntegrationFailureKind {
    None, AccessDenied, WriteError, Reverted, ReadError, LegacyResidue, BadBackup, PendingUpgrade,
    ChangedByOther, Unknown
};
struct ShellIntegrationFailure {
    ShellIntegrationFailureKind kind = ShellIntegrationFailureKind::None;
    std::wstring key;    // relative to HKCU
    std::wstring name;   // value name; empty is the default value
    long status = 0;     // Win32 status, when there was one
};
ShellIntegrationFailure LastShellIntegrationFailure();
const wchar_t* ShellIntegrationFailureName(ShellIntegrationFailureKind kind) noexcept;

} // namespace pulse::app
