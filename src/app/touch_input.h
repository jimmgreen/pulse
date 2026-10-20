#pragma once

#include "touch_gesture.h"
#include <windows.h>
#include <unordered_set>

namespace pulse {
struct AppState;
}

namespace pulse::app {

struct Tab;
struct Pane;

enum class TouchScrollTargetKind {
    None,
    Pane,
    Sidebar,
    Settings,
};

struct TouchScrollTarget {
    TouchScrollTargetKind kind = TouchScrollTargetKind::None;
    Pane* pane = nullptr;
    Tab* tab = nullptr;
    int settings_page = -1;
    bool column_major = false; // true for List view
    float max_scroll_x = 0.0f;
    float max_scroll_y = 0.0f;
};

class TouchInputAdapter {
public:
    explicit TouchInputAdapter(pulse::AppState& state);
    ~TouchInputAdapter() = default;

    // Returns true if the message was handled/consumed (return 0 in WndProc)
    bool HandlePointerMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& out_result);

    // Cancel current active gesture (called when modal overlay/menu appears, resize, tab switch, etc.)
    void CancelActiveGesture();

    // Timer tick to check hold gesture and advance inertia
    void OnTimerTick(HWND hwnd);

    bool IsInertiaActive() const noexcept { return inertia_.IsActive(); }

private:
    bool ClassifyPointer(WPARAM wParam, uint32_t& out_pointer_id, POINTER_INPUT_TYPE& out_type);
    void DispatchTap(HWND hwnd, float x, float y);
    void DispatchDoubleTap(HWND hwnd, float x, float y);
    void DispatchHold(HWND hwnd, float x, float y);
    void SetupScrollTarget(float x, float y);
    void ApplyScrollDelta(float delta_x, float delta_y, HWND hwnd);

    pulse::AppState& state_;
    TouchGestureReducer reducer_;
    TouchInertiaTracker inertia_;
    TouchScrollTarget scroll_target_;
    std::unordered_set<uint32_t> owned_touch_pointers_;
    std::unordered_set<uint32_t> fallback_pointers_;
    uint64_t last_tick_time_ms_ = 0;
};

} // namespace pulse::app
