#include "../app/picker_remote_wake.h"
#include <cstdio>

int main() {
    unsigned failures = 0;
    const auto check = [&](bool passed, const char* name) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
        std::fflush(stdout);
        if (!passed) ++failures;
    };
    HWND host = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    HWND picker = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!host || !picker || !wake) return 2;
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    // Force the exact worker-post / filtered-Peek interleaving in PumpOnce.
    check(!PeekMessageW(&message, host, 0, 0, PM_REMOVE), "host queue initially empty");
    PostMessageW(host, WM_APP + 417, 0, 0);
    SetEvent(wake);
    PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE);
    GetQueueStatus(QS_ALLINPUT); // deterministically clear the new-message bits
    check(MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, 0) == WAIT_TIMEOUT,
        "old queue-only wait loses an observed reply wakeup");
#if defined(PULSE_REMOTE_WAKE_BASELINE)
    const DWORD result = MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, 0);
#else
    const DWORD result = pulse::app::WaitPickerRemoteWake(wake, 20);
#endif
    check(result == WAIT_OBJECT_0, "reply completion wakes without another input message");
    check(PeekMessageW(&message, host, 0, 0, PM_REMOVE) && message.message == WM_APP + 417,
        "reply remains available to host dispatch");
    ResetEvent(wake);
    PostMessageW(picker, WM_KEYDOWN, VK_RETURN, 0);
    GetQueueStatus(QS_ALLINPUT);
    check(pulse::app::WaitPickerRemoteWake(wake, 20) == WAIT_TIMEOUT,
        "queued picker input does not spin the callback wait");
    check(PeekMessageW(&message, picker, WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE) && message.wParam == VK_RETURN,
        "callback wait preserves picker input for later dispatch");
    SetEvent(wake);
    check(pulse::app::WaitPickerRemoteWake(wake, 20) == WAIT_OBJECT_0,
        "writer completion uses the same durable wakeup");
    CloseHandle(wake);
    DestroyWindow(picker);
    DestroyWindow(host);
    return failures ? 1 : 0;
}
