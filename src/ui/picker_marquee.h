#pragma once
#include "../app/marquee_selection.h"
#include <d2d1.h>
#include <cmath>

namespace pulse::ui {
// The picker uses the main list's selection algebra with explicit indices,
// because its filename chips and COM result consume that same set.
struct PickerMarquee {
    bool pending = false, active = false, additive = false, toggle = false;
    D2D1_POINT_2F start{}, cursor{};
    float origin_scroll = 0;
    int caret = -1, anchor = -1;
    std::unordered_set<int> before;
    std::vector<int> hits;
    app::MarqueeSelection delta;

    void Begin(const std::unordered_set<int>& selection, int focus, int range_anchor,
               float x, float y, float scroll, bool ctrl, bool shift) {
        Reset();
        pending = true; additive = ctrl || shift; toggle = ctrl;
        start = cursor = {x, y}; origin_scroll = scroll;
        before = selection; caret = focus; anchor = range_anchor;
    }
    bool Move(float x, float y, float slop_x, float slop_y) {
        cursor = {x, y};
        if (pending && (std::abs(x - start.x) >= slop_x || std::abs(y - start.y) >= slop_y)) {
            pending = false; active = true;
        }
        return active;
    }
    D2D1_RECT_F Rect(float scroll, const D2D1_RECT_F& list) const {
        const float start_y = start.y - (scroll - origin_scroll);
        const float x = std::clamp(cursor.x, list.left, list.right);
        const float y = std::clamp(cursor.y, list.top, list.bottom);
        const float left = (std::min)(start.x, x), top = (std::min)(start_y, y);
        return {left, top, (std::max)((std::max)(start.x, x), left + 1),
            (std::max)((std::max)(start_y, y), top + 1)};
    }
    bool Apply(std::unordered_set<int>& selection, int count) {
        bool all = false;
        return delta.Apply(selection, all, before, additive, toggle, count, hits, false);
    }
    void Reset() {
        pending = active = false;
        app::ClearMarqueeScratch(before); app::ClearMarqueeScratch(hits); delta.Reset();
    }
};
} // namespace pulse::ui
