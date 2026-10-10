// ui_renderer.h — Full-window Fluent renderer (title bar, toolbar, sidebar, pane, tray).
#pragma once
#include "../index/content_result_store.h"
#include "ui_compositor.h"
#include "window_material.h"
#include "fluent_components.h"
#include "release_note_view.h"
#include "shell_icons.h"
#include "open_with_icons.h"
#include "view_layout.h"
#include "column_strip_layout.h"
#include "panel_metrics.h"
#include "toolbar_layout.h"
#include "thumbnail_cache.h"
#include "folder_thumbnail_cache.h"
#include "archive_preview.h"
#include "name_highlight.h"
#include "preview_handler_host.h"
#include "ui_motion.h"
#include "link_pill.h"
#include "ui_view_morph.h"
#include "group_wheel.h"
#include "../app/builtin_menu_items.h"
#include "details_column_set.h"
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

// SettingsToggle indexes of the Pulse menu preview (右键菜单 page): rows
// are kSettingsMenuRowHit + BuiltinMenuItem, the fixed rows
// kSettingsMenuFixedHit + BuiltinFixedRow; 68 restores the tab's order.
inline constexpr int kSettingsMenuFixedHit = 99990;
inline constexpr int kSettingsMenuRowHit = 100000;
inline constexpr int kSettingsMenuOrderReset = 68;
enum class PaneHeaderIcon;

inline constexpr unsigned kSettingsContextExpandedMask = 0x1f00u;
// Third-party context-menu groups (kSettingsContextExpandedMask) start
// collapsed under 其他软件添加的项; the Pulse menu card is always open.
inline constexpr unsigned kSettingsDefaultExpandedMask = 0x7u;

class BloomAccentPicker;

// Embedded Fluent Color SVG for a Segoe sidebar/toolbar glyph, or 0.
int FluentSvgIdForGlyph(std::wstring_view glyph);

// Values are persisted (folder sort prefs, session): append only.
enum class SortColumn { Name, Mtime, Type, Size, Path, Created, Accessed };
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
    float flash = 0.0f;     // 0..1 attention pulse: reopened folder already had this tab
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
    bool is_link = false;
    std::wstring link_destination;
    std::wstring name;
    std::wstring size_text;
    std::wstring date_text;
    std::wstring type_text;
    std::wstring path;
    std::wstring created_text;   // empty when the source has no creation time
    std::wstring accessed_text;
    DWORD attrs = 0;
    uint64_t size_value = 0;
    uint64_t modified_value = 0;
    uint64_t created_value = 0;
    uint64_t accessed_value = 0;
    bool is_dir = false;
    bool is_reparse = false;
    fs::LinkKind link_kind = fs::LinkKind::None;
    bool cloud_recall = false;
    bool record_only = false;
    bool cut = false;
    bool starred = false;
    std::wstring badge;
    D2D1_COLOR_F badge_color{};
    D2D1_COLOR_F tag_dots[3]{};
    int tag_dot_count = 0;
    std::wstring snippet;
    // This PC tiles: used fraction (-1 = unknown) and "X free of Y".
    float drive_used = -1.0f;
    std::wstring drive_space_text;
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

// One folder listing in the column view (an ancestor or the child column).
struct ColumnStripColumnView {
    std::wstring path;
    std::wstring title;           // caption; empty = last path segment
    fs::SnapshotPtr snapshot;
    // Visible source indices into snapshot (hidden files already filtered).
    std::shared_ptr<const std::vector<int>> rows;
    int highlight_row = -1;       // row on the navigation path (index into rows)
    float scroll_dip = 0.0f;      // used when auto_scroll is false
    bool auto_scroll = true;      // keep highlight_row centered until the user scrolls
    bool loading = false;
    bool error = false;
};

// Column view of a folder pane: ancestors on the left, the regular list in
// the middle, and the selected folder's contents on the right.
struct ColumnStripView {
    bool enabled = false;         // pane toggle state (header button)
    bool eligible = false;        // real folder view that can show columns
    std::vector<ColumnStripColumnView> ancestors; // root -> parent
    // Contents of the single selected folder. Files, multi-selection and no
    // selection show no extra column: the details panel covers those.
    bool has_child = false;
    ColumnStripColumnView child;
    std::vector<float> widths_dip; // see ColumnStripWidthDip
    float scroll_from_right_dip = 0.0f; // ancestor strip scroll (0 = parent visible)
    int resize_column = -1;       // column id whose divider is being dragged
    bool hscroll_pressed = false; // ancestor scrollbar thumb is being dragged

    bool Active() const { return enabled && eligible; }
};

// Folder compare (two panes): per-entry status relative to the other pane.
enum class CompareMark : uint8_t { Same = 0, OnlyHere = 1, Newer = 2, Older = 3, Differs = 4 };

struct PaneViewModel {
    using FilterMap = std::vector<int>;
    using TagDots = std::unordered_map<int, std::vector<D2D1_COLOR_F>>;

    std::wstring path;
    std::wstring header_text;
    std::unordered_map<int, ChangeBadge> change_badges;
    std::unordered_map<int, std::wstring> folder_size_labels;
    std::unordered_set<int> folder_size_actions;
    std::unordered_set<int> folder_size_muted;
    std::unordered_set<int> folder_size_running;
    // No value yet: pending rows (an answer is on its way) draw a skeleton bar;
    // idle rows (deferred, failed, never automatic) draw a dash that reads
    // as the calculate action while the row is hovered.
    std::unordered_set<int> folder_size_pending;
    std::unordered_set<int> folder_size_idle;
    ChangeBadge title_change_badge;
    bool is_changes = false;
    std::wstring change_empty_text, change_status_text;
    std::wstring change_time_label, change_type_label;
    bool change_has_more = false;
    std::wstring filter_text;
    float filter_expand = 0.0f;
    float header_controls_opacity = 1.0f;
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
    std::shared_ptr<index::ContentResultStore> content_results;
    mutable std::shared_ptr<RowPresentationCache> row_cache;
    const app::PlacesCatalog* tag_catalog = nullptr;
    std::unordered_set<std::wstring> cut_names;
    // Folder compare: CompareMark per source index (null = compare off).
    std::shared_ptr<const std::vector<uint8_t>> compare_marks;
    bool compare_active = false;
    bool compare_diff_only = false;
    std::array<int, 5> compare_counts{};   // indexed by CompareMark
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
    bool is_content_search = false;
    bool network_live_action = false;  // #74: banner offers "Add to network index"
    std::wstring search_query;
    // Query search breadcrumb: origin segments followed by one search segment.
    bool has_search_origin = false;
    std::wstring search_origin;
    std::wstring search_crumb;
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
    bool focus_first_hint = false; // first view row has keyboard focus but no selection
    float scroll_y = 0.0f;
    float scroll_x = 0.0f;
    ViewMode view_mode = ViewMode::Details;
    uint64_t view_generation = 1;
    SortColumn sort_column = SortColumn::Name;
    SortDirection sort_direction = SortDirection::Asc;
    // Manual details column widths (DIP, see DetailsColumnWidths). All zeroes
    // select the responsive defaults.
    DetailsColumnWidths details_column_dividers{};
    std::array<float, 4> search_column_dividers{};
    // "Group by" (app::GroupBy value) and its spans over view rows.
    int group_by = 0;
    std::shared_ptr<const ListGroups> groups;
    bool focused = true;
    bool marquee_active = false;
    D2D1_RECT_F marquee_rect{};
    ColumnStripView column_strip;

    bool IsRowSelected(int index) const {
        if (index < 0) return false;
        if (all_selected) return true;
        if (index == selected_index) return true;
        return selected_indices && selected_indices->contains(index);
    }

    size_t EntryCount() const {
        if(content_results) return content_results->Count();
        if (filter_map) return filter_map->size();
        if (!filter_text.empty()) return filter_map ? filter_map->size() : 0;
        return snapshot ? snapshot->size() : entries.size();
    }

    int SourceIndex(int view_row) const {
        if (view_row < 0) return -1;
        if(content_results) return view_row;
        if (filter_map || !filter_text.empty()) {
            if (!filter_map || view_row >= static_cast<int>(filter_map->size())) return -1;
            return (*filter_map)[static_cast<size_t>(view_row)];
        }
        return view_row;
    }

    // Spans for layout, or null when this view is not drawn grouped. Spans
    // must cover exactly EntryCount() rows or the layout would drift.
    const ListGroups* Groups() const {
        if (!groups || groups->empty() ||
            (view_mode != ViewMode::Details && view_mode != ViewMode::Content)) return nullptr;
        const ListGroup& last = groups->back();
        return static_cast<size_t>(last.first + last.count) == EntryCount() ? groups.get() : nullptr;
    }

    int ViewIndex(int source_index) const {
        if (source_index < 0) return -1;
        if (content_results || (!filter_map && filter_text.empty())) return source_index;
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
    bool tab_row = false;          // vertical tabs: row stands for a window tab
    int tab_number = 0;            // 1-based position, badged on the icon rail
    bool tab_active = false;
    float flash = 0.0f;            // tab rows: attention pulse, see TabView::flash
};

enum class SidebarAddAction { None, CreateTag, AddNetwork, AddQuickAccess, NewTab };

struct SidebarGroup {
    // Logical section id (pulse::app::SidebarSectionId). Collapse/hide masks and
    // the section menus key off this, not off the display position, because the
    // user can reorder the groups by dragging their headers.
    int id = -1;
    std::wstring header;
    std::wstring icon_glyph;       // optional leading icon on the header row
    std::vector<SidebarItem> items;
    bool collapsed = false;
    bool hidden = false;           // Section menu: the group is not laid out at all.
    SidebarAddAction add_action = SidebarAddAction::None;
    // The header title opens the section's own view; its chevron still folds.
    bool navigable = false;
    std::wstring navigation_path; // Empty is This PC.
    bool tabs_section = false;     // vertical tabs block: set apart by a divider
};

// Vertical span a section occupies in the laid-out sidebar (header top through
// the gap after its last row). Hit-testing uses it to attribute a click on the
// empty space below the rows to the section the user aimed at.
struct SidebarGroupBand {
    int group = -1;      // index into WindowViewModel::sidebar
    float top = 0.0f;    // header top (px, client space)
    float bottom = 0.0f; // end of the section's gap (px)
};

// Card stack inside the staging tray panel. Poses arrive pre-animated from
// the app-side state (TickTrayDeck); the renderer maps depth/offsets to one
// shared geometry (TrayStackGeometry), so draw and hit-test never disagree.
struct TrayCardView {
    std::wstring path;
    std::wstring name;
    std::wstring folder;       // parent folder for the third line (display form)
    DWORD attrs = 0;
    bool is_dir = false;
    bool missing = false;
    bool cut = false;          // staged with Ctrl+X (move intent)
    int batch = -1;            // staging-tray batch index (live cards only)
    int sub = -1;              // item index inside the batch (live cards only)
    uint64_t size = 0;         // item size in bytes (files)
    float depth = 0.0f;        // 0 = top card, 1/2 = peeking layers, 3 = hidden behind
    float hover = 0.0f;        // 0..1 top-card lift + close badge
    float appear = 1.0f;       // 0 = dropping in from above, 1 = settled
    float fly = 0.0f;          // horizontal offset in card widths (throw / tumble)
    float dx = 0.0f;           // gesture offset, DIPs
    float dy = 0.0f;
    float angle = 0.0f;        // rotation, degrees
    float shrink = 1.0f;       // extra scale (tumble)
    float opacity = 1.0f;
    bool ghost = false;        // exiting: drawn, never hit-tested
    bool on_top = false;       // painted above the stack (dragged / flying out)
};

// One smoke puff of the dismiss effect, anchored at the top card's × badge.
struct TrayPuffView {
    float t = 0.0f;            // 0..1 lifetime progress
    float angle = 0.0f;        // radians
    float dist = 0.0f;         // travel, DIPs
    float size = 1.0f;         // final scale
};

// Staging tray recent drop destination (chip row under the deck).
struct TrayDestView {
    std::wstring path;
    std::wstring label;              // folder name (drive root: the path)
    bool missing = false;            // folder gone: chip greyed, click ignored
};

// Two staged files side by side (column 0 = first staged).
struct TrayCompareView {
    std::wstring name[2], where[2], time[2], size[2];
    int newer = -1;            // column with the later mtime (2 s tolerance), -1 same
    bool size_differs = false;
    int content = 0;           // 0 not compared, 1 running, 2 identical, 3 different, 4 unreadable
    int text = -1;             // both files text: 1, binary: 0, not probed yet: -1
};

struct TrayDeckView {
    std::vector<TrayCardView> cards; // live window (top first) + ghosts
    std::vector<TrayPuffView> puffs;
    int live_count = 0;              // leading non-ghost entries in cards
    int total_count = 0;             // all staged items
    int offset = 0;                  // cyclic index of the top card (newest-first order)
    uint64_t total_size = 0;         // sum over all batches (footer text)
    int batch_count = 0;
    bool release_move = false;       // intent of the batch the header button releases
    float open = 0.0f;               // 0..1 drag-over highlight
    float spread = 0.0f;             // 0..1 peeking layers fan out (stack hovered)
    float thumb_dip = 48.0f;         // thumbnail edge (settings: staging tray icon size)
    bool hidden = false;             // host has no staging tray (file picker)
    int hovered = -1;                // live display index under the cursor (0 = top)
    std::vector<TrayDestView> dests; // recent drop folders, newest first (max 3)
    int stale_count = 0;             // staged items moved/deleted outside Pulse
    bool can_compare = false;        // exactly two existing files staged
    bool comparing = false;          // compare table replaces the card stack
    TrayCompareView compare;
};

// Right-side details panel for the current selection (ui.md §7.2 视图簇).
struct DetailsPanelView {
    bool is_link = false;
    bool has_selection = false;
    int multi_count = 0;            // >1 => multi-selection summary mode
    std::wstring name, path, type_text;
    std::wstring subtitle_text;     // under-name line: type · size short form
    bool is_dir = false;
    fs::LinkKind link_kind = fs::LinkKind::None;
    DWORD attrs = 0;
    uint64_t modified_value = 0;    // thumbnail cache key parts
    uint64_t size_value = 0;
    uint64_t view_generation = 1;
    float scroll_y = 0.0f;
    bool preview_only = false;
    bool preview_enabled = true;    // off: no preview request, placeholder only
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
    D2D1_RECT_F tag_add{}, preview{}, preview_toggle{}, preview_enable{};
    D2D1_RECT_F attr_readonly{}, attr_hidden{}, attr_advanced{};
    D2D1_RECT_F security_change{};
    std::vector<D2D1_RECT_F> preset_chips;
    std::vector<int> preset_ids;    // tag_index per chip
    std::vector<D2D1_RECT_F> section_headers;
    std::vector<int> section_ids;   // bit index into collapsed_mask
    float content_height_dip = 0.0f; // unclipped content height for wheel clamp
};

struct StatusBarView {
    bool query_active = false;
    bool query_cancellable = false;
    float query_progress = -1.0f; // 0..1 of this query's candidates; negative means unknown.
    std::wstring query_text;
    std::wstring status_text;
    std::wstring selection_text;
    std::wstring hint_text;        // contextual shortcut / hover prompt
    // Clickable action at the right end of the hint ("Compare →"); hint_action is
    // the app's StatusHintAction value, 0 = none.
    std::wstring hint_action_text;
    int hint_action = 0;
    std::wstring task_text;        // active/completed op summary; empty = idle
    float task_progress = -1.0f;   // 0..100; negative: hidden for ops, indeterminate for updates.
    bool task_is_update = false;   // Noninteractive, centered progress; never opens file operations.
    bool task_active = false;      // file operation running (drives the progress pill)
    bool task_failed = false;      // last operation ended with an error: no success check
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
    // Two-pane layouts: round swap button shown while the divider is hovered.
    // Empty otherwise.
    D2D1_RECT_F swap_rect{};
};
// Splitter hit index of the swap button on splitter i: kSplitterSwapIndex + i.
inline constexpr int kSplitterSwapIndex = 1000;

struct SettingsRowView {
    std::wstring key;
    std::wstring text;
    int group = 0; // 0 software, 1 open-with, 2 share, 3 system, 4 print
    bool on = false;
    bool slow_disabled = false; // auto-disabled after response timeouts (#77)
};

struct IndexVolumeRowView {
    std::wstring id;
    std::wstring title;
    std::wstring detail;
    std::wstring state;      // localized for display
    std::wstring raw_state;  // as reported by the index service (Simplified), for the badge
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

// HitTestResult::SettingsPackAction indices (Settings > 预览增强包).
enum class PackAction : int {
    Install = 0,        // FFmpeg pack: download and install
    Remove,             // FFmpeg pack: uninstall
    Enable,             // FFmpeg pack: on / off
    UseCustom,          // use an FFmpeg already on this PC
    Browse,             // choose that ffmpeg.exe
    UseDetected,        // take the ffmpeg.exe found on PATH
    OpenFolder,         // %LOCALAPPDATA%\Pulse\packs
    RemoveOnUninstall,  // delete the packs with Pulse
    ImagesInstall,      // image pack: download and install (or cancel)
    ImagesRemove,       // image pack: uninstall
    ImagesEnable,       // image pack: on / off
    RawInstall, RawRemove, RawEnable,
    ArchiveInstall, ArchiveRemove, ArchiveEnable,
};

struct WindowViewModel {
    std::wstring window_title;
    std::vector<TabView> tabs;
    std::vector<TabGroupView> tab_groups;
    int active_tab = 0;

    bool can_go_back = false;
    bool can_go_forward = false;
    bool can_go_up = false;

    PaneViewModel pane;
    std::vector<PaneSlotView> pane_slots;
    std::vector<SplitterView> splitters;
    std::vector<SidebarGroup> sidebar;
    float sidebar_scroll = 0.0f;
    // Sidebar scrollbar fade (#44): 0 hidden .. 1 shown; expand 0 thin thumb .. 1 thumb + track.
    float sidebar_scrollbar_opacity = 1.0f;
    float sidebar_scrollbar_expand = 1.0f;
    TrayDeckView tray_deck;
    bool details_visible = false;   // right details panel toggle (view menu)
    int layout_preset = 0;          // app::LayoutPreset of the active tab (toolbar split icon)
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
    // Sidebar section header drag: the id-identified section floats, the rest
    // stay put, and the insertion line marks the slot it would land in.
    int sidebar_group_drag_id = -1;
    float sidebar_group_gap_line_y = 0.0f; // px, 0 = hidden
    // Quick-access pin drag: the dragged row (layout index) plus its own
    // insertion line.
    int sidebar_pin_drag_index = -1;
    float sidebar_pin_gap_line_y = 0.0f; // px, 0 = hidden
    // Title-bar tab drag: floating tab follows the cursor (QFluent TabBar).
    int tab_drag_index = -1; // display index of the run's first tab, or -1
    int tab_drag_count = 1;  // >1: a whole group run floats as one block
    bool tab_drag_chip = false; // drag started from the group chip (collapse-safe)
    float tab_drag_x = 0.0f; // left edge of the floating tab (px)
    bool tray_drop = false;       // staging tray under a drag
    std::wstring drag_badge;      // action badge text near the cursor
    float drag_badge_x = 0.0f;
    float drag_badge_y = 0.0f;
    bool drag_badge_move = false; // the drop will move: badge gets an amber edge
    int hover_region = 0;         // numeric HitTestResult::Region
    int hover_control_index = -1;
    int hover_sub_index = -1;
    std::wstring tooltip_text;
    // One-time teaching bubble (bottom right, above the status bar).
    struct TeachBubbleView {
        bool visible = false;
        std::wstring title, body, primary, never;
    } teach;
    ChangePopover change_popover;
    float tooltip_x = 0.0f;
    float tooltip_y = 0.0f;

    bool focused = true;
    bool maximized = false;
    bool dark = false;
    bool backdrop_enabled = false;
    WindowEffect window_effect = WindowEffect::MicaAlt;
    std::wstring background_image;
    int wallpaper_look = 50; // interface transparency 0..90 (AppPrefs::wallpaper_look)
    int wallpaper_blur = 14; // wallpaper blur in DIPs 0..40
    bool safe_mode = false;
    bool address_editing = false;
    bool address_searching = false;
    bool address_search_current = false;
    bool address_search_content = false;
    bool settings_search_pinyin = true;
    bool settings_global_search_enabled = false;
    bool settings_global_search_capturing = false;
    std::wstring settings_global_search_hotkey;
    std::wstring settings_global_search_error;
    std::wstring settings_content_status, settings_content_summary;
    bool address_search_has_text = false;
    std::wstring address_search_text;
    std::wstring address_search_placeholder;
    std::wstring address_search_scope_label;
    float address_search_animation = 0.0f;
    float address_scope_animation = 0.0f;
    bool filter_editing = false;
    bool splitter_pressed = false;
    bool details_resize_pressed = false;
    bool column_resize_pressed = false;
    int hover_pane_index = -1;

    struct ContentFolderView { std::wstring path, status; bool error = false; };
    std::vector<ContentFolderView> settings_content_folders;
    bool settings_content_paused = false;
    bool settings_content_instant = false;
    unsigned settings_expanded = kSettingsDefaultExpandedMask;
    unsigned settings_preview_codecs = 0;  // DetectPreviewCodecs() mask, General page only
    // Settings > 预览增强包 (page 5), from SettingsController's cached pack state.
    uint32_t settings_pack_ffmpeg = 0;          // 0 none, 1 pack installed, 2 own FFmpeg in use
    bool settings_pack_ffmpeg_enabled = true;
    bool settings_pack_use_custom = false;
    bool settings_pack_remove_on_uninstall = true;
    uint32_t settings_pack_installed = 0;       // installed pack count
    bool settings_pack_media_available = false, settings_pack_images_available = false;
    bool settings_pack_media_installed = false; // the FFmpeg pack itself is on disk
    uint64_t settings_pack_bytes = 0;           // disk use of the packs folder
    std::wstring settings_pack_version;         // installed FFmpeg pack version
    std::wstring settings_pack_custom_path;     // chosen ffmpeg.exe
    std::wstring settings_pack_detected_path;   // ffmpeg.exe found on PATH
    std::wstring settings_pack_root;            // %LOCALAPPDATA%\Pulse\packs
    std::wstring settings_pack_notice;          // last action's message, empty when none
    bool settings_pack_installing = false;      // the FFmpeg pack is downloading
    float settings_pack_progress = 0.0f;        // 0..1 while downloading
    bool settings_pack_images_installed = false; // the image pack (现代图像格式) is on disk
    bool settings_pack_images_enabled = true;
    bool settings_pack_images_installing = false;
    float settings_pack_images_progress = 0.0f;
    std::wstring settings_pack_images_version;
    std::wstring settings_pack_images_notice;
    bool settings_pack_raw_installed = false, settings_pack_raw_enabled = true, settings_pack_raw_installing = false;
    bool settings_pack_raw_available = false;
    float settings_pack_raw_progress = 0.0f;
    std::wstring settings_pack_raw_version, settings_pack_raw_notice;
    bool settings_pack_archive_installed = false, settings_pack_archive_enabled = true, settings_pack_archive_installing = false;
    bool settings_pack_archive_available = false;
    float settings_pack_archive_progress = 0.0f;
    std::wstring settings_pack_archive_version, settings_pack_archive_notice;
    int settings_theme = 0; // system, light, dark
    bool settings_open = false;
    int settings_page = 0; // 0 general, 1 search/index, 2 context menu, 3 about, 4 duplicates
    float settings_scroll = 0.0f;
    bool settings_launch_on_startup = false;
    bool settings_start_in_tray = false;
    bool settings_keep_running = false;
    bool settings_show_hidden_files = false;
    bool settings_show_protected_os_files = false;
    bool show_pinned_tab_names = true;
    bool settings_list_smart_date = true;
    bool settings_list_zebra_rows = true;
    bool settings_list_size_bar = false;
    bool settings_list_tag_names = false;
    bool settings_list_selection_outline = false;
    bool settings_list_thumbnail_badges = true;
    bool settings_vertical_tabs = false;
    bool settings_show_hints = true;
    bool settings_tips_seen = false;   // any teaching bubble already shown
    int settings_folder_sort = 0; // 0 folders first, 1 follow direction, 2 mixed
    int settings_startup_open = 0; // 0 last tabs, 1 default location
    int settings_notify_icon = 0;  // 0 always, 1 in the background, 2 never (#57)
    int settings_new_tab_open = 0; // 0 current folder, 1 default location
    bool settings_close_last_tab = false;
    bool settings_close_tab_double_click = false;
    bool settings_confirm_delete = false;
    std::wstring settings_home_folder; // default location; empty = This PC
    int settings_text_render = 0; // 0 auto, 1 sharp, 2 smooth
    int settings_ui_font_scale = 100; // interface font size: 90 / 100 / 112 / 125
    bool settings_open_folders = false;
    bool settings_win_e = false;
    bool settings_this_pc = false;
    bool settings_explorer_windows = false;   // experimental Explorer window takeover
    // 设为默认文件管理器: 0 off, 1 partial, 2 full; the text lists what is missing.
    int settings_default_manager = 0;
    std::wstring settings_default_manager_desc;
    bool settings_integration_enabled = false;
    bool settings_integration_folders = false;
    bool settings_integration_win_e = false;
    bool settings_integration_this_pc = false;
    bool settings_integration_experimental = false;
    int settings_integration_state = 0; // 0 off, 1 on, 2 partial, 3 failed
    std::wstring settings_integration_summary;
    bool settings_integration_can_retry = false;
    bool settings_integration_can_restore = false;
    bool settings_shell_tags = false;
    int settings_blank_click_action = 0;   // app/blank_pane_click.h
    bool settings_change_tracking = false;
    int settings_change_days = 3;
    int settings_row_height = 34; // current row-height pref (DIPs) for density radios
    int settings_tray_icon = 48;  // current tray-deck icon pref (DIPs) for size radios
    int settings_language = 0;    // 0 system, 1 zh-CN, 2 zh-TW, 3 en-US
    BloomAccentPicker* settings_bloom = nullptr;
    bool settings_group_on[5] = { true, true, false, false, true };
    // 本地右键菜单 › Pulse 菜单 card: selected tab (0 on a file, 1 blank area,
    // 2 row buttons), the hidden-item mask, and where BuiltinMenuItem 0 sits
    // in settings_items.
    int settings_context_tab = 0;
    uint32_t settings_builtin_hidden = 0;
    size_t settings_builtin_first = 0;
    // Pulse menu order per BuiltinMenuSurface (normalized), and the preview
    // row being dragged: BuiltinMenuItem or -1, pointer position in px (y,
    // or x on the row-buttons tab) and where inside the row it was grabbed.
    app::BuiltinMenuOrder settings_builtin_order[static_cast<size_t>(app::BuiltinMenuSurface::Count)];
    int settings_menu_drag = -1;
    float settings_menu_drag_pos = 0.0f;
    float settings_menu_drag_grab = 0.0f;
    std::vector<SettingsRowView> settings_items;
    bool settings_index_service = false;
    bool settings_index_installed = false;
    std::wstring settings_index_status;
    std::wstring settings_index_path;
    bool settings_index_migrating = false;
    std::wstring settings_index_error;
    std::vector<IndexVolumeRowView> settings_index_volumes;
    std::vector<std::wstring> settings_index_excluded_paths;
    bool settings_index_system_known = false;   // service reports the system folder switch
    bool settings_index_exclude_system = false;
    uint32_t settings_index_system_groups = 0;  // bit i = index::kSystemExclusionGroups[i]
    bool settings_index_system_expanded = false;
    std::vector<NetworkRootRowView> settings_network_roots;
    std::wstring settings_version;
    std::wstring settings_build_id;
    std::vector<std::pair<std::wstring, std::wstring>> settings_about_rows; // label, value
    const std::vector<ReleaseNoteView>* settings_release_notes = nullptr;   // newest first
    int settings_release_expanded = 0;                                       // -1 = none
    std::wstring settings_update_status;
    std::wstring settings_update_version;
    bool settings_update_enabled = false;
    bool settings_update_checking = false;
    bool settings_update_downloading = false;
    bool settings_update_installing = false;
    bool settings_update_available = false;
    bool settings_update_auto = true;  // background checks + reminders (AppPrefs::auto_check_updates)
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
        PaneDetails,
        ToolbarSort,
        ToolbarMore,
        AddressBar,
        AddressSearch,
        AddressSearchInput,
        AddressSearchScope,
        AddressSearchMode,
        AddressSearchContent,
        AddressSearchOptions,
        ContentIndexManage,
        NetworkIndexAdd,
        SettingsContentIndex,
        SettingsFind, SettingsDisclosure, SettingsTheme, SettingsDropdown,
        SettingsContentAction,
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
        SidebarBlank,             // empty space in the sidebar: section menu
        TrayRelease,
        TrayClose,
        TrayItemRemove,
        TrayCard,
        TrayIntent,               // copy/move chip on the top tray card
        CompareDiffToggle,        // folder compare banner: differences only / show all
        CompareExit,              // folder compare banner: exit compare
        TrayClear,
        RowStar,
        RowFolderSize,
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
        DetailsPreviewEnable,   // preview pane on/off chip inside the well
        DetailsResize,
        StatusBar,
        StatusBarTask,
        StatusBarCancelSearch,
        SettingsNav,
        SettingsToggle,
        SettingsIntegration, // 0 master, 1 folders, 2 Win+E, 3 This PC, 4 experimental, 5 retry, 6 restore
        SettingsGlobalSearchHotkey,
        SettingsChangeDays,
        SettingsRestore,
        SettingsAccent,
        SettingsEffect,
        SettingsWallpaper,
        SettingsDensity,
        SettingsFolderSort,
        SettingsStartupOpen,
        SettingsNotifyIcon,
        SettingsNewTabOpen,
        SettingsBlankClick,
        SettingsHomeFolder,
        SettingsTextRender,
        SettingsUiFontSize,
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
        SettingsAboutAction,   // 0 copy info, 1 project page, 2 all releases, 3 LumenPDF, 4 LumaShot
        SettingsReleaseNote,
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
        SearchFilter,
        TrayPrev,                 // staging tray footer: previous card
        TrayNext,                  // staging tray footer: next card (top card to the back)
        PaneColumnLayout,
        ColumnStripRow,      // index = row, sub_index = column id
        ColumnStripDivider,  // sub_index = column id
        ColumnStripBlank,    // sub_index = column id
        ColumnStripHScroll,  // ancestor strip scrollbar
        SettingsWallpaperLook,
        SettingsWallpaperBlur,
        SidebarToggle,       // title-bar sidebar collapse button
        StatusHintAction,    // clickable action at the end of the status-bar hint
        SearchEmptyAction,   // suggestion button on an empty search result page; path = new query
        TeachPrimary,        // teaching bubble: Got it / Try it
        TeachDismiss,        // teaching bubble: close (x)
        TeachNever,          // teaching bubble: don't show tips
        TeachBubble,         // teaching bubble body (swallows clicks)
        FilterEmptyAction,   // pane filter with no matches: 0 search subfolders, 1 clear filter
        TrayDest,            // staging tray recent destination chip: index, path
        TrayStale,           // staging tray stale-items row: 0 find, 1 remove
        TrayCompare,         // staging tray compare: 0 footer toggle, 1 content check, 2 view diff
        GroupHeader,         // "group by" header (index = group): toggle collapse
        GroupSelect,         // header hover action (index = group): select the group
        ToolbarGroup,        // toolbar "Group" button / active chip: open the group menu
        ToolbarGroupClear,   // "x" on the active group chip: stop grouping
        SettingsPreviewStore,  // Settings > Quick Look formats: get a missing system extension (index = row)
        SettingsPackAction,    // Settings > 预览增强包: index = PackAction
        TabScroll,             // overflowing tab strip arrows: index -1 = left, 1 = right
    } region = None;
    SidebarAddAction sidebar_action = SidebarAddAction::None;
    int index = -1;          // tab/row/sidebar item/tray batch/tray item.
    int sub_index = -1;      // tray item inside batch, breadcrumb segment.
    int pane_index = -1;     // leaf in pane_slots, or -1 outside the content area.
    // Sidebar hits: the logical section (SidebarSectionId) and the row inside it,
    // so dragging can tell a pinned row from a section's own row.
    int sidebar_section = -1;
    int sidebar_item = -1;
    SortColumn column = SortColumn::Name;
    std::wstring path;
    // Name of the hovered sidebar row or section. The collapsed rail shows icons
    // only, so its tooltips read this.
    std::wstring label;
    D2D1_RECT_F control_bounds{};
};

class MainRenderer {
    friend struct FilenameRenderTest;
    friend struct PaneHeaderIconTest;
    friend struct FolderSizesUiTest;
    friend struct ColumnResizeUiTest;
    friend struct UiModuleAuditTest;
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
    void SetSidebarWidthDip(float width) {
        sidebar_width_dip_ = std::clamp(width, kSidebarMinWidthDip, kPanelWidthMaxDip);
        sidebar_width_ = sidebar_width_dip_ * scale_;
    }
    // Vertical tabs: the address row lives in the title bar, so the toolbar
    // keeps only its command row.
    void SetVerticalTabs(bool on) {
        vertical_tabs_ = on && !embedded_;
        toolbar_height_ = (vertical_tabs_ || embedded_ ? 44.0f : 88.0f) * scale_;
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
    // 0 = default width; otherwise the minimum search field width in DIP.
    void SetSearchBarMinWidth(float dip) { search_min_dip_ = dip; }
    // Toolbar "Group" button: -1 hidden, 0 not grouped, 1..4 app::GroupBy value.
    void SetToolbarGroup(int group_by) { toolbar_group_ = group_by; }
    int ToolbarGroup() const { return toolbar_group_; }
    // Drum "Group by" picker drawn over everything (app/group_wheel_ui.cpp).
    GroupWheel& GroupWheelPicker() { return group_wheel_; }
    float ToolbarGroupWidth(float w) const;
    std::wstring ToolbarGroupLabel() const;   // "Group: Date" text of the active chip
    float SearchBarMinWidth() const { return search_min_dip_; }
    D2D1_RECT_F SidebarToggleRect(float w) const;
    float PaneHeaderHeight() const { return pane_header_height_; }
    float ColumnHeaderHeight() const { return column_header_height_; }
    float RowHeight() const { return row_height_; }
    float ListRowHeightDip(const PaneViewModel& vm) const;
    // Same, but aware of the two-line (name + path) narrow search layout.
    float ListRowHeightDip(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const;
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
    D2D1_RECT_F PaneMediumIconsRect(const D2D1_RECT_F& pane_bounds,
                                    float filter_expand = 1.0f) const;
    D2D1_RECT_F PaneViewButtonRect(const D2D1_RECT_F& pane_bounds,
                                   float filter_expand = 1.0f) const;
    D2D1_RECT_F PaneDetailsRect(const D2D1_RECT_F& pane_bounds,
                                     float filter_expand = 1.0f) const;
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
    bool TickMotion(ULONGLONG now) {
        // Glides run on the NowMs clock; `now` (GetTickCount64) serves the
        // task pill and copy feedback below.
        const uint64_t motion_now = motion::NowMs();
        bool active = sidebar_pill_.Active(motion_now) || settings_nav_pill_.Active(motion_now) ||
                      settings_nav_hover_.Active(motion_now) || collapse_anim_;
        for (const auto& m : list_hover_motion_) active = active || m.Active(motion_now);
        for (const auto& m : list_shift_) active = active || m.Active(motion_now);
        for (const auto& m : view_morph_) active = active || m.Active(motion_now);
        for (const auto& m : link_pill_motion_) active = active || m.Active(motion_now);
        // Loading placeholders shimmer (and must appear on time) while any
        // pane is still enumerating.
        active = active || list_loading_active_;
        // Folder size skeleton shimmer and the scanning label's breathing.
        active = active || folder_size_anim_active_;
        // Operation pill: indeterminate ring spin and the completion check fade.
        active = active || task_pill_spinning_ ||
                 (task_pill_done_at_ != 0 && now - task_pill_done_at_ < kTaskPillDoneMs + 400);
        if (copy_feedback_.Tick(now)) active = true;
        if (group_wheel_.Tick(motion_now)) active = true;
        if (folder_thumbnail_cache_.Tick(now)) active = true;
        return active;
    }
    void RefreshFolderThumbnails() { folder_thumbnail_cache_.Refresh(); }
    // A preview pack was installed, removed or switched: files that had no
    // thumbnail may have one now, so cached results are dropped.
    void EvictThumbnails() { thumbnail_cache_.Evict(); }
    auto FolderThumbnailDebugStats() { return folder_thumbnail_cache_.ReadDebugStats(); }
    void SetFolderThumbnailsEnabled(bool enabled) { folder_thumbnails_enabled_ = enabled; }
    // region = HitTestResult region of the button that copied, index its index.
    void NotifyCopied(int region, int index) {
        copy_feedback_.Trigger(region, index, GetTickCount64());
    }
    void ScrollDetailsPreview(float steps, float x, float y, bool horizontal = false);
    void ToggleDetailsPreviewFit(float x, float y);

    // Date = modified. Values double as details_column_set.h bits: append only.
    enum class ColumnKind : uint8_t { Name, Path, Date, Type, Size, Created, Accessed };
    static constexpr size_t kMaxDetailsColumns = 7;
    static_assert(kDetailsColumnModified == 1u << static_cast<uint32_t>(ColumnKind::Date) &&
                  kDetailsColumnType == 1u << static_cast<uint32_t>(ColumnKind::Type) &&
                  kDetailsColumnSize == 1u << static_cast<uint32_t>(ColumnKind::Size) &&
                  kDetailsColumnCreated == 1u << static_cast<uint32_t>(ColumnKind::Created) &&
                  kDetailsColumnAccessed == 1u << static_cast<uint32_t>(ColumnKind::Accessed));
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
                                        ColumnKind::Size, ColumnKind::Size};
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
                              std::array<float, 4>& search_dividers, int divider_index) const;
    DetailsColumnLayout DetailsColumns(
        const D2D1_RECT_F& pane_bounds,
        const DetailsColumnWidths& dividers = {},
        bool search_view = false,
        const std::array<float, 4>& search_dividers = {}) const;
    DetailsColumnLayout DetailsColumns(const D2D1_RECT_F& pane_bounds,
                                       const PaneViewModel& vm) const;
    DetailsColumnWidths ResizeDetailsColumnDivider(
        const D2D1_RECT_F& pane_bounds,
        const DetailsColumnWidths& dividers,
        int divider_index, float cursor_x) const;
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
    D2D1_RECT_F SearchBarRect(float w) const;
    D2D1_RECT_F NewCommandRect(float w) const;
    D2D1_RECT_F SplitCommandRect(float w) const;
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
    // Settings slider value under x (0 transparency, 1 blur), clamped to the
    // track so a drag may leave the control.
    // Pulse menu preview drag: where row `item` starts along the drag axis
    // (NaN when it is not in the preview), and the order a drop at (x, y)
    // gives (empty when the pointer is too far outside the preview).
    float SettingsMenuRowStart(const WindowViewModel& vm, const D2D1_RECT_F& rect, int item);
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
    float SettingsMaxScroll(const WindowViewModel& vm, float window_w, float window_h) const;
    D2D1_RECT_F SettingsDropdownBounds(const WindowViewModel& vm, int index, float window_w, float window_h) const;
    float SettingsDestinationOffset(const WindowViewModel& vm, int setting_id, float window_w, float window_h) const;
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
        // Too many tabs for the strip: inactive tabs shrink to their icon
        // (`compact`) while the active one keeps its title (`active_w`); past
        // that the strip scrolls between the two arrows (`overflow`).
        int active = -1;          // visible, unpinned active tab, or -1
        float active_w = 0.0f;
        float pinned_w = 0.0f;
        bool compact = false;
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
    void DrawSidebar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawPane(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
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
    bool HitTestColumnStrip(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                            float x, float y, HitTestResult& out) const;
    void DrawStatusBar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawTeachBubble(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    // File-operation capsule in the status bar: ring progress while running,
    // accent check on success, then settles back to plain summary text.
    void DrawTaskPill(const WindowViewModel& vm, const D2D1_RECT_F& area, const Theme& theme);
    void DrawSettings(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawSettingsContext(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawSettingsPulseMenu(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawSettingsCore(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawSettingsPacks(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme);

    void DrawList(const PaneViewModel& vm, float x, float y, float w, float h, const Theme& theme,
                  int hover_region = 0, int hover_control_index = -1, bool pane_focused = true,
                  int pane_index = 0);
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
    void DrawFolderIcon(float x, float y, float size, const Theme& theme,
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
    // `truncated` (optional, costs one extra layout): the wrapped name did not fit.
    void DrawCenteredIconName(const std::wstring& name, const D2D1_RECT_F& bounds,
                              const D2D1_COLOR_F& color, const Theme& theme,
                              const std::vector<NameMatchRange>& matches, bool* truncated = nullptr);
    // Title-bar product mark from the app icon resource (nullptr until loaded).
    ID2D1Bitmap* LogoBitmap();

    WindowMaterial material_;
    Compositor* compositor_ = nullptr;
    fluent::Painter painter_;
    ShellIconCache icon_cache_;
    OpenWithIconCache open_with_icons_;
    std::unordered_map<ID2D1Bitmap*, ComPtr<ID2D1Effect>> tray_shadows_;
    // Grid/list thumbnails and the details-pane preview decode through
    // separate hosts and budgets: a 2048 px selection preview (up to 16 MB)
    // must neither evict the visible grid nor queue ahead of it.
    ThumbnailCache thumbnail_cache_{96ull * 1024ull * 1024ull, 1024};
    FolderThumbnailCache folder_thumbnail_cache_;
    bool folder_thumbnails_enabled_ = true;
    ThumbnailCache details_cache_{48ull * 1024ull * 1024ull, 24};
    PreviewHandlerHost preview_handler_;
    HWND notify_hwnd_ = nullptr;
    bool embedded_ = false;
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
    int toolbar_group_ = -1;
    bool sidebar_collapsed_ = false;
    bool sidebar_peek_ = false;
    bool collapse_anim_ = false;
    GroupWheel group_wheel_;
    float collapse_value_ = 0.0f;  // 0 expanded .. 1 rail (frame-stable)
    float collapse_from_ = 0.0f;
    uint64_t collapse_start_ = 0;  // motion::NowMs()
    static constexpr float kSidebarCollapseMs = 200.0f;
    float pane_header_height_ = 40.0f;
    float column_header_height_ = 32.0f;
    float row_height_ = 34.0f;
    float row_height_dip_ = 34.0f;
    bool list_smart_date_ = true, list_zebra_ = true, list_size_bar_ = false;
    bool list_tag_names_ = false;
    bool list_selection_outline_ = false;
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
    std::array<uint64_t, 8> list_loading_since_{};
    bool list_loading_animate_ = true;
    bool list_loading_active_ = false;
    // Tab strip overflow state: ComputeTabStrip clamps the scroll and reveals
    // a newly active tab, so hit testing and drawing always agree.
    mutable float tab_scroll_ = 0.0f;
    mutable int tab_scroll_active_ = -1;
    float tab_freeze_w_ = 0.0f, tab_freeze_active_w_ = 0.0f;
    size_t tab_freeze_count_ = 0;
    bool tab_freeze_compact_ = false;
    std::array<uint64_t, 8> folder_size_pending_since_{};
    bool folder_size_anim_active_ = false;
    motion::CopyFeedback copy_feedback_;
    static constexpr ULONGLONG kTaskPillDoneMs = 1600;
    static constexpr ULONGLONG kTaskPillFadeMs = 300;
    bool task_pill_was_active_ = false;
    bool task_pill_spinning_ = false;
    bool task_pill_animate_ = true;
    ULONGLONG task_pill_done_at_ = 0;
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
    mutable ComPtr<IDWriteTextFormat> tray_folder_format_;
    mutable float tray_formats_scale_ = 0.0f;
    float margin_ = 4.0f;
    float control_gap_ = 4.0f;
    D2D1_COLOR_F text_background_{};
    // Per-frame layer opacity (see ComputeLayerAlphas); 1 when opaque.
    float sheet_alpha_ = 1.0f;
    float card_alpha_ = 1.0f;
    bool details_visible_ = false;
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
    ComPtr<ID2D1StrokeStyle> paneHeaderStroke_;
    ComPtr<ID2D1PathGeometry> link_arrow_geometry_;
    ComPtr<ID2D1PathGeometry> play_triangle_geometry_;
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
