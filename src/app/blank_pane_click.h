#pragma once

#include <cstdint>

namespace pulse::app {

struct BlankPaneClickRelease {
    bool pending = false;
    bool marquee_active = false;
    bool owns_capture = false;
    bool same_context = false;
    bool modified = false;
    bool blank_list_hit = false;
    int delta_x = 0;
    int delta_y = 0;
    int drag_width = 0;
    int drag_height = 0;
};

inline bool IsBlankPaneBackClick(const BlankPaneClickRelease& click) {
    const int64_t dx = click.delta_x;
    const int64_t dy = click.delta_y;
    return click.pending && !click.marquee_active && click.owns_capture &&
        click.same_context && !click.modified && click.blank_list_hit &&
        dx > -static_cast<int64_t>(click.drag_width) && dx < click.drag_width &&
        dy > -static_cast<int64_t>(click.drag_height) && dy < click.drag_height;
}

} // namespace pulse::app
