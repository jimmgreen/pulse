#if defined(PULSE_WITH_TEST_HOOKS)
#include "app_commands.h"
#include "app_navigation.h"
#include "app_runtime.h"
#include "../ui/fluent_menu.h"
#include <filesystem>
#include <fstream>
#include <cstdio>

DWORD PrepareContextMenuSpeedDesktop(int argc, wchar_t** argv) {
    if (!GetEnvironmentVariableW(L"PULSE_TEST_CONTEXT_SPEED", nullptr, 0)) return ERROR_SUCCESS;
    if (argc != 2 || wcscmp(argv[1], L"--test-instance") != 0 ||
        !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0)) return ERROR_INVALID_PARAMETER;
    const auto name = L"PulseMenuSpeed-" + std::to_wstring(GetCurrentProcessId());
    static HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) return GetLastError();
    return ERROR_SUCCESS;
}
namespace {
struct SpeedRun {
    pulse::AppState* state = nullptr;
    ULONGLONG start = 0;
    long long ready_ms = -1;
    bool seen_open = false;
    bool dismiss_sent = false;
};
thread_local SpeedRun* speed_run = nullptr;
void CALLBACK ObserveMenu(HWND, UINT, UINT_PTR, DWORD) {
    auto* run = speed_run;
    if (!run || !run->state->menu) return;
    run->seen_open |= run->state->menu->IsOpen();
    const auto elapsed = GetTickCount64() - run->start;
    if (run->ready_ms < 0 && run->state->context_menu.AnswersComplete())
        run->ready_ms = static_cast<long long>(elapsed);
    if (elapsed >= 1100 && !run->dismiss_sent && run->state->menu->IsOpen()) {
        run->dismiss_sent = true;
        run->state->menu->Dismiss();
    }
}
}
int RunContextMenuSpeedTest(pulse::AppState& state, const wchar_t* output) {
    using namespace pulse;
    if (!state.isolatedTest) return 2;
    FILE* log = nullptr;
    if (_wfreopen_s(&log, output, L"w", stdout) || !log) return 3;
    setvbuf(log, nullptr, _IONBF, 0);
    const auto base = std::filesystem::path(output).parent_path();
    const auto fixture = base / L"fixture";
    std::filesystem::create_directories(fixture / L"folder");
    std::ofstream(fixture / L"sample.txt") << "Menu timing fixture";
    state.appPrefs.persist = false;
    state.ctxMenuPrefs.persist = false;
    NavigateTo(state, fixture.wstring());
    const auto deadline = GetTickCount64() + 15000;
    auto pump = [] {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return false;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        return true;
    };
    auto* tab = ActiveTab(state);
    while (GetTickCount64() < deadline && tab && (tab->loading || tab->pending_generation || tab->EntryCount() < 2)) {
        if (!pump()) return 4;
        Sleep(5);
    }
    if (!tab || tab->loading || tab->pending_generation || tab->EntryCount() < 2) return 5;
    wchar_t case_name[32] = L"txt";
    GetEnvironmentVariableW(L"PULSE_TEST_MENU_CASE", case_name, ARRAYSIZE(case_name));
    const bool background = wcscmp(case_name, L"background") == 0;
    if (!background) {
        const auto name = wcscmp(case_name, L"folder") == 0 ? L"folder" : L"sample.txt";
        bool selected = false;
        for (size_t i = 0; i < tab->EntryCount(); ++i) if (tab->EntryAt(i).name == name) {
            tab->SelectOnly(static_cast<int>(i)); selected = true; break;
        }
        if (!selected) return 6;
    }
    int failures = 0;
    SetEnvironmentVariableW(L"PULSE_MENU_LATENCY", L"1");
    for (int round = 0; round < 9; ++round) {
        pump();
        std::printf("[CASE] name=%ls round=%d cold=%d\n", case_name, round, round == 0);
        LARGE_INTEGER start{}; QueryPerformanceCounter(&start);
        SetEnvironmentVariableW(L"PULSE_MENU_REQUEST_QPC", std::to_wstring(start.QuadPart).c_str());
        SpeedRun run{&state, GetTickCount64()}; speed_run = &run;
        const auto timer = SetTimer(nullptr, 0, 10, ObserveMenu);
        if (!timer) return 7;
        if (background) ShowBackgroundContextMenu(state, {340, 220});
        else ShowItemContextMenu(state, {340, 220});
        KillTimer(nullptr, timer); speed_run = nullptr;
        const bool first_frame = !GetEnvironmentVariableW(L"PULSE_MENU_REQUEST_QPC", nullptr, 0);
        failures += !first_frame || !run.seen_open;
        std::printf("[CASE_END] seen_open=%d first_frame=%d live_ready_ms=%lld elapsed_ms=%llu\n",
            run.seen_open, first_frame, run.ready_ms, GetTickCount64() - run.start);
        SetEnvironmentVariableW(L"PULSE_MENU_REQUEST_QPC", nullptr);
        Sleep(60);
    }
    SetEnvironmentVariableW(L"PULSE_MENU_LATENCY", nullptr);
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
#endif

