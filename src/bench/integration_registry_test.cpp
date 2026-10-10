#include "../app/shell_integration_registry.h"
#include <cstdio>
#include <string>
#include <vector>
#include <aclapi.h>

namespace {
#ifdef PULSE_INTEGRATION_TEST
int writes = 0;
bool FailEnableAndRollback(const std::wstring&, const std::wstring&) {
    ++writes;
    return writes != 4 && writes != 5;
}
struct InterruptedUpgrade {};
bool InterruptUpgrade(const std::wstring&, const std::wstring&) {
    if (++writes == 3) throw InterruptedUpgrade{};
    return true;
}
bool FailRepairAndRollback(const std::wstring&, const std::wstring&) {
    ++writes;
    return writes != 3 && writes != 4;
}
#endif
int failures = 0;
void Check(bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures; }
struct Value { bool exists = false; DWORD type = REG_NONE; std::vector<BYTE> bytes; bool operator==(const Value&) const = default; };
Value Read(const std::wstring& path, const wchar_t* name = L"") {
    Value v; HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return v;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &v.type, nullptr, &size) == ERROR_SUCCESS) {
        v.bytes.resize(size);
        v.exists = RegQueryValueExW(key, name, nullptr, &v.type, v.bytes.data(), &size) == ERROR_SUCCESS;
    }
    RegCloseKey(key); return v;
}
bool Set(const std::wstring& path, const wchar_t* name, const std::wstring& text, DWORD type = REG_SZ) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const bool ok = RegSetValueExW(key, name, 0, type, reinterpret_cast<const BYTE*>(text.c_str()), static_cast<DWORD>((text.size()+1)*sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(key); return ok;
}
void Clear() { RegDeleteTreeW(HKEY_CURRENT_USER, L"Software"); }
bool KeyExists(const std::wstring& path) {
    HKEY key = nullptr;
    const LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key);
    if (key) RegCloseKey(key);
    return status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND;
}
void DeleteValue(const std::wstring& path, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, name);
        RegCloseKey(key);
    }
}
void SetLegacyOrphans() {
    for (const auto* group : {L"Directory", L"Drive"}) {
        const auto shell = std::wstring(L"Software\\Classes\\") + group + L"\\shell";
        Set(shell, L"", L"open");
        Set(shell + L"\\open", L"DelegateExecute", L"");
    }
    Set(L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\\shell\\opennewwindow\\command", L"DelegateExecute", L"");
    Set(L"Software\\Classes\\CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell", L"", L"open");
    Set(L"Software\\Classes\\CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\open\\command", L"DelegateExecute", L"");
}
}

int wmain() {
    const std::wstring sandbox = L"Software\\PulseTest\\IntegrationRegistry-" + std::to_wstring(GetCurrentProcessId());
    HKEY root = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sandbox.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) != ERROR_SUCCESS) return 2;
    if (RegOverridePredefKey(HKEY_CURRENT_USER, root) != ERROR_SUCCESS) { RegCloseKey(root); return 2; }
    using namespace pulse::app;
    const std::wstring exe = L"C:\\Pulse test\\pulse.exe", next = L"D:\\Pulse\\pulse.exe";
    const std::wstring shell = L"Software\\Classes\\Directory\\shell", command = shell + L"\\open\\command";
    const std::wstring win = L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\\shell\\opennewwindow\\command";
    const std::wstring pc = L"Software\\Classes\\CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell";
    SetLegacyOrphans();
    Check(HasLegacyShellIntegrationResidue(), "remote seven-value orphan signature is detected without a command or snapshot");
    Check(ApplyShellIntegration(ShellIntegrationKind::ThisPc, exe, false) && HasLegacyShellIntegrationResidue(),
        "ordinary disable does not infer ownership of a legacy orphan");
    Check(!ApplyShellIntegration(ShellIntegrationKind::ThisPc, exe, true) && HasLegacyShellIntegrationResidue() &&
        !Read(L"Software\\Pulse\\ShellIntegration\\Backups\\v1\\ThisPc", L"Snapshot").exists,
        "enable requires explicit repair instead of backing up damaged legacy overrides as originals");
    Check(LastShellIntegrationFailure().kind == ShellIntegrationFailureKind::LegacyResidue,
        "legacy residue is reported as its own failure reason");
    Set(shell + L"\\ThirdParty\\command", L"", L"third party command");
    Check(RepairLegacyShellIntegrationResidue() && !HasLegacyShellIntegrationResidue() &&
        !KeyExists(pc) && !KeyExists(win) && !Read(shell).exists &&
        Read(shell + L"\\ThirdParty\\command").exists &&
        Read(L"Software\\Pulse\\ShellIntegration\\Backups\\v1\\LegacyOrphanRepair", L"Snapshot").exists,
        "explicit legacy repair backs up seven values and preserves third-party menus");
    Check(!RepairLegacyShellIntegrationResidue() && Read(shell + L"\\ThirdParty\\command").exists,
        "repeated repair makes no changes after residue is gone");
    Clear(); SetLegacyOrphans(); Set(pc + L"\\open\\command", L"", L"foreign command");
    Check(!HasLegacyShellIntegrationResidue() && !RepairLegacyShellIntegrationResidue() && Read(shell).exists,
        "a new command aborts the entire legacy repair before writes");
    Clear(); SetLegacyOrphans(); Set(win, L"DelegateExecute", L"foreign delegate");
    Check(!HasLegacyShellIntegrationResidue() && !RepairLegacyShellIntegrationResidue() && Read(shell).exists,
        "a changed delegate aborts the entire legacy repair before writes");
    Clear(); SetLegacyOrphans();
    Set(L"Software\\Pulse\\ShellIntegration\\Backups\\v1\\LegacyOrphanRepair", L"Snapshot", L"different existing backup", REG_BINARY);
    Check(!RepairLegacyShellIntegrationResidue() && HasLegacyShellIntegrationResidue(),
        "an incompatible backup prevents all repair writes");
    Clear();
    Set(command, L"", L"%OTHER%\\manager.exe %1", REG_EXPAND_SZ);
    Set(command, L"DelegateExecute", L"", REG_SZ);
    Set(shell, L"", L"browse", REG_EXPAND_SZ);
    Set(win, L"", L"old win", REG_BINARY);
    Set(pc, L"", L"old pc", REG_BINARY);
    const auto original = Read(command), delegate = Read(command, L"DelegateExecute"), verb = Read(shell), old_win = Read(win), old_pc = Read(pc);
    bool enabled = true;
    for (auto kind : {ShellIntegrationKind::Folders, ShellIntegrationKind::WinE, ShellIntegrationKind::ThisPc}) enabled = ApplyShellIntegration(kind, exe, true) && ReadShellIntegration(kind, exe) && enabled;
    Check(LastShellIntegrationFailure().kind == ShellIntegrationFailureKind::None, "a successful apply clears the failure reason");
    Check(enabled, "enable all associations and read actual state");
    Check(ApplyShellIntegration(ShellIntegrationKind::Folders, exe, true), "repeated enable retains original snapshot");
    bool restored = true;
    for (auto kind : {ShellIntegrationKind::Folders, ShellIntegrationKind::WinE, ShellIntegrationKind::ThisPc}) restored = ApplyShellIntegration(kind, exe, false) && restored;
    Check(restored && Read(command)==original && Read(command,L"DelegateExecute")==delegate && Read(shell)==verb && Read(win)==old_win && Read(pc)==old_pc, "restore exact raw types contents and empty values");
    Check(!Read(L"Software\\Classes\\Drive\\shell\\open\\command").exists && !Read(pc+L"\\open\\command").exists, "restore original value absence");
    ApplyShellIntegration(ShellIntegrationKind::Directory, exe, true);
    Set(command,L"DelegateExecute",L"third party"); Set(shell,L"",L"third verb");
    Check(ApplyShellIntegration(ShellIntegrationKind::Directory,exe,false) && Read(command)==original && Read(command,L"DelegateExecute")!=delegate && Read(shell)!=verb, "restore preserves independently modified values");
    ApplyShellIntegration(ShellIntegrationKind::Directory,exe,true);
    Set(command,L"",L"\"C:\\Other\\manager.exe\" %1"); Set(command,L"DelegateExecute",L"new owner");
    const auto other=Read(command), other_delegate=Read(command,L"DelegateExecute");
    Check(ApplyShellIntegration(ShellIntegrationKind::Directory,exe,false) && Read(command)==other, "external command ownership prevents restoration");
    Check(ApplyShellIntegration(ShellIntegrationKind::Directory,exe,true) && ApplyShellIntegration(ShellIntegrationKind::Directory,exe,false) && Read(command)==other && Read(command,L"DelegateExecute")==other_delegate, "reacquire refreshes stale backup");
    Clear(); Set(shell,L"",L"open"); const auto open=Read(shell);
    Check(ApplyShellIntegration(ShellIntegrationKind::Folders,exe,false) && Read(shell)==open, "open default alone is not ownership");
    Set(command,L"",L"\""+exe+L"\" \"%1\""); Set(command,L"DelegateExecute",L"");
    // #81: an empty per-user DelegateExecute still overrides the system handler
    // after Pulse's command has gone. A missing backup must not leave it behind.
    Check(!ApplyShellIntegration(ShellIntegrationKind::Directory,exe,false) && !KeyExists(shell), "legacy restore removes owned overrides instead of leaving an unusable open verb");
    for (const bool migrate : {false, true}) {
        Clear();
        Set(win, L"", L"\"" + exe + L"\"");
        Set(win, L"DelegateExecute", L"");
        if (migrate) Check(ApplyShellIntegration(ShellIntegrationKind::WinE, exe, true), "legacy Win+E registration migrates");
        Check(!ApplyShellIntegration(ShellIntegrationKind::WinE, exe, false) && !KeyExists(win.substr(0, win.find(L"\\shell"))),
            "legacy Win+E restore removes empty per-user CLSID override with or without migration");
    }
    Clear();
    Set(pc, L"", L"open");
    Set(pc + L"\\open\\command", L"", L"\"" + exe + L"\" \"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\"");
    Set(pc + L"\\open\\command", L"DelegateExecute", L"");
    Check(!ApplyShellIntegration(ShellIntegrationKind::ThisPc, exe, false) && !KeyExists(pc.substr(0, pc.find(L"\\shell"))),
        "legacy This PC restore releases the system CLSID handler");
    {
        // Desktop Recycle Bin: same CLSID verb shape as This PC.
        const std::wstring bin = L"Software\\Classes\\CLSID\\{645FF040-5081-101B-9F08-00AA002F954E}\\shell";
        Clear();
        Check(!ReadShellIntegration(ShellIntegrationKind::RecycleBin, exe) &&
            ApplyShellIntegration(ShellIntegrationKind::RecycleBin, exe, true) &&
            ReadShellIntegration(ShellIntegrationKind::RecycleBin, exe) &&
            HasShellIntegrationOwnership(ShellIntegrationKind::RecycleBin, exe),
            "Recycle Bin takeover applies and reads back");
        const Value bin_command = Read(bin + L"\\open\\command");
        const std::wstring expected = L"\"" + exe + L"\" \"::{645FF040-5081-101B-9F08-00AA002F954E}\"";
        Check(bin_command.exists && bin_command.bytes.size() == (expected.size() + 1) * sizeof(wchar_t) &&
            std::wstring(reinterpret_cast<const wchar_t*>(bin_command.bytes.data())) == expected &&
            Read(bin + L"\\open\\command", L"DelegateExecute").exists && Read(bin).exists,
            "Recycle Bin open verb runs Pulse with the parsing name and is the default verb");
        Check(!ReadShellIntegration(ShellIntegrationKind::ThisPc, exe) && !KeyExists(pc),
            "Recycle Bin takeover leaves This PC alone");
        Check(ApplyShellIntegration(ShellIntegrationKind::RecycleBin, exe, false) && !KeyExists(bin.substr(0, bin.find(L"\\shell"))) &&
            !HasShellIntegrationOwnership(ShellIntegrationKind::RecycleBin, exe),
            "Recycle Bin restore removes the per-user CLSID override it created");
        Clear();
        Set(bin, L"", L"empty");
        const Value before = Read(bin);
        Check(ApplyShellIntegration(ShellIntegrationKind::RecycleBin, exe, true) &&
            ApplyShellIntegration(ShellIntegrationKind::RecycleBin, exe, false) && Read(bin) == before &&
            !Read(bin + L"\\open\\command").exists, "Recycle Bin restore brings back an earlier default verb");
    }
    for (const bool reenable : {false, true}) {
        Clear();
        Set(win, L"", L"\"" + exe + L"\""); Set(win, L"DelegateExecute", L"");
        Check(ApplyShellIntegration(ShellIntegrationKind::WinE, exe, true), "capture legacy ownership before partial restoration");
        DeleteValue(win, L""); // Previous releases removed the command but left the override and snapshot.
        if (reenable) Check(ApplyShellIntegration(ShellIntegrationKind::WinE, exe, true), "reenable retains partial restore provenance");
        Check(!ApplyShellIntegration(ShellIntegrationKind::WinE, exe, false) && !KeyExists(win.substr(0, win.find(L"\\shell"))),
            "snapshot-backed partial restore is repairable without preserving the broken override");
    }
    for (const auto kind : {ShellIntegrationKind::WinE, ShellIntegrationKind::ThisPc}) {
        Clear();
        const auto path = kind == ShellIntegrationKind::WinE ? win : pc + L"\\open\\command";
        Check(ApplyShellIntegration(kind, exe, true), "enable isolated namespace override");
        DeleteValue(path, L"");
        Check(HasShellIntegrationOwnership(kind, exe) && !ReadShellIntegration(kind, exe),
            "removed command retains detectable snapshot-backed Explorer override for repair");
        Check(!HasShellIntegrationOwnership(kind, next), "partial restore provenance belongs to the recorded executable");
        Check(ApplyShellIntegration(kind, exe, false) && !KeyExists(path.substr(0, path.find(L"\\shell"))) &&
            !HasShellIntegrationOwnership(kind, exe), "retry clears partial namespace override and repair state");
        Check(ApplyShellIntegration(kind, exe, true), "capture completed restore provenance");
        RegDeleteTreeW(HKEY_CURRENT_USER, path.substr(0, path.find(L"\\shell")).c_str());
        Check(!HasShellIntegrationOwnership(kind, exe), "stale snapshot without remaining overrides is not ownership");
    }
    Clear();
    Set(win, L"", L"\"" + exe + L"\""); Set(win, L"DelegateExecute", L"");
    Check(ApplyShellIntegration(ShellIntegrationKind::WinE, exe, true), "capture legacy ownership for external handler test");
    DeleteValue(win, L""); Set(win, L"DelegateExecute", L"{11111111-2222-3333-4444-555555555555}");
    const auto later_delegate = Read(win, L"DelegateExecute");
    Check(!HasShellIntegrationOwnership(ShellIntegrationKind::WinE, exe), "external COM handler is not reported as a Pulse residual");
    ApplyShellIntegration(ShellIntegrationKind::WinE, exe, false);
    Check(Read(win, L"DelegateExecute") == later_delegate, "partial restore preserves a later external COM handler");
    Clear();
    Set(win,L"",L"\""+exe+L"\""); Set(win,L"DelegateExecute",L"");
    Set(win,L"PulseBackup",L"legacy command"); Set(win,L"PulseBackupDelegateExecute",L"");
    const auto legacy_command=Read(win,L"PulseBackup"), legacy_delegate=Read(win,L"PulseBackupDelegateExecute");
    Check(!ApplyShellIntegration(ShellIntegrationKind::WinE,exe,false) && Read(win)==legacy_command && Read(win,L"DelegateExecute")==legacy_delegate, "legacy text backup is recovered but original type remains unknown");
    Set(win,L"",L"\""+exe+L"\"");
    Check(ApplyShellIntegration(ShellIntegrationKind::WinE,exe,true) && !ApplyShellIntegration(ShellIntegrationKind::WinE,exe,false) && Read(win)==legacy_command, "enable migrates available legacy backup without claiming exact restoration");
    Clear();
    Check(ApplyShellIntegration(ShellIntegrationKind::Directory,exe,true) && HasShellIntegrationOwnership(ShellIntegrationKind::Folders,exe) && !ReadShellIntegration(ShellIntegrationKind::Folders,exe), "partial Directory ownership is detectable");
    Clear(); Set(command,L"",L"original",REG_BINARY); const auto upgrade_original=Read(command);
    Check(ApplyShellIntegration(ShellIntegrationKind::Directory,exe,true) && UpgradeShellIntegration(ShellIntegrationKind::Directory,exe,next) && ReadShellIntegration(ShellIntegrationKind::Directory,next) && ApplyShellIntegration(ShellIntegrationKind::Directory,next,false) && Read(command)==upgrade_original, "upgrade executable path keeps original snapshot");
    ApplyShellIntegration(ShellIntegrationKind::Directory,exe,true); Set(command,L"",L"other"); const auto takeover=Read(command);
    Check(UpgradeShellIntegration(ShellIntegrationKind::Directory,exe,next) && Read(command)==takeover, "upgrade preserves later ownership takeover");
    Clear(); Set(win,L"",L"\""+exe+L"\""); Set(win,L"DelegateExecute",L""); Set(win,L"PulseBackup",L"legacy before upgrade");
    const auto upgrade_legacy=Read(win,L"PulseBackup");
    Check(PrepareShellIntegrationUpgrade(ShellIntegrationKind::WinE,exe),"prepare captures legacy backup before old uninstaller consumes it");
    Set(win,L"",L"legacy before upgrade");
    HKEY legacy_key=nullptr; RegOpenKeyExW(HKEY_CURRENT_USER,win.c_str(),0,KEY_SET_VALUE,&legacy_key);
    if(legacy_key) { RegDeleteValueW(legacy_key,L"PulseBackup"); RegCloseKey(legacy_key); }
    Check(UpgradeShellIntegration(ShellIntegrationKind::WinE,exe,next) && ReadShellIntegration(ShellIntegrationKind::WinE,next) && !ApplyShellIntegration(ShellIntegrationKind::WinE,next,false) && Read(win)==upgrade_legacy,"old uninstaller restored value is reacquired with preserved legacy backup");
    Clear(); ApplyShellIntegration(ShellIntegrationKind::Directory,exe,true); PrepareShellIntegrationUpgrade(ShellIntegrationKind::Directory,exe);
    Set(command,L"",L"changed during upgrade"); const auto concurrent=Read(command);
    Check(UpgradeShellIntegration(ShellIntegrationKind::Directory,exe,next) && Read(command)==concurrent,"prepared upgrade still preserves unexpected third-party command");
    Clear(); Set(L"Software\\Pulse\\ShellIntegration\\Backups\\v1\\Directory",L"Snapshot",L"corrupt",REG_BINARY);
    Check(!ApplyShellIntegration(ShellIntegrationKind::Directory,exe,true) && !Read(command).exists, "corrupt snapshot fails without association changes");
    Clear(); Set(command,L"",L"protected original"); const auto protected_original=Read(command);
    HKEY protected_key=nullptr; PACL old_acl=nullptr, denied_acl=nullptr; PSECURITY_DESCRIPTOR descriptor=nullptr;
    BYTE everyone[SECURITY_MAX_SID_SIZE]{}; DWORD everyone_size=sizeof(everyone);
    bool secured=RegOpenKeyExW(HKEY_CURRENT_USER,command.c_str(),0,KEY_ALL_ACCESS,&protected_key)==ERROR_SUCCESS;
    secured=secured && GetSecurityInfo(protected_key,SE_REGISTRY_KEY,DACL_SECURITY_INFORMATION,nullptr,nullptr,&old_acl,nullptr,&descriptor)==ERROR_SUCCESS;
    secured=secured && CreateWellKnownSid(WinWorldSid,nullptr,everyone,&everyone_size);
    EXPLICIT_ACCESSW deny{}; deny.grfAccessPermissions=KEY_SET_VALUE; deny.grfAccessMode=DENY_ACCESS;
    deny.Trustee.TrusteeForm=TRUSTEE_IS_SID; deny.Trustee.ptstrName=reinterpret_cast<LPWSTR>(everyone);
    secured=secured && SetEntriesInAclW(1,&deny,old_acl,&denied_acl)==ERROR_SUCCESS;
    secured=secured && SetSecurityInfo(protected_key,SE_REGISTRY_KEY,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,denied_acl,nullptr)==ERROR_SUCCESS;
    Check(secured && !ApplyShellIntegration(ShellIntegrationKind::Directory,exe,true) && Read(command)==protected_original && !HasShellIntegrationOwnership(ShellIntegrationKind::Directory,exe), "denied write reports failure and leaves original association");
    if (secured) {
        const auto denied = LastShellIntegrationFailure();
        Check(denied.kind==ShellIntegrationFailureKind::AccessDenied && denied.status==ERROR_ACCESS_DENIED && denied.key==command,
              "denied write is reported as access denied with its key");
    }
    if (secured) Check(SetSecurityInfo(protected_key,SE_REGISTRY_KEY,DACL_SECURITY_INFORMATION|UNPROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,old_acl,nullptr)==ERROR_SUCCESS,"isolated ACL restored");
    if (secured) {
        Check(ApplyShellIntegration(ShellIntegrationKind::Directory,exe,true),"retry succeeds after write access is restored");
        const bool denied_again=SetSecurityInfo(protected_key,SE_REGISTRY_KEY,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,denied_acl,nullptr)==ERROR_SUCCESS;
        Check(denied_again && !UpgradeShellIntegration(ShellIntegrationKind::Directory,exe,next) && ReadShellIntegration(ShellIntegrationKind::Directory,exe),"failed path upgrade rolls back snapshot and association");
        Check(!ApplyShellIntegration(ShellIntegrationKind::Directory,exe,false) && HasShellIntegrationOwnership(ShellIntegrationKind::Directory,exe) && !ReadShellIntegration(ShellIntegrationKind::Directory,exe),"failed restoration retains owned anchor and exposes incomplete state");
        const bool restored_acl=SetSecurityInfo(protected_key,SE_REGISTRY_KEY,DACL_SECURITY_INFORMATION|UNPROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,old_acl,nullptr)==ERROR_SUCCESS;
        Check(restored_acl && ApplyShellIntegration(ShellIntegrationKind::Directory,exe,false) && Read(command)==protected_original,"partial restore can be retried without losing original values");
    }
    if (denied_acl) LocalFree(denied_acl); if (descriptor) LocalFree(descriptor); if (protected_key) RegCloseKey(protected_key);
#ifdef PULSE_INTEGRATION_TEST
    for (bool retry_upgrade : {false, true}) {
        Clear(); Set(command, L"", L"original manager.exe %1");
        const auto original_upgrade = Read(command);
        Check(ApplyShellIntegration(ShellIntegrationKind::Directory, exe, true), "upgrade interruption fixture owns original command");
        writes = 0;
        SetIntegrationWriteHookForTesting(InterruptUpgrade);
        bool interrupted = false;
        try { UpgradeShellIntegration(ShellIntegrationKind::Directory, exe, next); }
        catch (const InterruptedUpgrade&) { interrupted = true; }
        SetIntegrationWriteHookForTesting(nullptr);
        Check(interrupted && ReadShellIntegration(ShellIntegrationKind::Directory, exe),
              "interruption after new snapshot commit retains old command and old ownership journal");
        const auto& owner = retry_upgrade ? next : exe;
        const bool recovered = !retry_upgrade || UpgradeShellIntegration(ShellIntegrationKind::Directory, exe, next);
        Check(recovered && ApplyShellIntegration(ShellIntegrationKind::Directory, owner, false) && Read(command) == original_upgrade,
              "interrupted upgrade supports direct restore or upgrade retry without losing original association");
    }
    Clear(); SetLegacyOrphans(); writes = 0;
    SetIntegrationWriteHookForTesting(FailRepairAndRollback);
    const bool repaired_with_failure = RepairLegacyShellIntegrationResidue();
    SetIntegrationWriteHookForTesting(nullptr);
    Check(!repaired_with_failure && HasLegacyShellIntegrationResidue(),
          "partial legacy repair remains discoverable using its exact backup");
    Check(RepairLegacyShellIntegrationResidue() && !HasLegacyShellIntegrationResidue(),
          "retry resumes partial legacy repair without requiring original full signature");
    Clear();
    Set(command, L"", L"third-party.exe %1");
    Set(command, L"DelegateExecute", L"third-party-delegate");
    Set(shell, L"", L"browse");
    const auto rollback_command = Read(command), rollback_delegate = Read(command, L"DelegateExecute");
    writes = 0;
    SetIntegrationWriteHookForTesting(FailEnableAndRollback);
    const bool enabled_with_failure = ApplyShellIntegration(ShellIntegrationKind::Directory, exe, true);
    SetIntegrationWriteHookForTesting(nullptr);
    const auto hook_failure = LastShellIntegrationFailure();
    Check(!enabled_with_failure && HasShellIntegrationOwnership(ShellIntegrationKind::Directory, exe),
          "enable and compensation failures retain command ownership anchor");
    Check(hook_failure.kind == ShellIntegrationFailureKind::WriteError && !hook_failure.key.empty(),
          "the first failed write is reported, not the compensation");
    Check(ApplyShellIntegration(ShellIntegrationKind::Directory, exe, false) &&
          Read(command) == rollback_command && Read(command, L"DelegateExecute") == rollback_delegate,
          "retry after failed enable compensation restores original command and delegate");
#endif
    RegOverridePredefKey(HKEY_CURRENT_USER,nullptr); RegCloseKey(root);
    const LONG cleanup=RegDeleteTreeW(HKEY_CURRENT_USER,sandbox.c_str());
    Check(cleanup==ERROR_SUCCESS, "isolated registry fixture removed");
    return failures ? 1 : 0;
}
