#pragma once
#include <windows.h>
#include <string>
#include <vector>
namespace pulse::setup {
struct LockedProgram {
    DWORD pid = 0;
    std::wstring name;
    FILETIME started{};
    bool CanEnd() const { return pid > 4 && pid != GetCurrentProcessId() && (started.dwLowDateTime || started.dwHighDateTime); }
};
// Explicit user action only; verifies the recorded creation time using the same
// handle used to terminate, so a reused PID is never acted on.
DWORD EndLockedProgram(const LockedProgram& program);
struct FileLockReport {
    std::vector<std::wstring> files;
    std::vector<LockedProgram> programs;
    DWORD error = ERROR_SUCCESS;
    DWORD identification_error = ERROR_SUCCESS;
    bool module_scan_complete = true;
    DWORD skipped_processes = 0;
    bool Ready() const { return error == ERROR_SUCCESS && files.empty(); }
};
// Read-only sharing probe; Restart Manager is used for identification only.
// known_blocked also identifies mapped images after a real deletion attempt failed.
FileLockReport InspectReplacementLocks(const std::wstring& root, const std::vector<std::wstring>& relative_files,
    bool known_blocked = false);
std::wstring DescribeReplacementLocks(const FileLockReport& report);
}
