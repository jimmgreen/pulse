#include "tray_controller.h"
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

void TrayController::HideWindow() {
    if (!hwnd_) return;
    SetVisible(true);
    ShowWindow(hwnd_, SW_HIDE);
}

void TrayController::RestoreWindow() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
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
        AppendMenuW(menu, MF_STRING, 1, L"打开");
        AppendMenuW(menu, MF_STRING, 2, L"退出");
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
