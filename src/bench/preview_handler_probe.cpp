#include "../ui/preview_handler_host.h"
#include <ole2.h>
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

namespace pulse::ui {
void ResetPreviewHandlerOpenAttemptsForTest();
uint32_t PreviewHandlerOpenAttemptsForTest();
bool PreviewHandlerCanActivateIsolatedForTest(const std::wstring& path);
}

namespace {

LRESULT CALLBACK OwnerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || ((wcscmp(argv[1], L"--selftest") == 0 ||
                      wcscmp(argv[1], L"--isolated-test") == 0) && argc < 3)) {
        wprintf(L"usage: pulse_preview_handler_probe.exe "
                L"[--selftest|--isolated-test] <file>\n");
        return 1;
    }
    const bool self_test = argc >= 3 && wcscmp(argv[1], L"--selftest") == 0;
    const bool isolated_test = argc >= 3 && wcscmp(argv[1], L"--isolated-test") == 0;
    const std::wstring path = argv[(self_test || isolated_test) ? 2 : 1];
    wprintf(L"probe file=%s\n", path.c_str());

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        wprintf(L"CoInitializeEx hr=0x%08X\n", static_cast<unsigned>(hr));
        return 2;
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = OwnerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"PulsePreviewProbeOwner";
    RegisterClassExW(&wc);

    HWND owner = CreateWindowExW(0, wc.lpszClassName, L"Pulse preview probe",
                                 WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 80, 80, 420, 860, nullptr, nullptr,
                                 wc.hInstance, nullptr);
    if (!owner) {
        wprintf(L"CreateWindow owner failed error=%lu\n", GetLastError());
        CoUninitialize();
        return 3;
    }
    UpdateWindow(owner);

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    const BOOL have_fad = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad);
    const DWORD attrs = have_fad ? fad.dwFileAttributes : INVALID_FILE_ATTRIBUTES;
    const uint64_t modified = have_fad
        ? (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32)
          | fad.ftLastWriteTime.dwLowDateTime
        : 0;
    const uint64_t size = have_fad
        ? (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow
        : 0;
    wprintf(L"attrs=0x%08X exist=%d size=%llu canhost=%d\n",
            attrs, have_fad ? 1 : 0, static_cast<unsigned long long>(size),
            pulse::ui::PreviewHandlerHost::CanHost(path) ? 1 : 0);
    if (isolated_test) {
        const bool isolated = pulse::ui::PreviewHandlerCanActivateIsolatedForTest(path);
        wprintf(L"[%s] preview handler supports isolated local-server activation\n",
                isolated ? L"PASS" : L"FAIL");
        DestroyWindow(owner);
        CoUninitialize();
        return isolated ? 0 : 5;
    }

    const D2D1_RECT_F bounds = D2D1::RectF(16.0f, 16.0f, 380.0f, 640.0f);
    const D2D1_COLOR_F bg = D2D1::ColorF(0.12f, 0.12f, 0.12f);
    const D2D1_COLOR_F fg = D2D1::ColorF(0.92f, 0.92f, 0.92f);
    if (self_test) {
        SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_TEST_DELAY_MS", L"1500");
        pulse::ui::ResetPreviewHandlerOpenAttemptsForTest();
        double max_sync_ms = 0.0;
        double max_reposition_ms = 0.0;
        double reset_ms = 0.0;
        double destroy_ms = 0.0;
        {
            auto host = std::make_unique<pulse::ui::PreviewHandlerHost>();
            host->SetNotifyWindow(owner);
            for (uint64_t generation = 1; generation <= 64; ++generation) {
                const auto start = std::chrono::steady_clock::now();
                host->Sync(owner, bounds, path, attrs, generation, modified, size,
                           true, bg, fg, true, generation == 64);
                const double elapsed = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                max_sync_ms = (std::max)(max_sync_ms, elapsed);
            }
            // Let the worker enter the injected slow COM section.
            Sleep(180);
            for (int i = 0; i < 64; ++i) {
                const auto start = std::chrono::steady_clock::now();
                host->Reposition();
                const double elapsed = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                max_reposition_ms = (std::max)(max_reposition_ms, elapsed);
            }
            const auto reset_start = std::chrono::steady_clock::now();
            host->Reset();
            reset_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - reset_start).count();
            const auto destroy_start = std::chrono::steady_clock::now();
            host.reset();
            destroy_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - destroy_start).count();
        }
        SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_TEST_DELAY_MS", nullptr);
        const bool sync_ok = max_sync_ms < 50.0;
        const bool reposition_ok = max_reposition_ms < 50.0;
        const bool reset_ok = reset_ms < 50.0;
        const bool destroy_ok = destroy_ms < 350.0;
        const uint32_t open_attempts = pulse::ui::PreviewHandlerOpenAttemptsForTest();
        const bool coalesced = open_attempts == 1;
        wprintf(L"[%s] COM preview Sync remains non-blocking (max %.2f ms)\n",
                sync_ok ? L"PASS" : L"FAIL", max_sync_ms);
        wprintf(L"[%s] preview reposition remains non-blocking (max %.2f ms)\n",
                reposition_ok ? L"PASS" : L"FAIL", max_reposition_ms);
        wprintf(L"[%s] COM preview Reset remains non-blocking (%.2f ms)\n",
                reset_ok ? L"PASS" : L"FAIL", reset_ms);
        wprintf(L"[%s] slow handler cannot hang destruction (%.2f ms)\n",
                destroy_ok ? L"PASS" : L"FAIL", destroy_ms);
        wprintf(L"[%s] rapid switches coalesce to one open attempt (%u)\n",
                coalesced ? L"PASS" : L"FAIL", open_attempts);
        DestroyWindow(owner);
        CoUninitialize();
        return sync_ok && reposition_ok && reset_ok && destroy_ok && coalesced ? 0 : 4;
    }

    pulse::ui::PreviewHandlerHost host;
    host.SetNotifyWindow(owner);
    host.Sync(owner, bounds, path, attrs, 1, modified, size, true, bg, fg, true);

    const ULONGLONG deadline = GetTickCount64() + 6000;
    MSG msg{};
    bool running = true;
    while (running && GetTickCount64() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (running) Sleep(16);
    }
    wprintf(L"state=%d (0 idle, 1 loading, 2 shown, 3 failed)\n",
            static_cast<int>(host.state()));
    host.Reset();
    DestroyWindow(owner);
    CoUninitialize();
    return 0;
}
