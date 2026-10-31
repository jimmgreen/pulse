#include "../app/startup_idle.h"
#include <cstdio>
#include <vector>

namespace {
std::vector<unsigned> events;
constexpr UINT kListing = WM_APP + 800;
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kListing) { events.push_back(100); return 0; }
    if (msg == pulse::WM_DEFERRED_STARTUP) {
        events.push_back(static_cast<unsigned>(wp));
        if (wp < 2) pulse::app::ScheduleStartupIdle(hwnd, static_cast<UINT>(wp + 1));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
void Pump(unsigned ms) {
    const auto until = GetTickCount64() + ms;
    do {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(1);
    } while (GetTickCount64() < until);
}
}
int main() {
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = L"PulseStartupIdleTest";
    if (!RegisterClassW(&wc)) return 2;
    const auto create = [&] {
        return CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    };
    HWND window = create();
    if (!window) return 2;
    pulse::app::ScheduleStartupIdle(window, 0);
    Sleep(20); // The timer is due before the directory result is posted.
    PostMessageW(window, kListing, 0, 0);
    Pump(150);
    const bool ordered = events == std::vector<unsigned>{100, 0, 1, 2};
    std::printf("[%s] queued listing precedes warmups; all three stages run once\n", ordered ? "PASS" : "FAIL");
    DestroyWindow(window);
    events.clear();
    window = create();
    if (!window) return 2;
    pulse::app::ScheduleStartupIdle(window, 0);
    DestroyWindow(window);
    Pump(80);
    const bool closed = events.empty();
    std::printf("[%s] closing startup window cancels pending warmup\n", closed ? "PASS" : "FAIL");
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ordered && closed ? 0 : 1;
}
