// app_model.h — Split tree, Pane, Tab, staging tray models.
#pragma once
#include "../fs/fs_enum.h"
#include "../fs/fs_recycle.h"
#include "../fs/fs_snapshot.h"
#include "../ui/ui_renderer.h"
#include "places.h"
#include "column_view_model.h"
#include "explorer_handoff.h"
#include "entry_order_hold.h"
#include "pane_header_animation.h"
#include "../index/content_result_store.h"
#include <map>
#include <memory>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <stack>
#include <optional>
#include <set>
#include <cstdint>
#include <unordered_set>

namespace pulse { struct ContentSizeSummary; struct ContentSelectionRestore; }
namespace pulse::app {
struct ContentNavigationState;

// When navigating from a descendant to one of its ancestors, returns the
// immediate child of the destination that the user just left. Empty means the
// destination is not an ancestor (or either path is virtual).
std::wstring NavigationReturnChildName(const std::wstring& from_path,
                                       const std::wstring& destination_path);

struct Tab {
    std::unique_ptr<ExplorerNavigationLease> explorer_handoff;
    std::wstring current_path;
    std::stack<std::wstring> back_stack;
    std::stack<std::wstring> forward_stack;
    // Search origins belong to history entries, not to query strings.
    std::stack<std::optional<std::wstring>> back_search_origins;
    std::stack<std::optional<std::wstring>> forward_search_origins;

    int selected_index = -1;
    int selection_anchor = -1;
    bool all_selected = false;
    std::unordered_set<int> selected;
    float scroll_y = 0.0f;
    float scroll_x = 0.0f;
    ui::ViewMode view_mode = ui::ViewMode::Details;
    uint64_t view_generation = 1;
    ui::SortColumn sort_column = ui::SortColumn::Name;
    ui::SortDirection sort_direction = ui::SortDirection::Asc;
    // "Group by" (app::GroupBy value) for current_path. Only list-like views
    // draw group headers, so the worker sorts by EffectiveGroup() and icon
    // views keep the plain order.
    int group_by = 0;
    int EffectiveGroup() const {
        return view_mode == ui::ViewMode::Details || view_mode == ui::ViewMode::Content
            ? group_by : 0;
    }
    // Collapsed group keys per folder (session only).
    std::map<std::wstring, std::set<std::wstring>> collapsed_groups;
    uint64_t group_collapse_rev = 0;
    ui::DetailsColumnWidths details_column_dividers{};
    std::array<float, 4> search_column_dividers{};
    std::wstring filter_text;
    bool show_hidden_files = false;
    // Column (Miller) view toggle and user-dragged widths in DIP
    // (slot 0 = child column, slot k = k-th ancestor; see ColumnStripWidthDip).
    bool column_layout = false;
    std::vector<float> column_widths_dip;
    bool show_protected_os_files = false;
    std::wstring virtual_title; // tag/search views; empty for real folders
    std::wstring banner_title;
    std::wstring banner_message;
    std::wstring network_live_root;        // #74: banner offers adding this share to the network index
    std::wstring network_live_added_root;  // #74: share added from the banner, crawl pending
    bool net_readonly = false;
    uint64_t cache_unix = 0;
    int recent_filter = 0; // RecentFilter; transient per tab.
    int change_days = 7;
    uint32_t change_kind = UINT32_MAX;
    uint64_t change_cursor = 0;
    uint32_t change_request = 0;
    std::wstring change_empty_text, change_status_text;

    // Transient UI state.
    bool loading = false;
    uint64_t pending_generation = 0;
    uint64_t applied_generation = 0;
    fs::SnapshotPtr snapshot;
    std::wstring snapshot_path;
    size_t directory_count = 0;
    size_t file_count = 0;
    std::wstring pending_selected_name;
    std::wstring pending_preview_rename;
    std::vector<std::wstring> pending_selected_names;
    bool pending_ensure_selection_visible = false;
    // File Explorer order hold (#13): change patches and non-explicit
    // refreshes keep rows in place until F5, a new sort or reopening.
    bool refresh_keeps_order = false;      // the pending refresh merges into the shown order
    bool order_held = false;               // rows may be out of sort order
    std::vector<EntryRename> held_renames; // Pulse renames not yet seen by a refresh
    // Size order (#58): fingerprint of the folder totals the rows were sorted
    // with, and when they last moved for new totals (GetTickCount64).
    uint64_t folder_size_signature = 0;
    uint64_t folder_size_resorted_at = 0;
    // Audio column order (#92): same pair for the tags the rows were sorted with.
    uint64_t audio_meta_signature = 0;
    uint64_t audio_meta_resorted_at = 0;
    ColumnStripState column_strip; // listings shown beside the list in column view
    std::wstring git_root;
    std::shared_ptr<std::vector<fs::DirEntry>> search_entries;
    std::shared_ptr<index::ContentResultStore> content_results;
    std::vector<std::shared_ptr<ContentNavigationState>> content_navigation;
    uint64_t content_revision = 0;
    bool content_count_final = false;
    DWORD content_scan_error = ERROR_SUCCESS;
    DWORD content_subscription_error = ERROR_SUCCESS;
    index::ContentSubscriptionFailure content_subscription_failure = index::ContentSubscriptionFailure::None;
    bool content_sort_override = false;
    uint64_t content_scanned_files = 0;
    uint64_t content_total_files = 0;
    std::shared_ptr<ContentSelectionRestore> content_selection_restore;
    uint64_t selection_revision = 0;
    std::shared_ptr<ContentSizeSummary> content_size_summary;
    // Cached file-size summary of a snapshot selection (see SelectionSizeSummary).
    struct SelectionSizeCache {
        uint64_t revision = UINT64_MAX;
        const void* snapshot = nullptr;
        size_t entry_count = 0;
        int selected_count = -1;
        uint64_t bytes = 0;
        int files = 0;
        int folders = 0;
    };
    mutable SelectionSizeCache selection_size_cache;
    std::wstring content_filter;
    // Explicit bulk actions resolve off-page selections asynchronously. These
    // rows are pinned only for the duration of the action, not for browsing.
    std::map<size_t,index::ContentResultStore::Row> content_action_rows;
    bool content_action_ready = false;
    size_t content_action_count = 0;
    size_t EntryCount() const { if(content_action_ready && all_selected) return content_action_count; return content_results ? content_results->Count() : snapshot ? snapshot->size() : 0; }
    fs::DirEntry EntryAt(size_t index) const;
    size_t search_total = 0;
    size_t search_next_offset = 0;
    size_t pending_search_offset = 0;
    bool search_loading_more = false;
    bool search_awaiting_content = false;
    bool search_content_active = false;
    bool search_content_stopped = false;
    uint64_t search_index_revision = 0;
    uint64_t search_session_id = 0;
    uint64_t search_live_generation = 0;
    uint64_t filename_live_generation = 0;
    std::shared_ptr<std::vector<std::wstring>> search_snippets;
    std::wstring search_input_path;
    std::wstring search_input_text;
    std::wstring search_input_root;
    bool search_input_current = false;
    bool search_input_content = false;
    bool search_content_empty = false;
    bool search_relevance = true;
    bool search_allow_scan = false;
    std::wstring search_preserve_selection;
    // Focused content row identity, captured while row indices still match
    // content_revision; live deltas re-sort the store and are remapped by it.
    std::wstring content_focus_path;
    uint64_t content_focus_selection = UINT64_MAX;
    uint64_t content_focus_revision = UINT64_MAX;
    uint64_t content_order_revision = 0;
    std::vector<std::wstring> content_selected_paths;
    void RememberContentSelection();
    std::wstring search_origin_path;
    bool search_origin_valid = false;
    bool search_retaining_results = false;

    // Expensive snapshot-derived presentation data. These caches are mutable
    // because building a read-only view model must not turn an O(visible rows)
    // paint into an O(all search results) operation on every frame.
    mutable fs::SnapshotPtr view_cache_snapshot;
    mutable const PlacesCatalog* view_cache_places = nullptr;
    mutable uint64_t view_cache_places_revision = 0;
    mutable std::wstring view_cache_filter_text;
    mutable std::shared_ptr<const ui::PaneViewModel::FilterMap> view_filter_map;
    mutable std::shared_ptr<const ui::PaneViewModel::TagDots> view_tag_dots;
    mutable std::shared_ptr<ui::RowPresentationCache> view_row_cache;
    // Group spans over view rows; rebuilt when any input below changes.
    mutable std::shared_ptr<const ui::ListGroups> view_groups;
    mutable fs::SnapshotPtr view_groups_snapshot;
    mutable std::shared_ptr<const ui::PaneViewModel::FilterMap> view_groups_filter;
    mutable int view_groups_by = 0;
    mutable int view_groups_sort = -1;
    mutable uint64_t view_groups_day = 0;
    mutable uint64_t view_groups_rev = UINT64_MAX;

    // Folder compare (set by UpdateFolderCompare while two panes compare).
    // compare_counts is indexed by ui::CompareMark.
    std::shared_ptr<const std::vector<uint8_t>> compare_marks;
    std::array<int, 5> compare_counts{};
    bool compare_diff_only = false;
    mutable std::shared_ptr<const ui::PaneViewModel::FilterMap> view_compare_base;
    mutable std::shared_ptr<const std::vector<uint8_t>> view_compare_marks;
    mutable std::shared_ptr<const ui::PaneViewModel::FilterMap> view_compare_map;

    void ClearSelection();
    bool EntryVisible(int index) const;
    // A hidden entry needs "show hidden"; an entry that is hidden *and* system is
    // also a protected operating system file and needs its own option, the way
    // File Explorer gates desktop.ini and friends behind a second checkbox.
    bool AllowsAttributes(DWORD attrs) const;
    bool ShowsEveryEntry() const { return show_hidden_files && show_protected_os_files; }
    void SetShowHiddenFiles(bool show);
    void SetShowProtectedOsFiles(bool show);
    int CountBound() const;
    void MaterializeSelection();
    void SelectOnly(int index);
    void ToggleSelect(int index);
    void SelectRange(int from, int to);
    void SelectAll();
    void SelectIndices(const std::vector<int>& indices);
    void InvertIndices(const std::vector<int>& universe);
    void MoveFocus(int index, bool extend);
    bool IsSelected(int index) const;
    int SelectedCount() const;
    std::vector<int> SelectedIndices() const;
    // Sum of selected file sizes (folders counted, not sized). Cached per
    // selection revision + snapshot so the status bar stays O(1) per paint.
    // Not for content_results tabs (use ContentSelectionSize).
    void SelectionSizeSummary(uint64_t* bytes, int* files, int* folders) const;
    void RemapSelection(const std::vector<std::wstring>& names, const std::wstring& focus_name);

    // Navigation helpers.
    void NavigateTo(const std::wstring& path);
    bool CanGoBack() const { return !back_stack.empty(); }
    bool CanGoForward() const { return !forward_stack.empty(); }
    std::wstring GoBack();
    std::wstring GoForward();
    std::wstring GoUp();
    void SetSnapshot(fs::SnapshotPtr value);
};

bool NameMatchesPattern(std::wstring_view name, std::wstring_view needle);
void CollectFilterMatches(const Tab& tab, const PlacesCatalog* places, std::vector<int>& out);
index::ContentResultStore::Filter ContentFilter(const std::wstring& text, const PlacesCatalog& places);

// A browser-style tab group: named, colored; window tabs join via LayoutTab::tab_group.
struct TabGroup {
    int id = 0;
    std::wstring name;
    uint32_t color_rgb = 0;
    bool collapsed = false; // header shows only the chip; member tabs hide
};

// Move a contiguous run of len items at pos one slot left (dir<0) or right
// (dir>0). Returns the new run position (unchanged when out of range).
int MoveTabRun(std::vector<int>& order, int pos, int len, int dir);

// Collapsed-group chip drag geometry, in px: chips are px-positioned on the
// strip, unlike tabs which animate in slot units.

// Visual width of a collapsed group's drag block: the chip plus the gap the
// strip layout reserves after every chip.
float CollapsedChipBlockW(float chip_w, float chip_gap);

// True when the floating block ([block_left, block_left+block_w) px) has
// crossed the neighbor's (animated) center while moving in dir (<0 left,
// >0 right). Mirrors the expanded-run crossing rule.
bool ChipBlockCrossed(float block_left, float block_w, float neighbor_center, int dir);

// Track start (px) for a strip unit displaced by a block move: it currently
// sits at old_rest + cur_off and must land at new_rest.
float DisplacedRestDelta(float old_rest, float cur_off, float new_rest);

// Contiguous run [pos, pos+len) of group gid's members in a strip order
// (display position -> tab index; tab_group_of maps tab index -> group id)
// that contains display position at. {0,0} when at is out of range or the
// tab there is not in gid. Single tabs hop a whole group by MoveTabRun on
// this run, so a dragged tab can never land inside a group.
struct GroupRun { int pos = 0; int len = 0; };
GroupRun FindGroupRun(const std::vector<int>& order,
                      const std::vector<int>& tab_group_of,
                      int at, int gid);
bool MoveTabGroupAcrossFreeTab(std::vector<int>& order,
                               const std::vector<int>& tab_group_of,
                               int member_pos, int dir);

struct Pane {
    Tab view;
    bool focused = false;
    bool target = false;
    float filter_expand = 0.0f;
    PaneHeaderAnimation header_animation;
    float filter_animation_from = 0.0f;
    float filter_animation_target = 0.0f;
    uint64_t filter_animation_start = 0;

    Tab* ActiveTab() { return &view; }
    const Tab* ActiveTab() const { return &view; }

    // Open this folder in the pane's single view (replaces the current path).
    void NewTab(const std::wstring& path);
};

enum class SplitOrientation { Horizontal, Vertical };

// Vertical = left | right (vertical divider). Horizontal = top / bottom.
enum class LayoutPreset : int {
    Single = 0,
    TwoVertical = 1,
    TwoHorizontal = 2,
    Three = 3,
    FourGrid = 4,
};

struct SplitContainer {
    bool is_leaf = true;
    SplitOrientation orientation = SplitOrientation::Horizontal;
    float ratio = 0.5f;
    std::unique_ptr<SplitContainer> first;
    std::unique_ptr<SplitContainer> second;
    Pane* pane = nullptr;

    static std::unique_ptr<SplitContainer> CreateLeaf(Pane* pane);
    static std::unique_ptr<SplitContainer> Join(SplitOrientation orient, float ratio,
                                                std::unique_ptr<SplitContainer> first,
                                                std::unique_ptr<SplitContainer> second);
    void CollectPanes(std::vector<Pane*>& out) const;
};

struct SplitterLayout {
    SplitContainer* node = nullptr;
    D2D1_RECT_F hit_rect{};
    D2D1_RECT_F parent_bounds{};
    SplitOrientation orientation = SplitOrientation::Vertical;
};

size_t LayoutPresetCount(LayoutPreset preset);
std::unique_ptr<SplitContainer> MakePresetTree(LayoutPreset preset,
                                               const std::vector<Pane*>& panes);
void LayoutSplitTree(const SplitContainer& root, const D2D1_RECT_F& bounds, float gap,
                     std::vector<std::pair<Pane*, D2D1_RECT_F>>& out,
                     std::vector<SplitterLayout>* splitters = nullptr);
float ClampSplitRatio(float ratio, const D2D1_RECT_F& bounds, SplitOrientation orientation,
                      float gap);
void ApplySplitRatio(SplitContainer& node, const D2D1_RECT_F& parent_bounds, float gap,
                     float pointer_x, float pointer_y);
void CollectSplitRatios(const SplitContainer& node, std::vector<float>& out);
void ApplySplitRatios(SplitContainer& node, const std::vector<float>& ratios);

// Browse-level folder compare: top level only, names matched case-insensitively,
// files compared by modified time (2 s tolerance) and then size.
struct FolderCompareResult {
    std::shared_ptr<const std::vector<uint8_t>> marks_a, marks_b;
    std::array<int, 5> counts_a{}, counts_b{};
};
FolderCompareResult CompareFolderTabs(const Tab& a, const Tab& b);
void FillPaneViewModel(ui::PaneViewModel& out, const Pane& pane,
                       const PlacesCatalog* places = nullptr);

// One window tab = one working layout (pane count, split ratios, each column).
struct LayoutTab {
    std::wstring title; // empty = derive from the focused folder
    uint32_t marker_rgb = 0;
    bool pinned = false;
    int tab_group = 0;
    LayoutPreset layout = LayoutPreset::Single;
    std::vector<std::unique_ptr<Pane>> panes;
    std::unique_ptr<SplitContainer> root;
    int focused_index = 0;
    int target_index = -1;
    // Two-pane folder compare. Inputs of the last pass decide when to redo it.
    bool compare = false;
    bool compare_diff_only = false;
    fs::SnapshotPtr compare_snap_a, compare_snap_b;
    int compare_visibility = -1;

    Pane* FocusedPane();
    const Pane* FocusedPane() const;
    Tab* ActiveFolder();
    const Tab* ActiveFolder() const;
    std::vector<Pane*> VisiblePanes() const {
        std::vector<Pane*> result;
        if (root) root->CollectPanes(result);
        return result;
    }
};

struct WindowTabs {
    std::vector<std::unique_ptr<LayoutTab>> items;
    size_t active = 0;
    std::vector<TabGroup> tab_groups;
    int next_tab_group_id = 1;

    LayoutTab* Active() { return active < items.size() ? items[active].get() : nullptr; }
    const LayoutTab* Active() const { return active < items.size() ? items[active].get() : nullptr; }

    void EnsureDefault();
    LayoutTab& NewTab(const std::wstring& path);
    LayoutTab& NewTabAt(size_t index, const std::wstring& path);
    // Empty is an explicit This PC location, not an omitted default folder.
    LayoutTab& NewTabAtLocation(size_t index, const std::wstring& path) {
        auto& created = NewTabAt(index, path);
        if (path.empty()) {
            if (auto* folder = created.ActiveFolder()) folder->current_path.clear();
        }
        return created;
    }
    void CloseTab(size_t idx);
    void SwitchTab(size_t idx);
    void MoveTab(size_t from, size_t to);
};

std::unique_ptr<LayoutTab> MakeSingleLayoutTab(const std::wstring& path,
                                               const Tab* source = nullptr);
void RebuildLayoutRoot(LayoutTab& tab);
std::wstring LayoutTabTitle(const LayoutTab& tab);
std::wstring TabTitle(const std::wstring& path);
void FillWindowTabStrip(ui::WindowViewModel& vm, const WindowTabs& tabs);

// Pull every group's members into one contiguous run (group order by first
// appearance; member order preserved). Chromium keeps groups always
// contiguous (TabGroup::ListTabs contract); call after membership changes
// that can split a run. Remaps WindowTabs::active by pointer identity.
void NormalizeGroupRuns(WindowTabs& tabs);
bool SetTabGroup(WindowTabs& tabs, size_t index, int group);

// ---------------------------------------------------------------------------
// Staging tray: collect file paths into batches.
// ---------------------------------------------------------------------------
struct TrayItem {
    std::wstring path;
    bool exists = true;
    bool is_dir = false;
    DWORD attrs = 0;
    uint64_t size = 0;
};

struct TrayBatch {
    std::vector<TrayItem> items;
    bool move_intent = false;
    uint64_t total_size = 0;
};

class StagingTray {
public:
    const std::vector<TrayBatch>& batches() const { return batches_; }

    // Collect selected full paths. move_intent = true for Ctrl+X.
    void Collect(const std::vector<std::wstring>& paths, bool move_intent);
    void RemoveBatch(size_t idx);
    // Follow a rename done from the tray (batch rename) so the card stays live.
    void ReplacePath(const std::wstring& from, const std::wstring& to);
    // Re-probe local items (moved/deleted outside Pulse); true when any flag flipped.
    bool RefreshExists();
    // Drop every item whose file is gone; returns how many were removed.
    size_t RemoveMissing();
    // Flip a batch between copy and move on release (tray intent chip).
    void SetMoveIntent(size_t idx, bool move_intent);
    void RemoveItem(size_t batch_idx, size_t item_idx);
    void RemoveDeleted(const std::vector<std::wstring>& paths);
    void Clear();

    // For serialization.
    void ToJson(std::wstring& out) const;
    bool FromJson(const std::wstring& in);

private:
    std::vector<TrayBatch> batches_;
};

// ---------------------------------------------------------------------------
// Sidebar data (quick access + drives).
// ---------------------------------------------------------------------------
struct SidebarEntry {
    std::wstring label;
    std::wstring detail;
    std::wstring glyph;
    std::wstring fallback;
    std::wstring badge;
    uint32_t badge_rgb = 0x0078D4;
    D2D1_COLOR_F color = {};
    D2D1_COLOR_F tag_dot = {};
    bool danger = false;
    bool is_tag = false;
    bool show_count = false;
    int count = 0;
    std::wstring path;
    bool is_drive = false;
    float used_ratio = 0.0f;
    bool expandable = false;
    // Built-in quick-access link (BuiltinQuickAccess), -1 for user entries.
    // The quick-access menu toggles these one by one.
    int builtin = -1;
};

// Built-in quick-access links, in the order they are listed. The stored mask
// (session "quickAccessHidden") is indexed by these values, so changing the set
// or the order needs a session migration (see kSessionVersion in session.cpp).
enum class BuiltinQuickAccess : int {
    Recent = 0,
    Desktop,
    Downloads,
    RecycleBin,
    Count,
};

// Sidebar sections. The ids are stable: the collapse/hide bitmasks stored in the
// session are indexed by them, and the user may reorder the groups by dragging.
enum class SidebarSectionId : int {
    Workspaces = 0,
    QuickAccess,
    SavedSearches,
    Drives,
    Tags,
    Networks,
    Cloud,   // OneDrive accounts; leads the pane, the way Explorer shows them.
    Starred, // Starred items: a section of their own, next to quick access.
    Count,
};

inline constexpr int kSidebarSectionCount = static_cast<int>(SidebarSectionId::Count);

// Sections whose rows are the section itself: they render without a header and
// cannot be folded as a whole. Single source of truth — the view-model builder
// and the section menu both ask here.
constexpr bool IsHeaderlessSection(SidebarSectionId id) {
    return id == SidebarSectionId::Starred || id == SidebarSectionId::Cloud;
}

// Display order used until the user drags a section header to a new position.
std::vector<int> DefaultSidebarOrder();

// Index of a section inside WindowViewModel::sidebar, or -1 when the model does
// not carry it. Lets callers address sections by id instead of open-coding a
// find_if over the group list.
int SidebarSectionIndex(const ui::WindowViewModel& vm, int section_id);

struct SidebarModel {
    std::vector<SidebarEntry> system_networks;
    std::vector<SidebarEntry> quick_access;
    std::vector<SidebarEntry> saved_searches;
    std::vector<SidebarEntry> drives;
    std::vector<SidebarEntry> cloud;   // OneDrive accounts (empty while signed out)
    std::vector<SidebarEntry> starred; // the "starred items" root row
};

SidebarModel BuildSidebarModel(const fs::RecycleBinInfo* recycle = nullptr);
// Blocking volume queries: call only on a background worker.
void RefreshSidebarDriveCapacity(std::vector<SidebarEntry>& drives);

// ---------------------------------------------------------------------------
// View-model builders.
// ---------------------------------------------------------------------------
std::wstring FindGitRoot(const std::wstring& path);
ui::WindowViewModel BuildWindowViewModel(const Pane& pane,
                                         const SidebarModel& sidebar, bool focused, bool maximized, bool dark,
                                         const PlacesCatalog* places = nullptr,
                                         uint32_t sidebar_collapsed_mask = 0,
                                         uint32_t sidebar_hidden_mask = 0,
                                         bool starred_expanded = true,
                                         const std::vector<int>* sidebar_order = nullptr,
                                         uint32_t quick_access_hidden_mask = 0);

// Sanitizes a stored section order: keeps only known ids, appends the missing
// ones, drops duplicates. Returns DefaultSidebarOrder() when nothing survives.
std::vector<int> NormalizeSidebarOrder(const std::vector<int>& order);

} // namespace pulse::app
