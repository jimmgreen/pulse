#include "app_model.h"

#include <algorithm>
#include <unordered_set>

namespace pulse::app {

int MoveTabRun(std::vector<int>& order, int pos, int len, int dir) {
    const int n = static_cast<int>(order.size());
    if (pos < 0 || len < 1 || pos + len > n) return pos;
    if (dir < 0 && pos > 0) {
        std::rotate(order.begin() + pos - 1, order.begin() + pos,
                    order.begin() + pos + len);
        return pos - 1;
    }
    if (dir > 0 && pos + len < n) {
        std::rotate(order.begin() + pos, order.begin() + pos + len,
                    order.begin() + pos + len + 1);
        return pos + 1;
    }
    return pos;
}

float CollapsedChipBlockW(float chip_w, float chip_gap) {
    return chip_w + chip_gap;
}

bool ChipBlockCrossed(float block_left, float block_w, float neighbor_center, int dir) {
    if (dir < 0) return block_left < neighbor_center;
    if (dir > 0) return block_left + block_w > neighbor_center;
    return false;
}

float DisplacedRestDelta(float old_rest, float cur_off, float new_rest) {
    return old_rest + cur_off - new_rest;
}

GroupRun FindGroupRun(const std::vector<int>& order,
                      const std::vector<int>& tab_group_of,
                      int at, int gid) {
    const int n = static_cast<int>(order.size());
    if (gid == 0 || at < 0 || at >= n) return {};
    const auto group_at = [&](int position) {
        const int tab_index = order[static_cast<size_t>(position)];
        return tab_index >= 0 && tab_index < static_cast<int>(tab_group_of.size())
            ? tab_group_of[static_cast<size_t>(tab_index)] : 0;
    };
    if (group_at(at) != gid) return {};
    int pos = at;
    while (pos > 0 && group_at(pos - 1) == gid) --pos;
    int end = at;
    while (end + 1 < n && group_at(end + 1) == gid) ++end;
    return GroupRun{ pos, end - pos + 1 };
}

bool MoveTabGroupAcrossFreeTab(std::vector<int>& order,
                               const std::vector<int>& tab_group_of,
                               int member_pos, int dir) {
    if (dir == 0 || member_pos < 0 || member_pos >= static_cast<int>(order.size()))
        return false;
    const int member = order[static_cast<size_t>(member_pos)];
    if (member < 0 || member >= static_cast<int>(tab_group_of.size())) return false;
    const int group = tab_group_of[static_cast<size_t>(member)];
    const GroupRun run = FindGroupRun(order, tab_group_of, member_pos, group);
    if (run.len == 0) return false;
    const int neighbor = dir < 0 ? run.pos - 1 : run.pos + run.len;
    if (neighbor < 0 || neighbor >= static_cast<int>(order.size())) return false;
    const int neighbor_tab = order[static_cast<size_t>(neighbor)];
    if (neighbor_tab < 0 || neighbor_tab >= static_cast<int>(tab_group_of.size()) ||
        tab_group_of[static_cast<size_t>(neighbor_tab)] != 0) return false;
    MoveTabRun(order, run.pos, run.len, dir);
    return true;
}

void NormalizeGroupRuns(WindowTabs& tabs) {
    std::vector<int> groups;
    std::unordered_set<int> seen;
    for (const auto& tab : tabs.items) {
        const int group = tab->tab_group;
        if (group != 0 && seen.insert(group).second) groups.push_back(group);
    }
    if (groups.empty()) return;
    const LayoutTab* active = tabs.Active();
    for (const int group : groups) {
        const auto first = std::find_if(tabs.items.begin(), tabs.items.end(),
            [group](const auto& tab) { return tab->tab_group == group; });
        if (first != tabs.items.end()) {
            std::stable_partition(first, tabs.items.end(),
                [group](const auto& tab) { return tab->tab_group == group; });
        }
    }
    if (!active) return;
    for (size_t i = 0; i < tabs.items.size(); ++i) {
        if (tabs.items[i].get() == active) {
            tabs.active = i;
            break;
        }
    }
}

} // namespace pulse::app
