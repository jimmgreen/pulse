// desktop_overlays.h - On-demand premultiplied-alpha DirectComposition WS_POPUP overlay controller
// (0 MB idle swapchain!) hosting 1:1 4-state context menus + cascading #dsub submenu, Type-to-Search,
// Space Quick Look, Health/Beautify drawers, Tray/Rule/StickyNote popovers, F2 EDIT rename, & toasts.
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

#include "host/ai/desktop_ai_organizer.h"
#include "host/model/desktop_model.h"
#include "host/overlays/desktop_menu_policy.h"
#include "host/render/desktop_renderer.h"
#include "ipc/desktop_ipc.h"
#include "../../../../src/ui/fluent_menu.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace pulse::desktop::overlays {

enum class OverlayKind : uint8_t {
    None = 0,
    ContextMenu = 1,
    SearchPopup = 2,
    QuickLook = 3,
    HealthDrawer = 4,
    BeautifyDrawer = 5,
    StagingTrayPopover = 6,
    GroupRulePopover = 7,
    ToastCapsule = 8,
    StickyNotePopover = 9,
    AiOrganizeDrawer = 10,
    WidgetGalleryDrawer = 11,
    InlineRename = 12,
    WidgetDetail = 13,   // the card a 天气 / 日历 / 系统 tile opens (widget_detail.cpp)
};

// What a widget detail card shows: a snapshot the host refreshes while it is open.
struct WidgetDetailData {
    model::TileKind kind = model::TileKind::Weather;
    render::LiveTileRuntimeData::WeatherView weather;
    render::LiveTileRuntimeData::SystemView system;
    std::vector<model::TodoItem> todos;
    std::wstring weather_city;
    std::vector<mount::DriveSpaceStats> drives;   // 磁盘
    bool analog_clock = true;                     // 时钟: the tile's face (the card's switch names the other)
};

enum class AiDrawerPhase : uint8_t {
    UnconfiguredGuide = 0,
    Loading = 1,
    PlanReady = 2,
    Completed = 3,
};

struct AiDrawerViewState {
    AiDrawerPhase phase = AiDrawerPhase::UnconfiguredGuide;
    ai::AiConnectionInfo connection{};
    ai::AiOrganizeStrategy strategy = ai::AiOrganizeStrategy::ArchiveClutter;
    std::wstring custom_instruction;
    std::string scope_group_id;
    std::wstring scope_group_title;
    std::wstring error_banner;
    ai::AiOrganizePlan plan{};
    bool pin_created_folders = true;
    size_t completed_moved_count = 0;
    size_t completed_folder_count = 0;
};

enum class ContextMenuTargetKind : uint8_t {
    BlankDesktop = 0,
    FileIcon = 1,
    GroupHeader = 2,
    LiveTile = 3,
    AppTileShortcut = 4,
};

struct ColorTagOption {
    std::wstring id;
    std::wstring name;
    uint32_t argb = 0;
};

struct ShellVerbEntry {
    uint32_t item_id = 0;
    bool enabled = true;
    bool separator_after = false;
    bool has_children = false;   // submenu header (not invokable)
    uint32_t depth = 0;          // preorder tree depth; 0 = top level
    std::wstring handler;        // registry handler name ("New" = Explorer's 新建)
    std::wstring verb;
    std::wstring label;
};

struct SearchHitEntry {
    std::wstring path;
    std::wstring name;
    uint64_t size_bytes = 0;
    bool is_folder = false;
    bool from_local_desktop = false;
    std::wstring location_badge; // e.g. L"进行中", L"桌面", or parent folder path
};

struct GroupMenuBrief {
    std::string id;
    std::wstring title;
    wchar_t icon_glyph = L'\xE8B7';
};

struct ContextMenuSpec {
    ContextMenuTargetKind target_kind = ContextMenuTargetKind::BlankDesktop;
    bool ai_organize_enabled = true;
    POINT screen_anchor{0, 0};
    std::wstring target_path;
    std::wstring target_name;
    bool target_is_folder = false;
    size_t selected_count = 1;
    bool item_in_tray = false;
    std::string current_group_id;

    std::string target_group_id;
    bool group_collapsed = false;
    bool group_tabbed = false;
    model::GroupViewMode group_view_mode = model::GroupViewMode::Icons;
    bool group_focus_visible = false;
    int group_cols = 4;
    model::CardMaterial group_material = model::CardMaterial::FollowHost;
    model::GroupSortBy group_sort = model::GroupSortBy::ModifiedTime;
    std::wstring group_mapped_folder_path;
    std::wstring group_single_subfolder_path;
    std::wstring group_single_subfolder_name;
    bool current_group_can_map_folder = false;
    bool target_folder_already_mapped = false;

    std::string target_tile_id;
    model::TileKind tile_kind = model::TileKind::PinnedFolder;
    model::TileSize tile_size{2, 2};
    bool tile_live_content = true;
    bool tile_analog_clock = true;
    model::CardMaterial tile_material = model::CardMaterial::FollowHost;
    model::TileLook tile_look;
    std::vector<std::wstring> tile_stack_names;   // a stack's widgets, in order (empty: not a stack)
    int tile_stack_index = 0;
    bool tile_smart_rotate = true;
    int tile_photo_interval_s = 10;               // 照片 tile: its slideshow interval (0 = manual)
    bool tile_photo_shuffle = false;
    bool tile_photo_custom_folder = false;        // a folder of its own rather than the Pictures library
    int tile_note_color = 0;                      // 便笺 tile: its paper colour
    bool tile_edit_mode = false;                  // 编辑磁贴区 is on
    bool tile_has_photos = false;                 // a 照片 tile exists to borrow pictures from
    bool item_in_apps_tile = false;               // FileIcon: the shortcut sits in the 应用 tile
    std::vector<std::pair<model::TileKind, bool>> widget_catalog;  // BlankDesktop: kind, already on the board

    std::wstring current_tag_id;
    std::vector<ColorTagOption> tag_palette;
    std::vector<GroupMenuBrief> groups_list;
    std::wstring undo_label;
    size_t health_issue_count = 0;
    std::wstring current_weather_city;
    bool recycle_bin_empty = true;
    std::string initial_open_submenu_id;
};

struct OverlayCallbacks {
    std::function<void(const std::string& command_id, const ContextMenuSpec& spec)> on_menu_command;
    std::function<void(uint64_t shell_session_id, uint32_t item_id, const std::wstring& verb)> on_invoke_shell_verb;
    std::function<void(uint64_t shell_session_id)> on_close_shell_session;
    std::function<void(POINT anchor_screen_px)> on_prepare_menu_underlay;

    std::function<void(const std::wstring& query)> on_search_query_changed;
    std::function<void(const std::wstring& path)> on_open_path;
    std::function<void(const std::wstring& query)> on_open_search_in_pulse;

    std::function<void(int delta)> on_cycle_quick_look;

    std::function<void(bool archive_instead_of_recycle, const std::vector<std::wstring>& paths)> on_execute_health_action;
    std::function<void()> on_undo_requested;
    std::function<void(const model::BeautifySettings& updated)> on_beautify_changed;
    std::function<void(const model::BeautifySettings& updated, bool dragging)> on_beautify_live_changed;
    std::function<void(model::TileKind widget_kind)> on_toggle_widget_tile;
    std::function<void(model::TileKind kind, model::TileSize size, std::optional<model::PointI> preferred_cell)> on_add_widget_tile;
    std::function<void(std::optional<model::TileKind> initial_kind)> on_request_open_widget_gallery;

    std::function<void(uint64_t tray_item_id)> on_tray_remove_item;
    std::function<void()> on_tray_clear;
    std::function<void()> on_tray_release_to_desktop;
    std::function<void()> on_tray_open_in_pulse;

    std::function<void(const std::string& group_id, const model::GroupRule& updated_rule)> on_group_rule_changed;
    std::function<void(const std::string& tile_id, const std::wstring& updated_note)> on_sticky_note_changed;

    // AI Smart Organize Drawer callbacks
    std::function<void()> on_ai_open_assistant_settings;
    std::function<void(ai::AiOrganizeStrategy strategy,
                       const std::wstring& custom_instruction,
                       bool force_local_fallback)> on_ai_request_plan;
    std::function<void()> on_ai_cancel;
    std::function<void(const ai::AiOrganizePlan& plan, bool pin_created_folders)> on_ai_execute_archive;
    std::function<void(const ai::AiOrganizePlan& plan)> on_ai_apply_virtual_groups;
    std::function<void()> on_ai_open_desktop_in_pulse;

    std::function<void()> on_overlay_dismissed;
};

class DesktopOverlayController {
public:
    static constexpr int kShadowMargin = 14;

    DesktopOverlayController();
    ~DesktopOverlayController();
    DesktopOverlayController(const DesktopOverlayController&) = delete;
    DesktopOverlayController& operator=(const DesktopOverlayController&) = delete;

    bool Initialize(OverlayCallbacks callbacks);
    void Shutdown() noexcept;

    void SetHostMaterial(model::CardMaterial material) noexcept { host_material_ = material; }

    void SetAppearance(bool dark_mode, uint32_t accent_argb) noexcept {
        dark_mode_ = dark_mode;
        accent_argb_ = accent_argb;
    }

    void SetTagPalette(std::vector<ColorTagOption> palette) {
        rule_tag_palette_ = std::move(palette);
    }

    // Overlays are laid out in DIPs (screen DIPs for positions); windows, swap chains and input use pixels.
    void SetDipScale(float scale) noexcept { scale_ = scale > 0.0f ? scale : 1.0f; }
    void SetBackdropSource(const render::WallpaperBackdrop* backdrop,
                           ID2D1DeviceContext* owner_dc) noexcept {
        backdrop_ = backdrop;
        backdrop_owner_dc_ = owner_dc;
    }

    // Dismisses popups and releases overlay surfaces. The shared menu device is
    // retained with a 1x1 target for bounded warm reuse; menu bitmaps are released.
    void Dismiss() noexcept;
    void TrimIdleResources() noexcept;
    bool HasCachedMenuDevice() const noexcept { return shared_menu_compositor_ != nullptr; }

    OverlayKind ActiveKind() const noexcept { return active_kind_; }
    bool IsOpen() const noexcept { return active_kind_ != OverlayKind::None; }
    bool IsAiOrganizeUiOpen() const noexcept {
        return active_kind_ == OverlayKind::AiOrganizeDrawer ||
               (active_kind_ == OverlayKind::ContextMenu && menu_spec_.ai_organize_enabled &&
                (menu_spec_.target_kind == ContextMenuTargetKind::BlankDesktop ||
                 menu_spec_.target_kind == ContextMenuTargetKind::GroupHeader));
    }
    bool IsSideDrawerOpen() const noexcept {
        return active_kind_ == OverlayKind::HealthDrawer ||
               active_kind_ == OverlayKind::BeautifyDrawer ||
               active_kind_ == OverlayKind::AiOrganizeDrawer;
    }
    bool HasActiveSwapChain() const noexcept { return swap_chain_ != nullptr || sub_swap_chain_ != nullptr; }
    bool IsUsingDirectComposition() const noexcept { return using_dcomp_; }
    bool IsDraggingBeautifySlider() const noexcept { return !dragging_beautify_slider_.empty(); }
    HWND OverlayHwnd() const noexcept { return hwnd_; }

    // Composites the active overlay (and any open cascading #dsub submenu) onto an external D2D target
    // for 1:1 --render-snapshots PNG export.
    void CompositeOnto(ID2D1DeviceContext* dc, int origin_x = 0, int origin_y = 0);

    // 1. 4-State Context Menu + cascading #dsub submenu + async system Shell verbs.
    bool OpenContextMenu(const ContextMenuSpec& spec, const model::BeautifySettings& settings);
    bool SaveContextMenuSnapshot(const wchar_t* path, const ContextMenuSpec& spec,
                                 const model::BeautifySettings& settings, bool dark);
    void SetAsyncShellVerbs(uint64_t session_id, std::vector<ShellVerbEntry> verbs);
    // Cache-only reply (start-up prewarm): remembered for the next menu of this kind, session closed.
    void CacheShellVerbs(const std::string& kind_key, std::vector<ShellVerbEntry> verbs);
    static std::string ShellCacheKey(const ContextMenuSpec& spec);

    // 2. Type-to-Search Popup.
    bool OpenSearchPopup(const RECT& work_area, const std::wstring& initial_query,
                         std::vector<SearchHitEntry> local_hits);
    void AppendSearchChar(wchar_t ch);
    void UpdateLocalSearchHits(std::vector<SearchHitEntry> local_hits);
    void UpdateRemoteSearchHits(std::vector<SearchHitEntry> remote_hits);
    const std::wstring& CurrentSearchQuery() const noexcept { return search_query_; }

    // 3. Space Quick Look Overlay.
    bool OpenQuickLook(const RECT& work_area, const model::DesktopItem& item,
                       const ipc::PreviewPayloadData* preview);
    void UpdateQuickLookPreview(const ipc::PreviewPayloadData& preview);

    // 4. Side Drawers (Health Check, Beautify Desktop, & AI Smart Organize).
    bool OpenHealthDrawer(const RECT& work_area, const model::HealthReport& report,
                          size_t total_desktop_items = 0);
    bool OpenBeautifyDrawer(const RECT& work_area, const model::BeautifySettings& settings,
                            const std::unordered_set<uint8_t>& active_widget_kinds = {});
    bool OpenWidgetGalleryDrawer(const RECT& work_area,
                                 int tile_board_screen_left,
                                 int tile_board_screen_top,
                                 std::span<const model::DesktopTile> current_tiles,
                                 std::optional<model::TileKind> initial_kind = std::nullopt,
                                 std::optional<model::PointI> target_cell = std::nullopt);
    void UpdateWidgetGalleryTiles(std::span<const model::DesktopTile> current_tiles);
    // The detail card beside a widget tile; Update refreshes it in place while it shows that kind.
    bool OpenWidgetDetail(const RECT& anchor_screen_rc, const std::string& tile_id, WidgetDetailData data);
    void UpdateWidgetDetail(WidgetDetailData data);
    bool WidgetDetailShows(model::TileKind kind) const noexcept {
        return active_kind_ == OverlayKind::WidgetDetail && detail_.kind == kind;
    }
    // As clicking a size in the drawer's 大小 control (snapshots use it).
    void SelectGallerySize(model::TileSize size) { gallery_selected_size_ = size; Render(); }
    bool OpenAiOrganizeDrawer(const RECT& work_area, const AiDrawerViewState& state);
    void UpdateAiOrganizeDrawer(const AiDrawerViewState& state);
    const AiDrawerViewState& CurrentAiDrawerState() const noexcept { return ai_state_; }

    // 5. Popovers, On-Demand F2 Inline Rename EDIT, and Bottom Capsule Toast.
    bool OpenStagingTrayPopover(const RECT& anchor_screen_rc,
                                std::vector<render::TrayViewEntry> entries,
                                uint64_t total_bytes);
    bool OpenGroupRulePopover(const RECT& anchor_screen_rc, const model::DesktopGroup& group);
    bool OpenStickyNotePopover(const RECT& anchor_screen_rc, const std::string& tile_id,
                               const std::wstring& note_body);

    bool BeginInlineRename(HWND parent_hwnd, const model::RectI& target_rect,
                           const std::wstring& initial_text, bool multiline,
                           std::function<void(const std::wstring& committed_text)> on_commit,
                           int select_end_char = -1,
                           bool allow_empty = false);
    void CancelInlineRename(bool commit) noexcept;
    bool IsInlineRenaming() const noexcept {
        return active_kind_ == OverlayKind::InlineRename ||
               (active_kind_ == OverlayKind::AiOrganizeDrawer && popup_edit_hwnd_ != nullptr);
    }

    bool ShowToast(const RECT& work_area, const std::wstring& message,
                   const std::wstring& action_label = {},
                   std::function<void()> on_action = {},
                   uint32_t duration_ms = 5000);

private:
    friend struct DesktopMenuPolicyTestPeer;
    friend struct DesktopBeautifyTestPeer;
    friend struct DesktopWidgetTextTestPeer;
    friend struct DesktopGroupMenuTestPeer;
    struct MenuRow {
        std::string command_id;
        wchar_t icon_glyph = 0;
        std::wstring label;
        std::wstring right_badge;
        bool separator_after = false;
        bool enabled = true;
        bool checked = false;
        bool is_icon_bar = false;
        bool is_tag_row = false;
        bool is_shell_verb = false;
        bool is_danger = false;
        bool is_accent_primary = false;
        uint32_t shell_item_id = 0;
        std::wstring shell_verb;
        uint32_t shell_depth = 0;
        std::vector<MenuRow> submenu_rows;
        struct ColorSwatch { std::string command_id; uint32_t argb = 0; bool checked = false; };
        std::vector<ColorSwatch> color_swatches;   // a row of colour dots (自定义磁贴 › 纯色)
        RECT bounds{}; // relative to card origin (0, 0)
    };

    struct ClickRegion {
        std::string action_id;
        RECT bounds{}; // relative to card origin (0, 0)
    };

    bool EnsureWindowCreated();
    bool EnsureRenderDevices();
    void ReleaseRenderDevices() noexcept;
    bool EnsureSwapChain(uint32_t win_w, uint32_t win_h);
    bool EnsureSubSwapChain(uint32_t win_w, uint32_t win_h);
    void DestroySubmenu() noexcept;
    void DestroySwapChain() noexcept;
    void RepositionAndShow(int card_screen_x, int card_screen_y, int card_w, int card_h, bool take_focus);
    void OpenSubmenuForRow(int row_idx);
    void Render();
    void RenderSubmenu();

    std::vector<ui::FluentMenuItem> BuildSharedMenuItems();
    void TrackSharedMenu();
    bool EnsureSharedMenu();
    std::unique_ptr<ui::Compositor> shared_menu_compositor_;
    // Pulse's UI device: the shared menu's compositor lives on this hidden window, never on hwnd_, so the overlay
    // keeps hwnd_'s one DComp binding to itself; its device context also paints Pulse controls into the overlay.
    HWND ui_device_hwnd_ = nullptr;
    float ui_compositor_scale_ = 0.0f;
    ui::fluent::Painter ui_painter_;
    bool EnsureUiCompositor();
    // The drawer's 大小 control, drawn with Pulse's own segmented control after the frame (PaintPulseControls).
    struct PulseSegment { D2D1_RECT_F rect{}; std::wstring text; bool selected = false; bool hovered = false; };
    // Every overlay button is Pulse's own (fluent::Painter::DrawButton), painted after the frame like the
    // segmented control. PulseButton records one and its click region; offline snapshots get a fluent look-alike.
    struct PulseButtonRecord { D2D1_RECT_F rect{}; std::wstring text; bool primary = false; bool hovered = false; bool enabled = true; };
    std::vector<PulseButtonRecord> pulse_buttons_;
    void PulseButton(ID2D1DeviceContext* dc, float ox, float oy, const D2D1_RECT_F& card_rect, const std::wstring& text,
                     const std::string& action, bool primary, bool enabled = true);
    int detail_sel_year_ = 0, detail_sel_month_ = 0, detail_sel_day_ = 0;   // the calendar card's chosen day
    std::vector<PulseSegment> gallery_segments_;
    D2D1_RECT_F gallery_segment_track_{};
    void PaintPulseControls();
    bool AttachHostedPopupEdit(const D2D1_RECT_F& card_dip_rect, const std::wstring& initial_text,
                               const std::wstring& cue_text, bool multiline, bool use_13px,
                               int select_start = -1, int select_end = -1, uint32_t max_chars = 1024);
    void PlaceHostedPopupEdit();
    void DestroyHostedPopupEdit() noexcept;
    std::wstring ReadHostedPopupEditText() const;
    D2D1_COLOR_F HostedPopupEditForeground() const noexcept;
    IDWriteTextFormat* HostedPopupEditFormat() const noexcept;
    void SyncHostedPopupEditChanged();
    void ShowPopupEditContextMenu(HWND edit_hwnd, POINT screen_pt);
    static LRESULT CALLBACK PopupEditSubclassProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                                  UINT_PTR subclass_id, DWORD_PTR ref_data) noexcept;
    std::unique_ptr<ui::FluentMenu> shared_menu_;
    uint64_t shared_menu_material_key_ = 0;
    const render::WallpaperBackdrop* shared_menu_source_ = nullptr;
    ID2D1DeviceContext* shared_menu_source_dc_ = nullptr;
    std::vector<MenuRow> shared_menu_commands_;
    bool shared_menu_tracking_ = false;
    uint32_t menu_generation_ = 0;

    void BuildMenuRows(const model::BeautifySettings& settings);
    void DrawPopupCardBackground(ID2D1DeviceContext* dc, const D2D1_RECT_F& card_rc,
                                 float radius, bool with_shadow, bool sticky_tint = false);
    void DrawPopupCardBorder(ID2D1DeviceContext* dc, const D2D1_RECT_F& card_rc,
                             float radius, bool sticky_tint = false);
    void DrawMainContent(ID2D1DeviceContext* dc, float ox, float oy, bool with_shadow);
    void DrawSubmenuContent(ID2D1DeviceContext* dc, float ox, float oy, bool with_shadow);

    void DrawMenuRowList(ID2D1DeviceContext* dc, float ox, float oy, float card_w,
                         const std::vector<MenuRow>& rows, int hot_row, int hot_icon_idx, int hot_tag_idx);
    void DrawSearchPopup(ID2D1DeviceContext* dc, float ox, float oy);
    void DrawQuickLook(ID2D1DeviceContext* dc, float ox, float oy);
    void DrawHealthDrawer(ID2D1DeviceContext* dc, float ox, float oy);
    void DrawBeautifyDrawer(ID2D1DeviceContext* dc, float ox, float oy);
    void DrawWidgetGalleryDrawer(ID2D1DeviceContext* dc, float ox, float oy);
    void DrawAiOrganizeDrawer(ID2D1DeviceContext* dc, float ox, float oy);
    void DrawTrayPopover(ID2D1DeviceContext* dc, float ox, float oy);
    void DrawRulePopover(ID2D1DeviceContext* dc, float ox, float oy);
    void DrawStickyNotePopover(ID2D1DeviceContext* dc, float ox, float oy);
    void DrawToastCapsule(ID2D1DeviceContext* dc, float ox, float oy);

    void HandleLeftClick(int card_x, int card_y);
    void HandleSubmenuClick(int sub_card_x, int sub_card_y);
    void HandleMouseMove(int card_x, int card_y);
    void HandleSubmenuMouseMove(int sub_card_x, int sub_card_y);
    void HandleKeyDown(WPARAM vk);
    void HandleChar(wchar_t ch);

    ID2D1SolidColorBrush* Brush(ID2D1DeviceContext* dc, uint32_t argb, float opacity = 1.0f);
    float MeasureTextW(const std::wstring& text, IDWriteTextFormat* fmt, float max_w = 500.0f) const;

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) noexcept;
    static LRESULT CALLBACK StaticSubWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) noexcept;
    LRESULT HandleWndProc(UINT msg, WPARAM wparam, LPARAM lparam) noexcept;
    LRESULT HandleSubWndProc(UINT msg, WPARAM wparam, LPARAM lparam) noexcept;

    int ToPx(int dip) const noexcept { return static_cast<int>(dip * scale_ + (dip >= 0 ? 0.5f : -0.5f)); }
    int ToDip(int px) const noexcept { return static_cast<int>(px / scale_ + (px >= 0 ? 0.5f : -0.5f)); }

    float scale_ = 1.0f;
    OverlayCallbacks callbacks_;
    HWND hwnd_ = nullptr;
    HWND sub_hwnd_ = nullptr;
    OverlayKind active_kind_ = OverlayKind::None;
    bool using_dcomp_ = false;
    bool sub_using_dcomp_ = false;

    int card_screen_x_ = 0;
    int card_screen_y_ = 0;
    int card_w_ = 0;
    int card_h_ = 0;
    uint32_t win_w_ = 0;
    uint32_t win_h_ = 0;

    bool submenu_open_ = false;
    int open_submenu_parent_row_ = -1;
    int sub_screen_x_ = 0;
    int sub_screen_y_ = 0;
    int sub_card_w_ = 0;
    int sub_card_h_ = 0;
    uint32_t sub_win_w_ = 0;
    uint32_t sub_win_h_ = 0;

    bool dark_mode_ = true;
    uint32_t accent_argb_ = 0xFF4CC2FFu;
    const render::WallpaperBackdrop* backdrop_ = nullptr;
    ID2D1DeviceContext* backdrop_owner_dc_ = nullptr;

    // Shared lightweight D3D11/D2D/DWrite factories; swapchains & DComp visuals are destroyed on Dismiss()!
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgi_device_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2d_dc_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_bitmap_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_device_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcomp_target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> dcomp_visual_;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> sub_swap_chain_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> sub_target_bitmap_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> sub_dcomp_target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> sub_dcomp_visual_;

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> quicklook_bitmap_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> popup_shadow_bitmap_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;

    render::DesktopTextRenderer detail_text_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_body_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_body_semi_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_title_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_stat_num_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_small_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_small_center_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_small_right_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_mono_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_fluent_12_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_fluent_15_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_fluent_20_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_gallery_big_;

    // Context menu state
    ContextMenuSpec menu_spec_{};
    uint64_t shell_session_id_ = 0;
    std::vector<ShellVerbEntry> shell_verbs_;
    bool shell_verbs_cached_ = false;  // rows shown from the cache; their ids are not live yet
    std::string shell_cache_key_;
    std::unordered_map<std::string, std::vector<ShellVerbEntry>> shell_verb_cache_;
    // A cached system row clicked before the live reply: run the live row with the same place in the tree.
    std::wstring pending_shell_label_;
    uint32_t pending_shell_depth_ = 0;
    std::vector<MenuRow> menu_rows_;
    DesktopMenuPolicyCache menu_policy_;
    std::vector<MenuRow> submenu_rows_;
    int hovered_row_ = -1;
    int hovered_sub_row_ = -1;
    int hovered_icon_bar_idx_ = -1;
    int hovered_tag_idx_ = -1;

    // Search state
    RECT work_area_{0, 0, 1920, 1040};
    WidgetDetailData detail_;
    std::string detail_tile_id_;
    int detail_month_offset_ = 0;   // the calendar card's month, relative to this one
    bool HandleWidgetDetailAction(const std::string& act);
    void DrawWidgetDetail(ID2D1DeviceContext* dc, float ox, float oy);
    std::wstring search_query_;
    std::vector<SearchHitEntry> local_hits_;
    std::vector<SearchHitEntry> remote_hits_;
    int search_selected_idx_ = 0;

    // Quick Look state
    model::DesktopItem ql_item_{};
    ipc::PreviewPayloadData ql_preview_{};
    bool ql_has_preview_ = false;

    // Drawers & popovers state
    model::HealthReport health_report_{};
    size_t health_total_desktop_items_ = 0;
    bool health_completed_ = false;
    size_t health_cleaned_count_ = 0;
    uint64_t health_cleaned_bytes_ = 0;

    model::BeautifySettings beautify_settings_{};
    model::CardMaterial host_material_ = model::CardMaterial::MicaAlt;
    std::unordered_set<uint8_t> active_widget_kinds_;
    struct BeautifySliderTrack {
        std::string id;
        RECT bounds{};
    };
    float beautify_scroll_y_ = 0.0f;
    float beautify_max_scroll_y_ = 0.0f;
    std::vector<BeautifySliderTrack> beautify_sliders_;
    std::string dragging_beautify_slider_;
    bool ApplyBeautifySliderAtX(const std::string& id, int card_x) noexcept;
    void NotifyBeautifyChanged(bool dragging);

    AiDrawerViewState ai_state_{};
    float ai_scroll_y_ = 0.0f;
    float ai_max_scroll_y_ = 0.0f;

    std::vector<model::DesktopTile> gallery_tiles_;
    model::TileKind gallery_selected_kind_ = model::TileKind::Calendar;
    model::TileSize gallery_selected_size_{1, 1};
    std::optional<model::PointI> gallery_target_cell_;
    int gallery_board_left_ = 0;
    int gallery_board_top_ = 0;

    std::vector<render::TrayViewEntry> tray_entries_;
    uint64_t tray_total_bytes_ = 0;
    model::DesktopGroup rule_group_{};
    std::vector<ColorTagOption> rule_tag_palette_;
    std::string sticky_tile_id_;
    std::wstring sticky_note_text_;

    std::vector<ClickRegion> click_regions_;
    std::string hovered_action_id_;

    // Toast state
    std::wstring toast_message_;
    std::wstring toast_action_label_;
    std::function<void()> toast_on_action_;

    // Pulse hosted child EDIT (EditHost + LumaTextRenderer + FluentMenu context menu)
    HWND popup_edit_hwnd_ = nullptr;
    HFONT popup_edit_font_14_ = nullptr;
    HFONT popup_edit_font_13_ = nullptr;
    HBRUSH popup_edit_brush_ = nullptr;
    bool popup_edit_brush_dark_ = true;   // the theme popup_edit_brush_ was made for
    D2D1_RECT_F popup_edit_card_dip_{};
    std::wstring popup_edit_cue_;
    bool popup_edit_multiline_ = false;
    bool popup_edit_use_13px_ = false;
    bool popup_edit_composing_ = false;
    bool popup_edit_ignore_killfocus_ = false;
    bool edit_multiline_ = false;
    bool edit_allow_empty_ = false;
    int rename_stem_len_ = -1;
    std::function<void(const std::wstring&)> edit_on_commit_;
};

} // namespace pulse::desktop::overlays
