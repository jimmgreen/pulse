#pragma once
#include <windows.h>
#include <algorithm>

namespace pulse::ui {
inline UINT PickerLegacyChildMessage() { static const UINT message = RegisterWindowMessageW(L"Pulse.Picker.AttachLegacyChild.v1"); return message; }
inline constexpr LRESULT kPickerLegacyChildAttached = 0x504C4348L;
class PickerLegacyChild {
public:
    bool Attach(HWND parent, HWND child, int minimum_content_height) {
        if (!child) { child_ = nullptr; height_ = 0; return true; }
        DWORD pid = 0;
        if (GetWindowThreadProcessId(child, &pid) != GetCurrentThreadId() || pid != GetCurrentProcessId() ||
            GetParent(child) != parent || !(GetWindowLongPtrW(child, GWL_STYLE) & WS_CHILD)) return false;
        RECT rect{}, owner{}; if (!GetWindowRect(child, &rect) || !GetWindowRect(parent, &owner)) return false;
        const int height = rect.bottom - rect.top;
        if (height <= 0 || height > 2048 || (child_ && child_ != child)) return false;
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST), &monitor) ||
            minimum_content_height + height > monitor.rcWork.bottom - monitor.rcWork.top) return false;
        const int total = (std::min)((std::max)(static_cast<int>(owner.bottom - owner.top) + height - Height(parent), minimum_content_height + height),
            static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
        const int top = static_cast<int>((std::max)(monitor.rcWork.top, (std::min)(owner.top, monitor.rcWork.bottom - total)));
        child_ = child; height_ = height;
        if (!SetWindowPos(parent, nullptr, owner.left, top, owner.right - owner.left, total, SWP_NOACTIVATE | SWP_NOZORDER)) {
            child_ = nullptr; height_ = 0; return false;
        }
        Layout(parent); return true;
    }
    int Height(HWND parent) const { return Alive(parent) ? height_ : 0; }
    bool Focus(HWND parent, bool reverse) const {
        if (!Alive(parent) || !IsWindowVisible(child_)) return false;
        const HWND target = GetNextDlgTabItem(child_, nullptr, reverse ? TRUE : FALSE);
        if (!target) return false;
        SetFocus(target); return GetFocus() == target;
    }
    bool TabExit(HWND parent, const MSG& message, int& direction) const {
        if (!Alive(parent) || message.message != WM_KEYDOWN || message.wParam != VK_TAB ||
            !IsChild(child_, message.hwnd)) return false;
        if (SendMessageW(message.hwnd, WM_GETDLGCODE, VK_TAB, reinterpret_cast<LPARAM>(&message)) & DLGC_WANTTAB) return false;
        const bool reverse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const HWND boundary = GetNextDlgTabItem(child_, nullptr, reverse ? FALSE : TRUE);
        direction = reverse ? -1 : 1;
        return GetFocus() == boundary;
    }
    bool DialogMessage(HWND parent, MSG& message) const {
        return Alive(parent) && IsWindowVisible(child_) &&
            (message.hwnd == child_ || IsChild(child_, message.hwnd)) && IsDialogMessageW(child_, &message);
    }
    void Layout(HWND parent) const {
        if (!Alive(parent)) return;
        RECT client{}; GetClientRect(parent, &client);
        SetWindowPos(child_, HWND_TOP, 0, (std::max)(0L, client.bottom - height_), client.right, height_, SWP_NOACTIVATE);
    }
    void Clear() noexcept { child_ = nullptr; height_ = 0; }
private:
    bool Alive(HWND parent) const {
        DWORD pid = 0;
        return child_ && IsWindow(child_) && GetParent(child_) == parent &&
            GetWindowThreadProcessId(child_, &pid) == GetCurrentThreadId() && pid == GetCurrentProcessId();
    }
    HWND child_ = nullptr;
    int height_ = 0;
};
}
