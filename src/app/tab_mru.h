#pragma once
// tab_mru.h - Ctrl+Tab "最近使用的标签" (ui modes step 7).
// The window keeps its tabs in the order they were last active. Ctrl+Tab
// opens a panel of the most recent ones (at most kTabMruRows), on the one
// before the current tab; Tab / Shift+Tab move, releasing Ctrl opens the
// selected one and Esc cancels. A quick Ctrl+Tab therefore flips between the
// last two tabs. The panel works on a snapshot taken when it opened.
// Identity is by pointer; pointers are only compared, never dereferenced,
// until they are matched against the live tab list again.
#include "app_model.h"
#include "tab_shortcuts.h"

#include <algorithm>
#include <vector>

namespace pulse::app {

inline constexpr size_t kTabMruRows = 9;

struct TabMruState {
    std::vector<const LayoutTab*> order;  // most recent first
    std::vector<const LayoutTab*> shown;  // the panel's rows while open
    int selected = -1;                    // index into shown; -1 = closed
    bool IsOpen() const noexcept { return selected >= 0 && !shown.empty(); }
};

inline bool TabMruLive(const WindowTabs& tabs, const LayoutTab* tab) {
    for (const auto& item : tabs.items)
        if (item.get() == tab) return true;
    return false;
}

// Brings the order up to date with the tab list: closed tabs go, new ones
// join at the end in strip order, the active tab moves to the front.
inline void ObserveTabMru(TabMruState& m, const WindowTabs& tabs) {
    m.order.erase(std::remove_if(m.order.begin(), m.order.end(),
                                 [&](const LayoutTab* t) { return !TabMruLive(tabs, t); }),
                  m.order.end());
    for (const auto& item : tabs.items)
        if (item && std::find(m.order.begin(), m.order.end(), item.get()) == m.order.end())
            m.order.push_back(item.get());
    if (const LayoutTab* active = tabs.Active()) {
        const auto it = std::find(m.order.begin(), m.order.end(), active);
        if (it != m.order.end() && it != m.order.begin()) std::rotate(m.order.begin(), it, it + 1);
    }
}

// Tabs the panel can offer: not inside a collapsed group (as Ctrl+1..9).
inline std::vector<const LayoutTab*> TabMruCandidates(const TabMruState& m, const WindowTabs& tabs) {
    const std::vector<size_t> visible = VisibleTabIndices(tabs);
    std::vector<const LayoutTab*> out;
    for (const LayoutTab* t : m.order) {
        if (out.size() >= kTabMruRows) break;
        const bool ok = std::any_of(visible.begin(), visible.end(),
                                    [&](size_t i) { return tabs.items[i].get() == t; });
        if (ok || t == tabs.Active()) out.push_back(t);
    }
    return out;
}

// Opens on the previous tab (or the oldest row with `back`). False when
// there is nothing to switch to.
inline bool OpenTabMru(TabMruState& m, const WindowTabs& tabs, bool back) {
    ObserveTabMru(m, tabs);
    m.shown = TabMruCandidates(m, tabs);
    if (m.shown.size() < 2) {
        m.shown.clear();
        m.selected = -1;
        return false;
    }
    m.selected = back ? static_cast<int>(m.shown.size()) - 1 : 1;
    return true;
}

inline void StepTabMru(TabMruState& m, int dir) {
    if (!m.IsOpen()) return;
    const int n = static_cast<int>(m.shown.size());
    m.selected = ((m.selected + dir) % n + n) % n;
}

inline void SelectTabMru(TabMruState& m, int row) {
    if (m.IsOpen() && row >= 0 && row < static_cast<int>(m.shown.size())) m.selected = row;
}

inline void CancelTabMru(TabMruState& m) {
    m.shown.clear();
    m.selected = -1;
}

// Drops rows whose tab closed while the panel was open.
inline void PruneTabMru(TabMruState& m, const WindowTabs& tabs) {
    if (!m.IsOpen()) return;
    const LayoutTab* sel = m.shown[static_cast<size_t>(m.selected)];
    m.shown.erase(std::remove_if(m.shown.begin(), m.shown.end(),
                                 [&](const LayoutTab* t) { return !TabMruLive(tabs, t); }),
                  m.shown.end());
    if (m.shown.size() < 2) { CancelTabMru(m); return; }
    const auto it = std::find(m.shown.begin(), m.shown.end(), sel);
    m.selected = it != m.shown.end() ? static_cast<int>(it - m.shown.begin())
                                     : std::min(m.selected, static_cast<int>(m.shown.size()) - 1);
}

// Index in tabs.items of the selected row's tab (closes the panel), or -1.
inline int CommitTabMru(TabMruState& m, const WindowTabs& tabs) {
    PruneTabMru(m, tabs);
    const LayoutTab* target = m.IsOpen() ? m.shown[static_cast<size_t>(m.selected)] : nullptr;
    CancelTabMru(m);
    for (size_t i = 0; i < tabs.items.size(); ++i)
        if (target && tabs.items[i].get() == target) return static_cast<int>(i);
    return -1;
}

}  // namespace pulse::app
