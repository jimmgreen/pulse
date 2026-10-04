#include "tray_controller.h"
#include "../common/localization.h"
#include "resource.h"

#include <shellapi.h>

namespace pulse::app {

TrayController::~TrayController() {
    Detach();
}

void TrayController::Attach(HWND hwnd, HINSTANCE instance) {
    if (hwnd_ == hwnd && instance_ == instance) return;
    Detach();
    hwnd_ = hwnd;
    instance_ = instance;
}

void TrayController::Detach() {
    SetVisible(false);
    restore_maximized_ = false;
    hwnd_ = nullptr;
    instance_ = nullptr;
}

NOTIFYICONDATAW TrayController::IconData(UINT flags) const {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    data.uFlags = flags;
    data.uCallbackMessage = kCallbackMessage;
    if (flags & NIF_ICON) {
        data.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_PULSE));
        if (!data.hIcon) data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    if (flags & NIF_TIP) wcscpy_s(data.szTip, L"Pulse");
    return data;
}

bool TrayController::SetVisible(bool visible) {
    if (!hwnd_) return false;
    wanted_visible_ = visible;
    if (visible) {
        if (icon_added_) return true;
        auto data = IconData(NIF_MESSAGE | NIF_ICON | NIF_TIP);
        icon_added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE ||
                      Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE;
        return icon_added_;
    }
    if (icon_added_) {
        auto data = IconData();
        Shell_NotifyIconW(NIM_DELETE, &data);
        icon_added_ = false;
    }
    return true;
}

void TrayController::HideWindow(bool show_icon) {
    if (!hwnd_) return;
    SetVisible(show_icon);
    ShowWindow(hwnd_, SW_HIDE);
}

void TrayController::RestoreWindow() {
    if (!hwnd_) return;
    if (before_restore_ && !IsWindowVisible(hwnd_)) before_restore_();
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : restore_maximized_ ? SW_SHOWMAXIMIZED : SW_SHOW);
    restore_maximized_ = false;
    SetForegroundWindow(hwnd_);
}

bool TrayController::StartHidden(bool maximized, bool show_icon) {
    if (!hwnd_) return false;
    if (show_icon && !SetVisible(true) && FindWindowW(L"Shell_TrayWnd", nullptr)) return false;
    restore_maximized_ = maximized;
    ShowWindow(hwnd_, SW_HIDE);
    return true;
}

UINT TrayController::TaskbarCreatedMessage() {
    static const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
    return message;
}

void TrayController::HandleTaskbarCreated() {
    icon_added_ = false;
    if (wanted_visible_) SetVisible(true);
}

TrayController::CallbackResult TrayController::HandleCallback(LPARAM event) {
    if (!hwnd_) return CallbackResult::NotHandled;
    switch (LOWORD(event)) {
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        RestoreWindow();
        return CallbackResult::Handled;
    case WM_RBUTTONUP: {
        POINT point{};
        GetCursorPos(&point);
        HMENU menu = CreatePopupMenu();
        if (!menu) return CallbackResult::Handled;
        // Open first, the one that ends the session last: a menu that grows from
        // the top keeps Exit where the pointer expects to find it. A second
        // window is not on this menu - the tab menu and the taskbar jump list
        // both offer it, and the tray stays the place for the session itself.
        AppendMenuW(menu, MF_STRING, 1, l10n::Get(l10n::StringId::Open).c_str());
        AppendMenuW(menu, MF_STRING, 2, l10n::Get(l10n::StringId::TrayExit).c_str());
        SetForegroundWindow(hwnd_);
        const int command = TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            point.x, point.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        if (command == 1) RestoreWindow();
        return command == 2 ? CallbackResult::ExitRequested : CallbackResult::Handled;
    }
    default:
        return CallbackResult::NotHandled;
    }
}

} // namespace pulse::app
