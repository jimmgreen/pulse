#include "../app/app_internal.h"
#include <cstdio>
#include <filesystem>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int);
int RunFileClickProbe(pulse::AppState&, FILE*);
namespace {
HWND target = nullptr;
HHOOK hook = nullptr;
int result = 2;
ULONGLONG started = 0;
void CALLBACK Probe(HWND, UINT, UINT_PTR timer, DWORD) {
    auto* state = target ? pulse::GetAppState(target) : nullptr;
    if (!state || !state->startupComplete || !IsWindowVisible(target)) {
        if (GetTickCount64() - started > 30000) { KillTimer(nullptr, timer); PostQuitMessage(2); }
        return;
    }
    KillTimer(nullptr, timer);
    wchar_t report[32768]{};
    GetEnvironmentVariableW(L"PULSE_TEST_FILE_CLICK_REPORT", report, ARRAYSIZE(report));
    FILE* file = nullptr;
    if (_wfopen_s(&file, report, L"w") == 0 && file) {
        result = RunFileClickProbe(*state, file);
        std::fclose(file);
    }
    DestroyWindow(target);
    PostQuitMessage(result);
}
LRESULT CALLBACK Observe(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && !target) {
        const auto* message = reinterpret_cast<CWPSTRUCT*>(lp);
        wchar_t name[128]{}; GetClassNameW(message->hwnd, name, ARRAYSIZE(name));
        if (wcscmp(name, L"PulseMainWindow") == 0) target = message->hwnd;
    }
    return CallNextHookEx(hook, code, wp, lp);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || wcscmp(argv[1], L"--test-instance") != 0) return 2;
    wchar_t profile[32768]{}, report[32768]{};
    if (!GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", profile, ARRAYSIZE(profile)) ||
        !GetEnvironmentVariableW(L"PULSE_TEST_FILE_CLICK_REPORT", report, ARRAYSIZE(report))) return 2;
    std::filesystem::create_directories(profile);
    hook = SetWindowsHookExW(WH_CALLWNDPROC, Observe, nullptr, GetCurrentThreadId());
    if (!hook) return 2;
    started = GetTickCount64();
    SetTimer(nullptr, 0, 100, Probe);
    wchar_t args[] = L"--test-instance";
    wWinMain(GetModuleHandleW(nullptr), nullptr, args, SW_SHOWNORMAL);
    UnhookWindowsHookEx(hook);
    std::printf("FILE_CLICK_LIVE_EXIT=%d\n", result);
    return result;
}
