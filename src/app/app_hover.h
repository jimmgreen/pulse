#pragma once
#include <windows.h>

namespace pulse {
struct AppState;
namespace ui { struct HitTestResult; }

struct HoverPointerApi {
    BOOL (WINAPI* cursor_position)(LPPOINT) = GetCursorPos;
    HWND (WINAPI* window_at_point)(POINT) = WindowFromPoint;
    BOOL (WINAPI* to_client)(HWND, LPPOINT) = ScreenToClient;
    HWND (WINAPI* capture)() = GetCapture;
};

// Applies hover only: never selects an item or changes keyboard/pane focus.
bool UpdatePointerHover(AppState& s, const ui::HitTestResult& hit, POINT point);
// Re-hit-test the displayed layout, not the animation's final scroll target.
bool RefreshScrolledHover(AppState& s, const HoverPointerApi& api = {});
} // namespace pulse
