#include "app/single_instance_coordinator.h"
#include <objbase.h>
#include <cstdio>
#include <string>
int wmain(int argc, wchar_t** argv) {
    if (argc != 3 || !std::wstring_view(argv[1]).starts_with(L"Local\\Pulse.ExplorerProbe.")) return 87;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HWND window = CreateWindowExW(0, L"STATIC", L"private explorer route receiver", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    pulse::app::SingleInstanceCoordinator receiver;
    if (!window || receiver.Acquire(argv[1]) != pulse::app::SingleInstanceCoordinator::AcquireResult::Primary) return 2;
    receiver.SetRevealEnabled(wcscmp(argv[2], L"reject") != 0);
    if (!receiver.PublishEndpoint(window, WM_APP + 946)) return 3;
    std::puts("READY"); std::fflush(stdout);
    const auto end = GetTickCount64() + 30000;
    while (GetTickCount64() < end) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { receiver.Release(); DestroyWindow(window); return 0; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        while (auto request = receiver.TakeQueuedOpenRequest()) {
            const auto& path = request->path;
            const int size = WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
            std::string utf8(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), utf8.data(), size, nullptr, nullptr);
            std::printf("REQUEST %s %s\n", request->wire_reveal ? "select" : "folder", utf8.c_str()); std::fflush(stdout);
        }
        Sleep(5);
    }
    receiver.Release(); DestroyWindow(window); CoUninitialize(); return 0;
}
