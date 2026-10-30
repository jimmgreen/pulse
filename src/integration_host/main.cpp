#include "../app/shell_integration_registry.h"
#include "../app/system_ctx_registry.h"

#include <shlobj.h>
#include <cstdio>
#include <cwchar>

namespace {

// "file in use" prompt, 立即释放 on an owner the window may not end (a service,
// another user's process): the same helper run elevated ends it. The start time
// pins the process instance, so a recycled PID is never killed.
//   --kill-pid <pid> --start <filetime> --result <file>
// Ending a service process (Pulse's index host runs as one) needs
// SeDebugPrivilege in addition to the elevated token.
bool EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    LUID id{};
    bool enabled = false;
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &id)) {
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Luid = id;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        enabled = AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) != FALSE &&
                  GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(token);
    return enabled;
}

int KillPidMain(unsigned long pid, unsigned long long start, const wchar_t* result_path) {
    const wchar_t* outcome = L"failed";
    EnableDebugPrivilege();
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, static_cast<DWORD>(pid));
    if (!process) {
        const DWORD error = GetLastError();
        outcome = error == ERROR_INVALID_PARAMETER ? L"notfound"
                : error == ERROR_ACCESS_DENIED    ? L"denied"
                                                  : L"failed";
    } else {
        FILETIME created{}, exited{}, kernel{}, user{};
        unsigned long long actual = 0;
        if (GetProcessTimes(process, &created, &exited, &kernel, &user))
            actual = (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
            outcome = L"notfound";
        } else if (start != 0 && actual != 0 && actual != start) {
            outcome = L"notfound";
        } else if (TerminateProcess(process, 1)) {
            outcome = WaitForSingleObject(process, 5000) == WAIT_OBJECT_0 ? L"ok" : L"timeout";
        } else {
            outcome = GetLastError() == ERROR_ACCESS_DENIED ? L"denied" : L"failed";
        }
        CloseHandle(process);
    }
    const HANDLE file = CreateFileW(result_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        char bytes[16]{};
        int length = 0;
        for (const wchar_t* c = outcome; *c && length < 15; ++c) bytes[length++] = static_cast<char>(*c);
        DWORD written = 0;
        WriteFile(file, bytes, static_cast<DWORD>(length), &written, nullptr);
        CloseHandle(file);
    }
    return wcscmp(outcome, L"ok") == 0 ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // Machine-wide context menu changes from Settings, one UAC per batch.
    if (argc >= 2 && wcscmp(argv[1], L"--system-ctx-apply") == 0) {
        if (argc != 5 || wcscmp(argv[3], L"--result") != 0) return 2;
        return pulse::app::sysctx::RunElevatedBatch(argv[2], argv[4]);
    }
    // One process the file-in-use prompt could not end without administrator
    // rights, started with "runas" by the window.
    if (argc >= 7 && wcscmp(argv[1], L"--kill-pid") == 0 && wcscmp(argv[3], L"--start") == 0 &&
        wcscmp(argv[5], L"--result") == 0) {
        wchar_t* end = nullptr;
        const unsigned long pid = wcstoul(argv[2], &end, 10);
        if (!end || *end != L'\0' || pid == 0) return 2;
        const unsigned long long start = wcstoull(argv[4], &end, 10);
        if (!end || *end != L'\0') return 2;
        return KillPidMain(pid, start, argv[6]);
    }
    bool restore = false, prepare = false, folders = false, win_e = false, this_pc = false, directory = false, drive = false,
        recycle_bin = false;
    std::wstring exe, previous_exe;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--restore") == 0) restore = true;
        else if (wcscmp(argv[i], L"--prepare-upgrade") == 0) prepare = true;
        else if (wcscmp(argv[i], L"--folders") == 0) folders = true;
        else if (wcscmp(argv[i], L"--win-e") == 0) win_e = true;
        else if (wcscmp(argv[i], L"--this-pc") == 0) this_pc = true;
        else if (wcscmp(argv[i], L"--recycle-bin") == 0) recycle_bin = true;
        else if (wcscmp(argv[i], L"--directory") == 0) directory = true;
        else if (wcscmp(argv[i], L"--drive") == 0) drive = true;
        else if (wcscmp(argv[i], L"--exe") == 0 && i + 1 < argc) exe = argv[++i];
        else if (wcscmp(argv[i], L"--upgrade-from") == 0 && i + 1 < argc) previous_exe = argv[++i];
        else return 2;
    }
    if (exe.empty() || (!restore && !folders && !win_e && !this_pc && !directory && !drive && !recycle_bin) ||
        (restore && !previous_exe.empty())) return 2;
    using namespace pulse::app;
    bool ok = true;
    auto apply = [&](ShellIntegrationKind kind) {
        if (prepare) return PrepareShellIntegrationUpgrade(kind, exe);
        return previous_exe.empty() ? ApplyShellIntegration(kind, exe, !restore)
                                   : UpgradeShellIntegration(kind, previous_exe, exe);
    };
    if (restore || folders) ok = apply(ShellIntegrationKind::Folders) && ok;
    if (restore || win_e) ok = apply(ShellIntegrationKind::WinE) && ok;
    if (restore || this_pc) ok = apply(ShellIntegrationKind::ThisPc) && ok;
    if (restore || recycle_bin) ok = apply(ShellIntegrationKind::RecycleBin) && ok;
    // Uninstall: overrides an old Pulse left without its command go too.
    if (restore && HasLegacyShellIntegrationResidue()) ok = RepairLegacyShellIntegrationResidue() && ok;
    if (directory) ok = apply(ShellIntegrationKind::Directory) && ok;
    if (drive) ok = apply(ShellIntegrationKind::Drive) && ok;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    std::printf("Shell integration %s: %s\n", restore ? "restore" : "apply",
                ok ? "complete" : "incomplete; unknown legacy values or write failures were preserved");
    if (restore && (HasShellIntegrationOwnership(ShellIntegrationKind::Folders, exe) ||
        HasShellIntegrationOwnership(ShellIntegrationKind::WinE, exe) ||
        HasShellIntegrationOwnership(ShellIntegrationKind::ThisPc, exe) ||
        HasShellIntegrationOwnership(ShellIntegrationKind::RecycleBin, exe))) return 3;
    return ok ? 0 : 1;
}
