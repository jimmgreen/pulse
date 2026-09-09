#include "index_engine.h"
#include <algorithm>

namespace pulse::index {

bool Engine::ShouldSkipName(std::wstring_view name) {
    return name == L"." || name == L".." ||
           name == L"$Recycle.Bin" || name == L"System Volume Information" ||
           name == L"WinSxS" || name == L"servicing" ||
           (name.size() == 12 && _wcsnicmp(name.data(), L"node_modules", 12) == 0);
}

void Engine::RefreshSubtreeVisibilityLocked(int32_t root, DeltaLog* delta) {
    // Base children are already ordered by parent. Index only the small mutable
    // overlay here, rather than scanning the whole volume for every descendant.
    std::vector<std::pair<int32_t, int32_t>> overlay;
    for (int32_t i = BaseCount(); i < LiveCount(); ++i) {
        if (!IsTomb(i)) overlay.emplace_back(NodeAt(i).parent, i);
    }
    for (const auto& [id, patch] : patches_) {
        if (id < BaseCount() && patch.has_meta && !IsTomb(id))
            overlay.emplace_back(NodeAt(id).parent, id);
    }
    std::sort(overlay.begin(), overlay.end());
    std::vector<int32_t> pending{root};
    std::unordered_set<int32_t> visited{root};
    for (size_t next = 0; next < pending.size(); ++next) {
        const int32_t parent = pending[next];
        const Node parent_node = NodeAt(parent);
        auto visit = [&](int32_t id) {
            if (IsTomb(id) || !visited.insert(id).second) return;
            const Node node = NodeAt(id);
            if (node.parent != parent) return;
            uint8_t flags = node.flags & static_cast<uint8_t>(~kFlagHidden);
            if ((parent_node.parent >= 0 && (parent_node.flags & kFlagHidden)) ||
                ShouldSkipName(NameOf(id)) || IsExcludedPath(BuildPathLocked(id)))
                flags |= kFlagHidden;
            if (flags != node.flags) {
                if (id >= BaseCount()) {
                    live_.nodes[static_cast<size_t>(id - BaseCount())].flags = flags;
                } else {
                    Patch& patch = patches_[id];
                    patch.parent = node.parent;
                    patch.flags = flags;
                    patch.has_meta = true;
                }
                if (delta) delta->QueuePatch(id, static_cast<uint8_t>(PatchBits::Meta),
                                             node.parent, flags, 0, 0, {});
            }
            if (node.flags & kFlagDir) pending.push_back(id);
        };
        if (map_) {
            const int32_t* end = map_->child_order + map_->n;
            auto it = std::lower_bound(map_->child_order, end, parent,
                [&](int32_t id, int32_t value) { return map_->nodes[id].parent < value; });
            for (; it != end && map_->nodes[*it].parent == parent; ++it) {
                if (NodeAt(*it).parent == parent) visit(*it);
            }
        }
        auto it = std::lower_bound(overlay.begin(), overlay.end(), std::make_pair(parent, -1));
        for (; it != overlay.end() && it->first == parent; ++it) visit(it->second);
    }
}

} // namespace pulse::index
