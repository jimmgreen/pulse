#pragma once
#include <windows.h>
#include <cwchar>

namespace pulse::app {
enum class WinEForegroundKind : unsigned { Windowed, Fullscreen, Unknown };
// Diagnostic classification only. Window geometry never controls Win+E routing.
inline WinEForegroundKind ClassifyWinEForeground(bool known, const RECT& window, const RECT& monitor) noexcept {
    if (!known) return WinEForegroundKind::Unknown;
    return window.left <= monitor.left && window.top <= monitor.top &&
        window.right >= monitor.right && window.bottom >= monitor.bottom
        ? WinEForegroundKind::Fullscreen : WinEForegroundKind::Windowed;
}
inline bool WinEIsShellDesktop(HWND foreground) noexcept {
    const HWND shell = GetShellWindow();
    if (!foreground || !shell) return false;
    DWORD shell_pid = 0, foreground_pid = 0;
    if (!GetWindowThreadProcessId(shell, &shell_pid) ||
        !GetWindowThreadProcessId(foreground, &foreground_pid) ||
        !shell_pid || shell_pid != foreground_pid) return false;
    if (foreground == shell) return true;
    wchar_t name[32]{};
    return GetClassNameW(foreground, name, ARRAYSIZE(name)) &&
        (wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0);
}
inline WinEForegroundKind ClassifyWinEForeground(HWND foreground) noexcept {
    if (WinEIsShellDesktop(foreground)) return WinEForegroundKind::Windowed;
    RECT bounds{};
    MONITORINFO monitor{sizeof(monitor)};
    const bool known = foreground && GetWindowRect(foreground, &bounds) &&
        GetMonitorInfoW(MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL), &monitor);
    return ClassifyWinEForeground(known, bounds, monitor.rcMonitor);
}
}
