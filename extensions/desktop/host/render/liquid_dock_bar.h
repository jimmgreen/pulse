// liquid_dock_bar.h - Reference-faithful macOS 26 Tahoe Liquid Glass Dock bar
// ported from reference demo/liquid_dock.cpp:
// - Bottom-center floating auto-width Liquid Glass capsule (dynamically expands with hover wave)
// - Pinned apps (non-destructive references; never hides/deletes desktop icons)
// - Live unpinned running taskbar applications (stable insertion order across Z-order shifts)
// - Translucent vertical glass divider + Downloads + Desktop Widgets toggle + live Recycle Bin
// - Raised-cosine fisheye magnification, dynamic contact shadows, glowing active/running dots
// - Damped 3-hop parabolic launch bounce & floating dark frosted HUD tooltip pill
// - High-res SHIL_JUMBO icon extraction with tight non-transparent bounding-box auto-crop
//   + custom Apple-style Direct2D vector squircles (Widgets, Settings, Terminal, Calculator)
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1_1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "host/live/dock_system_provider.h"
#include "host/model/desktop_model.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::desktop::render {

enum class DockSysSlotKind : uint8_t {
    None = 0,
    PinnedTrayApp = 1, // pinned tray icon inside .dk-sys
    TrayOverflow = 2,  // ⌃ tray overflow button
    Ime = 3,           // 中 / 英 IME toggle & layout picker
    Wifi = 4,          // Wi-Fi signal icon & panel
    Volume = 5,        // Volume icon (wheel ±2% + OSD; click panel)
    Bluetooth = 6,     // Bluetooth icon & panel
    Battery = 7,       // Battery icon & power mode (hidden on desktop PCs!)
    Clock = 8,         // HH:MM + M/D clock & calendar/notifications panel
};

struct DockSysSlotRect {
    DockSysSlotKind kind = DockSysSlotKind::None;
    int tray_index = -1; // index into sys_snap.tray_apps when kind == PinnedTrayApp
    D2D1_RECT_F rect{};  // desktop client DIP coordinates
    std::wstring tooltip;
};

enum class DockBarItemKind : uint8_t {
    App = 0,
    Folder = 1,
    WidgetsToggle = 2,
    Trash = 3,
    Divider = 4,
};

struct DockBarItem {
    DockBarItemKind kind = DockBarItemKind::App;
    std::wstring key;         // Normalized lowercase key (exe/path, "aumid:*", "ms-settings:", or "special:*")
    std::wstring launch_path; // Path or URI to launch (e.g. .exe, ms-settings:, shell:AppsFolder\<AUMID>)
    std::wstring aumid;       // Optional AppUserModelID for Store/MSIX/UWP matching and crisp icon loading
    std::wstring title;       // Friendly label shown in floating frosted tooltip
    bool pinned = false;
    bool running = false;
    bool active = false;
    int badge_count = 0;      // Parsed unread count from "(N)" window titles (WacOS dock.cpp)
    std::vector<HWND> windows;

    // Computed layout & physics state in desktop client DIP space
    float rest_cx = 0.0f;
    float rest_cy = 0.0f;
    float draw_cx = 0.0f;
    float draw_cy = 0.0f;
    float draw_size = 48.0f;
    float scale = 1.0f;
    float slot_factor = 1.0f; // 0..1 smooth slot expand/collapse transition (WacOS AppFactor)
    bool is_ghost = false;    // Transient shrinking ghost while slot closes after removal/exit
    double bounce_start_ms = -10000.0;
};

struct DockBarLeavingGhost {
    DockBarItem item{};
    size_t slot_pos = 0;
    double start_ms = -10000.0;
};

// Interactive drag-to-reorder / drag-out-to-unpin state (inspired by renzyxx/WacOS dock.cpp).
struct DockBarDragState {
    bool active = false;
    int source_index = -1;
    std::wstring source_key;
    float cursor_x = 0.0f;
    float cursor_y = 0.0f;
    bool drag_out = false;          // Pulled above the Dock -> release removes pin (non-destructive)
    int target_pinned_slot = -1;    // Live target slot index inside settings.dock_pinned_paths
};

// macOS-style multi-puff smoke burst when unpinning an icon by dragging out of the Dock.
struct DockBarPoof {
    float x = 0.0f;
    float y = 0.0f;
    double start_ms = -10000.0;
};

// Complete live state & computed geometry of the bottom-center macOS Liquid Glass Dock bar.
struct LiquidDockBarState {
    std::vector<DockBarItem> items;
    std::vector<std::wstring> unpinned_order; // Preserves stable slot order across focus Z-order changes
    std::wstring downloads_path;
    bool apps_stack_enabled = true; // 应用程序 stack item (settings.dock_apps_stack)
    bool trash_not_empty = false;
    DockBarPoof poof{};
    // Smooth icon enter/exit slot expand & collapse state (WacOS dock.cpp:98-119).
    bool built_once = false;
    std::vector<DockBarItem> prev_snapshot;
    std::unordered_map<std::wstring, double> item_appear_ms;
    std::vector<DockBarLeavingGhost> leaving_ghosts;

    // Live computed capsule geometry (desktop client DIP coordinates)
    D2D1_RECT_F pill_rect{};
    float pill_radius = 22.0f;
    float rest_pill_w = 0.0f;
    float rest_pill_h = 0.0f;
    float effective_base_icon = 48.0f;
    float reveal_progress = 1.0f; // 0=collapsed off edge, 1=fully expanded
    uint8_t dock_position = 0;    // 0=Bottom, 1=Left, 2=Right

    // Unified right/bottom-end .dk-sys system status & tray area (desktop_widgets_redesign.html)
    live::DockSystemSnapshot sys_snap{};
    bool sys_area_enabled = false;
    D2D1_RECT_F sys_divider_rect{};
    D2D1_RECT_F sys_capsule_rect{};
    std::vector<DockSysSlotRect> sys_slots;

    bool IsVertical() const noexcept { return dock_position == 1 || dock_position == 2; }
    bool HasItems() const noexcept { return !items.empty(); }
    bool HasActiveBounce(double now_ms) const noexcept;
    bool HasActivePoof(double now_ms) const noexcept;
    bool HasActiveSlotMotion(double now_ms) const noexcept;
    void TriggerPoof(float x_dip, float y_dip, double now_ms) noexcept;
    int FindItemIndexByKey(const std::wstring& key) const noexcept;
    int FindItemIndexForHwnd(HWND target) const noexcept;
    model::RectI RestingObstacleRect() const noexcept;
    model::RectI DamageRect(const model::RectI& work_area) const noexcept;

    // Live visibility check before bringing a running window to foreground (filters cloaked UWP hosts).
    static bool IsWindowLiveForActivation(HWND hwnd) noexcept;

    // Discovers default Windows apps matching liquid_dock.cpp (Explorer, Chrome/Edge, Terminal, Notepad, Calc, Settings).
    static std::vector<std::wstring> DiscoverDefaultPinnedApps();

    // Resolves a dropped shortcut (.lnk) or file path for non-destructive pinning to the Dock.
    static std::wstring NormalizePathForPin(const std::wstring& raw_path);
    static std::wstring FriendlyAppName(const std::wstring& path);

    // Rebuilds pinned + [divider] + live running taskbar apps + divider + Downloads + Widgets + Trash.
    // Returns true if visible item set or running/active/trash/badge state changed.
    // Pass `trash_override` (>= 0) when Recycle Bin state is refreshed asynchronously off the UI thread.
    bool RefreshSystemItems(std::vector<std::wstring>& pinned_paths,
                            DWORD own_pid,
                            int trash_override = -1,
                            double now_ms = 0.0);

    // Populates deterministic preview Dock items for headless visual snapshots.
    void BuildDemoItems(bool trash_full = true);
    void BuildDemoSystemSnapshot(bool has_battery = true, bool wired_ethernet = false);

    // Computes bottom-center resting & raised-cosine fisheye magnified geometry + dynamic pill width.
    // `reveal` in [0, 1] drives the macOS/WacOS vertical collapse/expand slide animation.
    // When `drag` is active, neighboring icons smoothly shift apart to open the live insertion slot.
    void ComputeGeometry(const model::RectI& work_area,
                         const model::BeautifySettings& settings,
                         float smooth_cursor_x,
                         float hover_presence,
                         double now_ms,
                         DockBarDragState* drag = nullptr,
                         float dt_sec = 0.016f,
                         float reveal = 1.0f) noexcept;

    // Hit-tests the Dock envelope, vertical resize dividers, individual icons, and .dk-sys slots.
    bool HitTestBar(float x_dip, float y_dip, bool currently_hovering) const noexcept;
    int HitTestDivider(float x_dip, float y_dip) const noexcept;
    int HitTestItem(float x_dip, float y_dip) const noexcept;
    const DockSysSlotRect* HitTestSysSlot(float x_dip, float y_dip) const noexcept;
    const DockSysSlotRect* FindSysSlot(DockSysSlotKind kind, int tray_index = -1) const noexcept;
    bool IsPointInSysArea(float x_dip, float y_dip) const noexcept;
};

// Dedicated high-res SHIL_JUMBO + WIC auto-cropped icon cache for Liquid Glass Dock items.
class DockBarIconCache {
public:
    DockBarIconCache() = default;
    ~DockBarIconCache() { Clear(); }
    DockBarIconCache(const DockBarIconCache&) = delete;
    DockBarIconCache& operator=(const DockBarIconCache&) = delete;

    void Clear() noexcept;
    ID2D1Bitmap* GetOrLoad(ID2D1DeviceContext* dc, const DockBarItem& item, bool trash_not_empty) noexcept;
    ID2D1Bitmap* GetOrLoadExeIcon(ID2D1DeviceContext* dc, const std::wstring& exe_path,
                                  HWND fallback_hwnd = nullptr) noexcept;

private:
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    ID2D1DeviceContext* bound_dc_ = nullptr;
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap>> cache_;
};

// Text callback routed through Pulse's LumaText engine (zero D2D DrawText fallback).
using DockBarTextFn = std::function<void(const std::wstring& text, const D2D1_RECT_F& rect,
                                         float font_size_dip, bool bold, uint32_t argb, float opacity)>;
using DockBarMeasureFn = std::function<float(const std::wstring& text, float font_size_dip)>;

// Draws the inner Dock scene (top specular sheen, glass divider, shadows, icons, running dots,
// unified .dk-sys system status capsule, drop highlight, and floating dark frosted tooltip pill).
// The caller draws the real Liquid Glass / material pill behind it at `dock.pill_rect` (`dock.pill_radius`).
void DrawLiquidDockBarContents(ID2D1Factory* factory,
                               ID2D1DeviceContext* dc,
                               DockBarIconCache& icon_cache,
                               const LiquidDockBarState& dock,
                               int pressed_index,
                               int tooltip_index,
                               float tooltip_alpha,
                               int drop_target_mode, // 0=none, 1=pin-to-dock, 2=trash
                               bool dark_ink,
                               float canvas_w_dip,
                               const DockBarMeasureFn& measure_text,
                               const DockBarTextFn& draw_text,
                               const DockBarDragState* drag = nullptr,
                               double now_ms = 0.0,
                               float canvas_h_dip = 1080.0f,
                               DockSysSlotKind hovered_sys_kind = DockSysSlotKind::None,
                               int hovered_sys_tray_idx = -1,
                               DockSysSlotKind open_sys_kind = DockSysSlotKind::None,
                               int open_sys_tray_idx = -1,
                               uint8_t battery_low_pct = 20) noexcept;

// Draws custom Apple-style squircle icons (reused by Widget Gallery preview as well).
void DrawDockWidgetsVectorIcon(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) noexcept;
void DrawDockSettingsVectorIcon(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) noexcept;
void DrawDockTerminalVectorIcon(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) noexcept;
void DrawDockCalculatorVectorIcon(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) noexcept;
// Fallback icons (used when the shell cannot supply a bitmap) and the Apps stack tile.
void DrawDockAppsVectorIcon(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) noexcept;
void DrawDockTrashVectorIcon(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, bool full) noexcept;
void DrawDockFolderVectorIcon(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, bool downloads_glyph) noexcept;
void DrawDockExplorerVectorIcon(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) noexcept;
void DrawDockGenericAppVectorIcon(ID2D1DeviceContext* dc,
                                  const D2D1_RECT_F& r,
                                  const std::wstring& title,
                                  const DockBarTextFn& draw_text) noexcept;

} // namespace pulse::desktop::render
