#include "session.h"

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
    snapshot.pinned = tab.pinned;
    snapshot.group = tab.tab_group;
    snapshot.layout = static_cast<int>(tab.layout);
    snapshot.focused = tab.focused_index;
    snapshot.target = tab.target_index;
    std::vector<Pane*> vis;
    if (tab.root) {
        CollectTreeRatios(*tab.root, snapshot.split_ratios);
        CollectTreePanes(*tab.root, vis);
    } else {
        vis.reserve(tab.panes.size());
        for (const auto& pane : tab.panes) vis.push_back(pane.get());
    }
    snapshot.panes.reserve(vis.size());
    for (const Pane* pane : vis) {
        const Tab* view = pane ? pane->ActiveTab() : nullptr;
        if (!view) {
            snapshot.panes.emplace_back();
            continue;
        }
        snapshot.panes.push_back({view->current_path, view->view_mode,
            view->details_column_dividers, view->search_column_dividers});
    }
    return snapshot;
}

void RestoreLayoutTab(LayoutTab& tab, const LayoutTabSnapshot& snapshot,
                      const SessionTabLoader& load_tab) {
    tab.title = snapshot.title;
    tab.pinned = snapshot.pinned;
    tab.tab_group = snapshot.pinned ? 0 : snapshot.group;
    tab.layout = static_cast<LayoutPreset>(std::clamp(snapshot.layout, 0, 4));
    tab.focused_index = snapshot.focused;
    tab.target_index = snapshot.target;
    tab.panes.clear();
    tab.root.reset();
    for (const auto& saved : snapshot.panes) {
        if (saved.path.empty()) continue;
        auto pane = std::make_unique<Pane>();
        pane->view.view_mode = saved.view;
        pane->view.details_column_dividers = saved.columns;
        pane->view.search_column_dividers = saved.search_columns;
        Tab* raw = pane->ActiveTab();
        tab.panes.push_back(std::move(pane));
        if (load_tab) load_tab(*raw, saved.path);
    }
    if (tab.panes.empty()) {
        auto pane = std::make_unique<Pane>();
        pane->focused = true;
        tab.panes.push_back(std::move(pane));
        if (load_tab) load_tab(*tab.panes.front()->ActiveTab(), L"C:\\");
    }
    const size_t need = SnapshotPresetCount(tab.layout);
    while (tab.panes.size() < need) {
        auto pane = std::make_unique<Pane>();
        const Tab& source = *tab.panes.front()->ActiveTab();
        pane->view.view_mode = source.view_mode;
        pane->view.details_column_dividers = source.details_column_dividers;
        pane->view.search_column_dividers = source.search_column_dividers;
        const std::wstring clone = source.current_path.empty() ? L"C:\\" : source.current_path;
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
        auto tab = std::make_unique<LayoutTab>();
        RestoreLayoutTab(*tab, saved, load_tab);
        if (tab->tab_group != 0 && !valid_groups.contains(tab->tab_group))
            tab->tab_group = 0;
        tabs.items.push_back(std::move(tab));
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
