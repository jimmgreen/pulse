#ifdef PULSE_WITH_SELFTEST
// tab_transfer_test.cpp - Multi-window M5b: WindowTabs::TakeTab/InsertTab and
// MoveLayoutTab move a live tab between two windows sharing one core.
#include "app_internal.h"
#include "tab_detach.h"
#include "pane_drop.h"
#include "split_tree_shape.h"
#include "dir_notify_queue.h"
#include "folder_move_remap.h"
#include "tab_spring.h"
#include "tab_transfer.h"
#include "window_merge.h"
#include "context_menu.h"
#include "../common/localization.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>

namespace pulse::app {
namespace {
bool GroupRunsContiguous(const WindowTabs& tabs) {
    std::set<int> ended;
    int previous = 0;
    for (const auto& tab : tabs.items) {
        if (tab->tab_group != previous) {
            if (previous) ended.insert(previous);
            if (tab->tab_group && ended.contains(tab->tab_group)) return false;
        }
        previous = tab->tab_group;
    }
    return true;
}

bool HasGroup(const WindowTabs& tabs, int id) {
    for (const auto& group : tabs.tab_groups)
        if (group.id == id) return true;
    return false;
}

// Tabs on This PC: no folder loads and no watches in a hidden test window.
void Seed(WindowTabs& tabs, size_t count) {
    tabs = WindowTabs{};
    for (size_t i = 0; i < count; ++i) tabs.NewTabAtLocation(tabs.items.size(), L"");
}
}  // namespace

bool RunTabTransferTest() {
    const auto output = std::filesystem::absolute(L"bench_data/tab-transfer");
    std::filesystem::create_directories(output);
    std::ofstream log(output / L"results.log");
    bool ok = log.good();
    const auto check = [&](bool value, const char* label) {
        log << (value ? "[PASS] " : "[FAIL] ") << label << std::endl;
        ok &= value;
        return value;
    };

    // --- WindowTabs model ------------------------------------------------
    {
        WindowTabs one;
        Seed(one, 1);
        check(!one.TakeTab(0) && one.items.size() == 1, "the last tab of a window cannot be taken");
        LayoutTab* last = one.items[0].get();
        std::unique_ptr<LayoutTab> taken_last = one.TakeTab(0, true);
        check(taken_last.get() == last && one.items.empty() && one.active == 0 && !one.Active(),
              "a closing window may give up its last tab (M6a)");

        WindowTabs src;
        Seed(src, 4);
        src.items[0]->pinned = true;
        src.tab_groups.push_back(TabGroup{});
        src.tab_groups.back().id = 5;
        src.tab_groups.push_back(TabGroup{});
        src.tab_groups.back().id = 6;
        src.items[2]->tab_group = 5;
        src.items[3]->tab_group = 6;
        src.active = 3;
        LayoutTab* grouped = src.items[2].get();
        std::unique_ptr<LayoutTab> taken = src.TakeTab(2);
        check(taken.get() == grouped && taken->tab_group == 0 && !HasGroup(src, 5) && HasGroup(src, 6),
              "a taken tab leaves its group; the group it left empty is dropped, others stay");
        check(src.items.size() == 3 && src.active == 2 && src.items[2]->tab_group == 6,
              "the active tab after the taken one keeps its identity");
        LayoutTab* pinned = src.items[0].get();
        std::unique_ptr<LayoutTab> taken_pin = src.TakeTab(0);
        check(taken_pin.get() == pinned && taken_pin->pinned, "a pinned tab may leave and stays pinned");

        WindowTabs dst;
        Seed(dst, 4);
        dst.items[0]->pinned = true;
        dst.items[2]->tab_group = 9;
        dst.items[3]->tab_group = 9;
        size_t at = dst.InsertTab(std::move(taken_pin), 99);
        check(at == 1 && dst.items[1].get() == pinned && dst.active == 1,
              "a pinned tab lands at the end of the pinned region and becomes active");
        at = dst.InsertTab(std::move(taken), 0);
        check(at == 2 && dst.items[2].get() == grouped, "an unpinned tab cannot land among pinned tabs");
        std::unique_ptr<LayoutTab> loose = src.TakeTab(0);
        LayoutTab* loose_raw = loose.get();
        at = dst.InsertTab(std::move(loose), 5);  // between the two members of group 9
        check(at == 6 && dst.items[6].get() == loose_raw && GroupRunsContiguous(dst),
              "a tab that would split a group run lands after the run");
    }

    // --- Drag-out geometry (M6a) ------------------------------------------
    {
        const RECT r = DetachedWindowRect(SIZE{1200, 750}, POINT{900, 300}, POINT{150, 20}, POINT{8, 0});
        check(r.left == 742 && r.top == 280 && r.right == 1942 && r.bottom == 1030,
              "the detached window keeps the source size and the cursor keeps its grab point on the tab");
        check(!TabDetachDistanceReached(POINT{100, 20}, POINT{400, 55}, 1.5f) &&
                  TabDetachDistanceReached(POINT{100, 20}, POINT{100, 56}, 1.5f) &&
                  TabDetachDistanceReached(POINT{100, 40}, POINT{100, 16}, 1.0f),
              "a tab leaves the strip after 24 DIP up or down; sideways travel alone reorders");
        // M6b: tabs 0, 1 and 3 visible (2 is in a collapsed group), 4 in all.
        const TabSpan spans[] = {{0, RECT{100, 10, 300, 50}}, {1, RECT{304, 10, 504, 50}},
                                 {3, RECT{508, 10, 708, 50}}};
        const auto drop = [&](LONG x, LONG y) { return TabDropIndex(spans, 3, 4, POINT{x, y}, 12, 900); };
        check(drop(150, 30) == 0 && drop(250, 30) == 1 && drop(450, 30) == 3 && drop(650, 30) == 4,
              "a dropped tab lands before the first tab whose middle is right of the cursor, else at the end");
        check(drop(760, 30) == 4 && drop(900, 30) == 4,
              "the empty strip right of the tabs, up to the caption buttons, drops at the end");
        check(drop(901, 30) == -1 && drop(150, 62) == 0 && drop(150, 63) == -1 && drop(87, 30) == -1 &&
                  TabDropIndex(spans, 0, 4, POINT{150, 30}, 12, 900) == -1,
              "off the strip (above, below, left of the tabs, on the caption buttons, no visible tab) nothing is marked");
    }

    // --- M6d: content-area drop zones and panes -----------------------------
    {
        const D2D1_RECT_F content{0.0f, 0.0f, 1000.0f, 800.0f};
        const auto zone = [&](LONG x, LONG y) { return ContentDropZone(content, POINT{x, y}, 0.25f); };
        check(zone(500, 400) == ContentDrop::Center && zone(100, 400) == ContentDrop::Left &&
                  zone(900, 400) == ContentDrop::Right && zone(500, 50) == ContentDrop::Top &&
                  zone(500, 790) == ContentDrop::Bottom,
              "the outer quarter of each side splits there, the middle opens a tab");
        check(zone(100, 100) == ContentDrop::Left && zone(200, 30) == ContentDrop::Top &&
                  zone(1000, 400) == ContentDrop::None && zone(-1, 400) == ContentDrop::None,
              "a corner goes to the relatively nearer edge; outside the content nothing");
        check(GrownPreset(1, ContentDrop::Left) == LayoutPreset::TwoVertical &&
                  GrownPreset(1, ContentDrop::Bottom) == LayoutPreset::TwoHorizontal &&
                  GrownPreset(2, ContentDrop::Top) == LayoutPreset::Three &&
                  GrownPreset(3, ContentDrop::Right) == LayoutPreset::FourGrid &&
                  GrownPreset(4, ContentDrop::Left) == LayoutPreset::Single &&
                  GrownPreset(1, ContentDrop::Center) == LayoutPreset::Single,
              "one more pane: 1 -> 2 side by side or stacked, 2 -> 3, 3 -> 4, never a fifth");

        auto tab = MakeSingleLayoutTab(L"");
        Pane* const first = tab->panes[0].get();
        check(PaneDropRect(*tab, ContentDrop::Left, content, 1.0f).right <= 500.0f &&
                  PaneDropRect(*tab, ContentDrop::Left, content, 1.0f).left == 0.0f &&
                  PaneDropRect(*tab, ContentDrop::Bottom, content, 1.0f).top >= 400.0f,
              "the preview is where the pane will be: the left half, the bottom half");
        auto one = MakeSingleLayoutTab(L"");
        Pane* const left = one->panes[0].get();
        check(AddPaneFromTab(*tab, std::move(one), ContentDrop::Left) && tab->layout == LayoutPreset::TwoVertical &&
                  tab->panes.size() == 2 && tab->panes[0].get() == left && tab->panes[1].get() == first &&
                  tab->focused_index == 0 && left->focused && !first->focused,
              "dropped on the left edge the pane comes first and is focused");
        tab->target_index = 1;  // `first` is the copy/move target
        auto two = MakeSingleLayoutTab(L"");
        Pane* const right = two->panes[0].get();
        const bool added_right = AddPaneFromTab(*tab, std::move(two), ContentDrop::Right);
        const std::vector<Pane*> three = tab->VisiblePanes();
        check(added_right && tab->layout == LayoutPreset::Three && three.size() == 3 && three[0] == left &&
                  three[1] == first && three[2] == right && tab->panes[2].get() == right &&
                  tab->panes[static_cast<size_t>(tab->target_index)].get() == first,
              "on the right edge it comes last; the panes are stored in screen order and the target stays");
        auto four = MakeSingleLayoutTab(L"");
        check(AddPaneFromTab(*tab, std::move(four), ContentDrop::Bottom) && tab->layout == LayoutPreset::FourGrid &&
                  tab->VisiblePanes().size() == 4,
              "a fourth pane makes the grid");
        auto fifth = MakeSingleLayoutTab(L"");
        const LayoutTab* const fifth_raw = fifth.get();
        check(!CanAddPane(*tab, *fifth, ContentDrop::Left) && fifth_raw->panes.size() == 1 &&
                  tab->VisiblePanes().size() == 4,
              "a tab with four panes takes no fifth");
        auto split = MakeSingleLayoutTab(L"");
        split->panes.push_back(std::make_unique<Pane>());
        auto single = MakeSingleLayoutTab(L"");
        check(!CanAddPane(*single, *split, ContentDrop::Left),
              "a dragged tab that keeps hidden panes can only become a tab, not panes (M7h: visible ones may)");

        // A pane dragged out by its header, and Esc putting it back.
        const std::vector<Pane*> grid = tab->VisiblePanes();
        std::vector<float> ratios;
        CollectSplitRatios(*tab->root, ratios);
        for (size_t i = 0; i < ratios.size(); ++i) ratios[i] = 0.3f + 0.1f * static_cast<float>(i);
        ApplySplitRatios(*tab->root, ratios);
        tab->target_index = 1;  // grid[1], the pane dragged out, is the copy/move target
        grid[1]->target = true;
        PaneOrigin origin;
        auto taken = TakePaneAsTab(*tab, grid[1], origin);
        const std::vector<Pane*> rest = tab->VisiblePanes();
        check(taken && taken->panes.size() == 1 && taken->panes[0].get() == grid[1] && taken->FocusedPane() == grid[1] &&
                  taken->layout == LayoutPreset::Single && !grid[1]->target,
              "a pane dragged out by its header becomes a focused single-pane tab");
        check(rest.size() == 3 && rest[0] == grid[0] && rest[1] == grid[2] && rest[2] == grid[3] &&
                  tab->FocusedPane() == grid[3] && tab->target_index == -1 && origin.position == 1 &&
                  origin.layout == LayoutPreset::FourGrid && origin.shape == L"HVPPVPP" && origin.target,
              "the grid keeps three panes in screen order, the focused one still focused");
        std::vector<float> rest_ratios;
        CollectSplitRatios(*tab->root, rest_ratios);
        check(tab->layout == LayoutPreset::Custom && SplitTreeShape(*tab->root) == L"HPVPP" &&
                  rest_ratios == std::vector<float>{ratios[0], ratios[2]},
              "M7k: the top left pane closes up over the top row; the bottom row stays where it was");
        std::vector<float> restored;
        check(CanPutPaneBack(*tab, *taken, origin) && PutPaneBack(*tab, std::move(taken), origin) &&
                  tab->layout == LayoutPreset::FourGrid && tab->VisiblePanes() == grid && tab->FocusedPane() == grid[1] &&
                  tab->target_index >= 0 && tab->panes[static_cast<size_t>(tab->target_index)].get() == grid[1],
              "Esc puts it back in its place in the grid, focused and still the target");
        CollectSplitRatios(*tab->root, restored);
        check(restored == ratios, "the grid gets its split ratios back");
        auto pair = MakeSingleLayoutTab(L"");
        Pane* const kept = pair->panes[0].get();
        AddPaneFromTab(*pair, MakeSingleLayoutTab(L""), ContentDrop::Right);
        Pane* const leaving = pair->VisiblePanes()[1];
        PaneOrigin pair_origin;
        auto lone = TakePaneAsTab(*pair, leaving, pair_origin);
        check(lone && pair->layout == LayoutPreset::Single && pair->VisiblePanes().size() == 1 &&
                  pair->VisiblePanes()[0] == kept && kept->focused,
              "out of two panes, the other one fills the tab");
        PaneOrigin none;
        check(!TakePaneAsTab(*pair, kept, none) && !TakePaneAsTab(*tab, leaving, none) && pair->panes.size() == 1,
              "a tab's only pane, or a pane of another tab, is not taken");
        auto three_tab = MakeSingleLayoutTab(L"");
        AddPaneFromTab(*three_tab, MakeSingleLayoutTab(L""), ContentDrop::Right);
        AddPaneFromTab(*three_tab, MakeSingleLayoutTab(L""), ContentDrop::Right);
        PaneOrigin three_origin;
        const std::vector<Pane*> trio = three_tab->VisiblePanes();
        check(TakePaneAsTab(*three_tab, trio[0], three_origin) &&
                  three_tab->layout == LayoutPreset::TwoHorizontal && three_tab->VisiblePanes() ==
                      std::vector<Pane*>{trio[1], trio[2]},
              "M7k: out of three panes, the big left one leaves and the stacked right ones fill the tab");
        auto other_three = MakeSingleLayoutTab(L"");
        AddPaneFromTab(*other_three, MakeSingleLayoutTab(L""), ContentDrop::Right);
        AddPaneFromTab(*other_three, MakeSingleLayoutTab(L""), ContentDrop::Right);
        const std::vector<Pane*> trio2 = other_three->VisiblePanes();
        PaneOrigin top_origin;
        auto top = TakePaneAsTab(*other_three, trio2[1], top_origin);
        check(top && other_three->layout == LayoutPreset::TwoVertical &&
                  other_three->VisiblePanes() == std::vector<Pane*>{trio2[0], trio2[2]},
              "M7k: the top right one leaves and the lower right one takes the right column");
        check(PutPaneBack(*other_three, std::move(top), top_origin) && other_three->layout == LayoutPreset::Three &&
                  other_three->VisiblePanes() == trio2,
              "and Esc puts it back in its place");
    }

    // --- M6e: a file drag resting on a tab opens it -------------------------
    {
        int spring = -1;
        uint64_t since = 0;
        check(!AdvanceTabSpring(spring, since, 2, 0, 1000) && spring == 2 && since == 1000 &&
                  !AdvanceTabSpring(spring, since, 2, 0, 1000 + kTabSpringMs - 1),
              "resting on another tab starts the spring timer, and it does not open early");
        check(AdvanceTabSpring(spring, since, 2, 0, 1000 + kTabSpringMs) && spring == -1,
              "after 650 ms on the same tab it opens, and the rest starts over");
        check(!AdvanceTabSpring(spring, since, 1, 0, 3000) && !AdvanceTabSpring(spring, since, 3, 0, 3000 + kTabSpringMs) &&
                  spring == 3 && since == 3000 + kTabSpringMs,
              "moving to another tab restarts the timer");
        check(!AdvanceTabSpring(spring, since, 0, 0, 9000) && spring == -1 &&
                  !AdvanceTabSpring(spring, since, -1, 0, 9999) && spring == -1,
              "the active tab and the space off the tabs never spring");
    }

    // --- M6e: paths follow a moved or renamed folder -----------------------
    {
        const FolderMove move(L"\\\\?\\C:\\work\\proj", L"\\\\?\\D:\\archive\\proj-2025\\");
        check(move.Valid() && move.Remap(L"\\\\?\\C:\\work\\proj") == L"\\\\?\\D:\\archive\\proj-2025" &&
                  move.Remap(L"\\\\?\\c:\\WORK\\Proj\\src\\ui") == L"\\\\?\\D:\\archive\\proj-2025\\src\\ui",
              "the moved folder and the folders inside it map to the new place, ignoring case");
        check(!move.Remap(L"\\\\?\\C:\\work\\project") && !move.Remap(L"\\\\?\\C:\\work") &&
                  !move.Remap(L"pulse:search:proj") && !move.Remap(L""),
              "a sibling sharing the name prefix, the parent, virtual views and This PC stay put");
        const FolderMove unc(L"\\\\?\\UNC\\nas\\team\\docs", L"\\\\?\\UNC\\nas\\team\\Docs");
        check(unc.Valid() && unc.Remap(L"\\\\?\\UNC\\nas\\team\\docs\\a") == L"\\\\?\\UNC\\nas\\team\\Docs\\a",
              "a share folder renamed only in case follows too");
        check(!FolderMove(L"\\\\?\\C:\\", L"\\\\?\\D:\\x").Valid() &&
                  !FolderMove(L"\\\\?\\UNC\\nas\\team", L"\\\\?\\UNC\\nas\\other\\x").Valid() &&
                  !FolderMove(L"pulse:tag:1", L"\\\\?\\C:\\x").Valid() &&
                  !FolderMove(L"\\\\?\\C:\\x", L"\\\\?\\C:\\x").Valid(),
              "drive and share roots, virtual paths and a move onto itself remap nothing");
        std::stack<std::wstring> history;
        for (const wchar_t* entry : {L"\\\\?\\C:\\a", L"\\\\?\\C:\\work\\proj\\src", L"\\\\?\\C:\\b"}) history.push(entry);
        const bool changed = move.RemapHistory(history);
        std::vector<std::wstring> order;
        while (!history.empty()) { order.push_back(history.top()); history.pop(); }
        check(changed && order == std::vector<std::wstring>{L"\\\\?\\C:\\b", L"\\\\?\\D:\\archive\\proj-2025\\src", L"\\\\?\\C:\\a"},
              "history entries inside the folder follow it and keep their order");
    }

    // --- MoveLayoutTab between two windows --------------------------------
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"PulseTabTransferTest";
    RegisterClassW(&wc);
    const auto make_window = [&] {
        return CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, -30000, -30000, 200, 200,
                               nullptr, nullptr, wc.hInstance, nullptr);
    };
    auto core = std::make_unique<AppCore>();
    auto src = std::make_unique<AppState>(*core);
    auto dst = std::make_unique<AppState>(*core);
    src->hwnd = make_window();
    dst->hwnd = make_window();
    if (check(src->hwnd && dst->hwnd, "two hidden windows share one core")) {
        core->windows.Add(src->hwnd, src.get());
        core->windows.Add(dst->hwnd, dst.get());
        core->windows.Touch(dst->hwnd);
        Seed(src->window_tabs, 3);
        Seed(dst->window_tabs, 2);
        BindCurrentLayout(*dst);

        // The moved tab: split in two, focus on the right pane, scrolled and
        // with a selection in it.
        LayoutTab* moved = src->window_tabs.items[1].get();
        auto extra = MakeSingleLayoutTab(L"");
        extra->panes.front()->ActiveTab()->current_path.clear();
        moved->panes.push_back(std::move(extra->panes.front()));
        moved->root.reset();
        src->window_tabs.SwitchTab(1);
        BindCurrentLayout(*src);
        Pane* right = moved->panes[1].get();
        Tab* view = right->ActiveTab();
        view->scroll_y = 240.0f;
        view->selected_index = 3;
        src->pane = right;
        src->renameClickCandidate = true;
        src->renameClickTab = view;
        src->renameClickPane = right;
        src->listTypeAheadTab = view;
        src->tabFlashKey = moved;
        view->current_path = L"pulse:search:report";
        view->pending_generation = 12;

        check(!MoveLayoutTab(*src, 1, *src, 0) && src->window_tabs.items.size() == 3,
              "a tab is not moved into its own window");
        const bool moved_ok = MoveLayoutTab(*src, 1, *dst, 99);
        check(moved_ok && src->window_tabs.items.size() == 2 && dst->window_tabs.items.size() == 3 &&
                  dst->window_tabs.items[2].get() == moved && dst->window_tabs.Active() == moved,
              "the same live tab object moves to the end of the other window and is active there");
        check(dst->pane == right && right->focused && !moved->panes[0]->focused && moved->root,
              "the split moves with it and the focused pane stays focused");
        check(right->ActiveTab()->selected_index == 3 && dst->scrollTargetY == 240.0f,
              "selection and scroll position travel with the tab");
        bool src_bound = false;
        for (const auto& tab : src->window_tabs.items)
            for (const auto& pane : tab->panes) src_bound |= pane.get() == src->pane;
        check(src_bound && src->window_tabs.Active() != moved, "the source window binds one of its own tabs");
        check(!src->renameClickCandidate && !src->renameClickTab && !src->listTypeAheadTab && !src->tabFlashKey,
              "the source window forgets clicks, type-ahead and flash aimed at the moved tab");
        check(&SearchResultWindow(*src, 12) == dst.get(),
              "a search still running in the moved tab now reaches the window it moved to");
        view->pending_generation = 0;
        view->current_path.clear();

        Seed(src->window_tabs, 1);
        BindCurrentLayout(*src);
        check(!MoveLayoutTab(*src, 0, *dst, 0) && src->window_tabs.items.size() == 1 &&
                  dst->window_tabs.items.size() == 3,
              "a window's only tab stays (the move is refused, nothing changes)");
        LayoutTab* only = src->window_tabs.items[0].get();
        check(MoveLayoutTab(*src, 0, *dst, 1, TransferSource::Closing) && src->window_tabs.items.empty() &&
                  !src->pane && dst->window_tabs.items.size() == 4 && dst->window_tabs.items[1].get() == only &&
                  dst->window_tabs.Active() == only,
              "a closing window hands over its last tab, which lands at the requested place (Esc after a drag-out)");

        // --- M6d: a dragged tab becomes one more pane of another window -----
        Seed(src->window_tabs, 2);
        BindCurrentLayout(*src);
        Seed(dst->window_tabs, 1);
        BindCurrentLayout(*dst);
        LayoutTab* const dropped = src->window_tabs.items[1].get();
        Pane* const dropped_pane = dropped->panes[0].get();
        dropped_pane->ActiveTab()->selected_index = 2;
        check(MoveTabIntoPane(*src, 1, *dst, ContentDrop::Right) && src->window_tabs.items.size() == 1 &&
                  dst->window_tabs.items.size() == 1 && dst->window_tabs.Active()->VisiblePanes().size() == 2 &&
                  dst->window_tabs.Active()->VisiblePanes()[1] == dropped_pane && dst->pane == dropped_pane &&
                  dropped_pane->ActiveTab()->selected_index == 2,
              "a tab dropped on the right edge becomes the focused right pane, its selection kept");
        check(!MoveTabIntoPane(*src, 0, *dst, ContentDrop::Left) && src->window_tabs.items.size() == 1,
              "a window's only tab does not leave unless the window is closing");

        // --- M6d: a pane dragged out of a split, and Esc ------------------
        Seed(src->window_tabs, 1);
        LayoutTab* const split_tab = src->window_tabs.items[0].get();
        AddPaneFromTab(*split_tab, MakeSingleLayoutTab(L""), ContentDrop::Right);
        BindCurrentLayout(*src);
        Pane* const out_pane = split_tab->VisiblePanes()[1];
        out_pane->ActiveTab()->selected_index = 4;
        PaneOrigin pane_origin;
        const std::optional<size_t> out_at = SplitPaneIntoTab(*src, out_pane, pane_origin);
        check(out_at && *out_at == 1 && src->window_tabs.items.size() == 2 && src->window_tabs.active == 1 &&
                  src->pane == out_pane && split_tab->layout == LayoutPreset::Single &&
                  split_tab->VisiblePanes().size() == 1,
              "a pane dragged out becomes the active tab right after its split, which keeps one pane");
        const size_t dst_tabs = dst->window_tabs.items.size();
        check(MoveLayoutTab(*src, 1, *dst, 99) && src->window_tabs.items.size() == 1 &&
                  dst->window_tabs.items.size() == dst_tabs + 1 && dst->pane == out_pane,
              "its tab moves to the new window like a tab pulled off the strip");
        check(!ReturnTabAsPane(*dst, dst_tabs, *src, dst->window_tabs.items[0].get(), pane_origin) &&
                  dst->window_tabs.items.size() == dst_tabs + 1,
              "a pane only goes back into a tab of the window it came from");
        check(ReturnTabAsPane(*dst, dst_tabs, *src, split_tab, pane_origin) && src->window_tabs.items.size() == 1 &&
                  dst->window_tabs.items.size() == dst_tabs && split_tab->layout == LayoutPreset::TwoVertical &&
                  split_tab->VisiblePanes()[1] == out_pane && src->pane == out_pane &&
                  out_pane->ActiveTab()->selected_index == 4,
              "Esc: the pane goes back to its split, focused, its selection kept");

        // --- M6c: a whole window merges into another and comes back -------
        app::WindowTabs& st = src->window_tabs;
        app::WindowTabs& dt = dst->window_tabs;
        Seed(st, 4);
        st.items[0]->pinned = true;
        st.tab_groups.push_back(TabGroup{5, L"work", 0x3366CC, false});
        st.items[1]->tab_group = 5;
        st.items[2]->tab_group = 5;
        st.next_tab_group_id = 6;
        st.SwitchTab(2);
        BindCurrentLayout(*src);
        Seed(dt, 3);
        dt.items[0]->pinned = true;
        dt.tab_groups.push_back(TabGroup{1, L"docs", 0xCC6633, false});
        dt.items[1]->tab_group = 1;
        dt.items[2]->tab_group = 1;
        dt.next_tab_group_id = 2;
        BindCurrentLayout(*dst);
        std::vector<const LayoutTab*> old_src, old_dst;
        for (const auto& tab : st.items) old_src.push_back(tab.get());
        for (const auto& tab : dt.items) old_dst.push_back(tab.get());

        const WindowMergeUndo undo = MergeTabsInto(*src, *dst, 2);  // between the two "docs" tabs
        const auto in_dst = [&](size_t i) { return i < dt.items.size() ? dt.items[i].get() : nullptr; };
        check(st.items.empty() && dt.items.size() == 7 && in_dst(0) == old_dst[0] && in_dst(1) == old_src[0] &&
                  in_dst(2) == old_dst[1] && in_dst(3) == old_dst[2] && in_dst(4) == old_src[1] &&
                  in_dst(5) == old_src[2] && in_dst(6) == old_src[3],
              "a merged window's tabs keep their order: the pinned one joins the pinned tabs, the rest land at "
              "the drop spot, moved past the group run it would split");
        const int merged_group = dt.items.size() == 7 ? dt.items[4]->tab_group : 0;
        bool group_copied = false;
        for (const auto& group : dt.tab_groups)
            group_copied |= group.id == merged_group && group.name == L"work" && group.color_rgb == 0x3366CC;
        check(merged_group != 0 && merged_group != 1 && group_copied && dt.items.size() == 7 &&
                  dt.items[5]->tab_group == merged_group && dt.items[6]->tab_group == 0 &&
                  dt.items[2]->tab_group == 1 && dt.tab_groups.size() == 2 && GroupRunsContiguous(dt) &&
                  dt.Active() == old_src[2],
              "its group comes along under a new id with its name and color, and its active tab is active");

        auto back = std::make_unique<AppState>(*core);
        back->hwnd = make_window();
        if (check(back->hwnd != nullptr, "a third hidden window takes the merged tabs back")) {
            core->windows.Add(back->hwnd, back.get());
            const size_t restored = RestoreMergedTabs(*dst, *back, undo);
            const app::WindowTabs& bt = back->window_tabs;
            bool same_order = bt.items.size() == old_src.size();
            for (size_t i = 0; same_order && i < old_src.size(); ++i) same_order = bt.items[i].get() == old_src[i];
            check(restored == 4 && same_order && bt.items[0]->pinned && bt.items[1]->tab_group == 5 &&
                      bt.items[2]->tab_group == 5 && bt.items[3]->tab_group == 0 && HasGroup(bt, 5) &&
                      bt.tab_groups.size() == 1 && bt.tab_groups[0].name == L"work" && bt.Active() == old_src[2],
                  "undo puts the same tabs back in their order, with their group and their active tab");
            bool dst_same = dt.items.size() == old_dst.size();
            for (size_t i = 0; dst_same && i < old_dst.size(); ++i) dst_same = dt.items[i].get() == old_dst[i];
            check(dst_same && dt.tab_groups.size() == 1 && HasGroup(dt, 1) && dt.items[1]->tab_group == 1 &&
                      dt.items[2]->tab_group == 1,
                  "the window merged into is as it was before");
            core->windows.Remove(back->hwnd);
            DestroyWindow(back->hwnd);
            back->hwnd = nullptr;
        }
        back.reset();

        // --- M6e: the shared folder watches serve every window ------------
        {
            const auto make_dir = [&](const wchar_t* name) {
                const auto dir = output / name;
                std::filesystem::remove_all(dir);
                std::filesystem::create_directories(dir);
                return fs::NormalizePath(dir.wstring());
            };
            const std::wstring watch_left = make_dir(L"watch-left");
            const std::wstring watch_right = make_dir(L"watch-right");
            { std::ofstream(std::filesystem::path(watch_right) / L"added.txt") << "x"; }
            Seed(src->window_tabs, 1);
            src->window_tabs.items[0]->ActiveFolder()->current_path = watch_left;
            BindCurrentLayout(*src);
            Seed(dst->window_tabs, 1);
            app::Tab* const right_view = dst->window_tabs.items[0]->ActiveFolder();
            right_view->current_path = watch_right;
            right_view->pending_generation = 0;
            right_view->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
            BindCurrentLayout(*dst);
            SyncVisibleWatches(*src);  // the window bound last no longer decides alone
            for (int i = 0; i < 100 && !(core->watches.Armed(watch_left) && core->watches.Armed(watch_right)); ++i) Sleep(20);
            check(core->watches.Armed(watch_left) && core->watches.Armed(watch_right),
                  "the shared folder watches cover the folders every window shows, not only one window's");
            fs::DirNotifyEvent added;
            added.action = FILE_ACTION_ADDED;
            added.name = L"added.txt";
            {
                std::lock_guard<std::mutex> lock(core->notify_mu);
                core->notify_queue.clear();
                QueueDirNotify(core->notify_queue, watch_right, false, std::vector<fs::DirNotifyEvent>{added});
            }
            DrainDirNotifies(*src);  // the change is drained by the window that does not show the folder
            bool listed = false;
            for (size_t i = 0; right_view->snapshot && i < right_view->EntryCount(); ++i)
                listed = listed || right_view->EntryAt(i).name == L"added.txt";
            check(listed, "a folder change drained by one window reaches the other window that lists the folder");
            core->watches.Sync({}, nullptr);
            std::lock_guard<std::mutex> lock(core->notify_mu);
            core->notify_queue.clear();
        }

        // --- M6e: tabs in every window follow a moved folder ---------------
        try {
            const auto base = output / L"follow";
            std::error_code fs_error;
            std::filesystem::remove_all(base, fs_error);
            std::filesystem::create_directories(base / L"old" / L"sub", fs_error);
            std::filesystem::create_directories(base / L"elsewhere", fs_error);
            const std::wstring old_root = fs::NormalizePath((base / L"old").wstring());
            const std::wstring new_root = fs::NormalizePath((base / L"new").wstring());
            const std::wstring elsewhere = fs::NormalizePath((base / L"elsewhere").wstring());
            Seed(src->window_tabs, 2);  // the tab added last is the one shown
            Tab* const background = src->window_tabs.items[0]->ActiveFolder();
            Tab* const shown = src->window_tabs.items[1]->ActiveFolder();
            background->current_path = old_root;
            shown->current_path = old_root + L"\\sub";
            shown->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(1));
            BindCurrentLayout(*src);
            Seed(dst->window_tabs, 1);
            Tab* const other = dst->window_tabs.items[0]->ActiveFolder();
            other->current_path = elsewhere;
            other->back_stack.push(old_root + L"\\sub");
            BindCurrentLayout(*dst);
            const std::wstring shown_before = old_root + L"\\sub";
            for (int i = 0; i < 100 && !core->watches.Armed(shown_before); ++i) Sleep(20);
            check(core->watches.Armed(shown_before), "the folder shown inside is watched before the move");
            std::filesystem::rename(base / L"old", base / L"new", fs_error);  // the shown folder is old\sub
            check(!fs_error, "a folder moves while a window shows a folder inside it (the watch steps aside)");
            FollowMovedFolder(*dst, old_root, new_root);  // the move finished in the other window
            check(shown->current_path == new_root + L"\\sub" && background->current_path == new_root,
                  "after a folder move, the shown and the background tab of another window follow it");
            check(shown->snapshot && shown->EntryCount() == 1,
                  "a tab that follows keeps its rows on screen while it re-lists");
            check(other->current_path == elsewhere && other->back_stack.top() == new_root + L"\\sub",
                  "a tab elsewhere stays, and its history follows the folder");
            const std::wstring shown_now = new_root + L"\\sub";
            for (int i = 0; i < 100 && !core->watches.Armed(shown_now); ++i) Sleep(20);
            check(core->watches.Armed(shown_now), "the shown folder is watched at its new place");
            core->watches.Sync({}, nullptr);
        } catch (const std::exception& error) {
            log << "follow: exception " << error.what() << std::endl;
            check(false, "following a moved folder runs without an exception");
        }

        // --- M6e: 复制到窗口 / 移动到窗口 in the item menu ------------------
        {
            auto third = std::make_unique<AppState>(*core);
            third->hwnd = make_window();
            if (check(third->hwnd != nullptr, "a third hidden window offers its folder as well")) {
                core->windows.Add(third->hwnd, third.get());
                Seed(third->window_tabs, 1);
                BindCurrentLayout(*third);
                BindCurrentLayout(*dst);
                BindCurrentLayout(*src);
                Tab* const here = ActiveTab(*src);
                Tab* const there = ActiveTab(*dst);
                Tab* const third_tab = ActiveTab(*third);
                const std::wstring folder_a = L"C:\\PulseTest\\a", folder_b = L"C:\\PulseTest\\b", folder_c = L"C:\\PulseTest\\c";
                here->current_path = folder_a;
                there->current_path = folder_b;
                third_tab->current_path = folder_c;
                core->windows.Touch(dst->hwnd);
                core->windows.Touch(third->hwnd);
                check(WindowTransferFolders(*src) == std::vector<std::wstring>{folder_c, folder_b},
                      "the other windows' folders are offered, the window used last first, the own window not");
                core->windows.Touch(dst->hwnd);
                check(WindowTransferFolders(*src) == std::vector<std::wstring>{folder_b, folder_c},
                      "the order follows which window was used last");
                there->current_path = L"c:\\pulsetest\\C";
                check(WindowTransferFolders(*src) == std::vector<std::wstring>{L"c:\\pulsetest\\C"},
                      "two windows that show one folder offer it once");
                there->current_path = L"C:\\PULSETEST\\A";
                check(WindowTransferFolders(*src) == std::vector<std::wstring>{folder_c},
                      "a window that shows the folder the selection is in is not offered");
                there->current_path = L"pulse:recent";
                third_tab->net_readonly = true;
                check(WindowTransferFolders(*src).empty(), "a virtual or a read-only folder is not offered");
                third_tab->net_readonly = false;
                there->current_path = folder_b;
                auto split_off = MakeSingleLayoutTab(L"");
                Tab* const extra_tab = split_off->panes[0]->ActiveTab();
                extra_tab->current_path = L"C:\\PulseTest\\d";
                if (check(AddPaneFromTab(*third->window_tabs.Active(), std::move(split_off), ContentDrop::Right),
                          "the third window is split, its new right pane focused")) {
                    BindCurrentLayout(*third);
                    core->windows.Touch(third->hwnd);
                    check(WindowTransferFolders(*src) == std::vector<std::wstring>{L"C:\\PulseTest\\d", folder_c, folder_b},
                          "every pane of a split window is offered, its focused pane first");
                }

                auto menu = BuildItemMenu(false, L"");
                const size_t plain = menu.size();
                AppendWindowTransferCommands(menu, {}, true);
                check(menu.size() == plain, "without another window the item menu has no window flyouts");
                AppendWindowTransferCommands(menu, {folder_c, L"\\\\?\\D:\\"}, false);
                // Anchored above 复制名称 so 名称/路径 stay neighbours (#303).
                const auto copy_name = std::find_if(menu.begin(), menu.end(), [](const ui::FluentMenuItem& row) {
                    return row.command == CmdCopyName;
                });
                const auto copy_path = std::find_if(menu.begin(), menu.end(), [](const ui::FluentMenuItem& row) {
                    return row.command == CmdCopyPath;
                });
                const size_t at = static_cast<size_t>(copy_name - menu.begin());
                if (check(menu.size() == plain + 2 && copy_name != menu.end() && copy_path == copy_name + 1 &&
                              at >= 2,
                          "the two flyouts come right before 复制名称, which stays above 复制路径")) {
                    const ui::FluentMenuItem& copy = menu[at - 2];
                    const ui::FluentMenuItem& move = menu[at - 1];
                    check(copy.command == CmdNone && copy.enabled && copy.text == l10n::Get(l10n::StringId::CopyToWindow) &&
                              copy.children.size() == 2 && copy.children[0].command == CmdCopyToWindowBase &&
                              copy.children[1].command == CmdCopyToWindowBase + 1,
                          "复制到窗口 lists one row per folder");
                    check(copy.children[0].text == L"c" && copy.children[0].shortcut == folder_c &&
                              copy.children[1].text == L"D:\\" && copy.children[1].shortcut == L"D:\\" &&
                              copy.children[0].tooltip == folder_c,
                          "a row shows the folder name and its full path (a drive by its root)");
                    check(move.text == l10n::Get(l10n::StringId::MoveToWindow) && !move.enabled &&
                              move.children.size() == 2 && move.children[1].command == CmdMoveToWindowBase + 1,
                          "移动到窗口 is there but disabled when the selection cannot move");
                }
                core->windows.Remove(third->hwnd);
                DestroyWindow(third->hwnd);
                third->hwnd = nullptr;
            }
            third.reset();
        }

        // --- M6f: a tab moved to a window at another DPI keeps its rows ------
        {
            const float src_scale = src->scale;
            const float dst_scale = dst->scale;
            src->scale = 1.0f;
            dst->scale = 1.5f;
            Seed(src->window_tabs, 2);  // replaces its tabs; the second one, shown, moves
            BindCurrentLayout(*src);
            const size_t moving = 1;
            LayoutTab* const carried = src->window_tabs.items[moving].get();
            Tab* const carried_view = carried->panes[0]->ActiveTab();
            carried_view->scroll_y = 300.0f;
            carried_view->scroll_x = 20.0f;
            check(MoveLayoutTab(*src, moving, *dst, 0) && carried_view->scroll_y == 450.0f &&
                      carried_view->scroll_x == 30.0f,
                  "a tab moved from a window at 100% to one at 150% scrolls 1.5 times as far (the same rows in view)");
            size_t home = dst->window_tabs.items.size();
            for (size_t i = 0; i < dst->window_tabs.items.size(); ++i)
                if (dst->window_tabs.items[i].get() == carried) home = i;
            check(home < dst->window_tabs.items.size() &&
                      MoveLayoutTab(*dst, home, *src, src->window_tabs.items.size()) &&
                      std::abs(carried_view->scroll_y - 300.0f) < 0.01f && std::abs(carried_view->scroll_x - 20.0f) < 0.01f,
                  "moved back, its scroll position is what it was");
            RescaleLayoutTabScroll(*carried, 0.0f);
            RescaleLayoutTabScroll(*carried, 1.0f);
            check(std::abs(carried_view->scroll_y - 300.0f) < 0.01f, "a ratio of 0 or 1 leaves the scroll position alone");
            RescaleLayoutTabScroll(*carried, 2.0f);
            check(std::abs(carried_view->scroll_y - 600.0f) < 0.01f && std::abs(carried_view->scroll_x - 40.0f) < 0.01f,
                  "a window that goes from 100% to 200% doubles its tabs' scroll positions");
            src->scale = src_scale;
            dst->scale = dst_scale;
        }

        // --- M6f: results still on their way find a moved tab -------------
        {
            Seed(src->window_tabs, 2);
            BindCurrentLayout(*src);
            BindCurrentLayout(*dst);
            Tab* const searching = src->window_tabs.items[1]->panes[0]->ActiveTab();  // shown
            searching->filename_live_generation = 0x6F01;  // a live network walk
            searching->pending_generation = 0x6F02;        // an index search page
            check(&SearchResultWindow(*dst, 0x6F01) == src.get(),
                  "before a move, search results go to the window that shows the searching tab");
            check(MoveLayoutTab(*src, 1, *dst, 0) && &SearchResultWindow(*src, 0x6F01) == dst.get() &&
                      &SearchResultWindow(*src, 0x6F02) == dst.get(),
                  "after the move, live network progress and index results go to the tab's new window");
            searching->filename_live_generation = 0;
            searching->pending_generation = 0;
        }

        core->windows.Remove(dst->hwnd);
        core->windows.Remove(src->hwnd);
    }
    for (HWND* hwnd : {&src->hwnd, &dst->hwnd}) {
        if (*hwnd) DestroyWindow(*hwnd);
        *hwnd = nullptr;
    }
    dst.reset();
    src.reset();
    core.reset();
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}
}  // namespace pulse::app
#endif
