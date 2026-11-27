#include "drag_shake_watcher.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>

namespace pulse::app {
namespace {

std::atomic<bool> g_internal_file_drag{false};
std::atomic<bool> g_shelf_own_drag{false};
thread_local DragShakeWatcher* g_shortcut_watcher = nullptr;
constexpr UINT kShortcutMessage = WM_APP + 1;

int64_t PackPoint(POINT pt) noexcept {
    return static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(pt.x)) << 32) |
                                static_cast<uint32_t>(pt.y));
}
POINT UnpackPoint(int64_t v) noexcept {
    return POINT{static_cast<LONG>(v >> 32), static_cast<LONG>(static_cast<int32_t>(v & 0xFFFFFFFF))};
}

float MonitorScaleAt(POINT pt) noexcept {
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (mon) {
        UINT dpi_x = 96, dpi_y = 96;
        if (SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) && dpi_x > 0)
            return static_cast<float>(dpi_x) / 96.0f;
    }
    return 1.0f;
}

bool PointerButtonHeld() noexcept {
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
}

} // namespace

void SetInternalFileDragActive(bool active) noexcept { g_internal_file_drag.store(active); }
bool IsInternalFileDragActive() noexcept { return g_internal_file_drag.load(); }
void SetShelfOwnDragActive(bool active) noexcept { g_shelf_own_drag.store(active); }
bool IsShelfOwnDragActive() noexcept { return g_shelf_own_drag.load(); }

bool IsSystemOleDragActive() noexcept {
    if (g_shelf_own_drag.load()) return false;
    QUERY_USER_NOTIFICATION_STATE state{};
    if (SUCCEEDED(SHQueryUserNotificationState(&state)) &&
        (state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE))
        return false;
    GUITHREADINFO gti{sizeof(gti)};
    if (GetGUIThreadInfo(0, &gti)) {
        if ((gti.flags & (GUI_INMOVESIZE | GUI_INMENUMODE |
                          GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)) != 0) {
            return false;
        }
        if (g_internal_file_drag.load()) return true;
        if (gti.hwndCapture) {
            wchar_t cls[64]{};
            if (GetClassNameW(gti.hwndCapture, cls, ARRAYSIZE(cls)) > 0) {
                if (_wcsicmp(cls, L"CLIPBRDWNDCLASS") == 0 ||
                    _wcsicmp(cls, L"OleMainThreadWndClass") == 0) {
                    return true;
                }
            }
        }
    } else if (g_internal_file_drag.load()) {
        return true;
    }
    if (HWND drag_img = FindWindowW(L"SysDragImage", nullptr)) {
        if (IsWindowVisible(drag_img)) return true;
    }
    return false;
}

bool DragShortcutDetector::Key(UINT key, bool down) noexcept {
    if (key >= down_.size()) return false;
    const bool was_down = down_[key];
    down_[key] = down;
    return key == 'S' && down && !was_down &&
        (down_[VK_LCONTROL] || down_[VK_RCONTROL] || down_[VK_CONTROL]) &&
        (down_[VK_LSHIFT] || down_[VK_RSHIFT] || down_[VK_SHIFT]) &&
        !(down_[VK_LMENU] || down_[VK_RMENU] || down_[VK_MENU]);
}

POINT DragShakeWatcher::LastTriggerPoint() const noexcept {
    return UnpackPoint(last_pt_.load());
}

void DragShakeWatcher::Update(HWND target, UINT message, bool shake_enabled, int sensitivity) {
    // Drain the old listener before its owning window may be destroyed. Update
    // and Stop are called serially by the controller's UI thread.
    if (target_.load() != target || exited_.load()) Stop();
    target_ = target;
    message_ = message;
    shake_enabled_ = shake_enabled;
    sensitivity_ = std::clamp(sensitivity, 0, 2);
    if (!target) { Stop(); return; }
    if (!thread_.joinable()) {
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_event_) return;
        exited_ = false;
        try {
            thread_ = std::thread([this] { Run(); exited_ = true; });
        } catch (...) {
            CloseHandle(stop_event_);
            stop_event_ = nullptr;
        }
    }
}

void DragShakeWatcher::Stop() {
    target_ = nullptr;
    if (stop_event_) SetEvent(stop_event_);
    if (thread_.joinable()) thread_.join();
    if (stop_event_) CloseHandle(stop_event_);
    stop_event_ = nullptr;
}

void DragShakeWatcher::Notify(POINT point) {
    last_pt_.store(PackPoint(point));
    if (const HWND target = target_.load()) PostMessageW(target, message_.load(), 0, 0);
}

void DragShakeWatcher::WatchShortcut(bool enabled) {
    if (enabled && !keyboard_hook_) {
        shortcut_.Reset();
        // Seed existing presses, including S: holding S before the listener
        // attaches must not become a new shortcut edge through auto-repeat.
        for (UINT key : {VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT, VK_LMENU, VK_RMENU, int('S')})
            shortcut_.Seed(key, (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0);
        g_shortcut_watcher = this;
        keyboard_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandleW(nullptr), 0);
        if (!keyboard_hook_) g_shortcut_watcher = nullptr;
    } else if (!enabled && keyboard_hook_) {
        UnhookWindowsHookEx(keyboard_hook_);
        keyboard_hook_ = nullptr;
        g_shortcut_watcher = nullptr;
        shortcut_.Reset();
    }
}

LRESULT CALLBACK DragShakeWatcher::KeyboardProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && g_shortcut_watcher) {
        auto& self = *g_shortcut_watcher;
        const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
        const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
        if (self.shortcut_.Key(key.vkCode, down)) {
            POINT point{};
            // Snapshot the chord at the S edge; releasing S/modifiers before
            // the posted message is processed must not erase this press.
            if (PointerButtonHeld() && !IsShelfOwnDragActive() && GetCursorPos(&point)) {
                self.last_pt_.store(PackPoint(point));
                PostMessageW(self.window_, kShortcutMessage, 0, 0);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

void DragShakeWatcher::PollPointer() {
    pointer_held_ = PointerButtonHeld();
    const bool eligible = pointer_held_ && !IsShelfOwnDragActive();
    WatchShortcut(eligible);
    if (!eligible || !shake_enabled_.load(std::memory_order_relaxed)) {
        detector_.Reset();
        last_sample_at_ = 0;
        return;
    }
    const auto now = GetTickCount64();
    if (last_sample_at_ && now - last_sample_at_ < 8u) return;
    last_sample_at_ = now;
    POINT point{};
    if (!GetCursorPos(&point)) return;
    detector_.SetSensitivity(sensitivity_.load(std::memory_order_relaxed));
    if (detector_.Feed(point.x, MonitorScaleAt(point), now) && IsSystemOleDragActive()) Notify(point);
}

void DragShakeWatcher::OnMouse(const RAWMOUSE& mouse) {
    constexpr USHORT button_changes = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_LEFT_BUTTON_UP |
                                      RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_UP;
    if (mouse.usButtonFlags & button_changes) {
        detector_.Reset();
        last_sample_at_ = 0;
    }
    PollPointer();
}

LRESULT CALLBACK DragShakeWatcher::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<DragShakeWatcher*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<DragShakeWatcher*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else if (self && message == WM_INPUT) {
        RAWINPUT input{};
        UINT size = sizeof(input);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &input, &size,
                            sizeof(RAWINPUTHEADER)) != UINT(-1) && input.header.dwType == RIM_TYPEMOUSE)
            self->OnMouse(input.data.mouse);
    } else if (self && message == kShortcutMessage) {
        if (PointerButtonHeld() && IsSystemOleDragActive()) self->Notify(self->LastTriggerPoint());
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void DragShakeWatcher::Run() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.lpszClassName = L"PulseShelfMouseInput";
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    window_ = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, instance, this);
    if (!window_) return;
    RAWINPUTDEVICE mouse{0x01, 0x02, RIDEV_INPUTSINK, window_};
    const bool registered = RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) != FALSE;
    running_ = true;
    // Polling also covers synthetic pointer changes and registration failure.
    // No filesystem, Shell call, or input sampling runs on the UI thread.
    while (WaitForSingleObject(stop_event_, 0) == WAIT_TIMEOUT) {
        PollPointer();
        const DWORD wait = MsgWaitForMultipleObjectsEx(1, &stop_event_, pointer_held_ ? 8 : 32,
                                                       QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
        MSG msg{};
        // Bounded drain avoids starving stop/polling under a busy input stream.
        for (unsigned i = 0; i < 64 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++i) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    WatchShortcut(false);
    if (registered) {
        mouse.dwFlags = RIDEV_REMOVE;
        mouse.hwndTarget = nullptr;
        RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
    }
    DestroyWindow(window_);
    window_ = nullptr;
    pointer_held_ = false;
    last_sample_at_ = 0;
    detector_ = DragShakeDetector{};
    running_ = false;
}

} // namespace pulse::app
