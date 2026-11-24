// desktop_renderer.h - Hardware D3D11 + Direct2D 1.1 + DirectComposition desktop surface
// renderer with 1/4-res wallpaper UV material sampling, 1:1 HTML tile/group/icon fidelity,
// dirty-rect Present1, offscreen PNG snapshot readback, and 0-Present idle sleep.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dcomp.h>
#include <wrl/client.h>

#include "host/desktop_mount.h"
#include "host/model/desktop_model.h"
#include "host/render/wallpaper_backdrop.h"
#include "host/render/host_backdrop_layer.h"
#include "host/render/desktop_text_renderer.h"
#include "host/render/liquid_glass_effect.h"
#include "host/render/live_wallpaper_source.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse::desktop::render {

struct TrayViewEntry {
    uint64_t item_id = 0;
    uint64_t size_bytes = 0;
    bool is_folder = false;
    bool move_intent = false;
    std::wstring path;
    std::wstring name;
};

struct LiveTileRuntimeData {
    std::vector<std::wstring> photo_paths;
    size_t active_photo_index = 0;
    size_t prev_photo_index = 0;
    float photo_crossfade = 1.0f;
    size_t photo_total_count = 128;

    std::vector<mount::MiniFolderItem> download_items;
    std::vector<mount::MiniFolderItem> recent_items;
    std::unordered_map<std::wstring, std::vector<mount::MiniFolderItem>> pinned_folder_items;
    mount::RecycleBinStats recycle_bin{};
    std::vector<mount::DriveSpaceStats> drives;

    std::vector<TrayViewEntry> tray_entries;
    uint64_t tray_total_bytes = 0;

    // 天气 (Open-Meteo, the city from settings). WMO weather codes.
    struct WeatherHour { std::wstring label; int code = 0; int temp_c = 0; };
    struct WeatherDay { std::wstring label; int code = 0; int lo_c = 0; int hi_c = 0; };
    struct WeatherView {
        bool valid = false;
        std::wstring city;
        std::wstring status;           // why there is no data ("在设置里选一个城市", "离线")
        int temp_c = 0, code = 0, hi_c = 0, lo_c = 0;
        int feels_c = 0, humidity_pct = 0, wind_kmh = 0;
        int utc_offset_s = 0;
        bool has_utc_offset = false;
        std::wstring updated;          // "15:42": when this forecast arrived
        std::vector<WeatherHour> hours;
        std::vector<WeatherDay> days;
    } weather;
    // 系统
    struct SystemView {
        bool valid = false;
        int cpu_pct = 0;
        uint64_t mem_used = 0, mem_total = 0;
        int disk_used_pct = 0;
        uint64_t disk_free = 0;
        uint64_t net_down_bps = 0;
    } system;
};

enum class GroupHeaderHitZone : uint8_t {
    None = 0,
    CollapseChevron = 1,
    SortButton = 2,
    RuleButton = 3,
    MenuButton = 4,
    FooterViewAll = 5,
    ResizeGrip = 6,
    CleanChip = 7,
    FooterPrevPage = 8,
    FooterNextPage = 9,
    FooterPagerDots = 10,
    MapSingleFolderChip = 11,
    Tab = 12,
    TabPrev = 13,
    TabNext = 14,
};

struct GroupTabItem { std::string group_id; model::RectI rect; bool active = false; };
struct GroupTabLayout {
    std::vector<GroupTabItem> tabs;
    model::RectI previous, next;
    std::string previous_group_id, next_group_id;
};
GroupTabLayout ComputeGroupTabLayout(const model::GroupComputedView& view);
std::string HitTestGroupTab(const model::GroupComputedView& view, int x, int y);

enum class TileResizeZone : uint8_t {
    None = 0,
    Right = 1,
    Bottom = 2,
    BottomRight = 3,
};

struct DesktopInteractionState {
    std::unordered_set<std::wstring> selected_paths;
    std::wstring focused_path;
    std::wstring hovered_path;

    std::string hovered_group_id;
    GroupHeaderHitZone hovered_group_zone = GroupHeaderHitZone::None;
    int hovered_group_dot = -1;
    std::unordered_map<std::string, int> group_pages; // 0-based active page per overflowing group card
    std::string hovered_tile_id;
    // The card under the pointer while the button is down sinks a little (1 = fully pressed; below 0 is the
    // spring's overshoot on release, which grows it a hair past rest), as a phone's widgets do.
    std::string pressed_tile_id;
    float tile_press = 0.0f;
    // An app pressed inside the 应用 tile (its index among the tile's apps): only that icon reacts.
    int pressed_app_index = -1;

    std::string drop_target_group_id;
    std::string drop_target_tile_id;
    std::string stack_drop_target_tile_id;   // a same-size tile under a dragged tile (edit mode): 叠放
    bool tile_edit_mode = false;             // 编辑磁贴区: tiles jiggle, remove badges, + cells
    float edit_phase = 0.0f;                 // 0..1 jiggle phase, advanced by the host while editing
    int hovered_todo_index = -1;             // a to-do row under the pointer
    int apps_page = 0;                       // the 应用 tile's page (the wheel turns it)
    int hovered_tile_row = -1;               // hovered item row inside Downloads / Recent / PinnedFolder / StagingTray
    bool hovered_tile_action = false;        // hovered inline action (e.g. "+ 添加待办…")
    bool hovered_tile_edit_btn = false;      // hovered top-right [✏] button on tile (.tedit)
    TileResizeZone hovered_tile_resize = TileResizeZone::None;
    std::string resizing_tile_id;
    int hovered_tile_header_btn = 0;         // 0=none, 1=[+ 添加小组件], 2=[完成] in tile_edit_mode header

    bool marquee_active = false;
    model::RectI marquee_rect{};

    bool dragging_tile = false;
    std::string dragging_tile_id;
    model::RectI drag_tile_preview_rect{};
    bool drag_tile_valid = true;

    bool dragging_items = false;
    model::PointI drag_cursor_pos{0, 0};
    size_t drag_item_count = 0;
    std::wstring drag_primary_ext;
    bool drag_primary_folder = false;

    std::wstring search_highlight_query;

    bool drop_hint_visible = false;
    model::PointI drop_hint_pos{0, 0};
    std::wstring drop_hint_label;

    bool side_drawer_open = false;          // hides #tiles when Health/Beautify drawer is open
    bool banner_suppressed = false;         // hides bottom-left #banner when Search popup is open
    bool force_opaque_wallpaper = false;    // draws full-res wallpaper on DComp surface (presets / snapshots)
    uint64_t cleanable_installer_bytes = 0; // drives [可清理 XX MB] chip on installer group

    bool host_dark_mode = true;
    model::CardMaterial host_material = model::CardMaterial::Acrylic;
    bool transparency_enabled = true;
    uint32_t host_accent_argb = 0xFF4CC2FFu;
    model::PointI cursor_pos{-1, -1};
    bool cursor_inside = false;
};

struct RenderMetrics {
    uint64_t total_presents = 0;
    uint64_t dirty_presents = 0;
    uint64_t glass_only_presents = 0;
    uint64_t idle_ticks_skipped = 0;
    double last_draw_ms = 0.0;
};

// Shared 160x160 (100 KB) 9-slice separable-Gaussian Windows 11 card shadow atlas.
// Interior pixels inside the rounded card boundary have strictly zero alpha so live
// DWM HostBackdropBrush Acrylic underneath transparent cards is never darkened.
Microsoft::WRL::ComPtr<ID2D1Bitmap1> CreateExteriorCardShadowBitmap(ID2D1DeviceContext* dc);
void DrawExteriorCardShadow(ID2D1DeviceContext* dc, ID2D1Bitmap1* shadow_bitmap,
                            const D2D1_RECT_F& rect, float radius, float opacity);

// Shared 1:1 vector file/folder icon renderer (used by desktop grid, tile rows, search, drawers, QuickLook).
void DrawVectorFileIcon(ID2D1Factory* factory, ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                        const D2D1_RECT_F& icon_rect, std::wstring_view ext, bool is_folder,
                        float opacity = 1.0f);

class DesktopRenderer {
    friend struct DesktopRendererTestAccess;
public:
    DesktopRenderer() = default;
    ~DesktopRenderer();
    DesktopRenderer(const DesktopRenderer&) = delete;
    DesktopRenderer& operator=(const DesktopRenderer&) = delete;

    // Initializes hardware D3D11 + D2D1.1 + DirectComposition (with automatic HWND swapchain fallback).
    bool InitializeForHwnd(HWND hwnd, uint32_t width, uint32_t height, bool prefer_dcomp = true,
                           bool load_default_wallpaper = true);

    // Initializes hardware D3D11 + D2D1.1 composition swapchain without requiring an explorer-attached HWND.
    bool InitializeOffscreen(uint32_t width, uint32_t height);

    void Shutdown() noexcept;
    bool Resize(uint32_t width, uint32_t height);
    void TrimDxgiMemory() noexcept;

    bool ReloadWallpaper(model::WallpaperPreset preset, bool dark_mode, const std::wstring& override_path = {});
    bool ReloadWallpaper(const std::wstring& override_path = {}) {
        return ReloadWallpaper(model::WallpaperPreset::System, true, override_path);
    }

    void InvalidateAll() noexcept;
    void InvalidateRect(const model::RectI& rc) noexcept;
    void InvalidateGlassOnly() noexcept { glass_only_dirty_ = true; }
    void SetAnimating(bool animating) noexcept { animating_ = animating; }
    bool IsAnimating() const noexcept { return animating_; }
    bool HasDirtyWork() const noexcept {
        return full_dirty_ || !dirty_rects_.empty() || animating_ ||
               glass_only_dirty_ || !glass_dirty_rects_.empty() || hover_light_.animating;
    }

    // Configures live dynamic-wallpaper frame wake message (`WM_APP + ...`) and updates detected target.
    void ConfigureLiveWallpaperNotification(HWND notify_hwnd, UINT frame_msg) noexcept;
    void SetLiveWallpaperDecision(const host::LiveWallpaperDecision& decision) noexcept;
    bool OnLiveWallpaperFrameNotification(bool paused = false) noexcept;
    LiveGlassSourceStats LiveGlassStats() const noexcept { return live_wallpaper_.Stats(); }

    // Captures a 1/2-resolution composite snapshot of `(wallpaper + desktop cards)` under `anchor_screen_px`
    // into `backdrop_.SetMenuUnderlaySnapshot(...)` so Liquid Glass menus refract underlying cards (G10).
    void CaptureMenuUnderlay(POINT anchor_screen_px,
                             const model::DesktopModelState& state,
                             std::span<const model::GroupComputedView> group_views,
                             const model::RectI& tile_board_rect,
                             const LiveTileRuntimeData& live_data,
                             const DesktopInteractionState& interaction);

    static bool ShouldPauseLiveActivity(bool enabled = true) noexcept;

    // Inline geometry helpers usable without linking pulse_desktop_model.
    static model::RectI ComputeBoardRectFallback(const model::RectI& work_area, bool tiles_enabled) noexcept;
    static model::RectI ComputeTileRectInline(const model::RectI& board_rect, const model::DesktopTile& tile) noexcept;
    static std::vector<model::GroupComputedView> BuildSimpleGroupViews(const model::DesktopModelState& state);

    // Hit-tests group header chips, action circles, overflow footer, and bottom-right resize grip.
    GroupHeaderHitZone HitTestGroupCard(const model::DesktopGroup& group,
                                        const model::GroupComputedView& view,
                                        bool focus_scene,
                                        uint64_t cleanable_installer_bytes,
                                        bool group_hovered,
                                        int px, int py) const noexcept;
    int HitTestGroupFooterDot(const model::GroupComputedView& view, int px, int py) const noexcept;
    static TileResizeZone HitTestTileResize(const model::DesktopTile& tile,
                                            const model::RectI& tile_rect,
                                            int px, int py) noexcept;

    // Renders dirty regions or returns false immediately with ZERO BeginDraw / ZERO Present1 when idle!
    bool Tick(const model::DesktopModelState& state,
              std::span<const model::GroupComputedView> group_views,
              const model::RectI& tile_board_rect,
              const LiveTileRuntimeData& live_data,
              const DesktopInteractionState& interaction);

    // Renders desktop + optional composited overlay onto the swapchain backbuffer and saves a PNG via WIC.
    bool RenderCompositePng(const std::wstring& png_path,
                            const model::DesktopModelState& state,
                            std::span<const model::GroupComputedView> group_views,
                            const model::RectI& tile_board_rect,
                            const LiveTileRuntimeData& live_data,
                            const DesktopInteractionState& interaction,
                            const std::function<void(ID2D1DeviceContext*)>& post_overlay_draw = {});

    uint32_t Width() const noexcept { return width_; }
    const DesktopTextCacheStats& WidgetTextCacheStats() const noexcept { return widget_text_.CacheStats(); }
    const ui::LumaTextStats& LabelTextStats() const noexcept { return widget_text_.Stats(); }
    uint32_t Height() const noexcept { return height_; }
    bool UsingComposition() const noexcept { return using_dcomp_; }
    bool UsingVirtualSurface() const noexcept { return vsurf_ != nullptr; }
    // The layer's DPI and where it sits on its monitor (pixels); the scene itself is laid out in DIPs.
    void SetSurfaceGeometry(uint32_t dpi, const model::RectI& monitor_px, const model::RectI& surface_px);
    ID2D1DeviceContext* DeviceContext() const noexcept { return d2d_dc_.Get(); }
    bool NativeAcrylicActive() const noexcept { return host_backdrop_.Active(); }
    uint32_t NativeAcrylicVisualCount() const noexcept { return host_backdrop_.VisualCount(); }
    uint64_t NativeAcrylicUpdateCount() const noexcept { return host_backdrop_.UpdateCount(); }
    ID2D1Factory1* D2dFactory() const noexcept { return d2d_factory_.Get(); }
    IDWriteFactory* DWriteFactory() const noexcept { return dwrite_factory_.Get(); }
    WallpaperBackdrop& Backdrop() noexcept { return backdrop_; }
    const WallpaperBackdrop& Backdrop() const noexcept { return backdrop_; }
    const RenderMetrics& Metrics() const noexcept { return metrics_; }
    void ResetMetrics() noexcept { metrics_ = {}; }
    model::RectI LastBannerActionRect() const noexcept { return last_banner_action_rect_; }

private:
    friend struct DesktopContrastTestPeer;
    bool CreateCoreDevices();
    bool CreateSwapChainResources(HWND hwnd, uint32_t width, uint32_t height, bool prefer_dcomp);
    bool BindSwapChainTargetBitmap();
    bool CreateVirtualSurfaceResources(HWND hwnd, uint32_t width, uint32_t height);
    bool TickVirtual(const model::DesktopModelState& state,
                     std::span<const model::GroupComputedView> group_views,
                     const model::RectI& tile_board_rect,
                     const LiveTileRuntimeData& live_data,
                     const DesktopInteractionState& interaction);
    std::vector<RECT> CollectOccupiedRects(const model::DesktopModelState& state,
                                           std::span<const model::GroupComputedView> group_views,
                                           const model::RectI& tile_board_rect,
                                           const DesktopInteractionState& interaction) const;
    RECT DipRectToPx(const model::RectI& rc, int margin_dip) const noexcept;
    bool IntersectsActiveClip(const model::RectI& rc, int margin_dip = 20) const noexcept;
    uint32_t BackdropWidth() const noexcept;
    uint32_t BackdropHeight() const noexcept;
    void ApplyBackdropMapping() noexcept;
    bool CreateTextFormats();
    float MeasureSingleLineWidth(const std::wstring& text, IDWriteTextFormat* fmt, float max_w = 600.0f) const;

    model::CardMaterial ResolveMaterial(model::CardMaterial requested,
                                        const model::BeautifySettings& settings,
                                        const DesktopInteractionState& interaction) const noexcept;
    uint32_t ResolveAccentArgb(uint32_t item_accent,
                               const model::BeautifySettings& settings,
                               const DesktopInteractionState& interaction) const noexcept;

    void DrawScene(const model::DesktopModelState& state,
                   std::span<const model::GroupComputedView> group_views,
                   const model::RectI& tile_board_rect,
                   const LiveTileRuntimeData& live_data,
                   const DesktopInteractionState& interaction);

    void DrawCardMaterial(const D2D1_RECT_F& rect, float radius,
                          model::CardMaterial material,
                          uint8_t opacity_percent,
                          bool dark_mode,
                          uint32_t accent_argb,
                          bool emphasized_border,
                          bool elevated_hover = false, float background_opacity = 1.0f,
                          uint8_t dispersion_level = 1,
                          float press_amount = 0.0f,
                          bool allow_lower_glass = false,
                          bool is_group_card = false,
                          std::string_view card_id = {},
                          const model::BeautifySettings* glass_settings = nullptr);

    void DrawGroupCard(const model::DesktopGroup& group,
                       const model::GroupComputedView& view,
                       const model::DesktopModelState& state,
                       const LiveTileRuntimeData& live_data,
                       const DesktopInteractionState& interaction,
                       float opacity_scale);

    void DrawIconCell(const D2D1_RECT_F& cell_rect,
                      const model::DesktopItem& item,
                      bool in_group,
                      bool is_staged,
                      model::DesktopIconSize icon_size,
                      model::IconLabelStyle label_style,
                      bool dark_mode,
                      uint32_t accent_argb,
                      bool selected,
                      bool hovered,
                      bool search_matched,
                      bool search_dimmed,
                      const model::BeautifySettings* surface_settings = nullptr,
                      model::CardMaterial surface_material = model::CardMaterial::Solid);

    void DrawLooseIcons(const model::DesktopModelState& state,
                        std::span<const model::GroupComputedView> group_views,
                        const LiveTileRuntimeData& live_data,
                        const DesktopInteractionState& interaction);

    void DrawIconLabel(std::wstring_view name, const D2D1_RECT_F& rect,
                       IDWriteTextFormat* format, model::IconLabelStyle style,
                       bool dark, bool card_backed, bool revealed, float alpha = 1.0f,
                       const text_contrast::Samples* background = nullptr);
    void DrawWidgetText(const wchar_t* text, UINT32 length, IDWriteTextFormat* format,
                        const D2D1_RECT_F& bounds, ID2D1SolidColorBrush* brush,
                        D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_NONE);
    void DrawLabelLayout(D2D1_POINT_2F origin, IDWriteTextLayout* layout,
                         uint32_t color, float opacity);
    void DrawGroupRow(const D2D1_RECT_F& rect, const model::DesktopItem& item,
                      model::GroupViewMode mode, bool dark, uint32_t accent,
                      bool selected, bool hovered, bool matched, bool dimmed);
    void DrawEllipsized(std::wstring_view text, const D2D1_RECT_F& rect,
                        IDWriteTextFormat* format, uint32_t color, float opacity = 1.0f);

    text_contrast::Samples CardBackgroundSamples(const D2D1_RECT_F& rect, model::CardMaterial material,
                                                uint8_t opacity_percent, bool theme_dark) const;
    text_contrast::Samples TileBackgroundSamples(const model::DesktopTile& tile, const D2D1_RECT_F& rect,
                                                const D2D1_RECT_F& text_rect, model::CardMaterial material,
                                                const model::BeautifySettings& settings) const;

    // True when text on a card at `rect` should be light: the wallpaper under it, through the card's material.
    bool CardInkLight(const D2D1_RECT_F& rect, model::CardMaterial material, uint8_t opacity_percent, bool theme_dark) const;
    void DrawHeaderBand(const D2D1_RECT_F& card, float band_bottom, float radius, bool dark,
                        uint8_t opacity_percent, uint32_t band_override, uint32_t line_override,
                        bool with_grain = false, bool liquid_glass_minimal = false);
    void DrawWeatherIcon(const D2D1_RECT_F& r, int code, bool dark);
    void DrawTileEditDecor(const model::DesktopTile& tile, const D2D1_RECT_F& r, bool dark, bool hovered,
                           bool resizing, uint32_t accent_argb);
public:
    // Hit areas of the edit-mode and hover decorations, in DIPs (the host tests them).
    static D2D1_RECT_F TileRemoveBadgeRect(const D2D1_RECT_F& tile_rect) noexcept;
    static D2D1_RECT_F TileSizePillRect(const D2D1_RECT_F& tile_rect, bool pixel_mode = false) noexcept;
    // The 应用 tile's grid: columns, cells per page and page count for `count` shortcuts (the host hit-tests with it).
    struct AppsPaging { int cols = 5; int rows = 2; size_t room = 10; int pages = 1; float top = 0.0f; float cell_h = 74.0f; };
    static AppsPaging AppsTilePaging(const model::DesktopTile& tile, const D2D1_RECT_F& rect, size_t count) noexcept;
    // A stack's page dots on the card's right edge (the active one is a longer pill), and the arrows shown above
    // and below them on hover. The rects are what the host clicks against too.
    static D2D1_RECT_F StackDotRect(const D2D1_RECT_F& card, size_t count, int active, size_t index) noexcept;
    static D2D1_RECT_F StackArrowRect(const D2D1_RECT_F& card, size_t count, bool up) noexcept;
    // The 照片 card's hover buttons for the previous / next picture (clear of a stack's dots on the right).
    static D2D1_RECT_F PhotoNavRect(const model::DesktopTile& tile, const D2D1_RECT_F& card, bool next) noexcept;
    // Row `index` of a 下载 / 最近使用 list (empty past the card's bottom), and its hover button
    // (在文件夹中显示) at the row's right end. Drawing and the host's clicks share these.
    static D2D1_RECT_F ListRowRect(model::TileKind kind, const D2D1_RECT_F& card, size_t index, bool show_title = true) noexcept;
    static D2D1_RECT_F ListRowActionRect(const D2D1_RECT_F& row) noexcept;
    // The × at the right of a hovered 待办 row.
    static D2D1_RECT_F TodoDeleteRect(const model::DesktopTile& tile, const D2D1_RECT_F& card, size_t index) noexcept;
    // 回收站's hover 清空 button (in place of its name).
    static D2D1_RECT_F RecycleEmptyRect(const D2D1_RECT_F& card) noexcept;
    // 暂存区 entry `index` of a non-empty tray drawn at rect: a chip strip on one row, an icon grid with names on
    // taller tiles. Empty when it does not fit. The host's hit test uses the same rects.
    static D2D1_RECT_F TrayChipRect(const model::DesktopTile& tile, const D2D1_RECT_F& rect, size_t index) noexcept;
    static size_t TrayChipCapacity(const model::DesktopTile& tile, const D2D1_RECT_F& rect) noexcept;
    static D2D1_RECT_F TileEditButtonRect(const D2D1_RECT_F& tile_rect, bool small_tile) noexcept;
    static model::RectI TileHeaderAddWidgetRect(const model::RectI& board_rect) noexcept;
    static model::RectI TileHeaderDoneRect(const model::RectI& board_rect) noexcept;
private:
    void DrawTileBoardHeader(const model::RectI& board_rect, bool dark_mode, uint32_t accent_argb,
                             bool edit_mode, bool show_idle_caption, int hovered_header_btn, float opacity);

    void DrawLiveTile(const model::DesktopTile& tile,
                      const D2D1_RECT_F& rect,
                      const model::DesktopModelState& state,
                      const LiveTileRuntimeData& live_data,
                      const DesktopInteractionState& interaction);

    void DrawSceneBanner(const model::DesktopModelState& state,
                         const DesktopInteractionState& interaction);

    ID2D1SolidColorBrush* Brush(uint32_t argb, float opacity_mul = 1.0f);

    struct PendingGlassCard {
        std::string id;
        bool is_group = false;
        GlassCardRequest req{};
        float shadow_strength = 0.0f;
        std::array<GlassNeighborDip, 4> neighbor_buf{};
        uint8_t neighbor_cnt = 0;
    };

    bool EnsureGlassVirtualSurface();
    bool DrawSingleLiquidGlassCard(ID2D1DeviceContext* dc, const GlassCardRequest& req, float shadow_strength);
    bool SyncGlassVirtualSurface(bool force_all, const std::vector<RECT>& content_regions_px);
    void UpdateHoverLight(const model::DesktopModelState& state,
                          std::span<const model::GroupComputedView> group_views,
                          const model::RectI& tile_board_rect,
                          const DesktopInteractionState& interaction) noexcept;

    HWND hwnd_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool using_dcomp_ = false;
    bool has_presented_full_frame_ = false;
    bool full_dirty_ = true;
    bool animating_ = false;
    bool glass_only_dirty_ = false;
    bool force_inline_glass_ = false;
    bool energy_saver_cached_ = false;
    uint64_t energy_saver_check_ms_ = 0;
    uint64_t last_glass_used_ms_ = 0;
    uint64_t last_hover_step_ms_ = 0;
    std::string hover_light_card_id_;
    D2D1_RECT_F hover_light_card_rect_{};
    HoverLightTracker hover_light_{};
    std::vector<RECT> dirty_rects_;
    std::vector<RECT> glass_dirty_rects_;
    std::vector<RECT> last_trimmed_occupied_;
    std::vector<RECT> last_glass_trimmed_occupied_;
    std::vector<PendingGlassCard> pending_glass_cards_;
    std::vector<PendingGlassCard> committed_glass_cards_;
    D2D1_RECT_F active_clip_dip_{0.0f, 0.0f, 1e6f, 1e6f};
    model::RectI last_banner_action_rect_{};

    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgi_device_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> shared_dcomp_;
    HostBackdropLayer host_backdrop_;
    LiquidGlassPassPool glass_pool_;
    LiveWallpaperSource live_wallpaper_;
    host::LiveWallpaperDecision live_decision_{};
    bool native_acrylic_allowed_ = false;
    model::BeautifySettings current_glass_settings_{};
    uint8_t current_glass_live_rate_ = 1;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2d_dc_;
    DesktopTextRenderer widget_text_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_bitmap_;

    Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_device_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcomp_target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> dcomp_root_visual_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> glass_visual_;
    Microsoft::WRL::ComPtr<IDCompositionVirtualSurface> glass_vsurf_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> dcomp_visual_;
    Microsoft::WRL::ComPtr<IDCompositionVirtualSurface> vsurf_;
    uint32_t dpi_ = 96;
    float dip_scale_ = 1.0f;
    model::RectI monitor_px_{};
    model::RectI surface_px_{};

    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_group_title_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_tile_hdr_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_tile_sub_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_row_name_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_row_lead_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_row_meta_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_item_label_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_caption_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_pill_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_tile_small_title_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_disk_pct_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_clock_big_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_note_body_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_empty_hint_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_banner_text_;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_fluent_12_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_fluent_14_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_fluent_21_;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solid_brush_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> card_shadow_bitmap_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> border_grad_brush_;
    uint64_t border_grad_key_ = 0;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dashed_stroke_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> round_stroke_;

    WallpaperBackdrop backdrop_;
    RenderMetrics metrics_{};
};

} // namespace pulse::desktop::render
