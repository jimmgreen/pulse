#include "tray_controller.h"
#include "app_messages.h"
#include "../common/localization.h"
#include "../common/runtime_log.h"
#include "resource.h"

#include <shellapi.h>

namespace pulse::app {
namespace {
void LogPlacement(const char* event, HWND window, bool snapshot, bool applied = false) {
    if (!diagnostics::runtime::Enabled()) return;
    RECT rect{};
    WINDOWPLACEMENT placement{sizeof(placement)};
    MONITORINFO monitor{sizeof(monitor)};
    GetWindowRect(window, &rect);
    GetWindowPlacement(window, &placement);
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    using GetDpi = UINT(WINAPI*)(HWND);
    static const auto get_dpi = reinterpret_cast<GetDpi>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    // Coordinates use signed 32-bit bit patterns: negative monitor coordinates
    // remain recoverable with the runtime logger's unsigned numeric fields.
    diagnostics::runtime::Event(event, {
        {"window", reinterpret_cast<uintptr_t>(window)}, {"visible", IsWindowVisible(window) != FALSE},
        {"snapshot", snapshot}, {"applied", applied}, {"show", placement.showCmd},
        {"dpi", get_dpi ? get_dpi(window) : 0},
        {"left_u32", static_cast<uint32_t>(rect.left)}, {"top_u32", static_cast<uint32_t>(rect.top)},
        {"width", static_cast<uint32_t>(rect.right - rect.left)},
        {"height", static_cast<uint32_t>(rect.bottom - rect.top)},
        {"work_width", static_cast<uint32_t>(monitor.rcWork.right - monitor.rcWork.left)},
        {"work_height", static_cast<uint32_t>(monitor.rcWork.bottom - monitor.rcWork.top)}});
}
}

TrayController::~TrayController() {
    Detach();
}

void TrayController::Attach(HWND window, HINSTANCE instance, HWND icon_window) {
    if (!icon_window) icon_window = window;
    if (hwnd_ == window && icon_window_ == icon_window && instance_ == instance) return;
    Detach();
    hwnd_ = window;
    icon_window_ = window ? icon_window : nullptr;
    instance_ = instance;
}

void TrayController::Detach() {
    SetVisible(false);
    restore_maximized_ = false;
    has_hidden_placement_ = false;
    hwnd_ = nullptr;
    icon_window_ = nullptr;
    instance_ = nullptr;
}

NOTIFYICONDATAW TrayController::IconData(UINT flags) const {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = icon_window_;
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
    // Keep the last visible placement, even if hiding is requested again.
    if (IsWindowVisible(hwnd_))
        has_hidden_placement_ = GetWindowPlacement(hwnd_, &hidden_placement_) != FALSE;
    LogPlacement("tray_hide", hwnd_, has_hidden_placement_);
    SetVisible(show_icon);
    ShowWindow(hwnd_, SW_HIDE);
}

void TrayController::RestoreWindow() {
    if (!hwnd_) return;
    const bool hidden = !IsWindowVisible(hwnd_);
    LogPlacement("tray_restore_begin", hwnd_, has_hidden_placement_);
    if (before_restore_ && hidden) before_restore_();
    if (hidden && has_hidden_placement_) {
        auto placement = hidden_placement_;
        // Restore a minimized window, including one minimized from maximized.
        placement.showCmd = placement.showCmd == SW_SHOWMAXIMIZED ||
            (placement.flags & WPF_RESTORETOMAXIMIZED) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        placement.flags &= ~WPF_RESTORETOMAXIMIZED;
        if (SetWindowPlacement(hwnd_, &placement)) {
            has_hidden_placement_ = false;
            restore_maximized_ = false;
            SetForegroundWindow(hwnd_);
            LogPlacement("tray_restore_end", hwnd_, false, true);
            return;
        }
    }
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : restore_maximized_ ? SW_SHOWMAXIMIZED : SW_SHOW);
    restore_maximized_ = false;
    SetForegroundWindow(hwnd_);
    LogPlacement("tray_restore_end", hwnd_, has_hidden_placement_);
}

bool TrayController::StartHidden(bool maximized, bool show_icon) {
    if (!hwnd_) return false;
    if (show_icon && !SetVisible(true) && FindWindowW(L"Shell_TrayWnd", nullptr)) return false;
    restore_maximized_ = maximized;
    if (!has_hidden_placement_ || IsWindowVisible(hwnd_))
        has_hidden_placement_ = GetWindowPlacement(hwnd_, &hidden_placement_) != FALSE;
    if (has_hidden_placement_) {
        hidden_placement_.showCmd = maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        hidden_placement_.flags &= ~WPF_RESTORETOMAXIMIZED;
    }
    LogPlacement("tray_start_hidden", hwnd_, has_hidden_placement_);
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

TrayController::CallbackResult TrayController::HandleCallback(
    LPARAM event, const std::function<int(POINT, HWND)>& show_menu) {
    if (!hwnd_) return CallbackResult::NotHandled;
    switch (LOWORD(event)) {
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        RestoreWindow();
        return CallbackResult::Handled;
    case WM_RBUTTONUP: {
        POINT point{};
        GetCursorPos(&point);
        if (!show_menu) return CallbackResult::Handled;
        // The popup owns foreground activation; the icon host may be hidden.
        const int command = show_menu(point, icon_window_);
        PostMessageW(icon_window_, WM_NULL, 0, 0);
        if (command == 1) RestoreWindow();
        if (command == 3) PostMessageW(hwnd_, WM_NEW_WINDOW, 0, 0);
        if (command == 4) { RestoreWindow(); PostMessageW(hwnd_, WM_MERGE_ALL_WINDOWS, 0, 0); }
        if (command >= 10 && command < 16) PostMessageW(hwnd_, WM_ARRANGE_WINDOWS, command-10, 1);
        return command == 2 ? CallbackResult::ExitRequested : CallbackResult::Handled;
    }
    default:
        return CallbackResult::NotHandled;
    }
}

} // namespace pulse::app
