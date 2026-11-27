// dock_tile.h - macOS 26 Tahoe Liquid Glass Dock layout, fisheye magnification,
// 3-hop parabolic launch bounce, floating frosted tooltip pill, and live running-app
// indicator helpers for Pulse's TileKind::Apps ("应用坞") tile.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1_1.h>

#include "host/model/desktop_model.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace pulse::desktop::render {

// Per-icon computed geometry inside a Dock tile (both resting and magnified positions).
struct DockSlotMetrics {
    size_t app_index = 0;
    int row = 0;
    int col = 0;
    float rest_cx = 0.0f;
    float rest_cy = 0.0f;
    float draw_cx = 0.0f;
    float draw_cy = 0.0f;
    float base_size = 44.0f;
    float draw_size = 44.0f;
    float scale = 1.0f;
    float lift_y = 0.0f;       // positive upward elevation (magnification lift + bounce)
    float dot_y = 0.0f;        // running-app indicator center Y
    D2D1_RECT_F label_rect{};  // non-empty in multi-row shelf mode
};

struct DockTileLayout {
    bool single_row = true;
    int cols = 1;
    int rows = 1;
    size_t room = 1;
    int pages = 1;
    int active_page = 0;
    size_t first_index = 0;
    float base_icon = 44.0f;
    float corner_radius = 24.0f;
    float pager_dot_y = 0.0f;
    std::vector<DockSlotMetrics> slots;
};

struct DockLayoutInput {
    float hover_presence = 0.0f;   // 0..1 smooth fisheye envelope
    float smooth_cursor_x = 0.0f;  // DIP X in desktop client space
    float cursor_y = 0.0f;         // DIP Y in desktop client space
    int apps_page = 0;
    int pressed_app_index = -1;
    int bouncing_app_index = -1;
    float bounce_offset_dip = 0.0f;
};

struct RunningAppSnapshot {
    std::unordered_set<std::wstring> running_exes; // lowercase exe basenames
    std::wstring active_exe;                       // foreground window lowercase exe basename
};

// Returns the corner radius for any tile: sleek capsule radius for Apps Dock, 16 DIP otherwise.
float TileCornerRadiusDip(const model::DesktopTile& tile, const D2D1_RECT_F& rect) noexcept;

// Evaluates damped 3-hop parabolic launch bounce offset in DIP (0 when inactive/complete).
float EvaluateDockBounceDip(double bounce_start_ms, double now_ms) noexcept;
bool IsDockBounceActive(double bounce_start_ms, double now_ms) noexcept;

// Computes Dock paging, resting coordinates, and raised-cosine fisheye magnification layout.
DockTileLayout ComputeDockTileLayout(const model::DesktopTile& tile,
                                     const D2D1_RECT_F& rect,
                                     size_t app_count,
                                     const DockLayoutInput& input) noexcept;

// Pixel-exact hit test against the live magnified Dock icon positions; returns app index or -1.
int HitTestDockTileApp(const model::DesktopTile& tile,
                       const D2D1_RECT_F& rect,
                       size_t app_count,
                       const DockLayoutInput& input,
                       float px, float py) noexcept;

// Enumerates real visible taskbar application windows to drive Dock running & active dots.
RunningAppSnapshot QueryRunningAppSnapshot() noexcept;

// Resolves a desktop shortcut (.lnk) or executable path to its lowercase exe basename.
std::wstring ResolveShortcutExeBasename(const std::wstring& path) noexcept;

// Returns the left/right subtle pager chevron hit rect for an overflowing single-row Dock.
D2D1_RECT_F DockPagerArrowRect(const D2D1_RECT_F& rect, bool right) noexcept;

// Dock visual primitives (matching liquid_dock.cpp).
void DrawDockTopSheen(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect,
                      float corner_radius, bool dark_ink) noexcept;

void DrawDockPagerChevron(ID2D1DeviceContext* dc, const D2D1_RECT_F& btn_rect,
                          bool right, bool enabled, bool dark_ink) noexcept;

void DrawDockContactShadow(ID2D1DeviceContext* dc, float cx, float shadow_y,
                           float icon_size, float elevation_dip) noexcept;

void DrawDockRunningDot(ID2D1DeviceContext* dc, float cx, float dot_y,
                        bool running, bool active, bool dark_ink) noexcept;

using DockTooltipTextFn = std::function<void(const std::wstring& text, const D2D1_RECT_F& rect,
                                             uint32_t argb, float opacity)>;

void DrawDockTooltipPill(ID2D1Factory* factory, ID2D1DeviceContext* dc,
                         const std::wstring& label,
                         float anchor_cx, float icon_top_y,
                         float clamp_left, float clamp_right,
                         float text_width_dip, float alpha,
                         const DockTooltipTextFn& draw_text) noexcept;

} // namespace pulse::desktop::render
