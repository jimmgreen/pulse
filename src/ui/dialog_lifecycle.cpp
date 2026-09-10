#include "dialog_lifecycle.h"
#include <dwmapi.h>
#include <vector>

namespace pulse::ui {
namespace {
bool OwnedBy(HWND window, HWND owner) {
    if (IsChild(owner, window)) return true;
    for (HWND current = window; current; current = GetWindow(current, GW_OWNER))
        if (current == owner) return true;
    return false;
}

void HideWithoutActivation(HWND window) {
    const BOOL disabled = TRUE;
    DwmSetWindowAttribute(window, DWMWA_TRANSITIONS_FORCEDISABLED, &disabled, sizeof(disabled));
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
        SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER);
}
}

void HideComposedDialog(HWND dialog, HWND modal_owner) {
    if (!dialog || !IsWindow(dialog)) return;
    const bool foreground = OwnedBy(GetForegroundWindow(), dialog);
    // Windows must have an enabled activation target before the active popup disappears.
    if (modal_owner && IsWindow(modal_owner)) {
        EnableWindow(modal_owner, TRUE);
        if (foreground) {
            SetActiveWindow(modal_owner);
            SetFocus(modal_owner);
        }
    }
    struct OwnedWindows { HWND dialog; std::vector<HWND> windows; } owned{dialog, {}};
    EnumThreadWindows(GetWindowThreadProcessId(dialog, nullptr), [](HWND window, LPARAM param) -> BOOL {
        auto& context = *reinterpret_cast<OwnedWindows*>(param);
        if (window != context.dialog && OwnedBy(window, context.dialog)) context.windows.push_back(window);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&owned));
    // Hide any remaining owned auxiliary windows before releasing composition.
    // Hide all of them while the dialog surface still covers the owner.
    for (HWND window : owned.windows) HideWithoutActivation(window);
    HideWithoutActivation(dialog);
    DwmFlush();
}
}
