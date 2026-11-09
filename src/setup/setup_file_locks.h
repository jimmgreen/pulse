#pragma once
#include <windows.h>
#include <string>
#include <vector>
namespace pulse::setup {
struct LockedProgram { DWORD pid = 0; std::wstring name; };
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
