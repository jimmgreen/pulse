#include "setup_desktop_launch.h"
#include <sddl.h>
#include <cstdio>
#include <filesystem>
#include <fstream>

DWORD Integrity() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 0;
    alignas(TOKEN_MANDATORY_LABEL) BYTE data[256]{}; DWORD bytes = 0;
    const bool read = GetTokenInformation(token, TokenIntegrityLevel, data, sizeof(data), &bytes) != FALSE;
    CloseHandle(token);
    if (!read) return 0;
    PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(data)->Label.Sid;
    return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
}
int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && wcscmp(argv[1], L"--child") == 0) {
        const DWORD integrity = Integrity();
        if (argc == 3) { std::ofstream report(std::filesystem::path(argv[2]), std::ios::trunc); report << integrity; }
        return static_cast<int>(integrity);
    }
    wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, 32768);
    if (argc == 2 && wcscmp(argv[1], L"--baseline-low") == 0) {
        HANDLE own = nullptr, low = nullptr; PSID sid = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &own)) return 2;
        const BOOL duplicated = DuplicateTokenEx(own, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &low);
        CloseHandle(own);
        if (!duplicated || !ConvertStringSidToSidW(L"S-1-16-4096", &sid)) return 2;
        TOKEN_MANDATORY_LABEL label{}; label.Label.Sid = sid; label.Label.Attributes = SE_GROUP_INTEGRITY;
        const BOOL lowered = SetTokenInformation(low, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(sid));
        LocalFree(sid);
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --child";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
        const BOOL started = lowered && CreateProcessAsUserW(low, exe, command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
        CloseHandle(low);
        if (!started) { std::printf("start error=%lu\n", GetLastError()); return 2; }
        WaitForSingleObject(child.hProcess, 10000); DWORD result = 0;
        GetExitCodeProcess(child.hProcess, &result); CloseHandle(child.hThread); CloseHandle(child.hProcess);
        std::printf("BASELINE child_integrity=%lu expected=8192 inherited_low=%d\n", result, result == 4096);
        return result == SECURITY_MANDATORY_MEDIUM_RID ? 0 : 1;
    }
    const auto folder = std::filesystem::absolute(L"bench_data/qq_drag_fix");
    std::filesystem::create_directories(folder);
    const auto report = folder / (L"desktop-child-" + std::to_wstring(GetCurrentProcessId()) + L".txt");
    const HRESULT hr = pulse::setup::LaunchFromDesktop(exe, L"--child \"" + report.wstring() + L"\"", folder.wstring());
    DWORD level = 0; const auto deadline = GetTickCount64() + 10000;
    while (SUCCEEDED(hr) && GetTickCount64() < deadline) {
        std::ifstream input(report); if (input >> level) break; Sleep(10);
    }
    std::printf("MODIFIED desktop_launch_hr=%08lX child_integrity=%lu expected=8192\n", static_cast<unsigned long>(hr), level);
    return SUCCEEDED(hr) && level == SECURITY_MANDATORY_MEDIUM_RID ? 0 : 1;
}
