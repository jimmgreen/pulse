// ui_view_model.h — Plain view models the app builds and MainRenderer draws
// (tabs, panes, sidebar, tray, hit results). Kept apart from ui_renderer.h so
// app headers can name them without the renderer class: edits to MainRenderer
// then rebuild only the sources that call it.
#pragma once
#include "../index/content_result_store.h"
#include "window_material.h"
#include "fluent_components.h"
#include "release_note_view.h"
#include "sysctx_view.h"
#include "shortcuts_view.h"
#include "ai_settings_view.h"
#include "ai_panel_view.h"
#include "view_layout.h"
#include "git_mark.h"
#include "mtime_hint.h"
#include "column_strip_layout.h"
#include "toolbar_layout.h"
#include "../common/menu_icon.h"
#include "thumbnail_cache.h"
#include "../app/builtin_menu_items.h"
#include "details_column_set.h"
#include "details_tree_rows.h"
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
#include <memory>

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
    uintptr_t id = 0;       // stable identity (app::LayoutTab*): tab motion only
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
    std::wstring alias;   // 别名 shown instead of the name (empty = none)
    size_t hidden_ext = 0;  // trailing characters of name not drawn (文件扩展名 off)
    std::wstring size_text;
    std::wstring date_text;
    std::wstring type_text;
    std::wstring path;
    // A phone's file: its shell id, which its thumbnail is read from (the
    // pulse:device: path names no file). Empty = path.
    std::wstring thumb_path;
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
    bool drive_mounted = false;  // This PC: mapped / user-mode mount (网络盘 chip in tiles)
    uint32_t launcher_rgb = 0;   // This PC program icon (网盘程序入口): brand tint, 0 = not one
    uint8_t net_kind = 0;        // pulse:networks device row: 1 PC, 2 NAS, 3 Mac (own icon), 0 = not one
    // This PC phone: fs::kPcDevice* bits (0 = not a phone) and one capacity
    // row per storage (used -1 = unknown).
    uint8_t device = 0;
    struct DeviceBar {
        std::wstring label;
        float used = -1.0f;
        std::wstring free_text;
    };
    std::vector<DeviceBar> device_bars;
};

// The name as drawn: without a known extension while 文件扩展名 is off.
inline std::wstring ShownName(const ListEntryView& e) {
    return e.hidden_ext && e.hidden_ext < e.name.size() ? e.name.substr(0, e.name.size() - e.hidden_ext) : e.name;
}

struct RowPresentationCache {
    fs::SnapshotPtr snapshot;
    uint64_t alias_revision = 0;   // rows rebuild when an alias changes
    uint64_t extension_revision = 0;  // ... or when 文件扩展名 is switched
    std::unordered_map<size_t, ListEntryView> rows;
    std::deque<size_t> order;
};

struct ChangeBadge {
    std::wstring label;
    std::wstring tooltip;
    int count = 0;
    bool has_deleted = false;
    int status = 0; // 0 recent, 1 older, 2 unavailable/scanning/offline
    uint64_t last_change = 0;  // FILETIME of the newest change inside (修改时间提示)
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
    // Source indices of folders probed empty under the current hidden-file
    // settings; drawn with the empty-folder icon and no chevron.
    std::unordered_set<int> empty_sources;
};

// Column view of a folder pane: ancestors on the left, the regular list in
// the middle, and the selected folder's contents on the right.
struct ColumnStripView {
    bool enabled = false;         // pane toggle state (header button)
    bool eligible = false;        // real folder view that can show columns
    std::vector<ColumnStripColumnView> ancestors; // root -> parent
    // Contents of the single selected folder. Multi-selection and no
    // selection show no extra column.
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
// Buttons on the strip above a file list (pane banner), main one first.
enum class PaneBannerAction : int {
    None = 0, Retry, GoUp, Reconnect, Unlock, Refresh, ManageIndex, AddNetworkIndex, Dismiss,
    NetworkSettings, LanRescan
};

// One chip of a tag view's condition bar or 细分 row.
struct TagQueryChipView {
    enum Kind { All = 0, Any = 1, Not = 2, Location = 3, RefineTag = 4, RefineLocation = 5, Add = 6, Save = 7 };
    std::wstring label;
    D2D1_COLOR_F color{};   // tag dot
    int kind = All;
    int count = -1;         // 细分 chips: the items it keeps
};

struct AiListOverlay;

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
    // Visible folders (source indices) probed empty under the current
    // hidden-file settings. Kept out of ListEntryView: that is cached per
    // snapshot, while emptiness changes without a new listing.
    std::unordered_set<int> empty_folders;
    ChangeBadge title_change_badge;
    bool is_changes = false;
    std::wstring change_empty_text, change_status_text;
    std::wstring change_time_label, change_type_label;
    bool change_has_more = false;
    std::wstring filter_text;
    float filter_expand = 0.0f;
    float header_controls_opacity = 1.0f;
    bool size_control = false;     // main window panes: icon views get the header size slider, the segment a photo wall button
    bool size_slider_open = false; // narrow pane: its size button opened the slider
    std::wstring banner_title;
    std::wstring banner_message;
    int banner_kind = 0; // 0 info, 1 success, 2 warning, 3 error, 4 neutral
    PaneBannerAction banner_actions[2] = {PaneBannerAction::None, PaneBannerAction::None};
    bool banner_dismissible = false;  // a close button that clears the banner
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
    // Rows being deleted: they slide out and fade from leaving_since (NowMs).
    std::unordered_set<std::wstring> leaving_names;
    uint64_t leaving_since = 0;
    // Pulse 助手 working on this folder: read walk, destination tags, the
    // sort's motion (ai_list_overlay.h).
    std::shared_ptr<const AiListOverlay> ai_list;
    // Rows a drag just dropped here: hidden while the drag's cards fly to
    // them, faded in from landing_reveal_at (NowMs; 0 = cards not sent yet).
    std::unordered_set<std::wstring> landing_names;
    uint64_t landing_reveal_at = 0;
    // Folder compare: CompareMark per source index (null = compare off).
    std::shared_ptr<const std::vector<uint8_t>> compare_marks;
    bool compare_active = false;
    bool compare_diff_only = false;
    std::array<int, 5> compare_counts{};   // indexed by CompareMark
    std::shared_ptr<const TagDots> tag_dots;
    // Hashes (StagedPathHash) of every staged item: rows show "已暂存".
    std::shared_ptr<const std::unordered_set<uint64_t>> staged;
    // In-app Git: source index -> working-tree state (git_mark.h). Rows tint
    // their name; Details files also get a pill in the badge slot.
    std::shared_ptr<const std::unordered_map<int, GitMark>> git_marks;
    GitMark GitMarkAt(int source) const {
        if (!git_marks) return GitMark::None;
        const auto it = git_marks->find(source);
        return it == git_marks->end() ? GitMark::None : it->second;
    }
    // The folder is inside a OneDrive root: rows show their sync state.
    bool onedrive_status = false;
    // Shell sync state per source index (PKEY_StorageProviderState values:
    // 1 online only ... 7 error, 8 warning, 9 excluded, 10 pending). Rows
    // without one fall back to their attributes.
    std::shared_ptr<const std::unordered_map<int, uint8_t>> onedrive_states;
    uint8_t OneDriveStateAt(int source) const {
        if (!onedrive_states) return 0;
        const auto it = onedrive_states->find(source);
        return it == onedrive_states->end() ? uint8_t{0} : it->second;
    }
    bool loading = false;
    bool search_retaining_results = false;
    bool can_go_back = false;
    bool can_go_forward = false;
    bool can_go_up = false;
    bool can_create = false;
    bool can_create_folder = false;  // 新建文件夹: can_create, or a writable phone folder (the app sets it)
    bool connect_server = false;  // the New button reads 连接服务器 (network page, \\server)
    bool is_file_system = false;
    bool curated_order = false;
    bool is_starred = false;
    bool is_recent = false;
    bool is_recycle = false;
    bool is_search = false;   // search results add a display-only 路径 column
    // Folder under a Cloud Files sync root: the Details view adds 状态.
    bool cloud_status_column = false;
    bool is_query_search = false;
    bool is_content_search = false;
    // Tag views (pulse:tag: / pulse:tags:): the condition bar and 细分 row.
    bool is_tag_query = false;
    std::vector<TagQueryChipView> tag_query_chips;
    std::vector<TagQueryChipView> tag_query_refine;
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
    std::wstring type_column_label;   // empty: 类型 (pulse:ai-list: 原因)
    int selected_index = -1;
    int selected_count = 0;
    bool all_selected = false;
    const std::unordered_set<int>* selected_indices = nullptr;
    int hover_index = -1;
    int drop_target_index = -1;   // folder row under an OLE drag (accent 2px stroke)
    float drop_spring_progress = -1.0f;  // 0..1 until the folder spring-opens; <0 = none
    bool header_drop = false;     // pane title bar is a navigate-to-folder target
    int rename_index = -1;        // name column is in edit mode; do not draw the label
    std::wstring rename_text;     // text being typed; icon grids grow the field to wrap it
    std::wstring rename_hint;     // note under the rename field (refused character, extension question)
    int rename_shake_ms = -1;     // since a refused character, while the field shakes; -1 = still
    int rename_hint_ms = 0;       // time the note has left (fades over the last 250 ms); -1 = stays
    float rename_field_w = 0.0f;  // icon grids: widest the field has been, it does not narrow while typing
    bool focus_first_hint = false; // first view row has keyboard focus but no selection
    int focus_ring_source = -1;    // unselected row holding keyboard focus (Ctrl+arrows)
    float scroll_y = 0.0f;
    float scroll_x = 0.0f;
    ViewMode view_mode = ViewMode::Details;
    float icon_dip = 0.0f;        // icon grids: thumbnail width in DIP (0 = the mode's preset)
    bool wall = false;            // icon grids: shown as a photo wall (rows from ListRowHeightDip)
    // "Try the photo wall" under the header of a photo folder shown as a grid:
    // GetTickCount64 when it appeared / goes (fades out from there); 0 = none.
    uint64_t wall_hint_since = 0;
    uint64_t wall_hint_until = 0;
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
    // Inline folder expansion (details view of a real folder): facts per
    // view row. filter_map then lists child rows after their folder, so it
    // is not sorted and ViewIndex uses tree->view_of_source instead.
    std::shared_ptr<const DetailsTreeRows> tree;
    // Details tree: the folder of the one selected child row, below `path`.
    // The breadcrumb shows the rest of the way there in faint segments.
    std::wstring tree_selection_dir;
    std::wstring tree_selection_rel;   // the same folder below the current one (a\b)
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

    // Spans for layout, or null when this view is not drawn grouped (List; a
    // photo wall gets no spans from the model). Spans must cover exactly
    // EntryCount() rows or the layout would drift.
    const ListGroups* Groups() const {
        if (!groups || groups->empty() || !ShowsGroups(view_mode)) return nullptr;
        const ListGroup& last = groups->back();
        return static_cast<size_t>(last.first + last.count) == EntryCount() ? groups.get() : nullptr;
    }

    int ViewIndex(int source_index) const {
        if (source_index < 0) return -1;
        if (tree) return tree->ViewOf(source_index);
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
    bool alias = false;   // text is the folder's 别名 and the mark leads it (mark on)
};

// Splits "C:\Users\TestUser" into [{C:\, C:\}, {Users, C:\Users}, ...].
// UNC roots split into a server segment plus a share segment. Pure; unit-tested.
std::vector<BreadcrumbSegment> SplitBreadcrumb(const std::wstring& path);

struct SidebarItem {
    std::wstring label;
    std::wstring detail;
    std::wstring icon_glyph;       // Segoe Fluent Icons codepoint string.
    std::wstring icon_path;        // shell icon drawn instead, once resolved (网盘: "::{CLSID}")
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
    int indent = 0;
    bool status_dot = false;
    D2D1_COLOR_F status_color = {};
    bool editing = false;
    bool expandable = false;
    bool expanded = false;
    bool starred_child = false;
    bool divider_above = false;    // 收藏夹: the user's separator, a line above the row
    bool bar_switch = false;       // 收藏夹 root: on hover, 改成横向收藏栏 left of its chevron
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
    // Rail mode: a count on the section's icon (网络位置: devices not seen yet), 0 = none.
    int badge_count = 0;
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

// One row of the open tray view (pulse_tray_redesign.html openHTML).
struct TrayOpenRow {
    uint64_t id = 0;
    std::wstring path, name, size;
    DWORD attrs = 0;
    bool is_dir = false;
    bool exists = true;
    bool checked = false;
    bool group_start = false;        // first row of its source folder: header above
    std::wstring group, group_path;  // source folder name / path
    float in_t = 1.0f;               // 0..1 stagger entrance
    float out_t = 0.0f;              // 0..1 Del slide-out
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

    // Shelf redesign (pulse_tray_redesign.html). The panel is one of three
    // layers: a 40 dip strip when empty, two drop halves while files are
    // dragged over the window, and the shelf (fan + release + 送到 chips).
    int stage = 0;                   // TrayStage: 0 strip, 1 drop, 2 shelf
    int prev_stage = -1;             // layer fading out during a swap, -1 none
    float swap = 1.0f;               // 0..1 layer swap progress (410 ms)
    float height_dip = 40.0f;        // animated panel height (spring)
    bool hide_empty = false;         // setting: the empty strip folds to 0 (fades below 40)
    bool drop_move = false;          // drop layer: 移动暂存 half is the active one
    bool move_mode = false;          // release moves (mode, flipped live by Shift)
    float mode_t = 0.0f;             // 0..1 copy -> move colour blend
    float label_t = 1.0f;            // 0..1 main label rise-in after a mode flip
    std::wstring target;             // current folder name; empty: not a target
    int conflicts = 0;               // staged names already in the current folder
    std::wstring sources_text;       // "来自 A、B 等 n 处"
    std::vector<TrayCardView> fan;   // up to three newest live items, oldest first
    float fan_spread = 0.0f;         // 0..1 fan hovered
    float bump = 0.0f;               // 0..1 progress of the count bump after a collect
    bool zipping = false;
    float zip_progress = 0.0f;       // 0..1 (ring)
    int flash_chip = -1;             // 送到 chip that just received items
    float flash_t = 1.0f;            // 0..1 flash progress
    int clip_count = 0;              // Ctrl+C offer (剪贴板 N 项 · 收入), 0 none
    float clip_t = 1.0f;             // 0..1 offer pop-in (450 ms)
    // Open view (stage 3).
    std::vector<TrayOpenRow> open_rows;  // filtered, grouped by source folder
    int filter = 0;                  // 0 all, 1 images, 2 documents, 3 other
    int filter_counts[4]{};          // per filter (0 = every item)
    int checked = 0;                 // checked live items
    int live_total = 0;              // existing items (未勾选 · 对全部 N 项)
    int target_count = 0;            // items the main button acts on
    float open_scroll = 0.0f;        // DIPs
    bool cmp_shown = false;          // compare card beside the sidebar
    float cmp_t = 1.0f;              // 0..1 card entrance
};

enum class TrayStage : int { Strip = 0, Drop = 1, Shelf = 2, Open = 3 };

// Panel height of each layer, DIPs (the drop layer never shrinks the panel).
inline float TrayShelfStageHeightDip(int stage, bool clip = false) {
    switch (static_cast<TrayStage>(stage)) {
    case TrayStage::Drop: return 112.0f;
    case TrayStage::Shelf: return clip ? 184.0f : 150.0f;  // + the 收入 row
    case TrayStage::Open: return 476.0f;  // capped at the sidebar height - 20
    default: return 40.0f;
    }
}

// Case-insensitive path hash for the "已暂存" row pill (separators and a
// trailing backslash ignored). Rows only hash their own path per frame.
inline uint64_t StagedPathHash(std::wstring_view path) noexcept {
    // Staged paths are normalized (long-path prefix); rows are not.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](wchar_t c) {
        if (c == L'/') c = L'\\';
        else if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
        else if (c >= 0x80) c = static_cast<wchar_t>(reinterpret_cast<uintptr_t>(
            CharLowerW(reinterpret_cast<LPWSTR>(static_cast<uintptr_t>(c)))));
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    };
    if (path.starts_with(L"\\\\?\\UNC\\")) {
        path.remove_prefix(8);
        mix(L'\\');
        mix(L'\\');
    } else if (path.starts_with(L"\\\\?\\")) {
        path.remove_prefix(4);
    }
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.remove_suffix(1);
    for (wchar_t c : path) mix(c);
    return h;
}

// HitTestResult::TrayShelf index values (sub_index: chip number).
enum class TrayShelfHit : int {
    Panel = 0,       // background: swallows the click
    Fan,             // card fan: drag the whole stack out
    Expand,          // ⌃ show every item
    More,            // ⋯ menu
    Release,         // main split button: copy / move to the current folder
    ReleaseMenu,     // ▾ part: mode + 送到 + choose folder
    Zip,
    Rename,
    Chip,            // 送到 chip (sub_index, path)
    Pick,            // ＋ chip: choose a folder
    DropCopy,        // drop layer halves
    DropMove,
    Strip,           // empty strip
    Clip,            // 剪贴板 N 项 · 收入 (strip button / shelf row)
    // Open view.
    Collapse,        // ⌄ back to the shelf
    Filter,          // type chip (sub_index: filter)
    Row,             // item row: toggle its check (sub_index: row)
    GroupOpen,       // 打开 › on a source header (path)
    SelNone,         // 取消: clear the checks
    Compare,         // ⇄ 比较这两个
    List,            // list background (wheel)
    // Compare card (beside the sidebar).
    Card,            // background: swallows the click
    CardAction,      // result line: compare content / view diff
    CardClose,
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
    // 下载并预览 (C4b): a file whose content is only in the cloud. The well
    // explains and offers the download instead of a preview.
    bool cloud_online = false;
    int cloud_download_percent = -1;   // 0..100 while downloading
    std::wstring cloud_provider;       // sidebar label of its cloud root
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
    D2D1_RECT_F cloud_download{};   // 下载并预览 inside the preview well
    D2D1_RECT_F attr_readonly{}, attr_hidden{}, attr_advanced{};
    D2D1_RECT_F security_change{};
    std::vector<D2D1_RECT_F> preset_chips;
    std::vector<int> preset_ids;    // tag_index per chip
    std::vector<D2D1_RECT_F> section_headers;
    std::vector<int> section_ids;   // bit index into collapsed_mask
    float content_height_dip = 0.0f; // unclipped content height for wheel clamp
};

// The AI chip at the left end of the status bar (app ai_status.h, Pulse 助手).
enum class StatusAiState : uint8_t { Hidden, Idle, Busy, Waiting, Done, Error, Warn };

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
    bool task_paused = false;      // paused or pausing: amber pill
    uint64_t task_failed_items = 0; // partial failure: red pill until the next operation
    bool task_can_undo = false;    // finished pill offers 撤销 (sub_index 0)
    bool task_can_open = false;    // finished pill offers 打开 (sub_index 1)
    std::wstring performance_text; // development diagnostics; empty hides it
    std::wstring performance_compact_text;
    StatusAiState ai_state = StatusAiState::Hidden;
    std::wstring ai_text;          // the chip's words (Idle: the client, may be empty)
    std::wstring ai_label;         // "AI"
    bool ai_open = false;          // Pulse 助手 is open (accent-soft chip)
    bool ai_unread = false;        // finished while closed: green dot
    bool ai_pressed = false;       // mouse down on the chip
    float ai_progress = -1.0f;     // 0..1 while a plan runs; negative hides the bar
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
    float ratio = 0.5f;    // share of the first pane (drag badge)
    bool snapped = false;  // being dragged and held at a magnet snap
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

// A folder under the hover card: its children, read off the window thread
// (app_hover_preview.cpp) and in the pane's order.
// The composition bar's kinds, by extension (direct files only).
enum HoverFolderKind : int {
    kHoverKindImage = 0, kHoverKindVideo, kHoverKindAudio, kHoverKindDocument,
    kHoverKindCode, kHoverKindArchive, kHoverKindOther, kHoverKindCount
};
struct HoverFolderItem {
    std::wstring name;
    DWORD attrs = 0;
    bool is_dir = false;
    int kind = kHoverKindOther;   // files: HoverFolderKind
    uint64_t size = 0;
    uint64_t modified = 0;        // FILETIME as uint64 (thumbnail cache key part)
    int children = -1;            // folders: entries inside, -1 not counted
};
struct HoverFolderInfo {
    enum class State : uint8_t { Reading, Ready, Denied, Failed };
    State state = State::Reading;
    std::vector<HoverFolderItem> items;   // the first kHoverFolderMaxItems children
    uint64_t folders = 0, files = 0;      // every child, also past the list's end
    std::array<uint32_t, kHoverKindCount> kinds{};
    std::wstring file_system;             // drives: "NTFS"
};
constexpr size_t kHoverFolderMaxItems = 5000;

// The hover preview card (Settings > 快速预览 > 预览方式 = 悬停预览): drawn
// over the window for the file under the pointer. Never hit-tested; the app
// hides it on click, wheel, keys and focus loss (app_hover_preview.cpp).
// A folder's card keeps the pointer instead: its list scrolls under the wheel.
struct HoverPreviewView {
    bool visible = false;
    D2D1_RECT_F anchor{};        // the hovered item, client px
    D2D1_POINT_2F pointer{};     // the pointer when the card opened, client px
    std::wstring path, name, type_text;
    DWORD attrs = 0;
    uint64_t size = 0;
    uint64_t modified = 0;       // FILETIME as uint64 (thumbnail cache key part)
    // Videos loop silently in the well once the card has been up for a
    // moment (owner: the window the player's child goes into).
    bool video = false;
    HWND owner = nullptr;
    unsigned long long shown_ms = 0;  // GetTickCount64 when the card opened
    // Formats that take a while to decode (video, PDF, RAW, archives ...):
    // the hovered row fills an accent line over the dwell (no card yet),
    // and the well sweeps a thin bar until the content arrives.
    bool slow = false;
    bool dwell = false;
    D2D1_RECT_F dwell_row{};
    float dwell_progress = 0.0f;  // 0..1
    // The card takes the content's shape (wide pictures and clips, tall
    // pages and portraits, text, or only the info for files without a
    // preview), sized by the 卡片尺寸 step and capped by the window.
    int size_step = 1;            // 0 small, 1 medium, 2 large
    bool zoom = false;            // Ctrl held: much larger, for a closer look
    bool instant = false;         // --shot: no size easing between frames
    // Folders and This PC drives: their contents instead of a preview.
    bool folder = false;
    bool drive = false;
    std::shared_ptr<const HoverFolderInfo> listing;  // null until the worker answers
    float folder_scroll = 0.0f;   // px the list is scrolled (clamped while drawn)
    std::wstring size_text;       // the 大小 fact: "1.24 GB", "≥ 1.6 GB", "—"
    bool size_running = false;    // still being counted
    uint64_t drive_total = 0, drive_free = 0;
    bool drive_locked = false;    // BitLocker volume not unlocked
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
    TriggerHover, TriggerSpace,                         // 预览方式: hover card / Space only
    HoverDelayShort, HoverDelayMedium, HoverDelayLong,  // 悬停延迟: 0.3 / 0.5 / 0.8 s
    HoverSizeSmall, HoverSizeMedium, HoverSizeLarge,    // 卡片尺寸: 小 / 中 / 大
    Update, ImagesUpdate, RawUpdate, ArchiveUpdate,     // 更新到: the catalog's newer release
    AutoUpdate,         // 自动更新增强包: on / off
    UpdateAll,          // 增强包 page: 全部更新 (every pack with a newer release)
    ManagePacks,        // 快速预览 page: open the 增强包 page
    ContentInstall,     // 内容搜索 module: download (asks first)
    ContentManage,      // 内容搜索 module: the index settings (搜索与索引)
    AiInstall,          // AI pack: download / update / repair (asks first)
    AiRemove,           // AI pack: uninstall (asks first)
    AiCancel,           // AI pack: stop the running download
    AiManage,           // AI pack: open 增强包 > AI 与 MCP (kSettingsPageAi)
};

// A tab dragged out of another window, over this one (multi-window M7h): the
// dragged window hides and this window draws the tab as a pill at the cursor,
// with what letting go does beside it.
struct DragGhostView {
    bool on = false;
    float x = 0.0f, y = 0.0f;  // the cursor, client px
    std::wstring title;        // the tab's title
    int panes = 1;             // its visible panes ("2 panes" on the pill)
    std::wstring hint;         // what letting go does
    size_t em_start = 0;       // the part of `hint` in the accent color (M7i)
    size_t em_length = 0;
    std::wstring detail;       // a smaller note after it (may be empty)
};

// 别名: one row of the list under the path editor ("fbb" → 发布包).
struct AddressSuggestionView {
    std::wstring label;   // the alias (plus any typed \rest), or a real subfolder's name
    std::wstring path;    // where choosing it goes
    std::wstring hint;    // right-aligned: 子文件夹 / 按拼音匹配 · Enter 前往 ...
    bool real = false;    // a real subfolder of the current folder (listed first)
};

// The filter box's # completion (filter_tag_complete.h).
struct FilterTagSuggestionView {
    std::wstring name;
    D2D1_COLOR_F color{};
    int count = 0;
};

struct WindowViewModel {
    AiPanelViewModel ai_panel;   // Pulse 助手 (ai_panel_view.h)
    std::wstring window_title;
    // Multi-window M7c: with two or more windows the title bar shows this
    // window's color dot and name in place of the product name.
    bool window_identity = false;
    std::wstring window_identity_name;
    uint32_t window_identity_rgb = 0;
    std::vector<TabView> tabs;
    std::vector<TabGroupView> tab_groups;
    int active_tab = 0;

    bool can_go_back = false;
    bool can_go_forward = false;
    bool paste_target = false;      // current folder is real and writable (toolbar paste)
    bool clipboard_files = false;   // CF_HDROP on the clipboard (refreshed on WM_CLIPBOARDUPDATE)
    // File commands the selection allows (app::SelectionCaps): drives, shares,
    // network locations and Recycle Bin items do not get all of them.
    bool selection_cut = false;
    bool selection_copy = false;
    bool selection_rename = false;
    bool selection_delete = false;
    // Toolbar selection chip: leading commands of the Pulse item menu.
    std::vector<SelectionChipAction> selection_actions;
    // Pinned toolbar commands (自定义工具栏), enabled per the current state.
    std::vector<SelectionChipAction> toolbar_pins;
    // Pinned apps after them (layout.pins[toolbar_pins.size() + i]).
    struct ToolbarAppPin {
        std::wstring name;
        common::MenuIconRef icon;
        // Against the selection, or the dragged items in drop mode:
        // 0 neutral, 1 opens all, 2 some, 3 none (app::AppFit).
        int fit = 0;
    };
    std::vector<ToolbarAppPin> toolbar_apps;
    // A file drag is over the window: the selection chip and the pinned
    // commands give way, the apps (drop targets) stay (prototype note 6).
    bool toolbar_drop_mode = false;
    int toolbar_drop_app = -1;            // app under the drag, index into toolbar_apps
    bool toolbar_drop_force = false;      // Shift: opening a type the app does not list
    bool toolbar_drop_pin = false;        // a dragged program goes onto the pins
    std::wstring toolbar_drop_label;      // "拖到软件上打开" / "固定到工具栏" (layout.pin_label)
    // 收藏栏 (favorites_bar_layout.h): the starred list under the command row.
    struct FavoritesBarItem {
        std::wstring label, path;
        bool folder = true;
        bool current = false;   // the active tab shows it
        bool offline = false;   // not reachable (cached): dimmed, takes no drop
        bool separator = false; // a thin line, not an item
        bool group = false;     // a group button (name + count + ⌄)
        int count = 0;          // a group's items
    };
    bool favorites_bar_on = false;
    bool favorites_bar_icon_only = false;
    std::vector<FavoritesBarItem> favorites_bar;
    std::wstring favorites_bar_hint;      // shown while the list is empty
    std::wstring favorites_bar_more;      // » tooltip
    // A favorite dragged along the bar (the toolbar pins' motion): its index,
    // left edge and drop below the row (px); out: letting go removes it.
    int favorites_bar_drag = -1;
    float favorites_bar_drag_left = 0.0f, favorites_bar_drag_dy = 0.0f;
    bool favorites_bar_drag_out = false;
    int favorites_bar_drop = -1;          // files dragged over this favorite folder
    bool favorites_bar_drop_add = false;  // items over the bar's empty part: 加入收藏夹
    bool favorites_here = false;          // the current folder is starred (address ★)
    int favorites_bar_focus = -1;         // keyboard focus (F6): an entry, favorites_bar.size() = »

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
    int tab_drop_index = -1;      // a tab dragged from another window lands before this tab (M6b)
    int tab_file_drop = -1;       // a file drag rests on this tab; it opens when the progress is full (M6e)
    float tab_file_drop_progress = 0.0f;
    bool content_drop = false;    // a tab dragged over the content area lands in content_drop_rects (M6d)
    bool content_drop_as_tab = false;
    std::vector<D2D1_RECT_F> content_drop_rects;  // one per pane it brings (M7h)
    DragGhostView drag_ghost;     // the dragged tab itself (M7h)
    // Sidebar section header drag: the id-identified section floats, the rest
    // stay put, and the insertion line marks the slot it would land in.
    int sidebar_group_drag_id = -1;
    float sidebar_group_gap_line_y = 0.0f; // px, 0 = hidden
    // Sidebar row drag (quick-access rows, vertical tabs, tags): the dragged
    // row (layout index) plus its own insertion line.
    int sidebar_pin_drag_index = -1;
    float sidebar_pin_gap_line_y = 0.0f; // px, 0 = hidden
    float sidebar_pin_drag_dy = 0.0f;    // cursor travel since the press (px)
    // Title-bar tab drag: floating tab follows the cursor (QFluent TabBar).
    int tab_drag_index = -1; // display index of the run's first tab, or -1
    int tab_drag_count = 1;  // >1: a whole group run floats as one block
    bool tab_drag_chip = false; // drag started from the group chip (collapse-safe)
    float tab_drag_x = 0.0f; // left edge of the floating tab (px)
    // Tear-off pull (motion v2 "tabdetach"): cursor travel off the strip, px,
    // and 0..1 of the distance at which the tab becomes a window.
    float tab_detach_dy = 0.0f;
    float tab_detach_pull = 0.0f;
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
    int frame_transparency = 10; // 侧边栏与标题栏 0..100 (AppPrefs::frame_transparency)
    bool safe_mode = false;
    bool address_editing = false;
    bool address_searching = false;
    std::vector<AddressSuggestionView> address_suggestions;
    std::wstring address_suggestion_header;   // 别名 / 当前文件夹里的同名文件夹优先
    int address_suggestion_selected = -1;
    std::vector<FilterTagSuggestionView> filter_tag_suggestions;
    int filter_tag_selected = -1;
    D2D1_RECT_F filter_tag_anchor{};   // the filter box (px); the list opens below it
    bool address_search_current = false;
    bool address_search_content = false;
    bool settings_search_pinyin = true;
    bool settings_global_search_enabled = false;
    bool settings_global_search_double_ctrl = true;
    int settings_global_search_double_ctrl_ms = 300;
    bool settings_dialog_jump_bar = true;
    bool settings_dialog_jump_auto = false;
    bool settings_jump_history = true;
    bool settings_jump_history_empty = true;
    bool settings_global_search_apps = true;
    bool settings_search_enabled = true;
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
    bool settings_pack_auto_update = true;      // 自动更新预览增强包
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
    std::wstring settings_pack_update_version;  // newer catalog release of the installed pack, or empty
    bool settings_pack_images_installed = false; // the image pack (现代图像格式) is on disk
    bool settings_pack_images_enabled = true;
    bool settings_pack_images_installing = false;
    float settings_pack_images_progress = 0.0f;
    std::wstring settings_pack_images_version;
    std::wstring settings_pack_images_notice;
    std::wstring settings_pack_images_update_version;
    bool settings_pack_raw_installed = false, settings_pack_raw_enabled = true, settings_pack_raw_installing = false;
    bool settings_pack_raw_available = false;
    float settings_pack_raw_progress = 0.0f;
    std::wstring settings_pack_raw_version, settings_pack_raw_notice, settings_pack_raw_update_version;
    bool settings_pack_archive_installed = false, settings_pack_archive_enabled = true, settings_pack_archive_installing = false;
    bool settings_pack_archive_available = false;
    float settings_pack_archive_progress = 0.0f;
    std::wstring settings_pack_archive_version, settings_pack_archive_notice, settings_pack_archive_update_version;
    int settings_theme = 0; // system, light, dark
    bool settings_open = false;
    // 0 外观, 1 search/index, 2 context menu, 3 about, 4 duplicates, 5 quick look, 6 system menu,
    // 7 shortcuts, 8 packs, 9 文件列表, 10 标签页与窗口, 11 启动与集成, 12 文件操作,
    // 13 AI 与 MCP (kSettingsPageAi, under 增强包)
    int settings_page = 0;
    // Settings > 系统右键菜单 (page 6): the model's last published view.
    std::shared_ptr<const SysCtxView> settings_sysctx;
    // Settings > 快捷键 (page 7): the page model's last view; the sidebar badge
    // (keys that may not work) is filled on every page.
    std::shared_ptr<const ShortcutsView> settings_shortcuts;
    // Settings > 增强包 > AI 与 MCP (kSettingsPageAi).
    std::shared_ptr<const AiPageView> settings_ai;
    int settings_shortcut_issues = 0;
    int settings_pack_updates = 0;   // 增强包 nav badge: packs with a newer release
    int settings_content_pack_state = 2;   // app::ContentPackState (2 = missing)
    int settings_ai_pack_state = 2;        // app::AiPackState, the same values
    bool settings_ai_pack_installing = false;
    float settings_ai_pack_progress = 0.0f;   // 0..1 while installing
    float settings_scroll = 0.0f;
    bool settings_launch_on_startup = false;
    bool settings_start_in_tray = false;
    bool settings_keep_running = false;
    bool settings_show_hidden_files = false;
    bool settings_show_protected_os_files = false;
    bool show_pinned_tab_names = true;
    bool settings_list_smart_date = true;
    bool settings_list_zebra_rows = true;
    bool settings_list_tree_sticky = true;
    bool settings_menu_acrylic = true;
    bool settings_list_size_bar = false;
    bool settings_list_tag_names = false;
    bool settings_list_selection_outline = false;
    bool settings_list_thumbnail_badges = true;
    // 缩略图缓存 row: 0 counting, 1 ready, 2 clearing, 3 cleared, 4 no cache
    // folder (SettingsController::ThumbCacheState).
    uint8_t settings_thumb_cache_phase = 0;
    uint64_t settings_thumb_cache_bytes = 0;
    uint64_t settings_thumb_cache_cleared = 0;
    MtimeHint settings_mtime;            // 修改时间提示
    bool settings_sync_onedrive = true;
    bool settings_sync_svn = true;
    bool settings_single_click = false;  // 鼠标与选择
    int settings_name_highlight = 0;
    bool settings_hover_select = false;
    bool settings_item_checkboxes = false;
    bool settings_vertical_tabs = false;
    bool settings_tray_hide_empty = false;
    // 文件操作 → 网络位置 (AppPrefs lan_discovery ...).
    bool settings_lan_discovery = true;
    bool settings_network_recent = true;
    bool settings_hidden_shares = false;
    int settings_lan_cache_mode = 0;  // 0 10 min, 1 1 h, 2 this session
    bool settings_show_hints = true;
    bool settings_tips_seen = false;   // any teaching bubble already shown
    int settings_folder_sort = 0; // 0 folders first, 1 follow direction, 2 mixed
    int settings_startup_open = 0; // 0 last tabs, 1 default location
    int settings_notify_icon = 0;  // 0 always, 1 in the background, 2 never (#57)
    int settings_new_tab_open = 0; // 0 current folder, 1 default location
    int settings_external_open = 0; // 0 smart, 1 tab, 2 new window (multi-window M7b)
    bool settings_close_last_tab = false;
    bool settings_close_tab_double_click = false;
    bool settings_confirm_delete = false;
    std::wstring settings_home_folder; // default location; empty = This PC
    int settings_text_render = 0; // 0 auto, 1 sharp, 2 smooth
    int settings_ui_font_scale = 100; // interface font size: 90 / 100 / 112 / 125
    std::wstring settings_ui_font_family; // interface font family; empty = built-in
    // 主题色 › 自定义: hue 0..360, saturation 0..100, lightness 0..100 (settings_expanded bit 5 opens it).
    int settings_accent_hsl[3] = {220, 60, 50};
    bool settings_open_folders = false;
    bool settings_win_e = false;
    bool settings_this_pc = false;
    bool settings_recycle_bin = false;
    bool settings_explorer_windows = false;   // experimental Explorer window takeover
    // 设为默认文件管理器: 0 off, 1 partial, 2 full; the text lists what is missing.
    int settings_default_manager = 0;
    std::wstring settings_default_manager_desc;
    bool settings_integration_enabled = false;
    bool settings_integration_folders = false;
    bool settings_integration_win_e = false;
    bool settings_integration_this_pc = false;
    bool settings_integration_recycle_bin = false;
    bool settings_integration_experimental = false;
    bool settings_integration_folders_compat = false; // 兼容打开方式
    int settings_integration_state = 0; // 0 off, 1 on, 2 partial, 3 failed
    std::wstring settings_integration_summary;
    bool settings_integration_can_retry = false;
    bool settings_integration_can_restore = false;
    // Read back from the registry: what actually opens in Pulse right now.
    bool settings_integration_folders_active = false;
    bool settings_integration_this_pc_active = false;
    bool settings_integration_win_e_active = false;
    bool settings_integration_just_enabled = false; // confirmation line after turning it on
    std::wstring settings_integration_other_exe;     // another Pulse copy owns the associations
    bool settings_shell_tags = false;
    bool settings_alias_mark = false;   // 别名旁显示小图标 (AliasStore)
    std::vector<std::wstring> settings_ai_marks;   // row titles marked 「助手刚改的」
    float settings_inset_left = 0.0f;   // the docked 助手 column left of Settings (pixels)
    int settings_blank_click_action = 0;   // app/blank_pane_click.h
    int settings_preview_trigger = 0;      // app::kPreviewTrigger*
    int settings_hover_delay_ms = 500;     // 300 / 500 / 800
    int settings_hover_card_size = 1;      // 0 small, 1 medium, 2 large
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
    // 备份与迁移 (page 3): a task runs / an import waits for the restart /
    // a pre-import backup exists, and its time.
    bool settings_transfer_busy = false;
    bool settings_transfer_pending = false;
    bool settings_transfer_backup = false;
    std::wstring settings_transfer_backup_time;
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
        PaneSizeSlider,     // icon views: the pane header's size slider (index = pane)
        PaneSizeButton,     // narrow pane: opens that slider over the title (index = pane)
        PaneWall,           // main window panes: the view segment's photo wall button (index = pane)
        ToolbarSort,
        ToolbarMore,
        ToolbarSelAction,   // selection chip: quick actions, kSelChipMore, kSelChipClear, -1 chip body
        ToolbarPin,         // pinned command or app (自定义工具栏): index into layout.pins
        ToolbarPinLabel,    // drop mode: the label ahead of the pinned apps
        FavoritesBarItem,   // 收藏栏: index into favorites_bar, -1 the bar's empty part
        FavoritesBarMore,   // 收藏栏 »: index = the first favorite folded into it
        FavoritesBarChevron,// 收藏栏: a hovered folder's ⌄ (its subfolders), index as FavoritesBarItem
        AddressFavorites,   // the address bar's ★⌄ while the 收藏栏 is hidden: the favorites dropdown
        AddressBar,
        AddressSearch,
        AddressSearchInput,
        AddressSearchScope,
        AddressSearchMode,
        AddressSearchContent,
        AddressSearchOptions,
        ContentIndexManage,
        NetworkIndexAdd,
        PaneBannerButton,         // index = ui::PaneBannerAction
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
        RowTagDot,   // a tag dot after the name: sub_index = the tag's index in the catalog
        RowFolderSize,
        RowNewTab,
        RowMore,
        RecentFilter,
        RecentClear,
        TagQueryChip,        // condition bar chip (index into tag_query_chips)
        TagQueryChipRemove,  // its ×
        TagQueryRefine,      // 细分 chip (index into tag_query_refine)
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
        DetailsCloudDownload,   // 下载并预览 button in the well of a cloud-only file
        DetailsResize,
        StatusBar,
        StatusBarTask,
        StatusBarCancelSearch,
        SettingsNav,
        SettingsToggle,
        SettingsIntegration, // 0 master, 4 experimental, 5 retry/restore, 6 restore all, 7 Recycle Bin, 8 try it
        SettingsGlobalSearchHotkey,
        SettingsChangeDays,
        SettingsRestore,
        SettingsAccent,
        SettingsEffect,
        SettingsWallpaper,
        SettingsDensity,
        SettingsMtimeStyle,
        SettingsMtimeRange,
        SettingsMtimePalette,
        SettingsFolderSort,
        SettingsStartupOpen,
        SettingsNotifyIcon,
        SettingsNewTabOpen,
        SettingsExternalOpen,
        SettingsBlankClick,
        SettingsHomeFolder,
        SettingsLanCache,  // 发现结果保留: 0 10 min, 1 1 h, 2 this session
        SettingsThumbCache,
        SettingsTextRender,
        SettingsUiFontSize,
        SettingsTrayIcon,
        SettingsDoubleCtrlGap,
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
        SettingsFrameLook,     // 窗口效果 › 侧边栏与标题栏 slider; index = value
        SettingsAccentCustom,  // 主题色 › 自定义 button
        SettingsAccentSlider,  // sub_index 0 hue, 1 saturation, 2 lightness; index = value
        SidebarToggle,       // title-bar sidebar collapse button
        WindowIdentity,      // title-bar window color dot and name (M7c)
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
        NewButtonArrow,        // chevron segment of the split New button: open the New menu
        SettingsSysCtx,        // Settings > 系统右键菜单: index = SysCtxHit
        SettingsCtxPivot,      // Settings > 右键菜单 layer card 系统右键菜单: index = page (6)
        SettingsShortcuts,     // Settings > 快捷键: index = ShortcutsHit
        TrayShelf,             // staging tray shelf: index = TrayShelfHit
        StagedBadge,           // "已暂存" pill on a list row: index = entry, path
        PaneWallHint,          // photo wall suggestion: index 0 card, 1 switch, 2 never again
        StatusBarAi,           // the AI chip at the right end of the status bar
        SettingsAi,            // Settings > 增强包 > AI 与 MCP: index = AiHit
        SettingsClickMode,     // Settings > 鼠标与选择: 0/1 double/single click
        SettingsNameBand,      // Settings > 鼠标与选择: name band 0 on hover, 1 always
        RowCheck,              // check box in front of a list item: index = entry
        HeaderCheck,           // check box in the Name column header: select all / none
        TreeSticky,            // details tree folder pinned over the list top: index = view row
        SidebarBarSwitch,      // 收藏夹 root row: move the list to the 收藏栏; index = run
        FavoritesBarDock,      // 收藏栏's right end: put the list back in the sidebar
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

class MainRenderer;
struct RenderCaches;
// Owns a MainRenderer where its definition is not visible (app_state.h).
struct MainRendererDeleter {
    void operator()(MainRenderer* renderer) const noexcept;
};
using MainRendererPtr = std::unique_ptr<MainRenderer, MainRendererDeleter>;
MainRendererPtr MakeMainRenderer(RenderCaches& caches);

} // namespace pulse::ui
