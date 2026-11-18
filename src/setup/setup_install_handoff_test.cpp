#include "setup_install_handoff.h"
#include <cstdio>
#include <string>

int main() {
    int failures = 0;
    auto check = [&](bool passed, const char* name) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    using pulse::setup::NestedInstallDirectories;
    check(!NestedInstallDirectories(L"C:\\Pulse", L"C:\\Pulse"), "same directory retains in-place upgrade");
    check(!NestedInstallDirectories(L"C:\\Pulse", L"D:\\Pulse"), "separate installation directories allowed");
    check(NestedInstallDirectories(L"C:\\Pulse", L"C:\\Pulse\\New") &&
        NestedInstallDirectories(L"C:\\Pulse\\Old", L"C:\\Pulse"), "both nesting directions rejected");
    check(!NestedInstallDirectories(L"C:\\Pulse", L"C:\\Pulse2"), "shared name prefix is not nesting");
    check(NestedInstallDirectories(L"c:/PULSE/", L"C:\\pulse\\new") &&
        !NestedInstallDirectories(L"c:/PULSE/", L"C:\\pulse"), "case and slash normalization preserve path boundaries");
    for (int fail_at = 0; fail_at < 4; ++fail_at) {
        bool old_exists = true;
        const bool new_exists = true; // committed before handing over associations
        bool directory_at_new = false, drive_at_new = false;
        std::string calls;
        std::wstring error;
        const bool ok = pulse::setup::CompleteDirectoryMove(
            [&](std::wstring& failure) {
                calls += 'M';
                directory_at_new = true;
                if (fail_at == 1) { failure = L"partial migration"; return false; }
                drive_at_new = true;
                return true;
            },
            [&](std::wstring& failure) {
                calls += 'U';
                if (fail_at == 2) { failure = L"uninstall failed"; return false; }
                old_exists = false;
                return true;
            },
            [&](std::wstring& failure) {
                calls += 'R';
                if (fail_at == 3) { failure = L"runtime failed"; return false; }
                return true;
            }, error);
        const bool all_targets_exist = (directory_at_new ? new_exists : old_exists) &&
            (drive_at_new ? new_exists : old_exists);
        if (fail_at == 0) check(ok && calls == "MUR" && !old_exists && all_targets_exist,
            "verified migration precedes removal and shared runtime registration");
        if (fail_at == 1) check(!ok && calls == "M" && old_exists && all_targets_exist && error == L"partial migration",
            "partial migration failure preserves both executable targets and skips uninstall");
        if (fail_at == 2) check(!ok && calls == "MU" && all_targets_exist && error == L"uninstall failed",
            "uninstall failure is reported and committed new target remains available");
        if (fail_at == 3) check(!ok && calls == "MUR" && all_targets_exist && error == L"runtime failed",
            "runtime failure cannot report successful installation or remove new target");
    }
    return failures ? 1 : 0;
}
