#pragma once
#include "desktop_model.h"
#include <algorithm>

namespace pulse::desktop::model {

struct DragDamageSnapshot {
    std::vector<RectI> frames;
    RectI preview;
    std::string stack_target;
};

inline bool UpdateMovedGroupViewOrigins(const std::vector<DesktopGroup>& groups,
                                       std::vector<GroupComputedView>& views) {
    if (groups.size() != views.size()) return false;
    for (size_t i = 0; i < groups.size(); ++i)
        if (groups[i].id != views[i].group_id) return false;
    for (size_t i = 0; i < groups.size(); ++i) {
        if (views[i].frame.empty()) continue;
        const int dx=groups[i].x-views[i].frame.x;
        const int dy=groups[i].y-views[i].frame.y;
        views[i].frame.x = groups[i].x;
        views[i].frame.y = groups[i].y;
        views[i].content_rect.x+=dx;
        views[i].content_rect.y+=dy;
    }
    return true;
}

inline RectI ComputeDragDamage(const DragDamageSnapshot& before, const DragDamageSnapshot& after) {
    RectI damage;
    auto include = [&](const RectI& rect) {
        if (rect.empty()) return;
        if (damage.empty()) { damage = rect; return; }
        const int right = std::max(damage.right(), rect.right());
        const int bottom = std::max(damage.bottom(), rect.bottom());
        damage.x = std::min(damage.x, rect.x);
        damage.y = std::min(damage.y, rect.y);
        damage.w = right - damage.x;
        damage.h = bottom - damage.y;
    };
    const size_t count = std::max(before.frames.size(), after.frames.size());
    const bool target_changed = before.stack_target != after.stack_target;
    for (size_t i = 0; i < count; ++i) {
        const RectI old = i < before.frames.size() ? before.frames[i] : RectI{};
        const RectI next = i < after.frames.size() ? after.frames[i] : RectI{};
        if (old != next) { include(old); include(next); }
    }
    if (before.preview != after.preview || target_changed) {
        include(before.preview);
        include(after.preview);
    }
    // Resize pills, edit badges and shadows extend beyond the card itself.
    // One union avoids InvalidateRect's sixteen-region full-redraw fallback.
    if (!damage.empty()) damage = {damage.x - 32, damage.y - 32, damage.w + 64, damage.h + 64};
    return damage;
}

} // namespace pulse::desktop::model
