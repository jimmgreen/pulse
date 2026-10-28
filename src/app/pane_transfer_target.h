#pragma once
#include <algorithm>
#include <vector>

namespace pulse::app {
struct Pane;
inline Pane* ResolveTransferTarget(const std::vector<Pane*>& visible, Pane* source, Pane* marked) {
    if (!source || std::find(visible.begin(), visible.end(), source) == visible.end()) return nullptr;
    if (marked && marked != source && std::find(visible.begin(), visible.end(), marked) != visible.end())
        return marked;
    if (visible.size() == 2) return visible[0] == source ? visible[1] : visible[0];
    return nullptr;
}
}
