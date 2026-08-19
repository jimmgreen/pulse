// app_model.h — Split tree, Pane, Tab, staging tray models.
#pragma once
#include "../fs/fs_enum.h"
#include "../fs/fs_snapshot.h"
#include "../ui/ui_renderer.h"
#include "places.h"
#include <memory>
#include <array>
#include <string>
#include <utility>
#include <vector>
#include <stack>
#include <cstdint>
#include <unordered_set>

namespace pulse::app {

// When navigating from a descendant to one of its ancestors, returns the
// immediate child of the destination that the user just left. Empty means the
// destination is not an ancestor (or either path is virtual).
std::wstring NavigationReturnChildName(const std::wstring& from_path,
                                       const std::wstring& destination_path);

struct Tab {
    std::wstring current_path;
    std::stack<std::wstring> back_stack;
    std::stack<std::wstring> forward_stack;

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
    std::array<float, 3> details_column_dividers{};
    std::wstring filter_text;
    std::wstring virtual_title; // tag/search views; empty for real folders
    std::wstring banner_title;
    std::wstring banner_message;
    bool net_readonly = false;
    uint64_t cache_unix = 0;
    int tab_group = 0; // 0 = none; TabGroup::id of the owning group
    bool pinned = false; // Chrome semantics: icon-only, left cluster, no close

    // Transient UI state.
    bool loading = false;
    uint64_t pending_generation = 0;
    uint64_t applied_generation = 0;
    fs::SnapshotPtr snapshot;
    std::wstring snapshot_path;
    size_t directory_count = 0;
    size_t file_count = 0;
    std::wstring pending_selected_name;
    std::vector<std::wstring> pending_selected_names;
    bool pending_ensure_selection_visible = false;
    std::wstring git_root;
    std::shared_ptr<std::vector<fs::DirEntry>> search_entries;
    size_t search_total = 0;
    size_t search_next_offset = 0;
    size_t pending_search_offset = 0;
    bool search_loading_more = false;

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

    void ClearSelection();
    int CountBound() const;
    void MaterializeSelection();
    void SelectOnly(int index);
    void ToggleSelect(int index);
    void SelectRange(int from, int to);
    void SelectAll();
    void MoveFocus(int index, bool extend);
    bool IsSelected(int index) const;
    int SelectedCount() const;
    std::vector<int> SelectedIndices() const;
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

// A browser-style tab group: named, colored; tabs join via Tab::tab_group.
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

struct Pane {
    std::vector<std::unique_ptr<Tab>> tabs;
    size_t active_tab = 0;
    bool focused = true;
    bool target = false;
    float filter_expand = 0.0f;
    std::vector<TabGroup> tab_groups;
    int next_tab_group_id = 1;

    Tab* ActiveTab() { return active_tab < tabs.size() ? tabs[active_tab].get() : nullptr; }
    const Tab* ActiveTab() const { return active_tab < tabs.size() ? tabs[active_tab].get() : nullptr; }

    void NewTab(const std::wstring& path);
    void NewTabAt(size_t index, const std::wstring& path); // insert + activate
    void CloseTab(size_t idx);
    void SwitchTab(size_t idx);
    void MoveTab(size_t from, size_t to);
};

// Pull every group's members into one contiguous run (group order by first
// appearance; member order preserved). Chromium keeps groups always
// contiguous (TabGroup::ListTabs contract); call after membership changes
// that can split a run. Remaps Pane::active_tab by pointer identity.
void NormalizeGroupRuns(Pane& pane);

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
void FillPaneViewModel(ui::PaneViewModel& out, const Pane& pane,
                       const PlacesCatalog* places = nullptr);

// ---------------------------------------------------------------------------
// Staging tray: collect file paths into batches.
// ---------------------------------------------------------------------------
struct TrayItem {
    std::wstring path;
    bool exists = true;
    bool is_dir = false;
    DWORD attrs = 0;
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
    void RemoveItem(size_t batch_idx, size_t item_idx);
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
    D2D1_COLOR_F color = {};
    D2D1_COLOR_F tag_dot = {};
    bool danger = false;
    bool is_tag = false;
    bool show_count = false;
    int count = 0;
    std::wstring path;
    bool is_drive = false;
    float used_ratio = 0.0f;
};

struct SidebarModel {
    std::vector<SidebarEntry> quick_access;
    std::vector<SidebarEntry> drives;
};

SidebarModel BuildSidebarModel();

// ---------------------------------------------------------------------------
// View-model builders.
// ---------------------------------------------------------------------------
std::wstring FindGitRoot(const std::wstring& path);
ui::WindowViewModel BuildWindowViewModel(const Pane& pane,
                                         const SidebarModel& sidebar, bool focused, bool maximized, bool dark,
                                         const PlacesCatalog* places = nullptr,
                                         uint32_t sidebar_collapsed_mask = 0);

} // namespace pulse::app
