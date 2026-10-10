#include "../app/shell_integration_registry.h"

#include <shlobj.h>
#include <cstdio>
#include <cwchar>

int wmain(int argc, wchar_t** argv) {
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
