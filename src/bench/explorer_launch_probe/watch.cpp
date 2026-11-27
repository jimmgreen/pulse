#include <windows.h>
#include <cstdio>
#include <cwchar>
namespace {
void CALLBACK Event(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD event_tick) {
    if (!window || object != OBJID_WINDOW || child != CHILDID_SELF) return;
    wchar_t name[128]{};
    GetClassNameW(window, name, ARRAYSIZE(name));
    if (wcscmp(name, L"CabinetWClass") || GetAncestor(window, GA_ROOT) != window) return;
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    std::printf("{\"tick_ms\":%llu,\"event_tick\":%lu,\"event\":%lu,\"hwnd\":%llu,\"pid\":%lu,\"visible\":%d}\n",
        GetTickCount64(), event_tick, event, reinterpret_cast<unsigned long long>(window), pid, IsWindowVisible(window));
    std::fflush(stdout);
}
}
int main() {
    auto hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, nullptr, Event, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!hook) return static_cast<int>(GetLastError());
    std::puts("READY"); std::fflush(stdout);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    UnhookWinEvent(hook); return 0;
}
