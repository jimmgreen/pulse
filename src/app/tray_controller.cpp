#include "tray_controller.h"
#include "tray_reveal.h"
#include "app_messages.h"
#include "../common/localization.h"
#include "../common/runtime_log.h"
#include "resource.h"

#include <shellapi.h>

namespace pulse::app {
namespace {
// Legacy `applied` is SetWindowPlacement's result, never a ShowWindow success flag.
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
    const HWND window = hwnd_;
    const auto started = GetTickCount64();
    const auto before = InspectWindowRevealState(window);
    const bool snapshot = has_hidden_placement_;
    auto placement = hidden_placement_;
    bool placement_attempted = false, placement_applied = false, show_called = false;
    bool show_previous_visible = false, foreground_attempted = false, foreground_set = false;
    DWORD placement_error = ERROR_SUCCESS;
    int requested_show = before.minimized ? SW_RESTORE : restore_maximized_ ? SW_SHOWMAXIMIZED : SW_SHOW;
    const auto stage = [&](const wchar_t* name) {
        diagnostics::runtime::TextEvent("tray_restore_stage", {{"stage", name}}, {
            {"window", reinterpret_cast<uintptr_t>(window)}, {"elapsed_ms", GetTickCount64() - started}});
    };
    const auto finish = [&](const wchar_t* reason) {
        const auto after = InspectWindowRevealState(window);
        const bool target_changed = hwnd_ != window;
        const bool restored = !target_changed && after.Restored();
        LogPlacement("tray_restore_end", window, !target_changed && has_hidden_placement_, placement_applied);
        diagnostics::runtime::Event("tray_restore_api", {
            {"window", reinterpret_cast<uintptr_t>(window)}, {"snapshot_available", snapshot},
            {"placement_attempted", placement_attempted}, {"placement_applied", placement_applied},
            {"placement_error", placement_error}, {"show_called", show_called},
            {"requested_show", static_cast<uint64_t>(requested_show)},
            {"foreground_attempted", foreground_attempted}, {"foreground_set", foreground_set}});
        diagnostics::runtime::TextEvent("tray_restore_result", {{"reason", reason}}, {
            {"window", reinterpret_cast<uintptr_t>(window)}, {"window_valid", after.valid},
            {"visible_after", after.visible}, {"minimized_after", after.minimized},
            {"foreground_after", after.foreground}, {"restore_ok", restored},
            {"requested_show", static_cast<uint64_t>(requested_show)}, {"placement_applied", placement_applied},
            {"show_called", show_called}, {"show_previous_visible", show_previous_visible},
            {"target_changed", target_changed}, {"elapsed_ms", GetTickCount64() - started}},
            restored ? diagnostics::runtime::Level::Info : diagnostics::runtime::Level::Warning);
    };
    LogPlacement("tray_restore_begin", window, snapshot);
    if (!before.valid) { finish(L"invalid_window"); return; }
    if (before_restore_ && !before.visible) {
        stage(L"before_restore");
        try { const auto hook = before_restore_; hook(); }
        catch (...) { finish(L"before_restore_exception"); return; }
    }
    if (hwnd_ != window) { finish(L"target_changed"); return; }
    if (!IsWindow(window)) { finish(L"window_destroyed"); return; }
    if (!before.visible && snapshot) {
        // Keep the pre-hook snapshot until a visible, nonminimized restore is confirmed.
        placement.showCmd = placement.showCmd == SW_SHOWMAXIMIZED ||
            (placement.flags & WPF_RESTORETOMAXIMIZED) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        placement.flags &= ~WPF_RESTORETOMAXIMIZED;
        requested_show = static_cast<int>(placement.showCmd);
        placement_attempted = true;
        stage(L"set_window_placement");
        SetLastError(ERROR_SUCCESS);
        placement_applied = SetWindowPlacement(window, &placement) != FALSE;
        if (!placement_applied) placement_error = GetLastError();
        if (hwnd_ != window) { finish(L"target_changed"); return; }
        if (!IsWindow(window)) { finish(L"window_destroyed"); return; }
    }
    if (!placement_applied || !InspectWindowRevealState(window).Restored()) {
        if (!placement_attempted)
            requested_show = IsIconic(window) ? SW_RESTORE : restore_maximized_ ? SW_SHOWMAXIMIZED : SW_SHOW;
        show_called = true;
        stage(L"show_window");
        // The return value is previous visibility, not success or a GetLastError contract.
        show_previous_visible = ShowWindow(window, requested_show) != FALSE;
    }
    if (hwnd_ != window) { finish(L"target_changed"); return; }
    if (!IsWindow(window)) { finish(L"window_destroyed"); return; }
    if (InspectWindowRevealState(window).Restored()) {
        foreground_attempted = true;
        stage(L"set_foreground");
        foreground_set = SetForegroundWindow(window) != FALSE;
    }
    if (hwnd_ != window) { finish(L"target_changed"); return; }
    if (!IsWindow(window)) { finish(L"window_destroyed"); return; }
    if (InspectWindowRevealState(window).Restored()) {
        has_hidden_placement_ = false;
        restore_maximized_ = false;
        finish(L"restored");
    } else finish(L"not_visible");
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
    const HWND window = hwnd_;
    LogPlacement("tray_start_hidden", window, has_hidden_placement_);
    ShowWindow(window, SW_HIDE);
    const auto after = InspectWindowRevealState(window);
    diagnostics::runtime::Event("tray_start_hidden_result", {
        {"window", reinterpret_cast<uintptr_t>(window)}, {"requested_show", SW_HIDE},
        {"window_valid", after.valid}, {"visible_after", after.visible}, {"minimized_after", after.minimized},
        {"hide_ok", hwnd_ == window && after.valid && !after.visible}});
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
