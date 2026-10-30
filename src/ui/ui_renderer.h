// ui_renderer.h — Full-window Fluent renderer (title bar, toolbar, sidebar, pane, tray).
#pragma once
#include "ui_view_model.h"
#include "ui_compositor.h"
#include "shell_icons.h"
#include "open_with_icons.h"
#include "panel_metrics.h"
#include "favorites_bar_layout.h"
#include "folder_thumbnail_cache.h"
#include "render_caches.h"
#include "archive_preview.h"
#include "name_highlight.h"
#include "preview_handler_host.h"
#include "video_preview.h"
#include "ui_motion.h"
#include "link_pill.h"
#include "ui_view_morph.h"
#include "group_wheel.h"
#include "ai_panel_view.h"
#include "window_overview_view.h"
#include "window_switcher_view.h"

namespace pulse::ui {

class MainRenderer {
    friend struct FilenameRenderTest;
    friend struct PaneHeaderIconTest;
    friend struct FolderSizesUiTest;
    friend struct ColumnResizeUiTest;
    friend struct UiModuleAuditTest;
public:
    MainRenderer();
    // Draws from caches shared with other windows (AppCore::render_caches).
    explicit MainRenderer(RenderCaches& caches);
    MainRenderer(const MainRenderer&) = delete;
    MainRenderer& operator=(const MainRenderer&) = delete;
    RenderCaches& Caches() noexcept { return caches_; }

    void SetScale(float scale);
    void SetCompositor(Compositor* comp);
    void InvalidateTypography();
    void SetIconNotifyWindow(HWND hwnd);
    void NotifyPreviewActivate(bool active) { preview_handler_.NotifyAppActivate(active); }
    void NotifyPreviewOwnerMoved() { preview_handler_.Reposition(); }
    // The hover preview card, over everything Render drew (ui_hover_preview.cpp).
    // Also stops the card's video when the view is no longer visible.
    void DrawHoverPreview(const HoverPreviewView& view, const D2D1_RECT_F& client,
                          const Theme& theme, bool high_contrast);
    // Motion v2 "star": a star arcs from (x, y) client px into the sidebar's
    // 已加星标 row; DrawStarFlight paints it over everything (ui_sidebar.cpp).
    void LaunchStarFlight(float x, float y);
    void DrawStarFlight(const Theme& theme);
    // Repaint interval the last drawn card asked for: 0 none, short while
    // something animates (dwell line, loading sweep), longer for a playing
    // video's progress line.
    unsigned HoverPreviewFrameMs() const noexcept { return hover_frame_ms_; }
    // The card drawn last, client px (empty when none): the pointer on a
    // folder card keeps it (app_hover.cpp). Its list and how far that scrolls.
    D2D1_RECT_F HoverCardRect() const noexcept { return hover_card_rect_; }
    D2D1_RECT_F HoverFolderListRect() const noexcept { return hover_list_rect_; }
    float HoverFolderScrollMax() const noexcept { return hover_scroll_max_; }
    float HoverFolderRowHeight() const noexcept { return 26.0f * scale_; }
    VideoPreview::State HoverVideoState() const;
    // --bench-hover (app_hover_preview.cpp): one card's content asked for
    // exactly as the card asks (facts, then the preview at pixel_size),
    // timed until it can be drawn. The card itself stays down meanwhile.
    struct HoverBenchSample {
        int result = -1;               // PreviewDrawResult; -1 timed out
        double content_ms = 0.0;       // request to drawable content
        double properties_ms = -1.0;   // to the facts strip; -1 not by then
        double upload_ms = 0.0;        // the Draw that uploaded the bitmap
        double first_ms = -1.0;        // HEIF: its embedded thumbnail on screen; -1 none
        uint32_t width = 0, height = 0;
    };
    HoverBenchSample BenchHoverPreview(const std::wstring& path, DWORD attrs, uint64_t size,
                                       uint64_t modified, uint32_t pixel_size, uint32_t timeout_ms,
                                       bool properties_first);
    // The silent loop: Open to the first picture in ms (-1 never); ready_ms
    // gets Open to the player being ready.
    double BenchHoverVideo(HWND owner, const std::wstring& path, uint32_t timeout_ms, double* ready_ms);
    void SetHoverBench(bool on) noexcept { hover_bench_ = on; }
    // The pointer settled on a file (app_hover_preview.cpp): its card preview
    // is asked for now, so the dwell covers the decode. Outside a frame.
    // photo_first false: a HEIF photo is asked for at full size straight
    // away, without its thumbnail (the bench's decoy request).
    int PrefetchHoverPreview(const std::wstring& path, DWORD attrs, uint64_t size, uint64_t modified,
                             bool photo_first = true);

    // Layout metrics (DIPs).
    float TitleBarHeight() const { return title_bar_height_; }
    float ToolbarHeight() const { return toolbar_height_; }
    // Where the panes start: the sidebar plus the docked Pulse 助手 column.
    float EffectiveSidebarWidth(float window_width) const;
    // The sidebar alone (title bar brand, tab strip, the sidebar itself).
    float SidebarOwnWidth(float window_width) const;
    float TitleSidebarWidth(float window_width) const;
    // Pulse 助手 (ai_panel_view.h). Docked it is a column right of the
    // sidebar, which folds to its rail while it is open (not persisted).
    AiPanelView& AiPanel() { return ai_panel_; }
    const AiPanelView& AiPanel() const { return ai_panel_; }
    void SetAiPanel(bool open, AiPanelMode mode, bool animate);
    float AiDockWidth(float window_width) const;
    D2D1_RECT_F AiColumnRect(float w, float h) const;
    D2D1_RECT_F AiFloatingArea(float w, float h) const;
    void SetSidebarWidthDip(float width) {
        sidebar_width_dip_ = std::clamp(width, kSidebarMinWidthDip, kPanelWidthMaxDip);
        sidebar_width_ = sidebar_width_dip_ * scale_;
    }
    // Vertical tabs: the address row lives in the title bar, so the toolbar
    // keeps only its command row.
    void SetVerticalTabs(bool on) {
        vertical_tabs_ = on && !embedded_;
        toolbar_height_ = ToolbarHeightDip() * scale_;
    }
    // 收藏栏: one more row under the command row (never in embedded hosts).
    void SetFavoritesBar(bool on) {
        favorites_bar_ = on && !embedded_;
        toolbar_height_ = ToolbarHeightDip() * scale_;
    }
    bool FavoritesBarShown() const { return favorites_bar_; }
    // The bar hidden, the address bar ends in ★⌄ (the favorites dropdown).
    bool FavoritesButtonShown() const { return !favorites_bar_ && !embedded_; }
    D2D1_RECT_F AddressFavoritesRect(float window_width) const {
        const D2D1_RECT_F a = AddressBarRect(window_width);
        return D2D1::RectF(a.right - 32 * scale_, a.top + 4 * scale_, a.right - 4 * scale_, a.bottom - 4 * scale_);
    }
    FavoritesBarLayout FavoritesBarLayoutAt(const WindowViewModel& vm, float window_width) const;
    float ToolbarHeightDip() const {
        return (vertical_tabs_ || embedded_ ? 44.0f : 88.0f) + (favorites_bar_ ? kFavoritesBarHeightDip : 0.0f);
    }
    // Hosted inside another window (the file picker): no title bar, tabs,
    // command row, status bar or details panel; only the address row, the
    // sidebar and the pane. `top_dip` is the host's own strip above the
    // address row and `bottom_dip` its footer below the panes.
    void SetEmbedded(float top_dip, float bottom_dip);
    bool Embedded() const { return embedded_; }
    bool VerticalTabs() const { return vertical_tabs_; }
    // Collapse/expand; `animate` eases the width (skipped when the system
    // turns animations off). Re-setting the current state is a no-op.
    void SetSidebarCollapsed(bool on, bool animate = false);
    bool SidebarCollapsed() const { return sidebar_collapsed_; }
    bool SidebarAnimating() const { return collapse_anim_; }
    // Hover peek: the collapsed rail shows the full sidebar as an overlay
    // above the panes without relayout.
    void SetSidebarPeek(bool on) { sidebar_peek_ = on; }
    bool SidebarPeek() const { return sidebar_peek_; }
    bool SidebarPeekVisible(float window_width) const {
        return sidebar_peek_ && sidebar_collapsed_ && !collapse_anim_ &&
               window_width / scale_ >= kSidebarRailWindowDip;
    }
    // Expanded width, ignoring the collapse state.
    float SidebarFullWidth(float window_width) const;
    // Toolbar geometry honoring the vertical-tabs split (row 1 in the title bar).
    ToolbarLayout ToolbarLayoutAt(float w, float create_width, float filter_expand = 0.0f) const;
    // Selection chip of the toolbar (empty while nothing is selected).
    ToolbarSelectionChip SelectionChipAt(const ToolbarLayout& layout, float width, int selected,
                                         int action_count) const;
    // 0 = default width; otherwise the minimum search field width in DIP.
    void SetSearchBarMinWidth(float dip) { search_min_dip_ = dip; }
    // Search master switch off: no search field in the toolbar.
    void SetSearchHidden(bool hidden) { search_hidden_ = hidden; }
    bool SearchHidden() const { return search_hidden_; }
    // Toolbar "Group" button: -1 hidden, 0 not grouped, 1..4 app::GroupBy value.
    void SetToolbarGroup(int group_by) { toolbar_group_ = group_by; }
    // 自定义工具栏 → 排序与筛选: which of 排序 / 筛选 / 分组 the toolbar shows
    // (the caller already forces an active filter or grouping on).
    void SetToolbarView(bool sort, bool filter, bool group) {
        toolbar_show_sort_ = sort;
        toolbar_show_filter_ = filter;
        toolbar_show_group_ = group;
    }
    // 自定义工具栏: the New button's main action (0 folder, 1 the type made
    // last, 2 menu only), whether that last type was a text document, and how
    // many commands are pinned.
    void SetToolbarPrefs(int new_primary, bool last_text, int pin_count) {
        new_primary_ = new_primary;
        new_last_text_ = last_text;
        pin_count_ = pin_count;
    }
    // Pinned apps: where they start among the pins, and drop mode with its label.
    void SetToolbarApps(int app_from, bool drop_mode, std::wstring drop_label) {
        pin_app_from_ = app_from;
        pin_drop_mode_ = drop_mode;
        pin_drop_label_ = std::move(drop_label);
    }
    // The user's separators between pinned tools (bit i: before pin i).
    void SetToolbarSeparators(uint64_t mask) { pin_separators_ = mask; }
    float ToolbarPinLabelWidth() const;
    // Selection chip input for the toolbar layout (the row reserves its room).
    void SetToolbarSelection(int selected, int action_count) {
        selection_count_ = selected;
        selection_actions_ = action_count;
    }
    float SelectionChipLabelWidth(float width, int selected) const;
    std::wstring NewButtonLabel() const;
    const wchar_t* NewButtonGlyph() const;
    int ToolbarGroup() const { return toolbar_group_; }
    // Drum "Group by" picker drawn over everything (app/group_wheel_ui.cpp).
    GroupWheel& GroupWheelPicker() { return group_wheel_; }
    // Ctrl+` window switcher drawn over everything (app/window_switcher_ui.cpp, M7d).
    WindowSwitcherView& WindowSwitcher() { return window_switcher_; }
    // Ctrl+Shift+O window overview, under the switcher (app/window_overview_ui.cpp, M7e).
    WindowOverviewView& WindowOverview() { return window_overview_; }
    float ToolbarGroupWidth(float w) const;
    std::wstring ToolbarGroupLabel() const;   // "Group: Date" text of the active chip
    float SearchBarMinWidth() const { return search_min_dip_; }
    D2D1_RECT_F SidebarToggleRect(float w) const;
    // The window's dot and name in the title bar (M7c). Without room for the
    // name (the icon rail) only a dot on the corner of the mark or of the
    // sidebar toggle shows; `hit` is then the mark, or the dot itself.
    struct WindowIdentityGeometry {
        D2D1_RECT_F hit{};  // empty: not shown
        D2D1_POINT_2F dot{};
        float dot_radius = 0.0f;
        bool compact = false;
    };
    WindowIdentityGeometry WindowIdentityLayout(const WindowViewModel& vm, float w) const;
    D2D1_RECT_F WindowIdentityRect(const WindowViewModel& vm, float w) const { return WindowIdentityLayout(vm, w).hit; }
    float PaneHeaderHeight() const { return pane_header_height_; }
    // Height the focused pane's banner strip took in the last frame (one or
    // two lines plus its gap); 0 before any frame or without a banner.
    float FocusedPaneBannerHeight() const { return focused_banner_height_; }
    float ColumnHeaderHeight() const { return column_header_height_; }
    float RowHeight() const { return row_height_; }
    float ListRowHeightDip(const PaneViewModel& vm) const;
    // Same, but aware of the two-line (name + path) narrow search layout; icon
    // grids get their name block and icon size (the pane's ViewLayout inputs).
    ViewLayoutParams ListRowHeightDip(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const;
    // File-list row height preference (DIPs); survives SetScale recompute.
    void SetRowHeightDip(float dip) {
        row_height_dip_ = std::clamp(dip, 24.0f, 48.0f);
        row_height_ = row_height_dip_ * scale_;
    }
    // Staging-tray deck icon edge preference (DIPs); single-item decks add 8.
    void SetTrayIconDip(float dip) { tray_icon_dip_ = std::clamp(dip, 32.0f, 64.0f); }
    float TrayIconDip() const { return tray_icon_dip_; }
    float Margin() const { return margin_; }

    // Right details panel: toggled from the view menu; ContentRect shrinks.
    // animate: the panel's width rides the smooth spring (motion v2 "layout").
    void SetDetailsPanelVisible(bool visible, bool animate = false) {
        if (visible != details_visible_ && animate && motion::SystemAnimationsEnabled()) {
            details_from_ = details_value_;
            details_anim_start_ = motion::NowMs();
            details_anim_ = true;
        } else if (!details_anim_) {
            details_value_ = visible ? 1.0f : 0.0f;
        }
        details_visible_ = visible;
        if (!visible) { EndDetailsPreviewPan(); details_preview_ready_ = false; }
    }
    // Preferred width, capped by the window so the file list keeps a usable width.
    float DetailsPanelWidth(float window_w) const;
    void SetDetailsPanelWidth(float width_dip) {
        details_width_ = std::clamp(width_dip, kDetailsMinWidthDip, kPanelWidthMaxDip);
    }
    // Splitter drag limits in DIPs for the current window size.
    float SidebarMaxWidthDip(float window_w) const;
    float DetailsMaxWidthDip(float window_w) const;
    float SidebarWidthDip() const { return sidebar_width_dip_; }
    // Unclipped content height (DIPs) of the details panel, for wheel clamping.
    float DetailsContentHeightDip(const WindowViewModel& vm, float w, float h);
    bool CachedPreviewProperties(const std::wstring& path, uint64_t modified, uint64_t size,
                                 std::vector<PreviewProperty>& properties) {
        return details_cache_.CachedProperties(path, modified, size, properties);
    }
    D2D1_RECT_F DetailsPanelRect(float w, float h) const;

    D2D1_RECT_F ContentRect(float w, float h) const;
    D2D1_RECT_F PaneListRect(const D2D1_RECT_F& pane_bounds, float extra_top = 0.0f,
                             ViewMode mode = ViewMode::Details) const;
    D2D1_RECT_F PaneListRect(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const;
    D2D1_RECT_F FilterBoxRect(const D2D1_RECT_F& pane_bounds,
                              float expand = 1.0f) const;
    // wall: the segment also holds the photo wall button (PaneWallRect), so
    // details and icons sit one step further left.
    D2D1_RECT_F PaneMediumIconsRect(const D2D1_RECT_F& pane_bounds,
                                    float filter_expand = 1.0f, bool wall = false) const;
    D2D1_RECT_F PaneViewButtonRect(const D2D1_RECT_F& pane_bounds,
                                   float filter_expand = 1.0f) const;
    D2D1_RECT_F PaneDetailsRect(const D2D1_RECT_F& pane_bounds,
                                     float filter_expand = 1.0f, bool wall = false) const;
    // The segment's photo wall button, left of the view menu; empty outside
    // main window panes (vm.size_control).
    D2D1_RECT_F PaneWallRect(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const;
    // The photo wall suggestion card under the pane header and its switch /
    // never-again parts; all empty while it is not shown.
    // Photo wall suggestion card under the pane header (P2 maybeSuggest).
    // alpha 0 = not shown; animating = it is fading in or out.
    struct WallHintRects { D2D1_RECT_F card{}, button{}, close{}, text{}; float alpha = 0.0f; bool animating = false; };
    WallHintRects PaneWallHintRects(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const;
    // Icon views: the header's size slider (empty when hidden). Panes under
    // 440 DIP show PaneSizeButtonRect instead, which opens it over the title.
    D2D1_RECT_F PaneSizeSliderRect(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const;
    D2D1_RECT_F PaneSizeButtonRect(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const;
    // Icon size (DIP) under x on that slider; presets catch it nearby. 0 = no slider.
    float PaneSizeSliderValueAt(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds, float x) const;
    // Right end of the header title, left of the view controls.
    float PaneHeaderTitleRight(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const;
    // Column view geometry for a pane. When active, PaneBodyBounds replaces
    // the pane bounds for the regular list (header stays full width).
    ColumnStripLayout ColumnStripGeometry(const PaneViewModel& vm,
                                          const D2D1_RECT_F& pane_bounds) const;
    D2D1_RECT_F PaneBodyBounds(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const;
    float ColumnStripScrollPx(const ColumnStripColumnView& column, float view_h) const;
    float ColumnStripMaxScrollPx(const ColumnStripColumnView& column, float view_h) const;
    // Height of the scrolling row area inside a column rect (below its caption).
    float ColumnStripViewHeight(const D2D1_RECT_F& column_rect) const;
    // Horizontal scrollbar of the ancestor strip; false when it does not overflow.
    bool ColumnStripHScrollRects(const ColumnStripLayout& layout, D2D1_RECT_F& track,
                                 D2D1_RECT_F& thumb) const;
    D2D1_RECT_F FilterEditRect(const D2D1_RECT_F& pane_bounds,
                               float expand = 1.0f, bool has_text = false) const;
    D2D1_RECT_F FilterClearRect(const D2D1_RECT_F& pane_bounds, float expand) const;

    bool BeginDetailsPreviewPan(float x, float y);
    void MoveDetailsPreviewPan(float x, float y);
    void EndDetailsPreviewPan() { details_preview_dragging_ = false; details_preview_drag_pending_ = false; }
    bool DetailsPreviewDragging() const { return details_preview_dragging_; }
    bool DetailsPreviewPointerActive() const { return details_preview_dragging_ || details_preview_drag_pending_; }
    bool CanDetailsPreviewPan() const { return details_preview_ready_; }
    // Archive listings draw an interactive tree instead of a pannable surface.
    bool DetailsPreviewIsArchive() const { return details_preview_ready_ && details_preview_archive_; }
    bool ClickDetailsPreview(float x, float y);
    bool HoverDetailsPreview(float x, float y);
    bool TickDetailsPreview(ULONGLONG now) {
        if (details_zoom_label_until_ && now >= details_zoom_label_until_) {
            details_zoom_label_until_ = 0;
            return true;
        }
        return false;
    }
    // Highlight glides and copy confirmations (ui_motion.h). True while another
    // frame is needed; false once everything has settled, so idle stays idle.
    // Owner-drawn text fields (Settings > 快捷键 search): milliseconds until
    // the caret should toggle, 0 when no drawn caret is showing.
    UINT CaretBlinkDueMs() const { return caret_due_ms_; }
    bool TickMotion(ULONGLONG now) {
        // Glides run on the NowMs clock; `now` (GetTickCount64) serves the
        // task pill and copy feedback below.
        const uint64_t motion_now = motion::NowMs();
        bool active = sidebar_pill_.Active(motion_now) || settings_nav_pill_.Active(motion_now) ||
                      settings_nav_hover_.Active(motion_now) || collapse_anim_ || details_anim_ ||
                      ai_panel_.Animating();
        for (const auto& m : list_hover_motion_) active = active || m.Active(motion_now);
        for (const auto& m : list_shift_) active = active || m.Active(motion_now);
        for (const auto& m : view_morph_) active = active || m.Active(motion_now);
        for (const auto& m : link_pill_motion_) active = active || m.Active(motion_now);
        // Operation pill: the completion check fade and pop.
        active = active ||
                 (task_pill_done_at_ != 0 && now - task_pill_done_at_ < task_pill_hold_ms_ + 400) ||
                 (task_pill_pop_at_ != 0 && now - task_pill_pop_at_ < kTaskPillPopMs + 40);
        bool ticked = false;
        if (copy_feedback_.Tick(now)) ticked = true;
        if (group_wheel_.Tick(motion_now)) ticked = true;
        if (folder_thumbnail_cache_.Tick(now)) ticked = true;
        ticks_active_ = ticked;
        active = active || ticked;
        if (motion_now < tab_motion_until_) active = true;
        if (motion_now < row_leave_until_) active = true;
        if (motion_now < mtime_flash_until_) active = true;
        if (motion_now < splitter_motion_until_) active = true;
        if (motion_now < spring_ring_until_) active = true;
        // Sidebar pill/plate, toolbar plates, column cascade, details sweep:
        // their last frame asked for another (RequestMotionFrame).
        if (motion_frame_wanted_) active = true;
        // Decoration only: loading placeholders shimmer (and must appear on
        // time) while a pane enumerates, the folder size skeleton and the
        // scanning label breathe, the pill's indeterminate ring spins. These
        // are fine at 30 fps (DecorativeMotionOnly), so they do not repaint
        // the whole window at the display rate for up to ten seconds.
        const bool decor = list_loading_active_ || folder_size_anim_active_ || task_pill_spinning_ ||
                           ai_panel_.WantsDecorFrame();
        decorative_only_ = decor && !active;
        return active || decor;
    }
    // The last TickMotion found nothing but decorative motion running.
    bool DecorativeMotionOnly() const { return decorative_only_; }

    // Partial redraw (plans/Pulse_局部重绘方案.md). Only a quiet window
    // redraws part of itself: nothing moves but the list hover plates and no
    // overlay is up, now or in the frame before. Anything else is a full frame.
    bool PartialRedrawSafe(const WindowViewModel& vm) const;
    // Where a row hover change in a details list repaints: both rows as drawn
    // in the last full frame and the plate's path. False: redraw everything.
    bool HoverRowDamage(int pane_index, int old_source, int new_source, D2D1_RECT_F* out) const;
    // A frame while only hover plates glide: their paths. False otherwise.
    bool HoverMotionDamage(D2D1_RECT_F* out) const;
    // The frame about to render redraws only `damage` (client px; null: all).
    void SetFrameDamage(const D2D1_RECT_F* damage) {
        frame_partial_ = damage != nullptr;
        if (damage) frame_damage_ = *damage;
    }
    // Where the rows in PaneViewModel::landing_names were drawn in the last
    // frame (window px: icon left, row top/right/bottom).
    const std::vector<D2D1_RECT_F>& LandingRects() const { return landing_rects_; }
    void RefreshFolderThumbnails() { folder_thumbnail_cache_.Refresh(); }
    // A preview pack was installed, removed or switched: files that had no
    // thumbnail may have one now, so cached results are dropped.
    void EvictThumbnails() { thumbnail_cache_.Evict(); hover_cache_.Evict(); }
    auto FolderThumbnailDebugStats() { return folder_thumbnail_cache_.ReadDebugStats(); }
    void SetFolderThumbnailsEnabled(bool enabled) { folder_thumbnails_enabled_ = enabled; }
    // region = HitTestResult region of the button that copied, index its index.
    void NotifyCopied(int region, int index) {
        copy_feedback_.Trigger(region, index, GetTickCount64());
    }
    // The columns of the pane in this slot were just auto-fitted: they glide
    // there from the widths drawn last.
    void NoteColumnAutoFit(int pane_index) {
        if (pane_index < 0 || pane_index >= static_cast<int>(col_fit_.size()) || !motion::SystemAnimationsEnabled())
            return;
        auto& f = col_fit_[static_cast<size_t>(pane_index)];
        if (f.last_count <= 0) return;
        f.from = f.last;
        f.count = f.last_count;
        f.start = motion::NowMs();
    }
    void ScrollDetailsPreview(float steps, float x, float y, bool horizontal = false);
    void ToggleDetailsPreviewFit(float x, float y);

    // Date = modified. Values double as details_column_set.h bits: append only.
    // Status: cloud-file state (仅在云端 / 已在本地 / 始终保留), only drawn in
    // folders under a Cloud Files sync root (PaneViewModel::cloud_status_column).
    enum class ColumnKind : uint8_t { Name, Path, Date, Type, Size, Created, Accessed, Status };
    static constexpr size_t kMaxDetailsColumns = 8;
    static_assert(kDetailsColumnModified == 1u << static_cast<uint32_t>(ColumnKind::Date) &&
                  kDetailsColumnType == 1u << static_cast<uint32_t>(ColumnKind::Type) &&
                  kDetailsColumnSize == 1u << static_cast<uint32_t>(ColumnKind::Size) &&
                  kDetailsColumnCreated == 1u << static_cast<uint32_t>(ColumnKind::Created) &&
                  kDetailsColumnAccessed == 1u << static_cast<uint32_t>(ColumnKind::Accessed) &&
                  kDetailsColumnStatus == 1u << static_cast<uint32_t>(ColumnKind::Status));
    // Details columns in display order. Name is always first; Path only in
    // wide search views; then the shown metadata columns (modified, created,
    // accessed, type, size). Accessed, created, type, then modified are
    // dropped first when space is short.
    struct DetailsColumnLayout {
        float left = 0.0f;
        float right = 0.0f;
        std::array<float, kMaxDetailsColumns> widths{};
        std::array<ColumnKind, kMaxDetailsColumns> kinds{ColumnKind::Name, ColumnKind::Date,
                                        ColumnKind::Type, ColumnKind::Size, ColumnKind::Size,
                                        ColumnKind::Size, ColumnKind::Size, ColumnKind::Size};
        int count = 4;
        // Narrow search view: the folder path is drawn under the name.
        bool two_line = false;

        float DividerX(int index) const {
            float x = left;
            for (int i = 0; i <= index && i < count - 1; ++i)
                x += widths[static_cast<size_t>(i)];
            return x;
        }
        int IndexOf(ColumnKind kind) const {
            for (int i = 0; i < count; ++i)
                if (kinds[static_cast<size_t>(i)] == kind) return i;
            return -1;
        }
        bool Has(ColumnKind kind) const { return IndexOf(kind) >= 0; }
        float Left(ColumnKind kind) const {
            const int i = IndexOf(kind);
            return i <= 0 ? left : DividerX(i - 1);
        }
        float Width(ColumnKind kind) const {
            const int i = IndexOf(kind);
            return i < 0 ? 0.0f : widths[static_cast<size_t>(i)];
        }
    };
    // Content-fitted metadata widths (DIP) measured from the current font,
    // language, DPI and date format; independent of row data so layout,
    // hit testing and painting always agree.
    struct ColumnAutoWidths {
        float date = 130.0f, type = 128.0f, size = 90.0f, created = 130.0f, accessed = 130.0f;
        float status = 112.0f;
    };
    ColumnAutoWidths AutoColumnWidths() const;
    float TypeChipWidthDip(const std::wstring& chip) const;
    // Cached max(LumaText, DWrite) advance for list cells (small=true: SmallFormat).
    float CellTextWidth(const std::wstring& text, bool small_text = false) const;
    void SetListStyle(bool smart_date, bool zebra, bool size_bar, bool tag_names,
                      bool selection_outline) {
        list_smart_date_ = smart_date; list_zebra_ = zebra; list_size_bar_ = size_bar;
        list_tag_names_ = tag_names; list_selection_outline_ = selection_outline;
        auto_widths_scale_ = -1.0f;
    }
    bool ListSmartDate() const { return list_smart_date_; }
    // 滚动时固定上级 (Settings > 列表): details tree folders pinned over the list.
    void SetTreeSticky(bool on) { tree_sticky_ = on; }
    // 修改时间提示 (mtime_hint.h).
    // 鼠标与选择: single click opens (names show a band), item check boxes.
    void SetItemClick(bool single_click, int name_band, bool check_boxes) {
        if (single_click == click_single_ && name_band == click_band_ && check_boxes == item_checks_) return;
        click_single_ = single_click;
        click_band_ = name_band;
        item_checks_ = check_boxes;
        auto_widths_scale_ = -1.0f;
    }
    bool SingleClickOpen() const { return click_single_; }
    // Details rows keep a gutter for the check box left of the tree chevron.
    float CheckGutterDip(const PaneViewModel& vm) const;
    // Tree indent plus the check gutter: how far a Details row's icon and name move right.
    float DetailsLeadDip(const PaneViewModel& vm, int view_index) const;
    // The check box of a row (Details: in the gutter at details_base_x; icon grids: the cell's corner).
    D2D1_RECT_F ItemCheckRect(const PaneViewModel& vm, const D2D1_RECT_F& cell, float details_base_x) const;
    D2D1_RECT_F HeaderCheckRect(float columns_left, float header_top) const;
    void SetMtimeHint(const MtimeHint& hint) {
        if (hint == mtime_hint_) return;
        mtime_hint_ = hint;
        auto_widths_scale_ = -1.0f;
        mtime_next_boundary_ = 0;
    }
    // A drawn mark's bucket changed (5 minutes passed, midnight, ...): repaint.
    bool MtimeBoundaryDue() {
        if (mtime_next_boundary_ == UINT64_MAX) return false;
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        const uint64_t now = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        if (now < mtime_next_boundary_) return false;
        mtime_next_boundary_ = UINT64_MAX;
        return true;
    }
    // Grid thumbnails: default-program badge and video playing time.
    void SetThumbnailBadges(bool on) { thumbnail_badges_ = on; }
    // Default programs changed (SHCNE_ASSOCCHANGED).
    void InvalidateOpenWithIcons() { open_with_icons_.InvalidateAssociations(); }
    // List-row hover buttons the user keeps: bit 0 star, bit 1 new tab, bit 2 more.
    // Bits 0-2 visible buttons, bits 3-8 their order (app::RowActionMask).
    void SetRowActions(unsigned mask) { row_actions_ = mask & 0x1FFu; }
    // Optional details columns (details_column_set.h bits).
    void SetDetailsColumns(uint32_t mask) { details_columns_ = NormalizeDetailsColumns(mask); }
    uint32_t DetailsColumnsMask() const { return details_columns_; }
    // Which stored slot holds a column's manual width (-1: flexible / none).
    // Folder views: DetailsColumnWidths order. Search views: 0 name, 1 modified,
    // 2 type, 3 size.
    static int ManualColumnSlot(ColumnKind kind, bool search_view) noexcept;
    // Columns shown by the last painted Details header of a pane: bit
    // (1 << ColumnKind), plus bit 8 for the two-line search layout. 0 = unknown.
    uint32_t PaintedColumnMask(int pane_index) const {
        return pane_index >= 0 && pane_index < static_cast<int>(painted_columns_.size())
            ? painted_columns_[static_cast<size_t>(pane_index)] : 0u;
    }
    // Whether the hovered row's name was drawn shortened in the pane's last
    // painted frame. Unknown (that row not painted as hovered yet) counts as
    // shortened, so the full-name tooltip is never lost.
    bool HoveredNameTruncated(int pane_index, int source_index) const {
        if (pane_index < 0 || pane_index >= static_cast<int>(hover_names_.size())) return true;
        const HoverNamePaint& painted = hover_names_[static_cast<size_t>(pane_index)];
        return painted.source != source_index || painted.truncated;
    }
    // Double-click on a divider: drop the manual widths on both sides so the
    // columns return to their fitted widths.
    void AutoFitColumnDivider(const D2D1_RECT_F& pane_bounds,
                              DetailsColumnWidths& dividers, bool search_view,
                              std::array<float, 4>& search_dividers, int divider_index,
                              bool status_column = false) const;
    DetailsColumnLayout DetailsColumns(
        const D2D1_RECT_F& pane_bounds,
        const DetailsColumnWidths& dividers = {},
        bool search_view = false,
        const std::array<float, 4>& search_dividers = {},
        bool status_column = false) const;
    DetailsColumnLayout DetailsColumns(const D2D1_RECT_F& pane_bounds,
                                       const PaneViewModel& vm) const;
    DetailsColumnWidths ResizeDetailsColumnDivider(
        const D2D1_RECT_F& pane_bounds,
        const DetailsColumnWidths& dividers,
        int divider_index, float cursor_x, bool status_column = false) const;
    std::array<float, 4> ResizeSearchColumnDivider(
        const D2D1_RECT_F& pane_bounds,
        const std::array<float, 4>& dividers,
        int divider_index, float cursor_x) const;

    D2D1_RECT_F NameCellRect(const D2D1_RECT_F& pane_bounds, int view_row, float scroll_y,
                             float extra_top = 0.0f, ViewMode mode = ViewMode::Details,
                             float scroll_x = 0.0f, size_t item_count = 0,
                             const DetailsColumnWidths& column_dividers = {},
                             bool search_view = false,
                             const std::array<float, 4>& search_dividers = {},
                             float row_height_px = 0.0f) const;
    // How far the focused pane's expanded name (the single selected icon-grid
    // item's whole name) ran past the bottom of the list in the last frame,
    // and whose it was; 0 and -1 when it fit or none was shown.
    float ExpandedNameOverflow(int* source_index) const noexcept {
        if (source_index) *source_index = expanded_overflow_index_;
        return expanded_overflow_px_;
    }
    bool PointInItemName(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                         int source_index, float x, float y) const;
    // The item's drag handle: icon + name in Details / List / Small icons
    // (the rest of the row starts a marquee, like File Explorer). Icon grids
    // and the photo wall: an unselected item drags from the middle of its
    // picture and its name text, a selected one from the whole picture.
    // The whole cell in the other views.
    bool PointInItemGrip(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                         int source_index, float x, float y, bool selected = false) const;
    // Details tree: view row whose chevron is under (x, y), else -1.
    // The folders pinned over the top of a details tree list (see
    // PinnedTreeRows); top is the list top, so pin k spans top + y .. + row_h.
    struct StickyTree {
        std::vector<TreePin> pins;
        float top = 0.0f;
        float row_h = 0.0f;
        float Bottom() const { return pins.empty() ? top : top + pins.back().y + row_h; }
    };
    StickyTree StickyTreeRows(const PaneViewModel& vm, const D2D1_RECT_F& list) const;
    int TreeChevronAt(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                      float x, float y) const;
    // Details tree "more" row at view_row: 0 show more, 1 show all,
    // 2 open in a new tab, -1 elsewhere on the row.
    int TreeMoreActionAt(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                         int view_row, float x, float y) const;
    // Exact geometry of the Fluent frame drawn for the rename row; the hosted
    // EDIT control is placed inside this rect (must stay in sync with DrawList).
    D2D1_RECT_F RenameFieldRect(const PaneViewModel& vm, const D2D1_RECT_F& list,
                                int source_index);
    D2D1_RECT_F SidebarRect(float w, float h) const;
    D2D1_RECT_F StagingTrayRect(const WindowViewModel& vm, float w, float h) const;
    D2D1_RECT_F TitleBarRect(float w) const;
    D2D1_RECT_F ToolbarRect(float w) const;
    D2D1_RECT_F AddressBarRect(float w) const;
    // The 别名 suggestion list under the path editor, and the row at (x, y) or -1.
    D2D1_RECT_F AddressSuggestionsRect(float w, size_t count) const;
    int AddressSuggestionAt(float w, size_t count, float x, float y) const;
    // The # completion card under the filter box: a row, -2 its padding, -1 outside.
    D2D1_RECT_F FilterTagSuggestionsRect(const D2D1_RECT_F& anchor, float w, size_t count) const;
    int FilterTagSuggestionAt(const D2D1_RECT_F& anchor, float w, size_t count, float x, float y) const;
    D2D1_RECT_F SearchBarRect(float w) const;
    D2D1_RECT_F NewCommandRect(float w) const;
    // Chevron segment of the split New button (opens the New menu); the rest creates a folder.
    D2D1_RECT_F NewArrowRect(float w) const;
    D2D1_RECT_F SplitCommandRect(float w) const;
    D2D1_RECT_F AddressSearchButtonRect(float w) const;
    void DrawAddressSearchChrome(const WindowViewModel& vm, float w, const Theme& theme);
    float NewButtonWidthPx(bool compact) const;
    D2D1_RECT_F NewArrowSegment(const D2D1_RECT_F& create, bool compact) const;
    static constexpr const wchar_t* kNewFolderGlyph = L"\xE8F4";

    // One placed breadcrumb segment. A path too long for the bar is one strip
    // that scrolls under the wheel: segments outside `BreadcrumbStrip::view`
    // are still placed (clipped when drawn, skipped by hit testing).
    struct BreadcrumbPlaced {
        D2D1_RECT_F rc{};
        std::wstring text;
        std::wstring path;
        bool alias = false;
        bool ghost = false;  // past the current folder, to a selected tree row's folder
    };
    // Shared by DrawToolbar, HitTest and the drop-target logic so the three
    // can never disagree about segment positions.
    struct BreadcrumbStrip {
        D2D1_RECT_F view{};     // the part of the address bar the strip shows through
        float overflow = 0.0f;  // strip width past the view (0: the path fits)
        float offset = 0.0f;    // shown scroll: 0 = the end in view, overflow = the root
        float width = 0.0f;     // whole strip
    };
    void BreadcrumbLayout(const PaneViewModel& vm, float w,
                          std::vector<BreadcrumbPlaced>& out, BreadcrumbStrip* strip = nullptr) const;
    // Address bar out of edit mode, path longer than the bar: the wheel
    // (`steps` = notches, positive = toward the root) scrolls it. False when
    // the path fits, the bar is being edited or the point is outside it.
    bool ScrollBreadcrumb(const WindowViewModel& vm, float window_w, float x, float y, float steps);
    // Files dragged to either end of a scrolled path: page that way (paced).
    bool DragScrollBreadcrumb(const WindowViewModel& vm, float window_w, float x);
    // The pointer left the address bar 1.5 s ago: scroll back to the end.
    // Called from the UI timer; true when it started moving back.
    bool SettleBreadcrumbScroll(uint64_t now_ms);

    void Render(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void InvalidateWallpaper() {
        material_.Invalidate();
        wallpaper_path_.clear();
        wallpaper_path_resolved_ = false;
    }

    HitTestResult HitTest(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                          float x, float y) const;
    // Settings slider value under x (0 transparency, 1 blur), clamped to the
    // track so a drag may leave the control.
    // Pulse menu preview drag: where row `item` starts along the drag axis
    // (NaN when it is not in the preview), and the order a drop at (x, y)
    // gives (empty when the pointer is too far outside the preview).
    float SettingsMenuRowStart(const WindowViewModel& vm, const D2D1_RECT_F& rect, int item);
    // 系统右键菜单 live preview: resting top of an item, and where a drag released at (x, y) lands.
    float SysCtxPreviewItemTop(const WindowViewModel& vm, const D2D1_RECT_F& rect, int item);
    int SysCtxDropZone(const WindowViewModel& vm, const D2D1_RECT_F& rect, float x, float y);
    app::BuiltinMenuOrder SettingsMenuDropOrder(const WindowViewModel& vm, const D2D1_RECT_F& rect, float x, float y);
    int SettingsSliderValueAt(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                              int which, float x) const;

    bool PaneScrollbarGeometry(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                               D2D1_RECT_F& track, D2D1_RECT_F& thumb, float& max_scroll) const;
    float MaxScrollForPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const;
    float MaxScrollXForPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const;
    D2D1_RECT_F ItemRectInPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                               int view_index) const;
    int MoveViewIndex(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                      int current, int dx, int dy) const;
    int PageDelta(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const;
    std::pair<int, int> VisibleRangeInPane(const PaneViewModel& vm,
                                           const D2D1_RECT_F& pane_bounds) const;
    // Source indices of items meeting rect (pane pixels), on or off screen.
    void ItemsInRectInPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                           const D2D1_RECT_F& rect, std::vector<int>& sources) const;
    int RowFromYInPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds, float y) const;
    int ItemFromPointInPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                            float x, float y) const;
    // Current layout rect of a tag sidebar slot (for drag-reorder geometry).
    bool TagItemRect(const WindowViewModel& vm, float w, float h, int group, int item,
                     D2D1_RECT_F* out) const;
    // Current layout rect of a row, addressed by logical section id + row index
    // (for the quick-access pin drag).
    bool SidebarRowRect(const WindowViewModel& vm, float w, float h, int section_id, int item,
                        D2D1_RECT_F* out) const;
    // Vertical span of every laid-out sidebar section, in display order (for the
    // section header drag).
    void SidebarGroupBands(const WindowViewModel& vm, float window_w, float window_h,
                           std::vector<SidebarGroupBand>& out) const;
    // Rest-slot rect of a title-bar tab (display index, no drag float).
    bool TabItemRect(const WindowViewModel& vm, float window_w, int index, D2D1_RECT_F* out) const;
    // Group chip rect (title-bar space); false when the group has no chip.
    bool TabGroupChipRect(const WindowViewModel& vm, float window_w, int group_index,
                          D2D1_RECT_F* out) const;
    // Uniform tab pitch (excludes group-chip offsets); used by drag math.
    float TabPitchPx(const WindowViewModel& vm, float window_w) const;
    // Overflowing tab strip: wheel over the strip (`steps` = wheel notches,
    // positive = toward the first tab) and arrow pages. False when the strip
    // fits or the point is outside it.
    bool ScrollTabStrip(const WindowViewModel& vm, float window_w, float x, float y, float steps);
    void ScrollTabStripPage(const WindowViewModel& vm, float window_w, int direction);
    // Closing tabs keeps the current widths until the pointer leaves the strip,
    // so repeated clicks land on the next close button (as in Chrome).
    void FreezeTabWidths(const WindowViewModel& vm, float window_w);
    bool ReleaseTabWidths();
    // Settings > 快捷键: how far to scroll so the open popover is in view (0: it is).
    float SettingsShortcutsRevealDelta(const WindowViewModel& vm, float window_w, float window_h) const;
    float SettingsMaxScroll(const WindowViewModel& vm, float window_w, float window_h) const;
    // The settings page's scrollbar as drawn (false: the page fits, no bar).
    bool SettingsScrollbarGeometry(const WindowViewModel& vm, float window_w, float window_h,
                                   D2D1_RECT_F& track, D2D1_RECT_F& thumb, float& max_scroll) const;
    D2D1_RECT_F SettingsDropdownBounds(const WindowViewModel& vm, int index, float window_w, float window_h) const;
    float SettingsDestinationOffset(const WindowViewModel& vm, int setting_id, float window_w, float window_h) const;
    float SidebarMaxScroll(const WindowViewModel& vm, float window_w, float window_h) const;
    bool SidebarScrollbarGeometry(const WindowViewModel& vm, float window_w, float window_h,
                                  D2D1_RECT_F& track, D2D1_RECT_F& thumb, float& max_scroll) const;
    // How many deck cards the tray panel fits at the current sidebar width
    // without breaking the max-50%-overlap rule (>= 1).
    int TrayDeckCapacity(float window_w) const;

private:
    UINT caret_due_ms_ = 0;
    DWORD shortcut_caret_epoch_ = 0;
    bool shortcut_caret_focus_ = false;
    std::wstring shortcut_caret_query_;
    struct TabStripMetrics {
        float x0 = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
        float pitch = 0.0f;
        float end_x = 0.0f;
        std::vector<float> extra; // per tab: px shift from group chips before it
        struct Chip {
            float left = 0.0f;
            float width = 0.0f;
            int group = -1; // index into WindowViewModel::tab_groups
        };
        std::vector<Chip> chips;  // one chip at the start of each same-group run
        // Too many tabs for the strip: they stop at the minimum width with
        // their titles, the active one a little wider (`active_w`) for its
        // close button, and the strip scrolls between two arrows (`overflow`).
        int active = -1;          // visible, unpinned active tab, or -1
        float active_w = 0.0f;
        float pinned_w = 0.0f;
        bool overflow = false;
        float view_left = 0.0f, view_right = 0.0f; // tab clip span when overflowing
        float arrow_w = 0.0f;
        float scroll = 0.0f, max_scroll = 0.0f;
        float new_x = 0.0f;       // "+" button left edge
        float Width(const WindowViewModel& vm, size_t i) const {
            if (vm.tabs[i].pinned) return pinned_w;
            return static_cast<int>(i) == active ? active_w : w;
        }
    };
    TabStripMetrics ComputeTabStrip(const WindowViewModel& vm, float window_w) const;
    void DrawTitleBar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawToolbar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawFavoritesBar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawToolbarApps(const WindowViewModel& vm, const ToolbarLayout& layout, const Theme& theme);
    void DrawSidebar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawPane(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawSpringRing(ID2D1DeviceContext* dc, const D2D1_RECT_F& icon, float progress, const Theme& theme);
    void DrawSplitterKnob(int index, const SplitterView& sp, bool hovered, bool pressed, bool dark,
                          const Theme& theme);
    void DrawDetailsPanel(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawSinglePane(const WindowViewModel& vm, const PaneViewModel& pane,
                        const D2D1_RECT_F& bounds, int pane_index, bool focused, bool target,
                        const Theme& theme);
    void DrawPaneEmptyState(const WindowViewModel& vm, const PaneViewModel& pane,
                            const D2D1_RECT_F& bounds, int pane_index, const Theme& theme);
    void DrawColumnStrip(const WindowViewModel& vm, const PaneViewModel& pane,
                         const D2D1_RECT_F& bounds, int pane_index, const Theme& theme);
    void DrawColumnStripColumn(const WindowViewModel& vm, const PaneViewModel& pane,
                               const ColumnStripColumnView& column, int column_id,
                               const D2D1_RECT_F& rc, int pane_index, const Theme& theme);
    void DrawPaneHeaderIcon(const D2D1_RECT_F& rc, PaneHeaderIcon icon, const D2D1_COLOR_F& color);
    // label_at: its left edge is where the size label ends, its middle the label's.
    void DrawPaneSizeSlider(const PaneViewModel& pane, int pane_index, const D2D1_RECT_F& slider,
                            const D2D1_RECT_F& label_at, bool hovered, const Theme& theme);
    // The rename field's note card, under the field (above it near the bottom).
    void DrawRenameHint(const PaneViewModel& vm, const D2D1_RECT_F& field, const D2D1_RECT_F& view,
                        const Theme& theme);
    bool HitTestColumnStrip(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                            float x, float y, HitTestResult& out) const;
    void DrawStatusBar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawAddressSuggestions(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawFilterTagSuggestions(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawTeachBubble(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    // File-operation capsule in the status bar: ring progress while running,
    // accent check on success, then settles back to plain summary text.
    void DrawTaskPill(const WindowViewModel& vm, const D2D1_RECT_F& area, const Theme& theme);
    void DrawSettings(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawSettingsContext(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawSettingsPulseMenu(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawSettingsCore(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawSettingsPacks(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawSettingsSysCtx(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawSettingsShortcuts(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawSettingsAi(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    // 右键菜单 layer cards, the same on pages 2 and 6: Pulse 右键菜单 | 系统右键菜单.
    void DrawCtxLayerCards(const D2D1_RECT_F& pulse, const D2D1_RECT_F& system, bool system_current,
                           bool other_hovered, const Theme& theme);
    IDWriteTextFormat* SysCtxTitleFormat();  // 13 dip semibold (creates the 系统右键菜单 formats)

    void DrawList(const PaneViewModel& vm, float x, float y, float w, float h, const Theme& theme,
                  int hover_region = 0, int hover_control_index = -1, bool pane_focused = true,
                  int pane_index = 0);
    // Grid views: queue the thumbnails of the screen below the visible one.
    void PrefetchNextScreen(const PaneViewModel& vm, int first, int last, uint32_t pixels, int pane_index);
    // Placeholder rows with a soft shimmer for slow (network) folders.
    void DrawListSkeleton(const D2D1_RECT_F& viewport, const Theme& theme, int pane_index);
    void DrawScrollbar(const PaneViewModel& vm, float x, float y, float w, float h, const Theme& theme);
    // "Group by" headers over a grouped details/content list (sticky top one).
    void DrawGroupHeaders(const PaneViewModel& vm, const ViewLayout& layout,
                          const D2D1_RECT_F& viewport, const Theme& theme,
                          int hover_region, int hover_control_index, int pane_index);
    static std::wstring GroupLabel(const PaneViewModel& vm, const ListGroup& g);
    int group_hover_pane_ = -1;   // pane under the mouse for header hover
    // Scatter deck of staged files inside the tray panel (draw + hit-test
    // share the geometry helpers in ui_renderer.cpp).
    IDWriteTextFormat* ShelfFormat(int role) const;
    float ShelfMeasure(const std::wstring& text, bool compact_text) const;
    void DrawTrayShelf(const WindowViewModel& vm, const D2D1_RECT_F& panel_rc,
                       const Theme& theme);
    void DrawTrayShelfLayer(const WindowViewModel& vm, int stage, const D2D1_RECT_F& panel_rc,
                            const Theme& theme);
    // 剪贴板 N 项 · 收入: the strip button (row false) or the shelf row.
    void DrawShelfClip(const WindowViewModel& vm, const D2D1_RECT_F& rc, bool row, const Theme& theme);
    void DrawTrayOpen(const WindowViewModel& vm, const D2D1_RECT_F& panel_rc, const Theme& theme);
    // Two checked files side by side, right of the sidebar (after the panes).
    void DrawTrayCompareCard(const WindowViewModel& vm, const Theme& theme);
    void DrawTrayDeck(const WindowViewModel& vm, const D2D1_RECT_F& panel_rc,
                      const Theme& theme);

    void UpdateBrushes(const Theme& theme);
    void DrawButton(const D2D1_RECT_F& rc, const Theme& theme, const D2D1_COLOR_F& bg,
                    const std::wstring& glyph, const std::wstring& fallback,
                    const D2D1_COLOR_F& fg, bool round_right = false, bool round_left = false,
                    float size_factor = 1.0f);
    void DrawIconText(float x, float y, float w, float h, const std::wstring& glyph,
                      const std::wstring& fallback, const D2D1_COLOR_F& color, float size_factor = 1.0f);
    void DrawTextRect(ID2D1DeviceContext* dc, IDWriteTextFormat* format,
                      ID2D1SolidColorBrush* brush, std::wstring_view text,
                      float x, float y, float width, float height,
                      D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_CLIP);
    // The extension inside a type chip, centered in its box in every text
    // rendering mode (Auto draws with LumaText, Sharp / Smooth with DirectWrite).
    void DrawTypeChipLabel(ID2D1DeviceContext* dc, const std::wstring& chip, const D2D1_RECT_F& box,
                           const D2D1_COLOR_F& ink, const D2D1_COLOR_F& background);
    void DrawFolderIcon(float x, float y, float size, const Theme& theme,
                        float opacity = 1.0f);
    // Slate-gray folder for a folder with nothing to show (same silhouette as
    // the shell folder, no paper). Fills [x, x+size] like DrawFolderIcon.
    void DrawEmptyFolderIcon(float x, float y, float size, const Theme& theme,
                             float opacity = 1.0f);
    PreviewDrawResult DrawEntryThumbnail(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
        const ListEntryView& entry, ViewMode mode, uint64_t generation, uint32_t pixels,
        float opacity, bool align_bottom, D2D1_RECT_F* artwork, uint32_t* duration = nullptr);
    void DrawSidebarPeek(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawFileIcon(float x, float y, float size, const Theme& theme,
                      float opacity = 1.0f);
    void DrawEntryIcon(const ListEntryView& entry, float x, float y, float size,
                       const Theme& theme, float opacity = 1.0f);
    void DrawLinkOverlay(float x, float y, float size, const Theme& theme, float opacity = 1.0f,
                         const std::wstring& label = {}, float expansion = 0.0f,
                         float right_limit = 0.0f, const D2D1_RECT_F* artwork = nullptr);
    // Grid thumbnail corners: the default program's icon bottom-right, and a
    // playing-time chip (videos) or "GIF" chip bottom-left. `artwork` is the
    // drawn image, `icon_size` the item's icon square, both in pixels.
    void DrawThumbnailBadges(const ListEntryView& entry, const D2D1_RECT_F& artwork, float icon_size,
                             uint32_t duration_ms, const Theme& theme);
    // Cloud-file state on a grid item in a cloud folder: a round plate on the
    // artwork's bottom-left (shell icons) or inside its top-left (pictures).
    void DrawCloudBadge(const ListEntryView& entry, const D2D1_RECT_F& artwork, bool picture,
                        const Theme& theme, float opacity);
    // Photo wall picture tile over its thumbnail: a hairline edge, the dark
    // fade its name sits on while hovered or selected, the selection ring.
    void DrawWallTileChrome(const D2D1_RECT_F& tile, bool hover, bool selected, const Theme& theme);
    // The photo wall suggestion card (PaneWallHintRects) over the list.
    void DrawPaneWallHint(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                          int hover_region, int hover_index, const Theme& theme);
    // One item's icon mid view-switch (ui_view_morph.h): thumbnail and shell
    // icon cross-fade while the rect travels. dx/dy: the row transform.
    void DrawMorphIcon(const ListEntryView& entry, const PaneViewModel& vm,
                       const motion::ViewMorphMotion::Sample& sample, float dx, float dy,
                       const D2D1_RECT_F& target_icon, ViewMode from_mode,
                       const Theme& theme);
    // The previous view's labels, and items that have no place in the new
    // view, fading out where they were at the start of a view switch.
    void DrawMorphFrom(const PaneViewModel& vm, const motion::ViewMorphMotion& morph,
                       const D2D1_RECT_F& viewport, const Theme& theme);
    bool DrawEmptyStateSvg(const D2D1_RECT_F& bounds, float opacity = 1.0f);
    bool EnsureEmptyStateSvg();
    bool DrawNoSelectionSvg(const D2D1_RECT_F& bounds, float opacity = 1.0f);
    bool EnsureNoSelectionSvg();
    bool DrawCuratedEmptyStateSvg(bool starred, const D2D1_RECT_F& bounds,
                                  float opacity = 1.0f);
    bool EnsureCuratedEmptyStateSvg(bool starred);
    bool DrawExcludeEmptySvg(const D2D1_RECT_F& bounds, float opacity = 1.0f);
    bool EnsureExcludeEmptySvg();
    bool EnsureFluentSvg(int resource_id, bool colorful = false);
    bool DrawFluentSvg(int resource_id, const D2D1_RECT_F& bounds, float opacity = 1.0f,
                       const D2D1_COLOR_F* foreground = nullptr, bool colorful = false);
    // Returns true when the name had to be shortened with an ellipsis.
    bool DrawTruncatedName(const std::wstring& name, float x, float y, float w, float h,
                           const Theme& theme, bool selected, const std::vector<NameMatchRange>& matches, bool dim_extension = false);
    // Icon-grid name: up to `max_lines` centered lines; a longer name keeps
    // its end ("a…z.ext"). Returns (and sets `truncated`) when it was elided.
    bool DrawCenteredIconName(const std::wstring& name, const D2D1_RECT_F& bounds,
                              const D2D1_COLOR_F& color, const Theme& theme,
                              const std::vector<NameMatchRange>& matches, bool* truncated = nullptr,
                              int max_lines = kIconGridNameLines, float* text_height = nullptr,
                              bool dim_extension = false);
    void DimNameExtension(IDWriteTextLayout* layout, const std::wstring& name,
                          const std::wstring& shown, const D2D1_COLOR_F& color);
    // The selected item's whole name (up to 6 lines) on an opaque plate that
    // covers the row below.
    float DrawExpandedIconName(const std::wstring& name, const D2D1_RECT_F& bounds,
                              const D2D1_COLOR_F& color, const Theme& theme,
                              const std::vector<NameMatchRange>& matches, bool pane_focused,
                              const D2D1_RECT_F& view, const std::wstring& note = {},
                              const D2D1_COLOR_F& note_color = {}, bool dim_extension = false);
    // Title-bar product mark from the app icon resource (nullptr until loaded).
    ID2D1Bitmap* LogoBitmap();

    WindowMaterial material_;
    // Windows builds without a DWM backdrop sample the desktop wallpaper so the
    // Mica/Acrylic tiles still change the window. Resolved once per wallpaper.
    const std::wstring& WallpaperPath();
    std::wstring wallpaper_path_;
    bool wallpaper_path_resolved_ = false;
    Compositor* compositor_ = nullptr;
    float expanded_overflow_px_ = 0.0f;
    int expanded_overflow_index_ = -1;
    fluent::Painter painter_;
    // Icon and thumbnail caches (render_caches.h): shared through AppCore, or
    // owned when the renderer stands alone. The member names alias into them.
    std::unique_ptr<RenderCaches> owned_caches_;
    RenderCaches& caches_;
    ShellIconCache& icon_cache_ = caches_.icons;
    OpenWithIconCache& open_with_icons_ = caches_.open_with_icons;
    std::unordered_map<ID2D1Bitmap*, ComPtr<ID2D1Effect>> tray_shadows_;
    ThumbnailCache& thumbnail_cache_ = caches_.thumbnails;
    FolderThumbnailCache& folder_thumbnail_cache_ = caches_.folder_thumbnails;
    bool folder_thumbnails_enabled_ = true;
    ThumbnailCache& details_cache_ = caches_.details;
    ThumbnailCache& hover_cache_ = caches_.hover;
    PreviewHandlerHost preview_handler_;
    HWND notify_hwnd_ = nullptr;
    bool embedded_ = false;
    bool favorites_bar_ = false;
    float embedded_top_dip_ = 0.0f;
    float embedded_bottom_dip_ = 0.0f;
    float scale_ = 1.0f;
    float title_bar_height_ = kTitleBarHeight;
    float toolbar_height_ = 88.0f;
    float status_height_ = 28.0f;
    float sidebar_width_ = 224.0f;
    float sidebar_width_dip_ = 224.0f;
    bool vertical_tabs_ = false;
    float search_min_dip_ = 0.0f;
    bool search_hidden_ = false;
    int toolbar_group_ = -1;
    bool toolbar_show_sort_ = true;
    bool toolbar_show_filter_ = true;
    bool toolbar_show_group_ = true;
    int new_primary_ = 0;
    bool new_last_text_ = false;
    int pin_count_ = 0;
    int pin_app_from_ = -1;
    uint64_t pin_separators_ = 0;
    bool pin_drop_mode_ = false;
    std::wstring pin_drop_label_;
    // Device bitmaps of the pinned apps' icons, keyed by their pixel buffers.
    struct AppIconEntry {
        common::MenuIconRef pixels;
        ComPtr<ID2D1Bitmap> bitmap;
    };
    std::vector<AppIconEntry> app_icon_bitmaps_;
    ID2D1DeviceContext* app_icon_dc_ = nullptr;
    ID2D1Bitmap* AppIconBitmap(const common::MenuIconRef& pixels);
    int selection_count_ = 0;
    int selection_actions_ = 0;
    bool sidebar_collapsed_ = false;   // effective: the user's choice or the docked assistant
    bool user_sidebar_collapsed_ = false;
    bool ai_rail_ = false;             // docked Pulse 助手 holds the sidebar on its rail
    AiPanelView ai_panel_;
    void ApplySidebarCollapse(bool animate);
    bool sidebar_peek_ = false;
    bool collapse_anim_ = false;
    GroupWheel group_wheel_;
    WindowOverviewView window_overview_;
    WindowSwitcherView window_switcher_;
    float collapse_value_ = 0.0f;  // 0 expanded .. 1 rail (frame-stable)
    float collapse_from_ = 0.0f;
    uint64_t collapse_start_ = 0;  // motion::NowMs()
    static constexpr float kSidebarCollapseMs = 200.0f;
    float pane_header_height_ = 40.0f;
    float focused_banner_height_ = 0.0f;
    float column_header_height_ = 32.0f;
    float row_height_ = 34.0f;
    float row_height_dip_ = 34.0f;
    bool list_smart_date_ = true, list_zebra_ = true, list_size_bar_ = false;
    bool list_tag_names_ = false;
    bool list_selection_outline_ = false;
    bool tree_sticky_ = true;
    bool thumbnail_badges_ = true;
    unsigned row_actions_ = 7u;
    uint32_t details_columns_ = kDetailsColumnsDefault;
    // Motion state: highlight plates glide between items (ui_motion.h).
    uint64_t motion_frame_ = 0;
    uint64_t motion_now_ = 0;  // motion::NowMs() sampled once per Render
    motion::RectMotion sidebar_pill_;
    motion::RectMotion settings_nav_pill_;
    motion::RectMotion settings_nav_hover_;
    std::array<motion::RectMotion, 8> list_hover_motion_{};
    std::array<LinkPillMotion, 8> link_pill_motion_{};
    std::array<motion::ListShiftMotion, 8> list_shift_{};
    std::array<motion::ViewMorphMotion, 8> view_morph_{};
    // Motion v2 "nav": after a pane opens another folder its rows come in
    // from 16 px on the side it was entered from (back/up: the left), a
    // short stagger apart. start 0 = idle; pending until the listing lands.
    struct NavEnter {
        std::wstring path;
        uint64_t start = 0;
        int dir = 0;   // +1 deeper, -1 to an ancestor, 0 elsewhere (rises 6 px)
        bool pending = false;
    };
    std::array<NavEnter, 8> nav_enter_{};
    // Motion v2 "colresize": double-clicking a divider springs the columns
    // to their fitted widths (from = what was drawn), and dragging one shows
    // a guide line plus a badge with the width and its change.
    struct ColumnFit {
        uint64_t start = 0;
        int count = 0, last_count = 0;
        std::array<float, kMaxDetailsColumns> from{}, last{};
    };
    std::array<ColumnFit, 8> col_fit_{};
    // Motion v2 "newfolder": the inline rename field pops in (frame 96% ->
    // 100% on Pop, an accent ripple leaving it).
    struct RenamePop {
        int index = -1;
        uint64_t since = 0;
    };
    std::array<RenamePop, 8> rename_pop_{};
    // Motion v2 "nav": breadcrumb segments that were not shown before slide
    // in 10 dip from the right, 30 ms apart (smooth spring). Value: start.
    std::vector<std::wstring> crumb_keys_;
    std::unordered_map<std::wstring, uint64_t> crumb_born_;
    DetailsColumnLayout AnimateColumns(DetailsColumnLayout target, int pane_index);
    struct ColumnBadge {
        bool on = false;
        int pane = -1, divider = -1;
        float w0 = 0.0f, w = 0.0f, guide_x = 0.0f, guide_top = 0.0f, guide_bottom = 0.0f;
        uint64_t since = 0, released = 0;
        std::wstring name;
    } col_badge_;
    void DrawColumnBadge(int pane_index, const Theme& theme);
    std::array<uint64_t, 8> list_loading_since_{};
    std::array<uint64_t, 8> prefetch_signature_{};  // per pane: what was last prefetched
    bool list_loading_animate_ = true;
    bool list_loading_active_ = false;
    // Tab strip overflow state: ComputeTabStrip clamps the scroll and reveals
    // a newly active tab, so hit testing and drawing always agree.
    mutable float tab_scroll_ = 0.0f;
    mutable int tab_scroll_active_ = -1;
    // Address bar scroll (see ScrollBreadcrumb): target and shown offsets,
    // eased per frame; a new path starts at its end again.
    mutable float crumb_scroll_ = 0.0f;
    mutable float crumb_scroll_shown_ = 0.0f;
    mutable std::wstring crumb_scroll_key_;
    uint64_t crumb_scroll_tick_ = 0;     // last eased frame (motion clock)
    uint64_t crumb_hover_left_ = 0;      // GetTickCount64 when the pointer left (0: over it)
    uint64_t crumb_drag_tick_ = 0;       // drag paging pace
    float tab_freeze_w_ = 0.0f, tab_freeze_active_w_ = 0.0f;
    size_t tab_freeze_count_ = 0;
    std::array<uint64_t, 8> folder_size_pending_since_{};
    bool folder_size_anim_active_ = false;
    bool decorative_only_ = false;   // TickMotion: only the shimmer/skeleton/spinner run
    motion::CopyFeedback copy_feedback_;
    // Tab strip motion (pulse_motion_v2.html "Tabs"): slots spring when tabs
    // open or close, the active plate glides, new tabs pop in, closed ones
    // fade out, a lifted tab grows. Visual only: hit-testing keeps the slots.
    struct TabSlotMotion {
        motion::SpringRectMotion rect;
        uint64_t born = 0;     // motion::NowMs when it appeared; 0 = no pop
        uint64_t seen = 0;     // motion_frame_ it was last drawn
        D2D1_RECT_F last{};    // where it was last drawn
        std::wstring title;
    };
    struct TabGhost {
        std::wstring title;
        D2D1_RECT_F rc{};
        uint64_t start = 0;
    };
    std::unordered_map<uintptr_t, TabSlotMotion> tab_slots_;
    std::vector<TabGhost> tab_ghosts_;
    std::vector<uintptr_t> tab_ids_prev_;   // sorted visible tab ids of the last frame
    uint64_t tab_gen_ = 0;                  // bumps whenever that set changes
    motion::SpringRectMotion tab_plate_;
    uint64_t tab_lift_start_ = 0;
    uint64_t tab_motion_until_ = 0;         // NowMs: keep frames coming until then
    bool tab_anim_on_ = true;
    uint64_t row_leave_until_ = 0;          // NowMs: a deleted row is still fading out
    bool click_single_ = false;
    int click_band_ = 0;
    bool item_checks_ = false;
    MtimeHint mtime_hint_ = MtimeHint{0};  // off until the app applies its prefs (pickers stay plain)
    uint64_t mtime_next_boundary_ = UINT64_MAX;  // FILETIME the next drawn mark changes bucket
    uint64_t mtime_flash_until_ = 0;              // NowMs: a changed row is still glowing
    // Path -> (the modified time that glowed, NowMs it started).
    std::unordered_map<std::wstring, std::pair<uint64_t, uint64_t>> mtime_flash_started_;
    bool name_semibold_ = false;                  // DrawTruncatedName: 今天修改的名称加粗
    ComPtr<IDWriteTextFormat> name_semibold_format_;
    IDWriteTextFormat* name_semibold_base_ = nullptr;
    float name_semibold_size_ = 0.0f;
    // FileNameFormat at semibold (same family, size and fallback).
    IDWriteTextFormat* NameSemiboldFormat();
    // Extra date-column width: the dots' lead and room for a semibold date.
    float MtimeDateExtraDip() const {
        if (mtime_hint_.style == 0) return 0.0f;
        return (mtime_hint_.style == 1 ? kMtimeDotLeadDip : 0.0f) + (mtime_hint_.bold ? 6.0f : 0.0f);
    }
    // Pane dividers (motion 4b): an accent knob springs in on hover and
    // stretches while dragged; snaps flash a guide line.
    struct SplitterKnobMotion {
        float show_from = 0.0f, show_to = 0.0f;
        float grab_from = 0.0f, grab_to = 0.0f;
        uint64_t show_start = 0, grab_start = 0, snap_at = 0;
        bool snapped = false;
    };
    std::array<SplitterKnobMotion, 8> splitter_knobs_{};
    // Pane header view controls (P1e): the selected block of the view
    // segment, the size slider sliding in and out, and the size label shown
    // for a moment after the size changes.
    struct HeaderSizeMotion {
        motion::SpringRectMotion segment;
        motion::SpringRectMotion slider;
        D2D1_RECT_F slider_rc{};  // last shown slider, drawn clipped while it folds away
        float dip = 0.0f;
        uint64_t label_until = 0;
        std::wstring path;
    };
    std::array<HeaderSizeMotion, 8> header_size_motion_{};
    uint64_t splitter_motion_until_ = 0;
    uint64_t spring_ring_until_ = 0;        // NowMs: a spring-load ring is filling
    // Set while drawing by motion whose state lives outside this class (the
    // sidebar, toolbar, column strip and details panel files); cleared at the
    // start of every frame. Rides the same frame pump as everything else.
    mutable bool motion_frame_wanted_ = false;
    // Partial redraw state (PartialRedrawSafe and friends).
    bool ticks_active_ = false;          // copy feedback, group wheel, thumbnails
    bool frame_partial_ = false;
    D2D1_RECT_F frame_damage_{};
    bool last_frame_overlay_ = true;     // the last frame drew a floating overlay
    std::array<std::vector<std::pair<int, D2D1_RECT_F>>, 8> drawn_rows_{};  // source, cell
    // Panes whose last frame drew tree guides: a hover moves the highlighted
    // line, which spans rows outside the two that changed.
    std::array<bool, 8> tree_guides_drawn_{};
    bool OverlayUp(const WindowViewModel& vm) const;
    bool NonHoverMotionActive() const;
    mutable std::vector<D2D1_RECT_F> landing_rects_;   // this frame's landing rows (window px)
    void RequestMotionFrame() const { motion_frame_wanted_ = true; }
    static constexpr ULONGLONG kTaskPillDoneMs = 1600;
    static constexpr ULONGLONG kTaskPillFadeMs = 300;
    bool task_pill_was_active_ = false;
    bool task_pill_spinning_ = false;
    bool task_pill_animate_ = true;
    ULONGLONG task_pill_done_at_ = 0;
    ULONGLONG task_pill_pop_at_ = 0;     // finish pop (pulse_ops_dialogs.html pillpop)
    static constexpr ULONGLONG kTaskPillPopMs = 500;
    // With 撤销 / 打开 the finished pill stays 6 s (held while hovered).
    static constexpr ULONGLONG kTaskPillActionsMs = 6000;
    ULONGLONG task_pill_hold_ms_ = kTaskPillDoneMs;
    D2D1_RECT_F task_action_rects_[2]{};   // as last drawn; empty = not offered
    mutable ColumnAutoWidths auto_widths_{};
    mutable float auto_widths_scale_ = -1.0f;
    mutable std::wstring auto_widths_language_;
    mutable std::unordered_map<std::wstring, float> cell_text_widths_;
    mutable float cell_text_widths_scale_ = 0.0f;
    std::array<uint32_t, 8> painted_columns_{};
    struct HoverNamePaint { int source = -1; bool truncated = true; };
    std::array<HoverNamePaint, 8> hover_names_{};   // per pane, see HoveredNameTruncated
    float tray_icon_dip_ = 48.0f;
    // Staging tray card text: 13 px semibold name, 11 px folder line.
    mutable ComPtr<IDWriteTextFormat> tray_name_format_;
    // Staging tray shelf text formats (ui_tray_shelf.cpp ShelfFormat roles).
    static constexpr int kShelfFormatCount = 11;
    mutable D2D1_RECT_F tray_panel_rc_{};  // last painted tray panel (compare card anchor)
    mutable ComPtr<IDWriteTextFormat> shelf_formats_[kShelfFormatCount];
    mutable float shelf_formats_scale_ = 0.0f;
    mutable ComPtr<IDWriteTextFormat> tray_folder_format_;
    mutable float tray_formats_scale_ = 0.0f;
    // 系统右键菜单 card titles: 13 px semibold (Painter keeps its own private).
    mutable ComPtr<IDWriteTextFormat> sysctx_title_format_;
    mutable ComPtr<IDWriteTextFormat> sysctx_stat_format_;  // 概览 stat numbers: 22 px semibold
    mutable float sysctx_title_scale_ = 0.0f;
    float margin_ = 4.0f;
    float control_gap_ = 4.0f;
    D2D1_COLOR_F text_background_{};
    // Per-frame layer opacity (see ComputeLayerAlphas); 1 when opaque.
    float sheet_alpha_ = 1.0f;
    float card_alpha_ = 1.0f;
    // Pane cards over DWM material: copy-blended with pane_color_ (alpha
    // pane_alpha_); pane_alpha_ < 0 when they blend with card_alpha_.
    float pane_alpha_ = -1.0f;
    D2D1_COLOR_F pane_color_{1.0f, 1.0f, 1.0f, 1.0f};
    bool details_visible_ = false;
    bool details_anim_ = false;
    float details_value_ = 0.0f;   // 0 closed .. 1 open (frame-stable while animating)
    float details_from_ = 0.0f;
    uint64_t details_anim_start_ = 0;
    float details_width_ = 340.0f;
    PreviewViewport details_viewport_;
    std::wstring details_preview_identity_;
    bool details_preview_ready_ = false;
    bool details_preview_dragging_ = false;
    bool details_preview_drag_pending_ = false;
    bool details_preview_archive_ = false;
    ArchivePreview details_archive_;
    POINT details_preview_pointer_{};
    ULONGLONG details_zoom_label_until_ = 0;

    mutable ComPtr<ID2D1SolidColorBrush> brBg_;
    mutable ComPtr<ID2D1SolidColorBrush> brText_;
    mutable ComPtr<ID2D1SolidColorBrush> brTextSecondary_;
    mutable ComPtr<ID2D1SolidColorBrush> brTextDisabled_;
    mutable ComPtr<ID2D1SolidColorBrush> brFillHover_;
    mutable ComPtr<ID2D1SolidColorBrush> brFillPressed_;
    mutable ComPtr<ID2D1SolidColorBrush> brFillSelected_;
    mutable ComPtr<ID2D1SolidColorBrush> brFillInput_;
    mutable ComPtr<ID2D1SolidColorBrush> brNameExt_;   // icon-grid name extensions
    mutable ComPtr<ID2D1SolidColorBrush> brStrokeCard_;
    mutable ComPtr<ID2D1SolidColorBrush> brStrokeDivider_;
    mutable ComPtr<ID2D1SolidColorBrush> brAccent_;
    mutable ComPtr<ID2D1SolidColorBrush> brAccentHover_;
    mutable ComPtr<ID2D1SolidColorBrush> brAccentText_;
    mutable ComPtr<ID2D1SolidColorBrush> brTagDot_;
    mutable ComPtr<ID2D1SolidColorBrush> brDanger_;
    mutable ComPtr<ID2D1SolidColorBrush> brDangerHover_;
    mutable ComPtr<ID2D1SolidColorBrush> brScrollbar_;
    mutable ComPtr<ID2D1SolidColorBrush> brIconFolder_;
    mutable ComPtr<ID2D1SolidColorBrush> brIconFile_;
    mutable ComPtr<ID2D1StrokeStyle> dashStroke_;
    ComPtr<ID2D1StrokeStyle> roundCapStroke_;  // spring-load ring arc
    ComPtr<ID2D1StrokeStyle> paneHeaderStroke_;
    // Toolbar icons (DrawToolbar / DrawFavoritesBar): one stroke in pixels
    // whatever the icon's size, bounds on whole pixels, and the box DrawIconText
    // scales from (20 DIP elsewhere). icon_stroke_px_ 0 = off.
    float icon_stroke_px_ = 0.0f;
    float icon_box_dip_ = 20.0f;
    ComPtr<ID2D1PathGeometry> link_arrow_geometry_;
    ComPtr<ID2D1PathGeometry> play_triangle_geometry_;
    // Empty-folder icon: back/front in a 24-unit box, and a 96-unit large form.
    ComPtr<ID2D1PathGeometry> empty_folder_geometry_[4];
    mutable ComPtr<ID2D1SolidColorBrush> brEmptyFolder_;
    ID2D1DeviceContext* empty_folder_dc_ = nullptr; // owner of the two above
    mutable ComPtr<ID2D1SolidColorBrush> brFpsBg_;
    mutable ComPtr<ID2D1SolidColorBrush> brFpsText_;

    ComPtr<ID2D1Bitmap> logo_bitmap_;
    ComPtr<IDWriteTextFormat> preview_mono_format_;
    // The hover card's silent video (ui_hover_preview.cpp).
    // shared_ptr: created in ui_hover_preview.cpp, so targets that build the
    // renderer without the card never need the player.
    std::shared_ptr<VideoPreview> hover_video_;
    std::wstring hover_video_path_;
    ComPtr<ID2D1Bitmap> hover_frame_bitmap_;   // FFmpeg frames, reused while the size stays
    ComPtr<ID2D1Device> hover_frame_device_;
    uint64_t hover_frame_serial_ = ~0ull;
    unsigned hover_frame_ms_ = 0;
    bool hover_video_failed_ = false;          // this path cannot play: keep the poster
    // The card's shape (ui_hover_preview.cpp): what earlier frames learned of
    // this path's preview, and the size easing toward it (px, 0 = closed).
    std::wstring hover_shape_path_;
    int hover_shape_kind_ = 0;
    float hover_shape_aspect_ = 0.0f;
    float hover_card_w_ = 0.0f, hover_well_h_ = 0.0f;
    unsigned long long hover_ease_ms_ = 0;
    bool hover_bench_ = false;                 // --bench-hover owns the cache and player
    // A folder's card (ui_hover_preview.cpp), and where the last card went.
    void DrawHoverFolderCard(const HoverPreviewView& view, const D2D1_RECT_F& client,
                             const Theme& theme, bool high_contrast);
    D2D1_RECT_F hover_card_rect_{}, hover_list_rect_{};
    float hover_scroll_max_ = 0.0f;
    ComPtr<IDWriteTextLayout> details_text_layout_;
    std::wstring details_layout_text_;
    float details_layout_scale_ = 0;
    std::unordered_map<int, ComPtr<IDWriteTextFormat>> sized_icon_formats_;
    ComPtr<ID2D1DeviceContext5> empty_state_svg_dc_;
    ComPtr<ID2D1SvgDocument> empty_state_svg_;
    ComPtr<ID2D1SvgDocument> no_selection_svg_;
    ComPtr<ID2D1SvgDocument> recent_empty_svg_;
    ComPtr<ID2D1SvgDocument> starred_empty_svg_;
    ComPtr<ID2D1SvgDocument> exclude_empty_svg_;
    std::unordered_map<int, ComPtr<ID2D1SvgDocument>> fluent_svgs_;
    std::unordered_set<int> fluent_svg_failed_;
    // Colour icons rasterized once per size and quarter-pixel offset:
    // DrawSvgDocument re-tessellates the artwork on every call.
    ComPtr<ID2D1DeviceContext> fluent_raster_dc_;
    std::unordered_map<uint64_t, ComPtr<ID2D1Bitmap1>> fluent_bitmaps_;
    ID2D1Bitmap1* FluentSvgBitmap(int resource_id, ID2D1SvgDocument* svg, int size_q, int fx, int fy);
    ID2D1DeviceContext* logo_dc_ = nullptr;
    float logo_scale_ = 0.0f;

};

} // namespace pulse::ui