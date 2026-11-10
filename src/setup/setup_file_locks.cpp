#include "setup_file_locks.h"
#include <restartmanager.h>
#include <filesystem>
#include <tlhelp32.h>
#include <algorithm>
namespace pulse::setup {
namespace {
struct Snapshot {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Snapshot() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
std::wstring CanonicalPath(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    std::wstring full(32768, L'\0');
    DWORD length = 0;
    if (file != INVALID_HANDLE_VALUE) {
        length = GetFinalPathNameByHandleW(file, full.data(), static_cast<DWORD>(full.size()), FILE_NAME_NORMALIZED);
        CloseHandle(file);
    }
    if (!length || length >= full.size()) length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (!length || length >= full.size()) return path;
    full.resize(length);
    if (full.starts_with(L"\\\\?\\UNC\\")) full = L"\\\\" + full.substr(8);
    else if (full.starts_with(L"\\\\?\\")) full.erase(0, 4);
    return full;
}
void IdentifyLoadedModules(FileLockReport& report, const std::vector<std::wstring>& paths) {
    struct Candidate { std::wstring original, canonical, name; };
    std::vector<Candidate> candidates;
    for (const auto& path : paths) candidates.push_back({path, CanonicalPath(path), std::filesystem::path(path).filename().wstring()});
    const ULONGLONG started = GetTickCount64();
    constexpr ULONGLONG budget_ms = 2000;
    constexpr size_t process_limit = 4096, module_limit = 65536;
    size_t process_count = 0, module_count = 0;
    Snapshot processes{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (processes.value == INVALID_HANDLE_VALUE) { report.module_scan_complete = false; return; }
    PROCESSENTRY32W process{sizeof(process)};
    if (!Process32FirstW(processes.value, &process)) { report.module_scan_complete = false; return; }
    do {
        if (++process_count > process_limit || GetTickCount64() - started >= budget_ms) { report.module_scan_complete = false; return; }
        if (!process.th32ProcessID) continue;
        FILETIME created{}, exited{}, kernel{}, user{};
        HANDLE identity = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.th32ProcessID);
        if (identity) { GetProcessTimes(identity, &created, &exited, &kernel, &user); CloseHandle(identity); }
        Snapshot modules;
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            modules.value = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process.th32ProcessID);
            if (modules.value != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
        }
        // Protected processes may decline this read-only snapshot. Never request
        // debug privileges, enumerate private handles, or stop a process.
        if (modules.value == INVALID_HANDLE_VALUE) {
            report.module_scan_complete = false;
            ++report.skipped_processes;
            continue;
        }
        MODULEENTRY32W module{sizeof(module)};
        if (!Module32FirstW(modules.value, &module)) {
            if (GetLastError() != ERROR_NO_MORE_FILES) { report.module_scan_complete = false; ++report.skipped_processes; }
            continue;
        }
        bool found = false;
        do {
            if (++module_count > module_limit || GetTickCount64() - started >= budget_ms) { report.module_scan_complete = false; return; }
            std::wstring canonical;
            for (const auto& candidate : candidates) {
                if (!SamePath(candidate.name, module.szModule)) continue;
                if (canonical.empty()) canonical = CanonicalPath(module.szExePath);
                if (!SamePath(candidate.canonical, canonical)) continue;
                if (std::none_of(report.files.begin(), report.files.end(), [&](const auto& path) { return SamePath(path, candidate.original); }))
                    report.files.push_back(candidate.original);
                found = true;
            }
        } while (Module32NextW(modules.value, &module));
        if (found && std::none_of(report.programs.begin(), report.programs.end(), [&](const auto& old) { return old.pid == process.th32ProcessID; }))
            report.programs.push_back({process.th32ProcessID, process.szExeFile, created});
    } while (Process32NextW(processes.value, &process));
}
}
FileLockReport InspectReplacementLocks(const std::wstring& root, const std::vector<std::wstring>& relative_files, bool known_blocked) {
    FileLockReport result;
    std::vector<std::wstring> candidates;
    for (const auto& relative : relative_files) {
        const std::filesystem::path rel(relative);
        bool invalid = relative.empty() || rel.has_root_path() || relative.find(L':') != std::wstring::npos;
        for (const auto& part : rel) if (part == L"..") invalid = true;
        if (invalid) { result.error = ERROR_INVALID_NAME; return result; }
        const auto path = (std::filesystem::path(root) / rel).wstring();
        candidates.push_back(path);
        HANDLE file = CreateFileW(path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); continue; }
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) continue;
        if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) result.files.push_back(path);
        else { result.error = error; result.files.push_back(path); }
    }
    if (result.files.empty() && !known_blocked) return result;
    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1]{};
    result.identification_error = RmStartSession(&session, 0, key);
    if (result.identification_error != ERROR_SUCCESS) { IdentifyLoadedModules(result, candidates); return result; }
    struct SessionGuard { DWORD id; ~SessionGuard() { RmEndSession(id); } } guard{session};
    std::vector<LPCWSTR> paths;
    for (const auto& path : (result.files.empty() ? candidates : result.files)) paths.push_back(path.c_str());
    DWORD error = RmRegisterResources(session, static_cast<UINT>(paths.size()), paths.data(), 0, nullptr, 0, nullptr);
    if (error == ERROR_SUCCESS) {
        UINT needed = 0, count = 0; DWORD reasons = 0;
        std::vector<RM_PROCESS_INFO> processes;
        for (int retry = 0; retry != 4; ++retry) {
            count = static_cast<UINT>(processes.size());
            error = RmGetList(session, &needed, &count, processes.empty() ? nullptr : processes.data(), &reasons);
            if (error != ERROR_MORE_DATA) break;
            processes.resize(needed);
        }
        if (error == ERROR_SUCCESS) for (UINT i = 0; i < count; ++i)
            result.programs.push_back({processes[i].Process.dwProcessId, processes[i].strAppName, processes[i].Process.ProcessStartTime});
    }
    result.identification_error = error;
    if (error != ERROR_SUCCESS || result.programs.empty()) IdentifyLoadedModules(result, candidates);
    return result;
}
DWORD EndLockedProgram(const LockedProgram& program) {
    if (!program.CanEnd()) return ERROR_ACCESS_DENIED;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, program.pid);
    if (!process) { const DWORD error = GetLastError(); return error == ERROR_INVALID_PARAMETER ? ERROR_SUCCESS : error; }
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{process};
    if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) return ERROR_SUCCESS;
    FILETIME started{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &started, &exited, &kernel, &user)) return GetLastError();
    if (CompareFileTime(&started, &program.started) != 0) return ERROR_INVALID_DATA;
    BOOL critical = FALSE;
    if (!IsProcessCritical(process, &critical)) return GetLastError();
    if (critical) return ERROR_ACCESS_DENIED;
    if (!TerminateProcess(process, ERROR_PROCESS_ABORTED)) return GetLastError();
    const DWORD waited = WaitForSingleObject(process, 5000);
    return waited == WAIT_OBJECT_0 ? ERROR_SUCCESS : waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
}

std::wstring DescribeReplacementLocks(const FileLockReport& report) {
    std::wstring text;
    for (const auto& path : report.files) text += path + L"\n";
    for (const auto& program : report.programs) text += program.name + L" (PID " + std::to_wstring(program.pid) + L")\n";
    if (report.error) text += L"Win32: " + std::to_wstring(report.error) + L"\n";
    if (report.identification_error) text += L"Restart Manager: " + std::to_wstring(report.identification_error) + L"\n";
    if (!report.module_scan_complete) text += L"Loaded-module identification incomplete (scan limit or snapshot unavailable).\n";
    if (report.skipped_processes) text += L"Processes unavailable for module inspection: " + std::to_wstring(report.skipped_processes) + L"\n";
    return text;
}
}
