#pragma once

#include "app_model.h"
#include <optional>

namespace pulse::app {

inline bool IsTabShortcut(UINT key, bool ctrl, bool shift, bool alt) {
    (void)shift;
    return ctrl && !alt && key == VK_TAB;
}

inline std::optional<size_t> TabShortcutTarget(const WindowTabs& tabs, bool shift) {
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

} // namespace pulse::app
