#pragma once
#include "preview_grab_cursor.h"
#include "preview_thumb_tracking.h"
#include "../common/windows_compat.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <windows.h>

namespace pulse::ui {

// Preview providers own HWNDs in other processes. Observe only left-button
// gestures over this host; never subclass or synchronously call provider windows.
class PreviewHandlerPan {
public:
    ~PreviewHandlerPan() {
        Disable();
        if (input_) {
            input_->stop = true;
            if (const DWORD thread = input_->thread_id.load())
                PostThreadMessageW(thread, WM_NULL, 0, 0);
        }
    }

    void Enable(HWND host) {
        if (!host || !IsWindowVisible(host) || host == host_) return;
        host_ = host;
        if (!input_) {
            input_ = std::make_shared<InputState>();
            input_->api = input_api_;
            auto* argument = new std::shared_ptr<InputState>(input_);
            input_->thread = CreateThread(nullptr, 0, InputMain, argument, 0, nullptr);
            if (!input_->thread) {
                delete argument;
                input_.reset();
                host_ = nullptr;
                return;
            }
        }
        input_->host = host;
        RequestHook(true);
    }

    void Disable() {
        Cancel();
        if (input_) {
            input_->host = nullptr;
            ++input_->generation;
        }
        host_ = nullptr;
        RequestHook(false);
    }

    bool HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
        if (message >= kBegin && message <= kEnd) {
            if (!input_ || wparam != input_->generation.load() || !host_) return true;
            if (message != kEnd && !input_->active) return true;
            POINT point{static_cast<SHORT>(LOWORD(lparam)), static_cast<SHORT>(HIWORD(lparam))};
            if (message == kBegin) {
                HWND target = input_->target.load();
                if (target == host_ || IsChild(host_, target)) Begin(target, point);
            } else if (message == kMove && dragging_) {
                Scroll(horizontal_, false, point.x - previous_.x, point);
                Scroll(vertical_, true, point.y - previous_.y, point);
                previous_ = point;
                SetCursor(PreviewGrabCursor(true));
            } else if (message == kEnd) {
                Cancel();
            }
            return true;
        }
        if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE ||
            message == WM_DESTROY) {
            Cancel();
        } else if (message == WM_TIMER && wparam == kCaptureTimer) {
            // The intercepted left-button press is not reflected reliably in
            // async key state. Its release is handled by the hook itself.
            if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || GetCapture() != host_)
                Cancel();
            return true;
        } else if (message == WM_SETCURSOR && dragging_) {
            SetCursor(PreviewGrabCursor(true));
            return true;
        }
        return false;
    }

private:
    friend struct PreviewHandlerPanTest;
    friend struct PreviewHandlerPanThreadTest;
    struct InputApi {
        decltype(&SetWindowsHookExW) set_hook = SetWindowsHookExW;
        decltype(&UnhookWindowsHookEx) unhook = UnhookWindowsHookEx;
        decltype(&GetMessageW) get_message = GetMessageW;
        decltype(&PeekMessageW) peek_message = PeekMessageW;
        decltype(&CallNextHookEx) call_next = CallNextHookEx;
        decltype(&WindowFromPoint) window_from_point = WindowFromPoint;
    };
    struct InputState;

    // The low-level hook is machine wide: while it is installed every mouse
    // event is marshalled into this process, so it must only stay live while a
    // preview can be panned. Installing it is posted to the hook thread, which
    // is the only thread that may receive its callbacks.
    void RequestHook(bool install) {
        if (!input_) return;
        input_->desired_hook = install;
        const DWORD thread = input_->thread_id.load();
        if (thread) PostThreadMessageW(thread, kHookCommand, 0, 0);
    }

    struct InputState {
        ~InputState() { if (thread) CloseHandle(thread); }
        InputApi api;
        HANDLE thread = nullptr;
        std::atomic<DWORD> thread_id{0};
        std::atomic<HWND> host{nullptr};
        std::atomic<UINT> generation{0};
        std::atomic<bool> active{false};
        std::atomic<bool> stop{false};
        std::atomic<bool> desired_hook{false};
        // Only the input thread touches this, including after host destruction.
        bool suppress_left_up = false;
        bool pending = false;
        POINT press{};
        HWND pending_host = nullptr;
        HWND pending_target = nullptr;
        UINT pending_generation = 0;
        std::atomic<HWND> target{nullptr};
        HWND thumb_host = nullptr;
        HWND thumb_target = nullptr;
        POINT thumb_press{};
        UINT thumb_generation = 0;
        UINT_PTR thumb_timer = 0;
        PreviewThumbTracking thumb_tracking;
    };

    static void SetHook(InputState& input, HHOOK& hook, bool install) {
        if (install == (hook != nullptr)) return;
        if (install) hook = input.api.set_hook(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
        else { input.api.unhook(hook); hook = nullptr; }
    }

    struct Axis {
        HWND target = nullptr;
        double position = 0;
        double units_per_pixel = 1;
        int minimum = 0;
        int maximum = 0;
        bool thumb = false;
    };

    static constexpr UINT_PTR kCaptureTimer = 0x50414e;
    static constexpr UINT kBegin = WM_APP + 0x504;
    static constexpr UINT kMove = kBegin + 1;
    static constexpr UINT kEnd = kBegin + 2;
    static constexpr UINT kHookCommand = kBegin + 3;
    static constexpr UINT kThumbPress = kBegin + 4;
    inline static thread_local InputState* current_input_ = nullptr;

    Axis FindAxis(HWND target, int bar) const {
        for (HWND window = target; window; window = GetParent(window)) {
            SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
            if (GetScrollInfo(window, bar, &info) && info.nMax > info.nMin &&
                info.nMax <= 65535 && info.nMin >= 0 &&
                static_cast<UINT>(info.nMax - info.nMin) >= info.nPage) {
                RECT rect{};
                GetClientRect(window, &rect);
                const int pixels = bar == SB_VERT ? rect.bottom : rect.right;
                Axis axis;
                axis.target = window;
                axis.position = info.nPos;
                axis.units_per_pixel = info.nPage > 0
                    ? static_cast<double>(info.nPage) / std::max(1, pixels) : 1.0;
                axis.minimum = info.nMin;
                axis.maximum = info.nMax - static_cast<int>(info.nPage > 0 ? info.nPage - 1 : 0);
                axis.thumb = true;
                return axis;
            }
            if (window == host_) break;
        }
        Axis axis;
        axis.target = target;
        // Custom preview canvases often expose only wheel input. Accumulate
        // partial detents so motion remains displacement-driven, never timed.
        axis.units_per_pixel = static_cast<double>(WHEEL_DELTA) / 48.0;
        return axis;
    }

    void Begin(HWND target, POINT point) {
        horizontal_ = FindAxis(target, SB_HORZ);
        vertical_ = FindAxis(target, SB_VERT);
        previous_ = point;
        dragging_ = true;
        SetCapture(host_);
        SetTimer(host_, kCaptureTimer, 50, nullptr);
        SetCursor(PreviewGrabCursor(true));
    }

    void Scroll(Axis& axis, bool vertical, LONG pixels, POINT point) {
        if (!pixels || !axis.target || !IsWindow(axis.target)) return;
        if (axis.thumb) {
            const int old = static_cast<int>(std::lround(axis.position));
            axis.position = std::clamp(axis.position - pixels * axis.units_per_pixel,
                static_cast<double>(axis.minimum), static_cast<double>(axis.maximum));
            const int position = static_cast<int>(std::lround(axis.position));
            if (old != position)
                PostMessageW(axis.target, vertical ? WM_VSCROLL : WM_HSCROLL,
                    MAKEWPARAM(SB_THUMBPOSITION, position), 0);
        } else {
            axis.position += pixels * axis.units_per_pixel * (vertical ? 1.0 : -1.0);
            const int delta = static_cast<int>(axis.position);
            axis.position -= delta;
            if (delta)
                PostMessageW(axis.target, vertical ? WM_MOUSEWHEEL : WM_MOUSEHWHEEL,
                    MAKEWPARAM(0, static_cast<SHORT>(std::clamp(delta, -32767, 32767))),
                    MAKELPARAM(point.x, point.y));
        }
    }

    void Cancel() {
        if (input_) input_->active = false;
        if (!dragging_) return;
        dragging_ = false;
        KillTimer(host_, kCaptureTimer);
        if (GetCapture() == host_) ReleaseCapture();
        for (const Axis* axis : {&horizontal_, &vertical_}) {
            if (axis->thumb && axis->target && IsWindow(axis->target))
                PostMessageW(axis->target, axis == &vertical_ ? WM_VSCROLL : WM_HSCROLL,
                    SB_ENDSCROLL, 0);
        }
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        horizontal_ = {};
        vertical_ = {};
    }

    static void StopThumbTracking(InputState& input) {
        if (input.thumb_timer) KillTimer(nullptr, input.thumb_timer);
        input.thumb_timer = 0;
        input.thumb_host = nullptr;
    }

    static bool QueueBegin(InputState& input, HWND host, POINT point, HWND target = nullptr) {
        input.target = target ? target : host;
        input.suppress_left_up = true;
        input.active = true;
        const UINT generation = ++input.generation;
        if (PostMessageW(host, kBegin, generation, MAKELPARAM(point.x, point.y))) return true;
        input.suppress_left_up = false;
        input.active = false;
        return false;
    }

    static bool CanGrabContent(HWND host, HWND target, POINT point) {
        if (!host || !target || (target != host && !IsChild(host, target))) return false;
        const UINT dpi = compat::WindowDpi(host);
        const int minimum = MulDiv(20, static_cast<int>(dpi), 96);
        const int right_lane = std::max(minimum, compat::SystemMetricsForDpi(SM_CXVSCROLL, dpi));
        const int bottom_lane = std::max(minimum, compat::SystemMetricsForDpi(SM_CYHSCROLL, dpi));
        for (HWND window = target; window; window = GetParent(window)) {
            RECT client{};
            POINT local = point;
            const bool in_client = GetClientRect(window, &client) &&
                ScreenToClient(window, &local) && PtInRect(&client, local);
            if (window == target && !in_client) return false; // Native scrollbar or border.
            // A provider can draw its scrollbar inside the client HWND. Don't
            // hand-pan from the edge of its canvas (or an enclosing viewport).
            const auto ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
            if (in_client && (local.x >= client.right - right_lane ||
                local.y >= client.bottom - bottom_lane ||
                ((ex_style & (WS_EX_LEFTSCROLLBAR | WS_EX_LAYOUTRTL)) &&
                    local.x < client.left + right_lane))) return false;
            if (window == host) break;
            wchar_t name[128]{};
            GetClassNameW(window, name, ARRAYSIZE(name));
            if (_wcsicmp(name, L"Button") == 0 || _wcsicmp(name, L"ScrollBar") == 0 ||
                _wcsicmp(name, L"ToolbarWindow32") == 0 || _wcsicmp(name, L"ComboBox") == 0 ||
                _wcsicmp(name, L"Edit") == 0) return false;
        }
        return true;
    }

    static void TrackPress(InputState& input, HWND host, HWND target, POINT point) {
        input.pending = true;
        input.press = point;
        input.pending_host = host;
        input.pending_target = target;
        input.pending_generation = input.generation.load();
    }

    static bool ConsumeMouse(InputState& input, WPARAM message, POINT point) {
        HWND host = input.host.load();
        if (input.pending && (input.stop || !input.desired_hook || host != input.pending_host ||
            input.generation.load() != input.pending_generation)) input.pending = false;
        if (message == WM_LBUTTONUP) {
            StopThumbTracking(input);
            input.pending = false;
            if (!input.suppress_left_up) return false;
            input.suppress_left_up = false;
            input.active = false;
            if (host) PostMessageW(host, kEnd, input.generation.load(), MAKELPARAM(point.x, point.y));
            if (input.stop) PostQuitMessage(0);
            else if (const DWORD thread = input.thread_id.load()) PostThreadMessageW(thread, kHookCommand, 0, 0);
            return true;
        }
        if (!host || input.stop || !input.desired_hook) return false;
        if (message == WM_LBUTTONDOWN && !input.active && !input.suppress_left_up) {
            StopThumbTracking(input);
            HWND target = input.api.window_from_point(point);
            if (IsWindowVisible(host) && target && (target == host || IsChild(host, target))) {
                if (CanGrabContent(host, target, point)) TrackPress(input, host, target, point);
                else if (const DWORD thread = input.thread_id.load()) {
                    input.thumb_host = host;
                    input.thumb_target = target;
                    input.thumb_press = point;
                    input.thumb_generation = input.generation.load();
                    PostThreadMessageW(thread, kThumbPress, 0, 0);
                }
            }
            // Let the provider receive the real press and release for clicks,
            // including custom PDF buttons drawn inside its content HWND.
            return false;
        }
        if (message == WM_RBUTTONDOWN) {
            StopThumbTracking(input);
            input.pending = false;
            if (input.active.exchange(false))
                PostMessageW(host, kEnd, input.generation.load(), 0);
            return false;
        }
        if (message == WM_MOUSEMOVE && input.pending) {
            if (std::abs(point.x - input.press.x) < GetSystemMetrics(SM_CXDRAG) &&
                std::abs(point.y - input.press.y) < GetSystemMetrics(SM_CYDRAG)) return false;
            input.pending = false;
            const HWND target = input.pending_target;
            if (!IsWindow(target) || (target != host && !IsChild(host, target))) return false;
            if (!QueueBegin(input, host, input.press, target)) return false;
            // The native press has already reached the provider. Cancel it
            // before taking capture so a drag cannot activate a pressed button.
            if (target != host) PostMessageW(target, WM_CANCELMODE, 0, 0);
        }
        if (message == WM_MOUSEMOVE && input.active) {
            PostMessageW(host, kMove, input.generation.load(), MAKELPARAM(point.x, point.y));
            SetCursor(PreviewGrabCursor(true));
        }
        return false; // Preserve pointer motion and native wheel browsing.
    }

    static LRESULT CALLBACK MouseHook(int code, WPARAM message, LPARAM parameter) {
        if (code >= 0 && current_input_) {
            const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(parameter);
            if (ConsumeMouse(*current_input_, message, mouse->pt)) return 1;
        }
        return current_input_ ? current_input_->api.call_next(nullptr, code, message, parameter)
                              : CallNextHookEx(nullptr, code, message, parameter);
    }

    static DWORD WINAPI InputMain(void* parameter) {
        std::unique_ptr<std::shared_ptr<InputState>> argument(
            static_cast<std::shared_ptr<InputState>*>(parameter));
        auto input = *argument;
        MSG message{};
        input->api.peek_message(&message, nullptr, 0, 0, PM_NOREMOVE);
        input->thread_id = GetCurrentThreadId();
        current_input_ = input.get();
        // This thread never calls preview providers. Even a blocked preview STA
        // cannot delay the mouse hook or leak a consumed gesture's release.
        HHOOK hook = nullptr;
        for (;;) {
            // Wake-up messages carry no state: Enable/Disable may precede queue
            // creation or race each other. An intercepted drag owns its release
            // even after the preview is hidden or this owner is destroyed.
            SetHook(*input, hook, (!input->stop && input->desired_hook) || input->suppress_left_up);
            if (input->stop && !input->suppress_left_up) break;
            if (input->thumb_host && (!input->desired_hook || input->stop ||
                input->host != input->thumb_host ||
                input->generation != input->thumb_generation)) StopThumbTracking(*input);
            if (input->api.get_message(&message, nullptr, 0, 0) <= 0) break;
            if (message.message == kHookCommand) continue;
            if (message.message == kThumbPress) {
                if (input->thumb_host && !input->thumb_timer && input->thumb_tracking.Begin(
                    input->thumb_host, input->thumb_target, input->thumb_press)) {
                    // The provider can be in a native scrollbar modal loop.
                    // Poll on our own queue, never in the hook callback.
                    input->thumb_timer = SetTimer(nullptr, 0, 32, nullptr);
                }
                if (!input->thumb_timer) StopThumbTracking(*input);
                continue;
            }
            if (message.message == WM_TIMER && !message.hwnd && input->thumb_timer &&
                message.wParam == input->thumb_timer) {
                if (input->host != input->thumb_host || input->generation != input->thumb_generation ||
                    !input->thumb_tracking.Tick()) StopThumbTracking(*input);
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        StopThumbTracking(*input);
        SetHook(*input, hook, false);
        current_input_ = nullptr;
        return 0;
    }

    InputApi input_api_;
    HWND host_ = nullptr;
    std::shared_ptr<InputState> input_;
    bool dragging_ = false;
    POINT previous_{};
    Axis horizontal_;
    Axis vertical_;
};

} // namespace pulse::ui
