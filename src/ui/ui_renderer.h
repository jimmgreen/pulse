// ui_renderer.h — Full-window Fluent renderer (title bar, toolbar, sidebar, pane, tray).
#pragma once
#include "ui_compositor.h"
#include "window_material.h"
#include "fluent_components.h"
#include "shell_icons.h"
#include "view_layout.h"
#include "thumbnail_cache.h"
#include "name_highlight.h"
#include "preview_handler_host.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_snapshot.h"
#include <algorithm>
#include <array>
#include <memory>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse::app { class PlacesCatalog; }

namespace pulse::ui {

class BloomAccentPicker;

// Embedded Fluent Color SVG for a Segoe sidebar/toolbar glyph, or 0.
int FluentSvgIdForGlyph(std::wstring_view glyph);

enum class SortColumn { Name, Mtime, Type, Size, Path };
enum class SortDirection { Asc, Desc };

struct TabView {
    std::wstring title;
    bool active = false;
    float x_offset = 0.0f; // slot units: sibling slide during tab reorder
    uint32_t color_rgb = 0; // resolved group color (0 = ungrouped)
    uint32_t marker_rgb = 0; // individual tab identity, independent of its group
    int group = -1;         // index into WindowViewModel::tab_groups
    bool hidden = false;    // member of a collapsed group: zero width, not drawn
    bool pinned = false;    // narrow icon-only slot, left cluster, no close
};

// A named, colored tab group shown as a chip at the start of its run.
struct TabGroupView {
    int id = 0;             // app::TabGroup::id
    std::wstring name;
    uint32_t color_rgb = 0;
    bool collapsed = false;
    float x_offset = 0.0f;  // px: chip slide during collapsed-group reorder
};

struct ListEntryView {
    std::wstring name;
    std::wstring size_text;
    std::wstring date_text;
    std::wstring type_text;
    std::wstring path;
    DWORD attrs = 0;
    uint64_t size_value = 0;
    uint64_t modified_value = 0;
    bool is_dir = false;
    bool is_reparse = false;
    bool cloud_recall = false;
    bool record_only = false;
    bool cut = false;
    bool starred = false;
    std::wstring badge;
    D2D1_COLOR_F badge_color{};
    D2D1_COLOR_F tag_dots[3]{};
    int tag_dot_count = 0;
    std::wstring snippet;
};

struct RowPresentationCache {
    fs::SnapshotPtr snapshot;
    std::unordered_map<size_t, ListEntryView> rows;
    std::deque<size_t> order;
};

struct ChangeBadge {
    std::wstring label;
    std::wstring tooltip;
    int count = 0;
    bool has_deleted = false;
    int status = 0; // 0 recent, 1 older, 2 unavailable/scanning/offline
};

struct ChangePopover {
    bool visible = false;
    float x = 0.0f, y = 0.0f;
    std::wstring summary;
    int pane_index = -1, row_index = -1;
};

struct PaneViewModel {
    using FilterMap = std::vector<int>;
    using TagDots = std::unordered_map<int, std::vector<D2D1_COLOR_F>>;

    std::wstring path;
    std::wstring header_text;
    std::unordered_map<int, ChangeBadge> change_badges;
    ChangeBadge title_change_badge;
    bool is_changes = false;
    std::wstring change_empty_text, change_status_text;
    std::wstring change_time_label, change_type_label;
    bool change_has_more = false;
    std::wstring filter_text;
    float filter_expand = 0.0f;
    std::wstring banner_title;
    std::wstring banner_message;
    int banner_kind = 0; // 0 info, 1 success, 2 warning, 3 error
    // Derived data is shared with the tab cache. Search views can contain
    // 100k rows, so copying these containers for every animation frame is not
    // acceptable.
    std::shared_ptr<const FilterMap> filter_map;
    std::vector<ListEntryView> entries;
    // Real directory views retain the immutable filesystem snapshot and only
    // materialize visible rows. entries remains available to gallery/tests.
    fs::SnapshotPtr snapshot;
    mutable std::shared_ptr<RowPresentationCache> row_cache;
    const app::PlacesCatalog* tag_catalog = nullptr;
    std::unordered_set<std::wstring> cut_names;
    std::shared_ptr<const TagDots> tag_dots;
    bool loading = false;
    bool search_retaining_results = false;
    bool can_go_back = false;
    bool can_go_forward = false;
    bool can_go_up = false;
    bool can_create = false;
    bool is_file_system = false;
    bool curated_order = false;
    bool is_starred = false;
    bool is_recent = false;
    bool is_recycle = false;
    bool is_search = false;   // search results add a display-only 路径 column
    bool is_query_search = false;
    std::wstring search_query;
    std::shared_ptr<const std::vector<std::wstring>> search_snippets;
    int recent_filter = 0;
    size_t recent_total = 0;
    std::wstring date_column_label;
    int selected_index = -1;
    int selected_count = 0;
    bool all_selected = false;
    const std::unordered_set<int>* selected_indices = nullptr;
    int hover_index = -1;
    int drop_target_index = -1;   // folder row under an OLE drag (accent 2px stroke)
    bool header_drop = false;     // pane title bar is a navigate-to-folder target
    int rename_index = -1;        // name column is in edit mode; do not draw the label
    float scroll_y = 0.0f;
    float scroll_x = 0.0f;
    ViewMode view_mode = ViewMode::Details;
    uint64_t view_generation = 1;
    SortColumn sort_column = SortColumn::Name;
    SortDirection sort_direction = SortDirection::Asc;
    // Cumulative column divider positions in the details view, normalized to
    // the usable header width. All zeroes select the responsive defaults.
    std::array<float, 3> details_column_dividers{};
    std::array<float, 4> search_column_dividers{};
    bool focused = true;
    bool marquee_active = false;
    D2D1_RECT_F marquee_rect{};

    bool IsRowSelected(int index) const {
        if (index < 0) return false;
        if (all_selected) return true;
        if (index == selected_index) return true;
        return selected_indices && selected_indices->contains(index);
    }

    size_t EntryCount() const {
        if (filter_map) return filter_map->size();
        if (!filter_text.empty()) return filter_map ? filter_map->size() : 0;
        return snapshot ? snapshot->size() : entries.size();
    }

    int SourceIndex(int view_row) const {
        if (view_row < 0) return -1;
        if (filter_map || !filter_text.empty()) {
            if (!filter_map || view_row >= static_cast<int>(filter_map->size())) return -1;
            return (*filter_map)[static_cast<size_t>(view_row)];
        }
        return view_row;
    }

    int ViewIndex(int source_index) const {
        if (source_index < 0) return -1;
        if (!filter_map && filter_text.empty()) return source_index;
        if (!filter_map) return -1;
        const auto it = std::lower_bound(filter_map->begin(), filter_map->end(), source_index);
        return it != filter_map->end() && *it == source_index
            ? static_cast<int>(it - filter_map->begin()) : -1;
    }
};

// One clickable breadcrumb segment (display text + its full path).
struct BreadcrumbSegment {
    std::wstring text;
    std::wstring path;
};

// Splits "C:\Users\TestUser" into [{C:\, C:\}, {Users, C:\Users}, ...].
// UNC roots split into a server segment plus a share segment. Pure; unit-tested.
std::vector<BreadcrumbSegment> SplitBreadcrumb(const std::wstring& path);

struct SidebarItem {
    std::wstring label;
    std::wstring detail;
    std::wstring icon_glyph;       // Segoe Fluent Icons codepoint string.
    std::wstring fallback_text;
    std::wstring badge;            // e.g. "Git"
    D2D1_COLOR_F badge_color = {};
    D2D1_COLOR_F icon_color = {};
    D2D1_COLOR_F tag_dot = {};
    bool danger = false;
    bool is_tag = false;
    bool show_count = false;
    int count = 0;
    std::wstring path;
    bool is_drive = false;
    float used_ratio = 0.0f;       // For drives: 0..1.
    float y_offset = 0.0f;         // Tags: slide-animation offset added at layout.
    int indent = 0;
    bool status_dot = false;
    D2D1_COLOR_F status_color = {};
    bool editing = false;
    bool expandable = false;
    bool expanded = false;
    bool starred_child = false;
};

enum class SidebarAddAction { None, CreateTag, AddNetwork };

struct SidebarGroup {
    std::wstring header;
    std::vector<SidebarItem> items;
    bool collapsed = false;
    SidebarAddAction add_action = SidebarAddAction::None;
};

// Scatter deck inside the staging tray panel. Poses arrive pre-smoothed from
// the app-side animation state; the renderer maps slots to geometry and adds
// a deterministic per-card jitter (path-keyed), so draw and hit-test can
// never disagree. Neighboring cards overlap by at most 50%.
struct TrayCardView {
    int exit_layout_count = 1;
    std::wstring path;
    std::wstring name;
    DWORD attrs = 0;
    bool is_dir = false;
    bool missing = false;
    int batch = -1;            // staging-tray batch index (live cards only)
    int sub = -1;              // item index inside the batch (live cards only)
    uint64_t batch_total_size = 0;
    float slot = 0.0f;         // deck slot: 0 = center, ±k = k positions out
    float hover = 0.0f;        // 0..1 raise + straighten
    float appear = 0.0f;       // 0 = just collected, 1 = settled in the deck
    float opacity = 1.0f;      // ghosts (exiting cards) fade toward 0
    bool ghost = false;        // exiting: drawn, never hit-tested
};

struct TrayDeckView {
    std::vector<TrayCardView> cards; // live cards (newest batch first) + ghosts
    int live_count = 0;              // leading non-ghost entries in cards
    int total_count = 0;             // all staged items (for the +N badge)
    int offset = 0;                  // window start into the newest-first list
    uint64_t total_size = 0;         // sum over all batches (footer text)
    int batch_count = 0;
    float open = 0.0f;               // 0..1 drag-over scatter boost
    int hovered = -1;                // live display index under the cursor
};

// Right-side details panel for the current selection (ui.md §7.2 视图簇).
struct DetailsPanelView {
    bool has_selection = false;
    int multi_count = 0;            // >1 => multi-selection summary mode
    std::wstring name, path, type_text;
    std::wstring subtitle_text;     // under-name line: type · size short form
    bool is_dir = false;
    DWORD attrs = 0;
    uint64_t modified_value = 0;    // thumbnail cache key parts
    uint64_t size_value = 0;
    uint64_t view_generation = 1;
    float scroll_y = 0.0f;
    bool preview_only = false;
    float preview_expansion = 0.0f;
    uint32_t collapsed_mask = 0;    // bit per section: 0基本信息 1属性 2标签 3安全 4其他
    std::wstring location_text, size_text, contains_text;
    std::wstring created_text, modified_text, accessed_text;
    std::wstring attributes_text;
    std::vector<PreviewProperty> preview_properties;
    // Async-fetched meta (details_meta.cpp); empty until ready.
    std::wstring owner_text, permissions_text;
    std::wstring drive_text, fs_text, free_space_text;
    bool size_pending = false;      // folder size still computing
    bool starred = false;
    struct TagChip {
        std::wstring name;
        D2D1_COLOR_F color{};
        int tag_index = -1;         // index into PlacesCatalog::tags
        bool assigned = false;      // current selection already has this tag
    };
    std::vector<TagChip> preset_tags; // full catalog with per-selection state
};

// Interactive rects inside the details panel, shared by draw and hit-test.
struct DetailsHitRects {
    D2D1_RECT_F open{}, new_tab{}, copy_path{}, more{}, star{}, rename{};
    D2D1_RECT_F tag_add{}, preview{}, preview_toggle{};
    D2D1_RECT_F attr_readonly{}, attr_hidden{}, attr_advanced{};
    D2D1_RECT_F security_change{};
    std::vector<D2D1_RECT_F> preset_chips;
    std::vector<int> preset_ids;    // tag_index per chip
    std::vector<D2D1_RECT_F> section_headers;
    std::vector<int> section_ids;   // bit index into collapsed_mask
    float content_height_dip = 0.0f; // unclipped content height for wheel clamp
};

struct StatusBarView {
    std::wstring status_text;
    std::wstring selection_text;
    std::wstring hint_text;        // contextual shortcut / hover prompt
    std::wstring task_text;        // active/completed op summary; empty = idle
    float task_progress = -1.0f;   // 0..100 while an op runs; <0 hides the bar
    std::wstring performance_text; // development diagnostics; empty hides it
    std::wstring performance_compact_text;
};

struct PaneSlotView {
    PaneViewModel pane;
    D2D1_RECT_F rect{};
    bool focused = false;
    bool target = false;
};

struct SplitterView {
    D2D1_RECT_F hit_rect{};
    D2D1_RECT_F parent_bounds{};
    bool vertical = true; // vertical divider between left/right panes
};

struct SettingsRowView {
    std::wstring key;
    std::wstring text;
    int group = 0; // 0 software, 1 open-with, 2 share, 3 system, 4 print
    bool on = false;
};

struct IndexVolumeRowView {
    std::wstring id;
    std::wstring title;
    std::wstring detail;
    std::wstring state;
    bool checked = false;
    bool enabled = false;
    bool pending = false;
    uint32_t progress = 0;
};

struct NetworkRootRowView {
    std::wstring path;
    std::wstring detail;
    std::wstring state;
    bool online = false;
    bool building = false;
};

struct DuplicateFileView {
    std::wstring name;
    std::wstring path;
    std::wstring detail;
    bool keep = false;
};

struct DuplicateGroupView {
    std::wstring title;
    std::vector<DuplicateFileView> files;
};

struct DuplicateDriveView {
    std::wstring label;
    std::wstring root;
    bool selected = false;
};

struct WindowViewModel {
    std::wstring window_title;
    std::vector<TabView> tabs;
    std::vector<TabGroupView> tab_groups;
    int active_tab = 0;

    bool can_go_back = false;
    bool can_go_forward = false;

    PaneViewModel pane;
    std::vector<PaneSlotView> pane_slots;
    std::vector<SplitterView> splitters;
    std::vector<SidebarGroup> sidebar;
    float sidebar_scroll = 0.0f;
    TrayDeckView tray_deck;
    bool details_visible = false;   // right details panel toggle (view menu)
    DetailsPanelView details;
    StatusBarView status;

    // OLE drag feedback (1B-2). Indices follow HitTestResult indexing.
    int breadcrumb_hover = -1;    // placed segment under the mouse
    int breadcrumb_drop = -1;     // placed segment under a drag
    int sidebar_drop_index = -1;  // sidebar item under a drag
    // Tag drag-reorder gesture (group/item = dragged tag, tag_drag_y = cursor).
    int tag_drag_group = -1;
    int tag_drag_item = -1;
    float tag_drag_y = 0.0f;
    float tag_gap_line_y = 0.0f; // insertion indicator position (px, 0 = hidden)
    // Title-bar tab drag: floating tab follows the cursor (QFluent TabBar).
    int tab_drag_index = -1; // display index of the run's first tab, or -1
    int tab_drag_count = 1;  // >1: a whole group run floats as one block
    bool tab_drag_chip = false; // drag started from the group chip (collapse-safe)
    float tab_drag_x = 0.0f; // left edge of the floating tab (px)
    bool tray_drop = false;       // staging tray under a drag
    std::wstring drag_badge;      // action badge text near the cursor
    float drag_badge_x = 0.0f;
    float drag_badge_y = 0.0f;
    int hover_region = 0;         // numeric HitTestResult::Region
    int hover_control_index = -1;
    int hover_sub_index = -1;
    std::wstring tooltip_text;
    ChangePopover change_popover;
    float tooltip_x = 0.0f;
    float tooltip_y = 0.0f;

    bool focused = true;
    bool maximized = false;
    bool dark = false;
    bool backdrop_enabled = false;
    WindowEffect window_effect = WindowEffect::MicaAlt;
    std::wstring background_image;
    bool safe_mode = false;
    bool address_editing = false;
    bool address_searching = false;
    bool address_search_current = false;
    bool address_search_has_text = false;
    std::wstring address_search_text;
    float address_search_animation = 0.0f;
    float address_scope_animation = 0.0f;
    bool filter_editing = false;
    bool splitter_pressed = false;
    bool details_resize_pressed = false;
    bool column_resize_pressed = false;
    int hover_pane_index = -1;

    bool settings_open = false;
    int settings_page = 0; // 0 general, 1 search/index, 2 context menu, 3 about, 4 duplicates
    float settings_scroll = 0.0f;
    bool settings_launch_on_startup = false;
    bool settings_keep_running = false;
    bool settings_show_hidden_files = false;
    bool show_pinned_tab_names = true;
    bool settings_open_folders = false;
    bool settings_blank_click_go_back = false;
    bool settings_change_tracking = false;
    int settings_change_days = 3;
    int settings_row_height = 34; // current row-height pref (DIPs) for density radios
    int settings_tray_icon = 48;  // current tray-deck icon pref (DIPs) for size radios
    int settings_language = 0;    // 0 system, 1 zh-CN, 2 en-US
    BloomAccentPicker* settings_bloom = nullptr;
    bool settings_group_on[5] = { true, true, false, false, true };
    std::vector<SettingsRowView> settings_items;
    bool settings_index_service = false;
    bool settings_index_installed = false;
    std::wstring settings_index_status;
    std::wstring settings_index_path;
    bool settings_index_migrating = false;
    std::wstring settings_index_error;
    std::vector<IndexVolumeRowView> settings_index_volumes;
    std::vector<std::wstring> settings_index_excluded_paths;
    std::vector<NetworkRootRowView> settings_network_roots;
    std::wstring settings_version;
    std::wstring settings_build_id;
    std::wstring settings_update_status;
    std::wstring settings_update_version;
    bool settings_update_enabled = false;
    bool settings_update_checking = false;
    bool settings_update_downloading = false;
    bool settings_update_installing = false;
    bool settings_update_available = false;
    bool settings_diagnostics_exporting = false;
    bool settings_show_performance = false;
    int dup_scope = 0;
    std::wstring dup_folder;
    std::vector<DuplicateDriveView> dup_drives;
    int dup_min_size = 0;
    bool dup_scanning = false;
    bool dup_can_scan = false;
    bool dup_show_progress = false;
    bool dup_progress_indeterminate = false;
    float dup_progress_value = 0.0f;
    float dup_animation = 0.0f;
    std::wstring dup_status;
    std::wstring dup_speed;
    std::wstring dup_hint;
    std::wstring dup_empty;
    bool dup_show_delete_all = false;
    std::wstring dup_delete_all;
    std::vector<DuplicateGroupView> dup_groups;
};

struct HitTestResult {
    enum Region {
        None,
        Tab,
        TabClose,
        TabNew,
        TabGroup,
        ThemeToggle,
        SettingsButton,
        Minimize,
        Maximize,
        Close,
        NavBack,
        NavForward,
        NavUp,
        NavRefresh,
        NewButton,
        Cut,
        Copy,
        Paste,
        Rename,
        Delete,
        SplitButton,
        DetailsToggle,
        PaneMediumIcons,
        PaneViewButton,
        AddressBar,
        AddressSearch,
        AddressSearchScope,
        AddressSearchClear,
        AddressSearchClose,
        BreadcrumbSegment,
        ColumnHeader,
        ColumnDivider,
        FilterBox,
        FilterClear,
        Splitter,
        Scrollbar,
        Row,
        ChangeBadge,
        ChangeOpen,
        ChangeTimeFilter,
        ChangeTypeFilter,
        ChangeMore,
        Pane,
        PaneHeader,               // split-pane title strip (path + nav/view)
        SidebarHeader,
        SidebarHeaderAction,
        SidebarItem,
        SidebarItemAction,
        SidebarItemExpand,
        TrayRelease,
        TrayClose,
        TrayItemRemove,
        TrayCard,
        TrayClear,
        RowStar,
        RowNewTab,
        RowMore,
        RecentFilter,
        RecentClear,
        PaneEmptyNewFolder,
        DetailsOpen,
        DetailsStar,
        DetailsMore,
        DetailsRename,
        DetailsTagAdd,
        DetailsNewTab,
        DetailsCopyPath,
        DetailsSection,
        DetailsAttrToggle,
        DetailsSecurityChange,
        DetailsPresetTag,
        DetailsPreview,
        DetailsPreviewToggle,
        DetailsResize,
        StatusBar,
        StatusBarTask,
        SettingsNav,
        SettingsToggle,
        SettingsChangeDays,
        SettingsRestore,
        SettingsAccent,
        SettingsEffect,
        SettingsWallpaper,
        SettingsDensity,
        SettingsTrayIcon,
        SettingsLanguage,
        SettingsIndexVolume,
        SettingsIndexAction,
        SettingsIndexExcludeAction,
        SettingsIndexExcludeRemove,
        SettingsNetworkAction,
        SettingsNetworkRemove,
        SettingsDiagnosticsAction,
        SettingsUpdateAction,
        SettingsDupScope,
        SettingsDupDrive,
        SettingsDupBrowse,
        SettingsDupScan,
        SettingsDupCancel,
        SettingsDupMinSize,
        SettingsDupKeep,
        SettingsDupOpen,
        SettingsDupGroupDelete,
        SettingsDupDeleteAll,
        SearchFilter
    } region = None;
    SidebarAddAction sidebar_action = SidebarAddAction::None;
    int index = -1;          // tab/row/sidebar item/tray batch/tray item.
    int sub_index = -1;      // tray item inside batch, breadcrumb segment.
    int pane_index = -1;     // leaf in pane_slots, or -1 outside the content area.
    SortColumn column = SortColumn::Name;
    std::wstring path;
};

class MainRenderer {
public:
    MainRenderer();

    void SetScale(float scale);
    void SetCompositor(Compositor* comp);
    void InvalidateTypography();
    void SetIconNotifyWindow(HWND hwnd);
    void NotifyPreviewActivate(bool active) { preview_handler_.NotifyAppActivate(active); }
    void NotifyPreviewOwnerMoved() { preview_handler_.Reposition(); }

    // Layout metrics (DIPs).
    float TitleBarHeight() const { return title_bar_height_; }
    float ToolbarHeight() const { return toolbar_height_; }
    float EffectiveSidebarWidth(float window_width) const;
    float PaneHeaderHeight() const { return pane_header_height_; }
    float ColumnHeaderHeight() const { return column_header_height_; }
    float RowHeight() const { return row_height_; }
    float ListRowHeightDip(const PaneViewModel& vm) const;
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
    void SetDetailsPanelVisible(bool visible) {
        details_visible_ = visible;
        if (!visible) { EndDetailsPreviewPan(); details_preview_ready_ = false; }
    }
    float DetailsPanelWidth(float window_w) const {
        return details_visible_ && window_w >= 1000.0f * scale_
            ? details_width_ * scale_ + margin_ : 0.0f;
    }
    void SetDetailsPanelWidth(float width_dip) {
        details_width_ = std::clamp(width_dip, 300.0f, 480.0f);
    }
    // Unclipped content height (DIPs) of the details panel, for wheel clamping.
    float DetailsContentHeightDip(const WindowViewModel& vm, float w, float h);
    bool CachedPreviewProperties(const std::wstring& path, uint64_t modified, uint64_t size,
                                 std::vector<PreviewProperty>& properties) {
        return thumbnail_cache_.CachedProperties(path, modified, size, properties);
    }
    D2D1_RECT_F DetailsPanelRect(float w, float h) const;

    D2D1_RECT_F ContentRect(float w, float h) const;
    D2D1_RECT_F PaneListRect(const D2D1_RECT_F& pane_bounds, float extra_top = 0.0f,
                             ViewMode mode = ViewMode::Details) const;
    D2D1_RECT_F FilterBoxRect(const D2D1_RECT_F& pane_bounds,
                              float expand = 1.0f) const;
    D2D1_RECT_F PaneMediumIconsRect(const D2D1_RECT_F& pane_bounds,
                                    float filter_expand = 1.0f) const;
    D2D1_RECT_F PaneViewButtonRect(const D2D1_RECT_F& pane_bounds,
                                   float filter_expand = 1.0f) const;
    D2D1_RECT_F PaneNavUpRect(const D2D1_RECT_F& pane_bounds,
                              float filter_expand = 1.0f) const;
    D2D1_RECT_F PaneNavForwardRect(const D2D1_RECT_F& pane_bounds,
                                   float filter_expand = 1.0f) const;
    D2D1_RECT_F PaneNavBackRect(const D2D1_RECT_F& pane_bounds,
                                float filter_expand = 1.0f) const;
    D2D1_RECT_F FilterEditRect(const D2D1_RECT_F& pane_bounds,
                               float expand = 1.0f, bool has_text = false) const;
    D2D1_RECT_F FilterClearRect(const D2D1_RECT_F& pane_bounds, float expand) const;

    bool BeginDetailsPreviewPan(float x, float y);
    void MoveDetailsPreviewPan(float x, float y);
    void EndDetailsPreviewPan() { details_preview_dragging_ = false; details_preview_drag_pending_ = false; }
    bool DetailsPreviewDragging() const { return details_preview_dragging_; }
    bool DetailsPreviewPointerActive() const { return details_preview_dragging_ || details_preview_drag_pending_; }
    bool CanDetailsPreviewPan() const { return details_preview_ready_; }
    bool TickDetailsPreview(ULONGLONG now) {
        if (details_zoom_label_until_ && now >= details_zoom_label_until_) {
            details_zoom_label_until_ = 0;
            return true;
        }
        return false;
    }
    void ScrollDetailsPreview(float steps, float x, float y, bool horizontal = false);
    void ToggleDetailsPreviewFit(float x, float y);

    struct DetailsColumnLayout {
        float left = 0.0f;
        float right = 0.0f;
        std::array<float, 5> widths{};
        int count = 4; // 5 in search views (extra 路径 column)

        float DividerX(int index) const {
            float x = left;
            for (int i = 0; i <= index && i < count - 1; ++i)
                x += widths[static_cast<size_t>(i)];
            return x;
        }
    };
    DetailsColumnLayout DetailsColumns(
        const D2D1_RECT_F& pane_bounds,
        const std::array<float, 3>& dividers = {},
        bool search_view = false,
        const std::array<float, 4>& search_dividers = {}) const;
    DetailsColumnLayout DetailsColumns(const D2D1_RECT_F& pane_bounds,
                                       const PaneViewModel& vm) const;
    std::array<float, 3> ResizeDetailsColumnDivider(
        const D2D1_RECT_F& pane_bounds,
        const std::array<float, 3>& dividers,
        int divider_index, float cursor_x) const;
    std::array<float, 4> ResizeSearchColumnDivider(
        const D2D1_RECT_F& pane_bounds,
        const std::array<float, 4>& dividers,
        int divider_index, float cursor_x) const;

    D2D1_RECT_F NameCellRect(const D2D1_RECT_F& pane_bounds, int view_row, float scroll_y,
                             float extra_top = 0.0f, ViewMode mode = ViewMode::Details,
                             float scroll_x = 0.0f, size_t item_count = 0,
                             const std::array<float, 3>& column_dividers = {},
                             bool search_view = false,
                             const std::array<float, 4>& search_dividers = {},
                             float row_height_px = 0.0f) const;
    bool PointInItemName(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                         int source_index, float x, float y) const;
    // Exact geometry of the Fluent frame drawn for the rename row; the hosted
    // EDIT control is placed inside this rect (must stay in sync with DrawList).
    D2D1_RECT_F RenameFieldRect(const PaneViewModel& vm, const D2D1_RECT_F& list,
                                int source_index);
    D2D1_RECT_F SidebarRect(float w, float h) const;
    D2D1_RECT_F StagingTrayRect(const WindowViewModel& vm, float w, float h) const;
    D2D1_RECT_F TitleBarRect(float w) const;
    D2D1_RECT_F ToolbarRect(float w) const;
    D2D1_RECT_F AddressBarRect(float w) const;
    D2D1_RECT_F AddressSearchButtonRect(float w) const;
    void DrawAddressSearchChrome(const WindowViewModel& vm, float w, const Theme& theme);
    float NewButtonWidthPx(bool compact) const;

    // One placed breadcrumb segment (after left-truncation to fit the bar).
    struct BreadcrumbPlaced {
        D2D1_RECT_F rc{};
        std::wstring text;
        std::wstring path;
    };
    // Shared by DrawToolbar, HitTest and the drop-target logic so the three
    // can never disagree about segment positions.
    void BreadcrumbLayout(const PaneViewModel& vm, float w,
                          std::vector<BreadcrumbPlaced>& out) const;

    void Render(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void InvalidateWallpaper() { material_.Invalidate(); }

    HitTestResult HitTest(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                          float x, float y) const;

    float MaxScrollForPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const;
    float MaxScrollXForPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const;
    D2D1_RECT_F ItemRectInPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                               int view_index) const;
    int MoveViewIndex(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                      int current, int dx, int dy) const;
    int PageDelta(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const;
    std::pair<int, int> VisibleRangeInPane(const PaneViewModel& vm,
                                           const D2D1_RECT_F& pane_bounds) const;
    int RowFromYInPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds, float y) const;
    int ItemFromPointInPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                            float x, float y) const;
    // Current layout rect of a tag sidebar slot (for drag-reorder geometry).
    bool TagItemRect(const WindowViewModel& vm, float w, float h, int group, int item,
                     D2D1_RECT_F* out) const;
    // Rest-slot rect of a title-bar tab (display index, no drag float).
    bool TabItemRect(const WindowViewModel& vm, float window_w, int index, D2D1_RECT_F* out) const;
    // Group chip rect (title-bar space); false when the group has no chip.
    bool TabGroupChipRect(const WindowViewModel& vm, float window_w, int group_index,
                          D2D1_RECT_F* out) const;
    // Uniform tab pitch (excludes group-chip offsets); used by drag math.
    float TabPitchPx(const WindowViewModel& vm, float window_w) const;
    float SettingsMaxScroll(const WindowViewModel& vm, float window_w, float window_h) const;
    float SidebarMaxScroll(const WindowViewModel& vm, float window_w, float window_h) const;
    bool SidebarScrollbarGeometry(const WindowViewModel& vm, float window_w, float window_h,
                                  D2D1_RECT_F& track, D2D1_RECT_F& thumb, float& max_scroll) const;
    // How many deck cards the tray panel fits at the current sidebar width
    // without breaking the max-50%-overlap rule (>= 1).
    int TrayDeckCapacity(float window_w) const;

private:
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
    };
    TabStripMetrics ComputeTabStrip(const WindowViewModel& vm, float window_w) const;
    void DrawTitleBar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawToolbar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawSidebar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawPane(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawDetailsPanel(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawSinglePane(const WindowViewModel& vm, const PaneViewModel& pane,
                        const D2D1_RECT_F& bounds, int pane_index, bool focused, bool target,
                        const Theme& theme);
    void DrawPaneEmptyState(const WindowViewModel& vm, const PaneViewModel& pane,
                            const D2D1_RECT_F& bounds, int pane_index, const Theme& theme);
    void DrawStatusBar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawSettings(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);

    void DrawList(const PaneViewModel& vm, float x, float y, float w, float h, const Theme& theme,
                  int hover_region = 0, int hover_control_index = -1);
    void DrawScrollbar(const PaneViewModel& vm, float x, float y, float w, float h, const Theme& theme);
    // Scatter deck of staged files inside the tray panel (draw + hit-test
    // share the geometry helpers in ui_renderer.cpp).
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
    void DrawFolderIcon(float x, float y, float size, const Theme& theme);
    void DrawFileIcon(float x, float y, float size, const Theme& theme);
    void DrawEntryIcon(const ListEntryView& entry, float x, float y, float size, const Theme& theme);
    bool DrawEmptyStateSvg(const D2D1_RECT_F& bounds, float opacity = 1.0f);
    bool EnsureEmptyStateSvg();
    bool DrawNoSelectionSvg(const D2D1_RECT_F& bounds, float opacity = 1.0f);
    bool EnsureNoSelectionSvg();
    bool DrawCuratedEmptyStateSvg(bool starred, const D2D1_RECT_F& bounds,
                                  float opacity = 1.0f);
    bool EnsureCuratedEmptyStateSvg(bool starred);
    bool DrawExcludeEmptySvg(const D2D1_RECT_F& bounds, float opacity = 1.0f);
    bool EnsureExcludeEmptySvg();
    bool EnsureFluentSvg(int resource_id);
    bool DrawFluentSvg(int resource_id, const D2D1_RECT_F& bounds, float opacity = 1.0f);
    void DrawTruncatedName(const std::wstring& name, float x, float y, float w, float h,
                           const Theme& theme, bool selected, const std::vector<NameMatchRange>& matches);
    void DrawCenteredIconName(const std::wstring& name, const D2D1_RECT_F& bounds,
                              const D2D1_COLOR_F& color, const Theme& theme,
                              const std::vector<NameMatchRange>& matches);
    // Title-bar product mark from the app icon resource (nullptr until loaded).
    ID2D1Bitmap* LogoBitmap();

    WindowMaterial material_;
    Compositor* compositor_ = nullptr;
    fluent::Painter painter_;
    ShellIconCache icon_cache_;
    std::unordered_map<ID2D1Bitmap*, ComPtr<ID2D1Effect>> tray_shadows_;
    ThumbnailCache thumbnail_cache_;
    PreviewHandlerHost preview_handler_;
    HWND notify_hwnd_ = nullptr;
    float scale_ = 1.0f;
    float title_bar_height_ = kTitleBarHeight;
    float toolbar_height_ = 44.0f;
    float status_height_ = 28.0f;
    float sidebar_width_ = 224.0f;
    float pane_header_height_ = 40.0f;
    float column_header_height_ = 32.0f;
    float row_height_ = 34.0f;
    float row_height_dip_ = 34.0f;
    float tray_icon_dip_ = 48.0f;
    float margin_ = 4.0f;
    float control_gap_ = 4.0f;
    D2D1_COLOR_F text_background_{};
    bool details_visible_ = false;
    float details_width_ = 340.0f;
    PreviewViewport details_viewport_;
    std::wstring details_preview_identity_;
    bool details_preview_ready_ = false;
    bool details_preview_dragging_ = false;
    bool details_preview_drag_pending_ = false;
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
    mutable ComPtr<ID2D1SolidColorBrush> brFpsBg_;
    mutable ComPtr<ID2D1SolidColorBrush> brFpsText_;

    ComPtr<ID2D1Bitmap> logo_bitmap_;
    ComPtr<IDWriteTextFormat> preview_mono_format_;
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
    ID2D1DeviceContext* logo_dc_ = nullptr;
    float logo_scale_ = 0.0f;

};

} // namespace pulse::ui
