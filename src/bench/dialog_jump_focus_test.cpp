// Deterministic activation ordering without sending input to the user's desktop.
#include <windows.h>
#include <cstdio>

static HWND foreground_for_test = nullptr;
static HWND ForegroundForTest() { return foreground_for_test; }
#define GetForegroundWindow ForegroundForTest
#include "../app/dialog_jump.cpp"
#undef GetForegroundWindow

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int failures = 0;
    auto check = [&](bool passed, const char* name) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    {
        using namespace pulse::app;
        DialogJump::Impl state;
        t_impl = &state;
        state.instance = GetModuleHandleW(nullptr);
        auto window = [&]() {
            return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Pulse focus test",
                                   WS_POPUP, 100, 100, 800, 400, nullptr, nullptr, state.instance, nullptr);
        };
        state.bar.hwnd = window();
        state.list.hwnd = window();
        const HWND dialog = window();
        state.dlg.dialog = dialog;
        state.hotkey = true; // Do not register a real global hotkey in this unit fixture.
        ShowWindow(dialog, SW_SHOWNOACTIVATE);
        ShowWindow(state.bar.hwnd, SW_SHOWNOACTIVATE);
        foreground_for_test = state.bar.hwnd;
        DialogJump::Impl::OnForeground(nullptr, EVENT_SYSTEM_FOREGROUND, GetDesktopWindow(),
                                       OBJID_WINDOW, CHILDID_SELF, 0, 0);
        check(state.dlg.dialog == dialog && IsWindowVisible(state.bar.hwnd),
              "queued activation cannot detach the currently active jump bar");

        state.dlg.dialog = dialog;
        state.hotkey = true;
        state.observed_foreground = dialog;
        foreground_for_test = dialog;
        ShowWindow(state.bar.hwnd, SW_HIDE);
        state.HandleBar(state.bar.hwnd, WM_TIMER, kTimerForeground, 0);
        check(IsWindowVisible(state.bar.hwnd), "foreground reconciliation restores a hidden bar without a new event");

        foreground_for_test = GetDesktopWindow();
        state.HandleBar(state.bar.hwnd, WM_TIMER, kTimerForeground, 0);
        check(!IsWindowVisible(state.bar.hwnd) && !state.dlg.dialog,
              "foreground reconciliation hides the bar after a missed switch-away event");
        state.Detach();
        if (state.field_brush) DeleteObject(state.field_brush);
        state.bar.comp.reset();
        DestroyWindow(dialog);
        DestroyWindow(state.list.hwnd);
        DestroyWindow(state.bar.hwnd);
        t_impl = nullptr;
    }
    CoUninitialize();
    return failures ? 1 : 0;
}
