#include "../app/host_routing.h"
#include "../app/recycle_refresh_state.h"
#include "../app/recycle_shell_watch.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
constexpr UINT kChanged = WM_APP + 19;
pulse::app::RecycleRefreshGate gate;
unsigned changes = 0;
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kChanged) {
        PIDLIST_ABSOLUTE* pidls = nullptr;
        LONG event = 0;
        if (HANDLE lock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wparam),
            static_cast<DWORD>(lparam), &pidls, &event)) {
            SHChangeNotification_Unlock(lock);
            ++changes;
            gate.Invalidate();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
void PumpUntil(ULONGLONG deadline) {
    while (GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
    }
}
}
int main() {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!passed) ++failures;
    };
    check(pulse::app::IsHostRoutedMessage(WM_RECYCLE_SHELL_CHANGED) &&
        pulse::app::IsHostRoutedMessage(pulse::WM_RECYCLE_INFO),
        "hidden service host forwards both recycle notifications and query results");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto root = std::filesystem::absolute(std::filesystem::path("bench_data") /
        ("recycle-watch-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    std::filesystem::create_directories(root);
    WNDCLASSW klass{};
    klass.lpfnWndProc = WindowProc; klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = L"PulseIsolatedRecycleWatchTest";
    RegisterClassW(&klass);
    HWND window = CreateWindowExW(0, klass.lpszClassName, L"", WS_OVERLAPPED,
        0, 0, 0, 0, nullptr, nullptr, klass.hInstance, nullptr);
    PIDLIST_ABSOLUTE pidl = nullptr;
    check(window && SUCCEEDED(SHParseDisplayName(root.c_str(), nullptr, &pidl, 0, nullptr)),
        "prepare hidden window and isolated directory PIDL");
    if (!window || !pidl) return 2;
    pulse::app::RecycleShellWatch watch(window);
    watch.Register(kChanged, pidl);
    CoTaskMemFree(pidl);
    check(watch.Active(), "register actual Shell notifications only for the isolated fixture");
    const auto first = gate.Begin();
    unsigned dispatched = first ? 1 : 0;
    for (unsigned i = 0; i < 1000; ++i) dispatched += gate.Begin() ? 1 : 0;
    check(dispatched == 1, "1000 refresh requests share one in-flight query");
    const auto file = root / "external.txt";
    { std::ofstream output(file); output << "isolated external change"; }
    SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, file.c_str(), nullptr);
    const auto deadline = GetTickCount64() + 3000;
    while (!changes && GetTickCount64() < deadline) PumpUntil(GetTickCount64() + 10);
    check(changes > 0, "external fixture change reaches the real Shell notification callback");
    const auto stale = gate.Complete(first);
    check(stale.matched && !stale.current, "query started before external mutation cannot overwrite newer state");
    const auto current = gate.Begin();
    dispatched += current ? 1 : 0;
    for (unsigned i = 0; i < 1000; ++i) dispatched += gate.Begin() ? 1 : 0;
    const auto latest = gate.Complete(current);
    check(current && latest.matched && latest.current && !gate.active_request && dispatched == 2,
        "one follow-up query accepts the latest generation and releases the in-flight slot");
    check(!gate.Complete(first).matched, "late duplicate response cannot consume a newer request");
    watch.Cancel();
    PumpUntil(GetTickCount64() + 100);
    const auto before = changes;
    std::filesystem::remove(file);
    SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, file.c_str(), nullptr);
    PumpUntil(GetTickCount64() + 100);
    check(!watch.Active() && changes == before, "cancelled registration stops receiving fixture changes");
    DestroyWindow(window);
    std::filesystem::remove_all(root);
    CoUninitialize();
    return failures ? 1 : 0;
}
