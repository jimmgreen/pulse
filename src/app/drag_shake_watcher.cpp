#include "drag_shake_watcher.h"
#include "double_ctrl.h"
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>

namespace pulse::app {
namespace {

std::atomic<bool> g_internal_file_drag{false};
std::atomic<bool> g_shelf_own_drag{false};

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
    if (DoubleCtrlWatcher::QuietMoment()) return false;
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

void DragShakeDetector::SetSensitivity(int level) noexcept {
    level = std::clamp(level, 0, 2);
    if (level == sensitivity_) return;
    sensitivity_ = level;
    Reset();
}

void DragShakeDetector::Reset() noexcept {
    has_anchor_ = false;
    dir_ = 0;
    anchor_ = 0;
    extreme_ = 0;
    rev_count_ = 0;
}

bool DragShakeDetector::Feed(int x_px, float dpi_scale, uint64_t now_ms) noexcept {
    if (now_ms < cooldown_until_) return false;
    const float s = std::max(0.5f, dpi_scale);
    // Low / Medium / High thresholds matching staging_shelf_mockup.html.
    float init_dip = 6.0f, turn_dip = 10.0f, amp_dip = 22.0f;
    uint64_t window_ms = 900;
    int need_rev = 3;
    if (sensitivity_ == 0) {
        init_dip = 8.0f; turn_dip = 12.0f; amp_dip = 40.0f; window_ms = 850; need_rev = 4;
    } else if (sensitivity_ == 2) {
        init_dip = 5.0f; turn_dip = 7.0f; amp_dip = 14.0f; window_ms = 1000; need_rev = 3;
    }
    const int init_px = std::max(2, static_cast<int>(std::lround(init_dip * s)));
    const int turn_px = std::max(3, static_cast<int>(std::lround(turn_dip * s)));
    const int amp_px = std::max(6, static_cast<int>(std::lround(amp_dip * s)));

    if (!has_anchor_) {
        has_anchor_ = true;
        anchor_ = extreme_ = x_px;
        dir_ = 0;
        return false;
    }
    const int dx = x_px - extreme_;
    if (dir_ == 0) {
        if (std::abs(dx) > init_px) {
            dir_ = dx > 0 ? 1 : -1;
            extreme_ = x_px;
        }
        return false;
    }
    if ((dx > 0 && dir_ > 0) || (dx < 0 && dir_ < 0)) {
        extreme_ = x_px;
        return false;
    }
    if (std::abs(dx) < turn_px) return false;

    const bool counted_stroke = std::abs(extreme_ - anchor_) >= amp_px;
    anchor_ = extreme_;
    extreme_ = x_px;
    dir_ = -dir_;

    // Keep the boundary sample if three reversals fit in exactly window_ms.
    int kept = 0;
    for (int i = 0; i < rev_count_; ++i) {
        if (now_ms - reversals_[i] <= window_ms)
            reversals_[kept++] = reversals_[i];
    }
    rev_count_ = kept;
    if (counted_stroke) reversals_[rev_count_++] = now_ms;

    if (rev_count_ >= need_rev) {
        Reset();
        cooldown_until_ = now_ms + 1200;
        return true;
    }
    return false;
}

POINT DragShakeWatcher::LastTriggerPoint() const noexcept {
    return UnpackPoint(last_pt_.load());
}

void DragShakeWatcher::Update(HWND target, UINT message, bool shake_enabled, int sensitivity) {
    target_ = target;
    message_ = message;
    shake_enabled_ = shake_enabled;
    sensitivity_ = std::clamp(sensitivity, 0, 2);
}

void DragShakeWatcher::Notify(POINT point) {
    last_pt_.store(PackPoint(point));
    if (const HWND target = target_.load()) PostMessageW(target, message_.load(), 0, 0);
}

void DragShakeWatcher::PollPointer() {
    if (!target_.load(std::memory_order_relaxed) ||
        !shake_enabled_.load(std::memory_order_relaxed)) {
        detector_.Reset();
        pointer_held_ = false;
        last_sample_at_ = 0;
        return;
    }
    const auto now = GetTickCount64();
    if (now - last_sample_at_ < (pointer_held_ ? 8u : 32u)) return;
    last_sample_at_ = now;
    pointer_held_ = PointerButtonHeld();
    if (!pointer_held_) {
        detector_.Reset();
        return;
    }
    POINT point{};
    if (!GetCursorPos(&point)) return;
    detector_.SetSensitivity(sensitivity_.load(std::memory_order_relaxed));
    if (detector_.Feed(point.x, MonitorScaleAt(point), now) && IsSystemOleDragActive()) Notify(point);
}

void DragShakeWatcher::OnInput(const RAWINPUT& raw) {
    if (!target_.load(std::memory_order_relaxed)) return;
    if (raw.header.dwType == RIM_TYPEMOUSE) {
        // Raw Input is the fast path, not a prerequisite: the cursor and button
        // state may change without a matching raw button-down or movement packet.
        constexpr USHORT button_changes = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_LEFT_BUTTON_UP |
                                          RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_UP;
        if (raw.data.mouse.usButtonFlags & button_changes) {
            detector_.Reset();
            last_sample_at_ = 0;
        }
        PollPointer();
    } else if (raw.header.dwType == RIM_TYPEKEYBOARD && raw.data.keyboard.VKey == 'S') {
        if (raw.data.keyboard.Flags & RI_KEY_BREAK) {
            shortcut_down_ = false;
            return;
        }
        if (shortcut_down_) return;
        shortcut_down_ = true;
        if (!PointerButtonHeld() || !(GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
            !(GetAsyncKeyState(VK_SHIFT) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000) ||
            !IsSystemOleDragActive()) return;
        POINT p{};
        if (GetCursorPos(&p)) Notify(p);
    }
}

} // namespace pulse::app
