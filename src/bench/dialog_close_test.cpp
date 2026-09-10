#include "../ui/dialog_lifecycle.h"
#include <cstdio>

namespace {
HWND owner = nullptr;
HWND dialog = nullptr;
HWND editor = nullptr;
bool enabled_before_hide = true;
bool editors_hidden_first = true;
int failures = 0;
void Check(bool value, const char* message) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", message);
    if (!value) ++failures;
}
LRESULT CALLBACK WindowProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_WINDOWPOSCHANGING && (reinterpret_cast<WINDOWPOS*>(lp)->flags & SWP_HIDEWINDOW)) {
        if (window == dialog || window == editor) enabled_before_hide &= IsWindowEnabled(owner) != FALSE;
        if (window == dialog) editors_hidden_first &= !IsWindowVisible(editor);
    }
    return DefWindowProcW(window, msg, wp, lp);
}
HWND Make(HWND parent) {
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"PulseCloseTest", L"", WS_POPUP,
        -30000, -30000, 10, 10, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}
}
int main() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = L"PulseCloseTest";
    wc.hInstance = GetModuleHandleW(nullptr);
    RegisterClassW(&wc);
    owner = Make(nullptr);
    HWND unrelated = Make(nullptr);
    const HWND foreground = GetForegroundWindow();
    for (int pass = 0; pass < 2; ++pass) {
        dialog = Make(owner);
        editor = Make(dialog);
        HWND nested = Make(editor);
        ShowWindow(owner, SW_SHOWNOACTIVATE);
        ShowWindow(dialog, SW_SHOWNOACTIVATE);
        ShowWindow(editor, SW_SHOWNOACTIVATE);
        ShowWindow(nested, SW_SHOWNOACTIVATE);
        ShowWindow(unrelated, SW_SHOWNOACTIVATE);
        EnableWindow(owner, FALSE);
        enabled_before_hide = editors_hidden_first = true;
        pulse::ui::HideComposedDialog(dialog, owner);
        Check(enabled_before_hide && IsWindowEnabled(owner), "owner enabled before any modal surface is hidden");
        Check(editors_hidden_first, "hosted editors hidden before dialog surface");
        Check(!IsWindowVisible(dialog) && !IsWindowVisible(editor) && !IsWindowVisible(nested),
              "entire owned popup tree hidden on first and repeated close");
        Check(IsWindowVisible(unrelated) && GetForegroundWindow() == foreground,
              "unrelated window visibility and foreground preserved");
        DestroyWindow(dialog);
        dialog = editor = nullptr;
    }
    DestroyWindow(unrelated);
    DestroyWindow(owner);
    return failures ? 1 : 0;
}
