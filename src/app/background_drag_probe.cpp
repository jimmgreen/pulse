#include "app_input.h"
#include "../ui/ui_renderer.h"
#include <cstdio>
#include <thread>
#include <atomic>
#include <cwchar>
#include <commctrl.h>

namespace {
struct InputTrace {
    pulse::AppState& state;
    FILE* log;
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                 UINT_PTR, DWORD_PTR data) {
        auto& trace = *reinterpret_cast<InputTrace*>(data);
        const bool tracked = msg == WM_MOUSEACTIVATE || msg == WM_ACTIVATE ||
            msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_LBUTTONDOWN ||
            msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED;
        if (tracked) {
            fprintf(trace.log, "[MESSAGE] before msg=%u wp=%llu lp=%lld foreground=%p deferred=%d pending=%d\n",
                msg, static_cast<unsigned long long>(wp), static_cast<long long>(lp),
                static_cast<void*>(GetForegroundWindow()), trace.state.deferredFileActivation,
                trace.state.listSelection.PendingFile());
            fflush(trace.log);
        }
        const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        if (tracked) {
            fprintf(trace.log, "[MESSAGE] after msg=%u result=%lld foreground=%p deferred=%d pending=%d\n",
                msg, static_cast<long long>(result), static_cast<void*>(GetForegroundWindow()),
                trace.state.deferredFileActivation, trace.state.listSelection.PendingFile());
            fflush(trace.log);
        }
        return result;
    }
    ~InputTrace() { RemoveWindowSubclass(state.hwnd, Proc, 1); }
};
}

int RunBackgroundDragProbe(pulse::AppState& s, FILE* log) {
    using namespace pulse;
    auto* tab = ActiveTab(s);
    wchar_t pid_text[32]{};
    GetEnvironmentVariableW(L"PULSE_TEST_BACKGROUND_DROP_PID", pid_text, ARRAYSIZE(pid_text));
    const DWORD expected_pid = wcstoul(pid_text, nullptr, 10);
    HWND target = nullptr;
    while ((target = FindWindowExW(nullptr, target, L"PulseBackgroundDropProbe", nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(target, &pid);
        if (expected_pid && pid == expected_pid) break;
    }
    if (!s.isolatedTest || !tab || tab->EntryCount() < 2 || !target) return 2;
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) return 2;
    InputTrace input_trace{s, log};
    if (!SetWindowSubclass(s.hwnd, InputTrace::Proc, 1, reinterpret_cast<DWORD_PTR>(&input_trace))) return 2;
    struct InputCleanup {
        HWND hwnd;
        POINT cursor{};
        bool have_cursor = false;
        std::atomic<bool> pressed{false};
        void Release() {
            if (!pressed.exchange(false)) return;
            INPUT inputs[3]{};
            inputs[0].type = inputs[1].type = INPUT_KEYBOARD;
            inputs[0].ki.wVk = inputs[1].ki.wVk = VK_ESCAPE;
            inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
            inputs[2].type = INPUT_MOUSE;
            inputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(3, inputs, sizeof(INPUT));
        }
        ~InputCleanup() {
            Release();
            if (GetCapture() == hwnd) ReleaseCapture();
            if (have_cursor) SetCursorPos(cursor.x, cursor.y);
        }
    } cleanup{s.hwnd};
    cleanup.have_cursor = GetCursorPos(&cleanup.cursor) != FALSE;
    const auto owned = [](POINT point, HWND window) {
        HWND hit = WindowFromPoint(point);
        return hit && GetAncestor(hit, GA_ROOT) == window;
    };
    const auto pump = [](ULONGLONG ms) {
        const auto end = GetTickCount64() + ms;
        while (GetTickCount64() < end) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            Sleep(1);
        }
    };
    const auto mouse = [](POINT point, DWORD flags) {
        INPUT input{}; input.type = INPUT_MOUSE;
        input.mi.dx = MulDiv(point.x - GetSystemMetrics(SM_XVIRTUALSCREEN), 65535,
            GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
        input.mi.dy = MulDiv(point.y - GetSystemMetrics(SM_YVIRTUALSCREEN), 65535,
            GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
        input.mi.dwFlags = flags | MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        return SendInput(1, &input, sizeof(input)) == 1;
    };
    const auto foreground = [&](HWND hwnd) {
        ShowWindow(hwnd, SW_RESTORE);
        SetLastError(ERROR_SUCCESS);
        BOOL activated = SetForegroundWindow(hwnd);
        pump(100);
        bool attach = false;
        if (GetForegroundWindow() != hwnd) {
            const DWORD thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
            attach = thread && thread != GetCurrentThreadId() && AttachThreadInput(GetCurrentThreadId(), thread, TRUE);
            activated = SetForegroundWindow(hwnd);
            if (attach) AttachThreadInput(GetCurrentThreadId(), thread, FALSE);
            pump(100);
        }
        // An inert, owned receiver can be activated by actual input when Windows
        // declines a cross-process foreground request from the test process.
        if (GetForegroundWindow() != hwnd && hwnd == target) {
            SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            POINT activation{40, 40};
            ClientToScreen(hwnd, &activation);
            if (owned(activation, hwnd)) {
                cleanup.pressed = mouse(activation, MOUSEEVENTF_LEFTDOWN);
                pump(30);
                if (cleanup.pressed && mouse(activation, MOUSEEVENTF_LEFTUP)) cleanup.pressed = false;
                pump(100);
            }
        }
        const DWORD error = GetLastError();
        const bool foreground_matches = GetForegroundWindow() == hwnd;
        if (!foreground_matches || !IsWindowVisible(hwnd)) {
            DWORD actual_pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &actual_pid);
            fprintf(log, "[STATE] receiver activation visible=%d iconic=%d activated=%d error=%lu attached=%d foreground=%p receiver=%p foreground_pid=%lu receiver_pid=%lu\n",
                IsWindowVisible(hwnd), IsIconic(hwnd), activated, error, attach,
                static_cast<void*>(GetForegroundWindow()), static_cast<void*>(hwnd), actual_pid, expected_pid);
            fflush(log);
        }
        return foreground_matches && IsWindowVisible(hwnd);
    };
    int failures = 0;
    const auto check = [&](bool pass, const char* label) {
        fprintf(log, "[%s] %s\n", pass ? "PASS" : "FAIL", label); fflush(log);
        if (!pass) ++failures;
        return pass;
    };
    s.shot.active = false;
    s.appPrefs.single_click_open = true;
    tab->view_mode = ui::ViewMode::Details;
    ShowWindow(s.hwnd, SW_RESTORE);
    SetWindowPos(s.hwnd, nullptr, 50, 50, 1000, 700, SWP_NOZORDER | SWP_NOACTIVATE);
    pump(300);
    POINT client{};
    int row = -1;
    const auto vm = BuildVm(s, false);
    const auto bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    // Find a visible file grip via the same renderer used by real pointer input.
    for (int y = 100; y < 500 && row < 0; y += 3) {
        for (int x = 150; x < 500; x += 3) {
            const auto hit = s.renderer.HitTest(vm, bounds, static_cast<float>(x), static_cast<float>(y));
            bool interior = hit.region == ui::HitTestResult::Row && hit.index >= 0;
            if (!interior) continue;
            for (int dy : {-4, 0, 4}) {
                for (int dx : {-4, 0, 4}) {
                    const auto nearby = s.renderer.HitTest(vm, bounds, static_cast<float>(x + dx), static_cast<float>(y + dy));
                    interior = interior && nearby.region == ui::HitTestResult::Row && nearby.index == hit.index &&
                        DeferFileMouseActivation(s, {x + dx, y + dy});
                }
            }
            if (interior) {
                client = {x, y}; row = hit.index; break;
            }
        }
    }
    if (row < 0) return 3;
    POINT start = client;
    ClientToScreen(s.hwnd, &start);
    SetWindowPos(target, HWND_TOP, start.x + 150, start.y - 40, 350, 300, SWP_NOACTIVATE);
    POINT end{start.x + 250, start.y + 70};
    const auto target_above_source = [&] {
        for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
            if (window == target) return true;
            if (window == s.hwnd) return false;
        }
        return false;
    };
    std::atomic<unsigned> null_foreground_samples{0};
    const auto safe_drag_surface = [&] {
        const HWND current = GetForegroundWindow();
        if (current && current != target) return false;
        if (!owned(end, target) || !target_above_source()) return false;
        if (!current) ++null_foreground_samples;
        return true;
    };
    const auto trace = [&](const char* stage) {
        POINT actual{};
        GetCursorPos(&actual);
        ScreenToClient(s.hwnd, &actual);
        const auto current_vm = BuildVm(s, false);
        const auto hit = s.renderer.HitTest(current_vm, bounds, static_cast<float>(actual.x), static_cast<float>(actual.y));
        fprintf(log, "[STATE] stage=%s foreground=%p pulse=%p receiver=%p deferred=%d pending_file=%d pending_band=%d selected=%zu selected_index=%d cursor=(%ld,%ld) requested=(%ld,%ld) hit_region=%d hit_index=%d grip=%d capture=%p activation_tick=%llu\n",
            stage, static_cast<void*>(GetForegroundWindow()), static_cast<void*>(s.hwnd), static_cast<void*>(target),
            s.deferredFileActivation, s.listSelection.PendingFile(), s.listSelection.PendingBand(),
            static_cast<size_t>(tab->SelectedCount()), tab->selected_index, actual.x, actual.y, client.x, client.y,
            static_cast<int>(hit.region), hit.index, DeferFileMouseActivation(s, actual),
            static_cast<void*>(GetCapture()), static_cast<unsigned long long>(s.activateClickTick));
        fflush(log);
    };
    if (!check(foreground(target), "external receiver becomes foreground")) return 1;
    if (!check(owned(start, s.hwnd) && owned(end, target), "physical endpoints belong to isolated test windows")) return 1;
    cleanup.pressed = true;
    if (!check(mouse(start, MOUSEEVENTF_LEFTDOWN), "physical background file press delivered")) return 1;
    pump(70);
    trace("click-down");
    check(safe_drag_surface() && owned(start, s.hwnd) && s.listSelection.PendingFile() && s.deferredFileActivation,
        "background file press arms drag while receiver stays above Pulse");
    check(!s.fileClick.SingleOpen(), "activation click does not arm single-click open");
    if (!check(owned(start, s.hwnd), "click release remains over isolated Pulse")) return 1;
    if (!check(mouse(start, MOUSEEVENTF_LEFTUP), "physical click release delivered")) return 1;
    cleanup.pressed = false;
    pump(120);
    check(GetForegroundWindow() == s.hwnd && !s.deferredFileActivation && GetCapture() != s.hwnd,
        "plain release activates Pulse and releases capture");
    for (int cancel = 0; cancel < 2; ++cancel) {
        // Each scenario is a separate gesture, not the second half of a double click.
        pump(static_cast<ULONGLONG>(GetDoubleClickTime()) + 100);
        tab->SelectOnly(row);
        tab->ToggleSelect(row == 0 ? 1 : 0);
        const auto expected = tab->SelectedCount();
        const auto before = SendMessageW(target, WM_APP + 1, 0, 0);
        if (!check(foreground(target), "receiver foreground before drag")) return 1;
        if (!check(owned(start, s.hwnd) && owned(end, target), "drag endpoints remain owned by isolated windows")) return 1;
        cleanup.pressed = true;
        if (!check(mouse(start, MOUSEEVENTF_LEFTDOWN), "physical drag press delivered")) return 1;
        pump(60);
        trace(cancel ? "cancel-down" : "drag-down");
        check(tab->SelectedCount() == expected, "background press preserves multiple selection");
        bool injected = true;
        std::thread input([&] {
            Sleep(80);
            if (!owned(start, s.hwnd) || !safe_drag_surface()) {
                injected = false; cleanup.Release(); return;
            }
            const POINT threshold{start.x + 25, start.y};
            if (!owned(threshold, s.hwnd)) {
                injected = false; cleanup.Release(); return;
            }
            injected = mouse(threshold, 0);
            Sleep(100);
            if (!injected || !safe_drag_surface()) {
                injected = false; cleanup.Release(); return;
            }
            injected = mouse(end, 0) && injected;
            Sleep(200);
            if (!injected || !safe_drag_surface()) {
                injected = false; cleanup.Release(); return;
            }
            if (cancel) {
                INPUT keys[2]{};
                keys[0].type = keys[1].type = INPUT_KEYBOARD;
                keys[0].ki.wVk = keys[1].ki.wVk = VK_ESCAPE;
                keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
                injected = SendInput(1, &keys[0], sizeof(INPUT)) == 1 && injected;
                Sleep(150);
                injected = SendInput(1, &keys[1], sizeof(INPUT)) == 1 && injected;
                Sleep(70);
            }
            if (!safe_drag_surface()) {
                injected = false; cleanup.Release(); return;
            }
            injected = mouse(end, MOUSEEVENTF_LEFTUP) && injected;
            if (injected) cleanup.pressed = false;
            else cleanup.Release();
        });
        pump(900);
        input.join();
        pump(100);
        if (!check(injected, "physical drag input delivered")) return 1;
        fprintf(log, "[STATE] null_foreground_samples=%u receiver_ole_null_samples=%lld target_above_pulse=%d\n",
            null_foreground_samples.load(), static_cast<long long>(SendMessageW(target, WM_APP + 5, 0, 0)), target_above_source());
        check(GetForegroundWindow() == target && target_above_source() && SendMessageW(target, WM_APP + 4, 0, 0) == 0,
            "external receiver finishes foreground with no foreign foreground during OLE drag");
        check(SendMessageW(target, WM_APP + 1, 0, 0) == before + (cancel ? 0 : 1),
            cancel ? "Escape cancels drop" : "cross-process receiver accepts drop");
        if (!cancel) check(SendMessageW(target, WM_APP + 2, 0, 0) == static_cast<LRESULT>(expected),
            "OLE payload contains every selected fixture");
        check(!s.deferredFileActivation && !s.listSelection.PendingFile() && GetCapture() != s.hwnd,
            "drag completion clears pending activation and capture");
    }
    return failures ? 1 : 0;
}
