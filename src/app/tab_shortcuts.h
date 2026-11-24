#pragma once

#include "app_model.h"
#include <algorithm>
#include <optional>
#include <vector>

namespace pulse::app {

inline bool IsTabShortcut(UINT key, bool ctrl, bool shift, bool alt) {
    (void)shift;
    return ctrl && !alt && key == VK_TAB;
}

// Tab indices in strip order, without tabs inside collapsed groups.
inline std::vector<size_t> VisibleTabIndices(const WindowTabs& tabs) {
    std::vector<size_t> visible;
    for (size_t i = 0; i < tabs.items.size(); ++i) {
        const auto& tab = tabs.items[i];
        if (!tab) continue;
        bool hidden = false;
        for (const auto& group : tabs.tab_groups) {
            if (tab->tab_group != 0 && group.id == tab->tab_group && group.collapsed) {
                hidden = true;
                break;
            }
        }
        if (!hidden) visible.push_back(i);
    }
    return visible;
}

inline std::optional<size_t> TabShortcutTarget(const WindowTabs& tabs, bool shift) {
    const std::vector<size_t> visible = VisibleTabIndices(tabs);
    if (visible.empty()) return std::nullopt;
    if (shift) {
        for (auto it = visible.rbegin(); it != visible.rend(); ++it)
            if (*it < tabs.active) return *it;
        return visible.back();
    }
    for (size_t index : visible)
        if (index > tabs.active) return index;
    return visible.front();
}

// Accumulates WM_MOUSEWHEEL deltas (including sub-notch touchpad ticks) into
// discrete signed wheel steps; pauses >400 ms or reversals reset the remainder.
inline int ConsumeTabWheelSteps(int delta, ULONGLONG now, int& remainder, ULONGLONG& tick) {
    if (delta == 0) return 0;
    if (now - tick > 400 || (remainder > 0) != (delta > 0)) remainder = 0;
    tick = now;
    remainder += delta;
    const int steps = remainder / WHEEL_DELTA;
    remainder %= WHEEL_DELTA;
    return steps;
}

// Target tab index when stepping `offset` visible slots from `tabs.active`
// without wrapping around at either end.
inline std::optional<size_t> TabWheelTarget(const WindowTabs& tabs, int offset) {
    if (offset == 0) return std::nullopt;
    const std::vector<size_t> visible = VisibleTabIndices(tabs);
    if (visible.empty()) return std::nullopt;
    size_t cur_pos = 0;
    const auto it = std::find(visible.begin(), visible.end(), tabs.active);
    if (it != visible.end()) {
        cur_pos = static_cast<size_t>(it - visible.begin());
    } else {
        const auto lb = std::lower_bound(visible.begin(), visible.end(), tabs.active);
        cur_pos = (lb == visible.end()) ? (visible.size() - 1)
                                        : static_cast<size_t>(lb - visible.begin());
    }
    const ptrdiff_t max_pos = static_cast<ptrdiff_t>(visible.size() - 1);
    const ptrdiff_t next_pos = std::clamp(
        static_cast<ptrdiff_t>(cur_pos) + static_cast<ptrdiff_t>(offset),
        ptrdiff_t{0}, max_pos);
    const size_t target = visible[static_cast<size_t>(next_pos)];
    if (target == tabs.active) return std::nullopt;
    return target;
}

} // namespace pulse::app
