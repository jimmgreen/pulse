#include "../app/rename_click_timing.h"
#include <cstdio>

int main() {
    WNDCLASSW cls{};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"PulseRenameClickTimingTest";
    if (!RegisterClassW(&cls)) return 2;
    HWND window = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
        nullptr, cls.hInstance, nullptr);
    if (!window) return 2;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    const auto now = GetTickCount64();
    check(!pulse::app::RenameClickReady(now, 0), "unarmed candidate never renames");
    check(!pulse::app::RenameClickReady(now, now + 500), "double-click interval remains open");
    check(PostMessageW(window, WM_LBUTTONDBLCLK, MK_LBUTTON, 0) != FALSE,
        "queue real window second-click message before delayed UI timer");
    check(!pulse::app::RenameClickReady(now + 600, now + 500),
        "expired rename timer yields to queued double-click");
    check(PeekMessageW(&message, window, WM_LBUTTONDBLCLK, WM_LBUTTONDBLCLK, PM_REMOVE) &&
        message.message == WM_LBUTTONDBLCLK, "timing check never consumes second-click message");
    check(PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0) != FALSE &&
        !pulse::app::RenameClickReady(now + 600, now + 500), "queued cancellation runs before rename editor");
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
        std::puts("[SKIP] physical mouse button held during quiet-queue check");
    else check(pulse::app::RenameClickReady(now + 600, now + 500),
        "quiet queue still permits ordinary delayed rename");
    DestroyWindow(window);
    UnregisterClassW(cls.lpszClassName, cls.hInstance);
    return failures ? 1 : 0;
}
