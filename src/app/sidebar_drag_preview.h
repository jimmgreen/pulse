#pragma once
#include "../ui/ui_renderer.h"

namespace pulse::app {

// Read the already-built sidebar: no Shell, filesystem or icon-provider work
// on the mouse-move / paint path. Call only for an active (not pending) drag.
inline ui::DragGhostView BuildSidebarDragPreview(const std::vector<ui::SidebarGroup>& groups,
                                                const std::wstring& path, int section,
                                                POINT point) {
    ui::DragGhostView preview;
    for (const auto& group : groups) {
        for (const auto& item : group.items) {
            if (path.empty() || item.path != path) continue;
            preview.on = true;
            preview.item_preview = true;
            preview.title = item.label;
            preview.icon_glyph = item.icon_glyph;
            preview.icon_fallback = item.fallback_text;
            preview.icon_color = item.is_tag ? item.tag_dot : item.icon_color;
            preview.x = static_cast<float>(point.x);
            preview.y = static_cast<float>(point.y);
            return preview;
        }
    }
    for (const auto& group : groups) {
        if (group.id != section || group.header.empty()) continue;
        preview.on = true;
        preview.item_preview = true;
        preview.title = group.header;
        preview.icon_glyph = group.icon_glyph;
        preview.x = static_cast<float>(point.x);
        preview.y = static_cast<float>(point.y);
        return preview;
    }
    return preview;
}

} // namespace pulse::app
