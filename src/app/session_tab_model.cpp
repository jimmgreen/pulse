#include "session.h"
#include "split_tree_shape.h"

#include <algorithm>
#include <memory>
#include <unordered_set>

namespace pulse::app {

namespace {

size_t SnapshotPresetCount(LayoutPreset preset) {
    switch (preset) {
    case LayoutPreset::TwoVertical:
    case LayoutPreset::TwoHorizontal: return 2;
    case LayoutPreset::Three: return 3;
    case LayoutPreset::FourGrid: return 4;
    case LayoutPreset::Single:
    default: return 1;
    }
}

void CollectTreePanes(const SplitContainer& node, std::vector<Pane*>& out) {
    if (node.is_leaf) {
        if (node.pane) out.push_back(node.pane);
        return;
    }
    if (node.first) CollectTreePanes(*node.first, out);
    if (node.second) CollectTreePanes(*node.second, out);
}

void CollectTreeRatios(const SplitContainer& node, std::vector<float>& out) {
    if (node.is_leaf) return;
    out.push_back(node.ratio);
    if (node.first) CollectTreeRatios(*node.first, out);
    if (node.second) CollectTreeRatios(*node.second, out);
}

} // namespace

LayoutTabSnapshot CaptureLayoutTab(const LayoutTab& tab) {
    LayoutTabSnapshot snapshot;
    snapshot.title = tab.title;
    snapshot.marker_rgb = tab.marker_rgb;
    snapshot.pinned = tab.pinned;
    snapshot.group = tab.tab_group;
    snapshot.layout = static_cast<int>(tab.layout);
    std::vector<Pane*> vis;
    if (tab.root) {
        CollectTreeRatios(*tab.root, snapshot.split_ratios);
        CollectTreePanes(*tab.root, vis);
        if (tab.layout == LayoutPreset::Custom) {
            snapshot.split_tree = SplitTreeShape(*tab.root);
            snapshot.layout = static_cast<int>(PresetForPaneCount(vis.size()));
        }
    } else {
        vis.reserve(tab.panes.size());
        for (const auto& pane : tab.panes) vis.push_back(pane.get());
    }
    // Saved panes are in tree order, while live indices refer to all owned panes.
    const auto visible_index = [&](int index) {
        if (index < 0 || static_cast<size_t>(index) >= tab.panes.size()) return -1;
        const auto found = std::find(vis.begin(), vis.end(), tab.panes[index].get());
        return found == vis.end() ? -1 : static_cast<int>(found - vis.begin());
    };
    snapshot.focused = std::max(0, visible_index(tab.focused_index));
    snapshot.target = visible_index(tab.target_index);
    snapshot.panes.reserve(vis.size());
    for (const Pane* pane : vis) {
        const Tab* view = pane ? pane->ActiveTab() : nullptr;
        if (!view) {
            snapshot.panes.emplace_back();
            continue;
        }
        snapshot.panes.push_back({view->current_path, view->view_mode,
            view->details_column_dividers, view->search_column_dividers,
            view->column_layout, view->column_widths_dip,
            view->column_strip.widths_by_path});
    }
    return snapshot;
}

void RestoreLayoutTab(LayoutTab& tab, const LayoutTabSnapshot& snapshot,
                      const SessionTabLoader& load_tab) {
    tab.title = snapshot.title;
    tab.marker_rgb = snapshot.marker_rgb;
    tab.pinned = snapshot.pinned;
    tab.tab_group = snapshot.pinned ? 0 : snapshot.group;
    tab.layout = static_cast<LayoutPreset>(std::clamp(snapshot.layout, 0, 4));
    tab.focused_index = snapshot.focused;
    tab.target_index = snapshot.target;
    tab.panes.clear();
    tab.root.reset();
    for (const auto& saved : snapshot.panes) {
        // An empty saved path is the valid This PC location, not a missing pane.
        auto pane = std::make_unique<Pane>();
        pane->view.view_mode = saved.view;
        pane->view.details_column_dividers = saved.columns;
        pane->view.search_column_dividers = saved.search_columns;
        pane->view.column_layout = saved.column_layout;
        pane->view.column_widths_dip = saved.column_widths;
        Tab* raw = pane->ActiveTab();
        tab.panes.push_back(std::move(pane));
        if (load_tab) load_tab(*raw, saved.path);
        // The loader can reset a Tab; restore its widths only afterwards.
        raw->column_strip.widths_by_path = saved.column_path_widths;
    }
    if (tab.panes.empty()) {
        auto pane = std::make_unique<Pane>();
        pane->focused = true;
        tab.panes.push_back(std::move(pane));
        if (load_tab) load_tab(*tab.panes.front()->ActiveTab(), L"C:\\");
    }
    // A custom tree (M7k) comes back in its shape, its panes in tree order;
    // a shape that does not fit them leaves the preset saved with it.
    if (!snapshot.split_tree.empty() && ShapePaneCount(snapshot.split_tree) == tab.panes.size()) {
        std::vector<Pane*> order;
        for (const auto& pane : tab.panes) order.push_back(pane.get());
        tab.root = MakeShapedTree(snapshot.split_tree, order);
        if (tab.root) tab.layout = PresetOfTree(*tab.root);
        if (tab.layout != LayoutPreset::Custom) tab.root.reset();  // a preset is rebuilt from its panes
    }
    const size_t need = tab.layout == LayoutPreset::Custom ? 0 : SnapshotPresetCount(tab.layout);
    while (tab.panes.size() < need) {
        auto pane = std::make_unique<Pane>();
        const Tab& source = *tab.panes.front()->ActiveTab();
        pane->view.view_mode = source.view_mode;
        pane->view.details_column_dividers = source.details_column_dividers;
        pane->view.search_column_dividers = source.search_column_dividers;
        pane->view.column_layout = source.column_layout;
        pane->view.column_widths_dip = source.column_widths_dip;
        pane->view.column_strip.widths_by_path = source.column_strip.widths_by_path;
        const std::wstring clone = source.current_path;
        pane->view.current_path = clone;
        Tab* raw = pane->ActiveTab();
        tab.panes.push_back(std::move(pane));
        if (load_tab) load_tab(*raw, clone);
    }
    tab.focused_index = std::clamp(tab.focused_index, 0,
                                   static_cast<int>(tab.panes.size()) - 1);
    if (tab.target_index >= static_cast<int>(tab.panes.size())) tab.target_index = -1;
    for (size_t i = 0; i < tab.panes.size(); ++i) {
        tab.panes[i]->focused = static_cast<int>(i) == tab.focused_index;
        tab.panes[i]->target = static_cast<int>(i) == tab.target_index;
    }
}

void RestoreWindowTabs(WindowTabs& tabs,
                       const std::vector<LayoutTabSnapshot>& layout_tabs,
                       const std::vector<GroupSessionSnapshot>& groups,
                       int active_index,
                       const SessionTabLoader& load_tab) {
    tabs.items.clear();
    tabs.tab_groups.clear();
    std::unordered_set<int> valid_groups;
    int max_group = 0;
    for (const auto& saved : groups) {
        if (saved.id <= 0 || !valid_groups.insert(saved.id).second) continue;
        tabs.tab_groups.push_back({saved.id, saved.name, saved.color_rgb, saved.collapsed});
        max_group = std::max(max_group, saved.id);
    }
    tabs.next_tab_group_id = std::max(1, max_group + 1);
    for (const auto& saved : layout_tabs) {
        // Loading can reuse requests from panes already registered in the window.
        tabs.items.push_back(std::make_unique<LayoutTab>());
        auto& tab = tabs.items.back();
        RestoreLayoutTab(*tab, saved, load_tab);
        if (tab->tab_group != 0 && !valid_groups.contains(tab->tab_group))
            tab->tab_group = 0;
    }
    if (tabs.items.empty()) {
        auto tab = std::make_unique<LayoutTab>();
        auto pane = std::make_unique<Pane>();
        pane->focused = true;
        tab->panes.push_back(std::move(pane));
        tabs.items.push_back(std::move(tab));
    }
    tabs.active = 0;
    if (active_index >= 0 && active_index < static_cast<int>(tabs.items.size()))
        tabs.active = static_cast<size_t>(active_index);
    NormalizeGroupRuns(tabs);
}

} // namespace pulse::app
