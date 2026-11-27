#include "route.h"
#include "common/direct_reveal_client.h"
#include <shellapi.h>
#include <cstdio>
int Dispatch(int argc, wchar_t** argv) {
    if (argc != 7 || wcscmp(argv[1], L"--dispatch") ||
        !std::wstring_view(argv[2]).starts_with(L"Local\\Pulse.ExplorerProbe.")) return 87;
    const bool select = wcscmp(argv[3], L"select") == 0;
    if (!select && wcscmp(argv[3], L"folder")) return 87;
    wchar_t expected_cwd[32768]{};
    const DWORD expected_size = GetEnvironmentVariableW(L"PULSE_EXPLORER_PROBE_EXPECT_CWD", expected_cwd, ARRAYSIZE(expected_cwd));
    if (expected_size) {
        wchar_t actual_cwd[32768]{}, marker[32]{};
        if (expected_size >= ARRAYSIZE(expected_cwd) || !GetCurrentDirectoryW(ARRAYSIZE(actual_cwd), actual_cwd) ||
            wcscmp(expected_cwd, actual_cwd) ||
            !GetEnvironmentVariableW(L"PROBE_CHILD_ENV", marker, ARRAYSIZE(marker)) || wcscmp(marker, L"preserved")) return 13;
        std::puts("CONTEXT cwd-env-preserved");
    }
    wchar_t live[2]{};
    const bool live_approved = std::wstring_view(argv[2]) == L"Local\\Pulse.ExplorerProbe.LiveApproved" &&
        GetEnvironmentVariableW(L"PULSE_EXPLORER_PROBE_LIVE", live, ARRAYSIZE(live)) == 1 && live[0] == L'1';
    const std::wstring_view endpoint = live_approved ? std::wstring_view{} : std::wstring_view(argv[2]);
    const DWORD attributes = GetFileAttributesW(argv[4]);
    const bool valid = attributes != INVALID_FILE_ATTRIBUTES && (select || (attributes & FILE_ATTRIBUTE_DIRECTORY));
    pulse::DirectRevealDiagnostics diagnostic;
    const auto result = !valid ? pulse::DirectRevealResult::Rejected : select ?
        pulse::TryDirectReveal(argv[4], 1000, endpoint, &diagnostic) :
        pulse::TryDirectOpenFolder(argv[4], 1000, endpoint, &diagnostic);
    if (result == pulse::DirectRevealResult::Accepted) {
        std::puts("ACK accepted"); return 0;
    }
    // Test harness chooses a stand-in only for fault cases, explicitly recorded
    // in results. The default fallback is the actual original Explorer command.
    wchar_t fixture[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"PULSE_EXPLORER_PROBE_FALLBACK_FIXTURE", fixture, ARRAYSIZE(fixture));
    std::wstring application = length && length < ARRAYSIZE(fixture) ? fixture : argv[5];
    std::wstring command = length && length < ARRAYSIZE(fixture) ?
        explorer_probe::Quote(application) + L" /d /c exit 37" : argv[6];
    STARTUPINFOW startup{sizeof(startup)};
    GetStartupInfoW(&startup);
    PROCESS_INFORMATION process{};
    std::printf("ACK %s fallback=%s\n", result == pulse::DirectRevealResult::Indeterminate ? "indeterminate" : "rejected",
        length ? "test-stand-in" : "native");
    if (!CreateProcessW(application.empty() ? nullptr : application.c_str(), command.data(), nullptr, nullptr,
        TRUE, CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &process)) return static_cast<int>(GetLastError());
    CloseHandle(process.hThread);
    const DWORD waited = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = ERROR_GEN_FAILURE;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count = 0;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return ERROR_INVALID_PARAMETER;
    const int result = Dispatch(count, args);
    LocalFree(args);
    return result;
}
