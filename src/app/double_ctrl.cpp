#include "double_ctrl.h"
#include <shellapi.h>
#include <system_error>

namespace pulse {
namespace {
constexpr UINT kTrigger = WM_APP + 1;
bool IsCtrl(UINT key) { return key == VK_LCONTROL || key == VK_RCONTROL; }
}
bool DoubleCtrlDetector::Blocked(UINT except) const {
    for (UINT key = 0; key < pressed_.size(); ++key)
        if ((key != except && pressed_[key]) || injected_[key]) return true;
    return false;
}

bool DoubleCtrlDetector::PhysicalKey(UINT key, bool down, uint64_t now, bool injected) {
    if (key >= pressed_.size()) { Interrupt(); return false; }
    if (injected) {
        injected_[key] = down;
        Interrupt();
        return false;
    }
    const bool was_down = pressed_[key];
    pressed_[key] = down;
    if (!IsCtrl(key)) { Interrupt(); return false; }
    if (down) {
        if (Blocked(key)) { Interrupt(); return false; }
        if (was_down) {
            if (now - down_at_ > kHoldMs) Interrupt();
            return false;
        }
        // A different side starts a new pair; two overlapping Ctrl keys spoil it.
        phase_ = phase_ == Phase::FirstUp && tap_key_ == key && now - up_at_ <= gap_ ? Phase::SecondDown : Phase::FirstDown;
        tap_key_ = key;
        down_at_ = now;
        return false;
    }
    if (!was_down) return false;
    if (Blocked(key)) { Interrupt(); return false; }
    const bool tap = now - down_at_ <= kHoldMs;
    if (phase_ == Phase::FirstDown && tap) {
        phase_ = Phase::FirstUp;
        up_at_ = now;
        return false;
    }
    const bool fired = phase_ == Phase::SecondDown && tap;
    phase_ = Phase::Idle;
    return fired;
}

void DoubleCtrlDetector::Interrupt() {
    phase_ = pressed_[VK_LCONTROL] || pressed_[VK_RCONTROL] ? Phase::Spoiled : Phase::Idle;
}

bool DoubleCtrlWatcher::QuietMoment() {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (FAILED(SHQueryUserNotificationState(&state))) return false;
    return state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE;
}

void DoubleCtrlWatcher::Update(HWND target, UINT message, bool enabled, uint32_t gap_ms) {
    std::lock_guard lock(lifecycle_);
    if (enabled && target && Running() && target_ == target && message_ == message && gap_ == gap_ms) return;
    StopLocked();
    target_ = target;
    message_ = message;
    gap_ = gap_ms;
    error_ = ERROR_SUCCESS;
    if (!enabled || !target) return;
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready_) {
        error_ = GetLastError();
        StopLocked();
        return;
    }
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_) {
        error_ = GetLastError();
        StopLocked();
        return;
    }
    try { thread_ = std::thread([this] { Run(); }); }
    catch (const std::system_error&) {
        error_ = ERROR_NOT_ENOUGH_MEMORY;
        StopLocked();
        return;
    }
    // Run publishes readiness on both success and failure. No thread-ID polling.
    WaitForSingleObject(ready_, INFINITE);
    if (!Running()) StopLocked();
}

void DoubleCtrlWatcher::Stop() {
    std::lock_guard lock(lifecycle_);
    StopLocked();
}

void DoubleCtrlWatcher::StopLocked() {
    running_ = false;
    if (++generation_ == 0) ++generation_;
    if (stop_) SetEvent(stop_);
    if (thread_.joinable()) thread_.join();
    if (ready_) CloseHandle(ready_);
    if (stop_) CloseHandle(stop_);
    ready_ = stop_ = nullptr;
}

thread_local DoubleCtrlWatcher* DoubleCtrlWatcher::current_ = nullptr;

LRESULT CALLBACK DoubleCtrlWatcher::KeyboardProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && current_) {
        const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
        UINT key = event.vkCode;
        if (key == VK_CONTROL) key = (event.flags & LLKHF_EXTENDED) ? VK_RCONTROL : VK_LCONTROL;
        const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
        if (current_->detector_.PhysicalKey(key, down, GetTickCount64(),
                (event.flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)) != 0))
            PostThreadMessageW(GetCurrentThreadId(), kTrigger, current_->generation_.load(), 0);
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK DoubleCtrlWatcher::MouseProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && current_ && wparam != WM_MOUSEMOVE) {
        UINT key = 0;
        bool down = false;
        switch (wparam) {
        case WM_LBUTTONDOWN: down = true; [[fallthrough]];
        case WM_LBUTTONUP: key = VK_LBUTTON; break;
        case WM_RBUTTONDOWN: down = true; [[fallthrough]];
        case WM_RBUTTONUP: key = VK_RBUTTON; break;
        case WM_MBUTTONDOWN: down = true; [[fallthrough]];
        case WM_MBUTTONUP: key = VK_MBUTTON; break;
        case WM_XBUTTONDOWN: down = true; [[fallthrough]];
        case WM_XBUTTONUP:
            key = HIWORD(reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam)->mouseData) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
            break;
        default: break;
        }
        // Track buttons outside a gesture too: an already held button blocks Ctrl.
        // Injected mouse input contaminates, but can never create a Ctrl tap.
        if (key) current_->detector_.PhysicalKey(key, down, GetTickCount64(),
            (reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam)->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) != 0);
        else current_->detector_.Interrupt();
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

void DoubleCtrlWatcher::Run() {
    current_ = this;
    detector_ = {};
    detector_.SetGap(gap_);
    MSG msg{};
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    const auto module = GetModuleHandleW(nullptr);
    const HHOOK keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, module, 0);
    HHOOK mouse = nullptr;
    if (!keyboard) error_ = GetLastError();
    else {
        // Install together, not lazily from a callback: no gap can miss a click
        // or wheel between taps, and callbacks never install hooks.
        mouse = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, module, 0);
        if (!mouse) error_ = GetLastError();
    }
    // Seed after installation; callbacks run only when this thread pumps.
    // GetAsyncKeyState in a hook cannot report the current event's new state.
    for (UINT key = 1; key < 256; ++key) {
        if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU) continue;
        if (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) detector_.Prime(key);
    }
    running_ = keyboard && mouse;
    SetEvent(ready_);
    if (Running()) {
        const auto generation = generation_.load();
        bool done = false;
        while (!done) {
            const DWORD wait = MsgWaitForMultipleObjectsEx(1, &stop_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (wait == WAIT_OBJECT_0) break;
            if (wait == WAIT_FAILED) { error_ = GetLastError(); break; }
            for (int count = 0; count < 64 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++count) {
                if (msg.message == WM_QUIT) { done = true; break; }
                if (msg.message == kTrigger && !msg.hwnd) {
                    if (Running() && generation == generation_.load() && msg.wParam == generation)
                        PostMessageW(target_, message_, generation, 0);
                } else {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        }
    }
    running_ = false;
    if (mouse) UnhookWindowsHookEx(mouse);
    if (keyboard) UnhookWindowsHookEx(keyboard);
    current_ = nullptr;
    detector_ = {};
}
}
