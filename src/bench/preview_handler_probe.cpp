#include "../ui/preview_handler_host.h"
#include <ole2.h>
#include <windows.h>
#include <cstdio>
#include <string>

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
    if (argc < 2) {
        wprintf(L"usage: pulse_preview_handler_probe.exe <file>\n");
        return 1;
    }
    const std::wstring path = argv[1];
    wprintf(L"probe file=%s\n", path.c_str());
    wprintf(L"log: C:\\Users\\SS\\Desktop\\pulse\\bench_data\\preview_handler.log\n");

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

    pulse::ui::PreviewHandlerHost host;
    host.SetNotifyWindow(owner);
    const D2D1_RECT_F bounds = D2D1::RectF(16.0f, 16.0f, 380.0f, 640.0f);
    const D2D1_COLOR_F bg = D2D1::ColorF(0.12f, 0.12f, 0.12f);
    const D2D1_COLOR_F fg = D2D1::ColorF(0.92f, 0.92f, 0.92f);
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
