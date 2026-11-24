// dialog_window_watch_test.cpp — WindowShowWatch (event-driven helper discovery).
// A child process shows off-screen, non-activating 1x1 tool windows. Checks the
// filters (own process, child windows, duplicates, classifier) and measures
// show-to-discovery latency against the previous loop (500 ms EnumWindows sweep,
// 100 ms waits) under the same conditions. Nothing is visible on the desktop.
#include "../dialog_launcher/window_show_watch.h"
#include <objbase.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <random>
#include <string>
#include <vector>

using pulse::dialog_launcher::WindowShowWatch;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* text) { ++checks; if (!ok) ++failures; std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text); std::fflush(stdout); }
constexpr wchar_t kClass[] = L"PulseWatchTestDialog";
constexpr wchar_t kOtherClass[] = L"PulseWatchTestOther";
constexpr int kRounds = 30;
struct Shared {
    volatile LONG command = 0;  // 1 show one, 2 show two, 3 hide, 9 exit
    volatile LONG done = 0;     // child echoes the command when finished
    LONGLONG shown_qpc = 0;
};
LONGLONG Qpc() { LARGE_INTEGER v{}; QueryPerformanceCounter(&v); return v.QuadPart; }
double Ms(LONGLONG ticks) { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return 1000.0 * static_cast<double>(ticks) / static_cast<double>(f.QuadPart); }
HWND Popup(const wchar_t* cls) {
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls, L"", WS_POPUP, -20000, -20000, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}
void Register(const wchar_t* cls) {
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = cls; RegisterClassW(&wc);
}
bool IsTestDialog(HWND hwnd) noexcept {
    wchar_t name[64]{};
    return GetClassNameW(hwnd, name, 64) && std::wcscmp(name, kClass) == 0;
}
void Pump() { MSG m{}; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } }

int Child(const wchar_t* map_name, const wchar_t* event_name) {
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, map_name);
    HANDLE wake = OpenEventW(SYNCHRONIZE, FALSE, event_name);
    auto* shared = map ? static_cast<Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared))) : nullptr;
    if (!shared || !wake) return 2;
    Register(kClass); Register(kOtherClass);
    HWND main = Popup(kClass), other = Popup(kOtherClass);
    // A child control: its SHOW events must not count as a new top-level window.
    HWND control = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 1, 1, main, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!main || !other || !control) return 3;
    InterlockedExchange(&shared->done, -1);
    for (;;) {
        MsgWaitForMultipleObjects(1, &wake, FALSE, 50, QS_ALLINPUT);
        Pump();
        const LONG command = InterlockedCompareExchange(&shared->command, 0, 0);
        if (!command || command == shared->done) continue;
        if (command == 9) break;
        if (command == 1 || command == 2 || command == 4) {
            shared->shown_qpc = Qpc();
            ShowWindow(main, SW_SHOWNOACTIVATE);
            ShowWindow(control, SW_SHOWNA);
            if (command == 2) ShowWindow(other, SW_SHOWNOACTIVATE);
        } else if (command == 3) {
            ShowWindow(control, SW_HIDE); ShowWindow(main, SW_HIDE); ShowWindow(other, SW_HIDE);
        }
        Pump();
        InterlockedExchange(&shared->done, command);
    }
    DestroyWindow(control); DestroyWindow(main); DestroyWindow(other);
    return 0;
}

struct Fixture {
    HANDLE map = nullptr, wake = nullptr;
    Shared* shared = nullptr;
    PROCESS_INFORMATION child{};
    bool Send(LONG command) {
        // Alternate 1/4 so consecutive shows are distinct commands.
        InterlockedExchange(&shared->command, command);
        SetEvent(wake);
        const ULONGLONG until = GetTickCount64() + 5000;
        while (GetTickCount64() < until) {
            if (InterlockedCompareExchange(&shared->done, 0, 0) == command) return true;
            Sleep(0);
        }
        return false;
    }
    ~Fixture() {
        if (shared) { InterlockedExchange(&shared->command, 9); SetEvent(wake); }
        if (child.hProcess) { if (WaitForSingleObject(child.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(child.hProcess, 1); CloseHandle(child.hProcess); CloseHandle(child.hThread); }
        if (shared) UnmapViewOfFile(shared);
        if (map) CloseHandle(map);
        if (wake) CloseHandle(wake);
    }
};
struct Stats { double median = 0, p95 = 0, max = 0, mean = 0; };
Stats Summarize(std::vector<double> v) {
    Stats s; if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.median = v[v.size() / 2]; s.p95 = v[(v.size() * 95) / 100 < v.size() ? (v.size() * 95) / 100 : v.size() - 1]; s.max = v.back();
    for (double x : v) s.mean += x; s.mean /= static_cast<double>(v.size());
    return s;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wcscmp(argv[1], L"--child") == 0) return Child(argv[2], argv[3]);
    SetProcessDPIAware();
    GUID guid{}; CoCreateGuid(&guid); wchar_t text[40]{}; StringFromGUID2(guid, text, 40);
    const std::wstring map_name = std::wstring(L"Local\\PulseWatchTestMap-") + text, event_name = std::wstring(L"Local\\PulseWatchTestWake-") + text;
    Fixture f;
    f.map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), map_name.c_str());
    f.wake = CreateEventW(nullptr, FALSE, FALSE, event_name.c_str());
    f.shared = f.map ? static_cast<Shared*>(MapViewOfFile(f.map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared))) : nullptr;
    if (!f.shared || !f.wake) { Check(false, "fixture shared memory"); return 1; }

    WindowShowWatch watch;
    Check(watch.Start(&IsTestDialog) && watch.Active(), "out-of-context window watch starts");
    {
        WindowShowWatch second;
        Check(!second.Start() && GetLastError() == ERROR_ALREADY_INITIALIZED, "one watch per thread");
    }
    // Own process: skipped.
    Register(kOtherClass);
    HWND own = Popup(kOtherClass);
    ShowWindow(own, SW_SHOWNOACTIVATE);
    for (int i = 0; i < 10; ++i) watch.Wait(nullptr, 0, 10);
    const auto own_seen = watch.Take();
    Check(std::none_of(own_seen.begin(), own_seen.end(), [](const auto& s) { return s.pid == GetCurrentProcessId(); }), "own process windows are not reported");
    HWND own_control = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, own, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(WindowShowWatch::TopLevel(own) && own_control && !WindowShowWatch::TopLevel(own_control) && !WindowShowWatch::TopLevel(nullptr),
          "filter accepts top-level popups and rejects child controls");
    if (own_control) DestroyWindow(own_control);
    DestroyWindow(own);

    wchar_t self[MAX_PATH]{}; GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" --child " + map_name + L" " + event_name;
    STARTUPINFOW si{}; si.cb = sizeof(si);
    if (!CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &f.child)) { Check(false, "child starts"); return 1; }
    const DWORD pid = f.child.dwProcessId;
    {
        const ULONGLONG until = GetTickCount64() + 10000;
        while (GetTickCount64() < until && InterlockedCompareExchange(&f.shared->done, 0, 0) != -1) watch.Wait(nullptr, 0, 10);
        Check(f.shared->done == -1, "child created its hidden windows");
    }
    (void)watch.Take();

    // Two top-level windows and a control in one burst: one entry, classified.
    Check(f.Send(2), "child shows two windows");
    std::vector<WindowShowWatch::Shown> seen;
    for (int i = 0; i < 50; ++i) { watch.Wait(nullptr, 0, 10); for (auto& s : watch.Take()) seen.push_back(s); }
    const auto count = std::count_if(seen.begin(), seen.end(), [&](const auto& s) { return s.pid == pid; });
    Check(count == 1, "a burst from one process is reported once");
    Check(count == 1 && std::find_if(seen.begin(), seen.end(), [&](const auto& s) { return s.pid == pid; })->classified, "classifier marks the dialog-class window");
    Check(f.Send(3), "child hides");
    for (int i = 0; i < 10; ++i) watch.Wait(nullptr, 0, 10);
    (void)watch.Take();
    {
        Check(f.Send(1), "child shows the dialog-class window and its control");
        bool found = false;
        for (int i = 0; i < 50 && !found; ++i) { watch.Wait(nullptr, 0, 10); for (auto& s : watch.Take()) found |= s.pid == pid; }
        Check(found, "a single shown window is reported");
        Check(f.Send(3), "child hides again");
        for (int i = 0; i < 10; ++i) watch.Wait(nullptr, 0, 10);
        (void)watch.Take();
    }

    // Latency: event-driven vs the previous sweep loop, same child, same windows.
    std::mt19937 random(12345);
    std::vector<double> event_ms, sweep_ms;
    LONG next_show = 1;
    auto show = [&]() { next_show = next_show == 1 ? 4 : 1; return f.Send(next_show); };
    bool sent = true;
    for (int round = 0; round < kRounds && sent; ++round) {
        sent = show();
        LONGLONG found_at = 0;
        const ULONGLONG until = GetTickCount64() + 3000;
        while (!found_at && GetTickCount64() < until) {
            watch.Wait(nullptr, 0, 100);
            for (auto& s : watch.Take()) if (s.pid == pid && !found_at) found_at = Qpc();
        }
        if (found_at) event_ms.push_back(Ms(found_at - f.shared->shown_qpc));
        sent = sent && f.Send(3);
        for (int i = 0; i < 5; ++i) watch.Wait(nullptr, 0, 10);
        (void)watch.Take();
    }
    Check(sent && event_ms.size() == kRounds, "every event-driven round discovers the window");
    // The previous loop: a 500 ms EnumWindows sweep and 100 ms waits, at a random phase.
    ULONGLONG next_scan = 0;
    auto sweep_sees = [&]() {
        struct Find { DWORD pid; bool found; } find{pid, false};
        EnumWindows([](HWND hwnd, LPARAM raw) -> BOOL {
            auto& f2 = *reinterpret_cast<Find*>(raw);
            if (!IsWindowVisible(hwnd)) return TRUE;
            DWORD owner = 0; GetWindowThreadProcessId(hwnd, &owner);
            if (owner == f2.pid) { f2.found = true; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&find));
        return find.found;
    };
    auto old_loop_step = [&](bool& seen_now) {
        if (GetTickCount64() >= next_scan) { next_scan = GetTickCount64() + 500; seen_now = sweep_sees(); }
        Sleep(100);
    };
    for (int round = 0; round < kRounds && sent; ++round) {
        const ULONGLONG phase_until = GetTickCount64() + std::uniform_int_distribution<int>(0, 499)(random);
        bool ignored = false;
        while (GetTickCount64() < phase_until) old_loop_step(ignored);
        sent = show();
        LONGLONG found_at = 0;
        const ULONGLONG until = GetTickCount64() + 3000;
        while (!found_at && GetTickCount64() < until) {
            bool seen_now = false;
            if (GetTickCount64() >= next_scan) { next_scan = GetTickCount64() + 500; seen_now = sweep_sees(); if (seen_now) found_at = Qpc(); }
            if (!found_at) Sleep(100);
        }
        if (found_at) sweep_ms.push_back(Ms(found_at - f.shared->shown_qpc));
        sent = sent && f.Send(3);
    }
    Check(sent && sweep_ms.size() == kRounds, "every sweep round discovers the window");
    const auto e = Summarize(event_ms), s = Summarize(sweep_ms);
    std::printf("discovery ms (n=%d)  event: median=%.2f mean=%.2f p95=%.2f max=%.2f | old 500ms sweep: median=%.1f mean=%.1f p95=%.1f max=%.1f\n",
        kRounds, e.median, e.mean, e.p95, e.max, s.median, s.mean, s.p95, s.max);
    Check(e.p95 < 50.0, "event-driven discovery p95 under 50 ms");
    std::printf("Checks: %d, failures: %d\n", checks, failures);
    return failures ? 1 : 0;
}
