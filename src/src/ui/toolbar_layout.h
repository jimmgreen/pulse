#pragma once
#include <d2d1.h>
#include <algorithm>
#include <array>

namespace pulse::ui {
inline float NavigationStep(float available, float scale) {
    return (available < 500 * scale ? 26.0f : 34.0f) * scale;
}
inline D2D1_RECT_F NavigationButtonAt(float x, float y, float scale, float step) {
    return {x, y, x + step - 4 * scale, y + 32 * scale};
}
struct ToolbarLayout {
    std::array<D2D1_RECT_F, 4> navigation{};
    D2D1_RECT_F address{}, search{}, create{};
    D2D1_RECT_F sort{}, filter{}, overflow{};
    D2D1_RECT_F group{};   // "Group" button / active chip (empty when not offered)
    std::array<D2D1_RECT_F, 9> commands{};
};

// search_min_dip widens the search field while a search is active or being
// edited, borrowing from the address bar but leaving it at least 120 DIP.
inline ToolbarLayout MakeToolbarLayout(float width, float scale, float top, float margin, float create_width,
                                      float left, float filter_expand = 0.0f, float search_min_dip = 0.0f,
                                      float group_width = 0.0f, float sort_width = 0.0f,
                                      float filter_width = 0.0f) {
    ToolbarLayout out;
    const float available = width - left;
    const float nav_step = NavigationStep(available, scale);
    float x = left + margin;
    for (int i=0;i<3;++i) {
        out.navigation[i] = NavigationButtonAt(x, top + 6 * scale, scale, nav_step);
        x += nav_step;
    }
    float search_width = std::clamp(available*0.27f, 140*scale, 360*scale);
    if (search_min_dip > 0.0f && available >= 480*scale) {
        // While a query is being edited (>= 500 DIP requested) the address bar may
        // shrink to 60 DIP; a results page keeps 120 DIP of breadcrumb.
        const float address_min = (search_min_dip >= 500.0f ? 60.0f : 120.0f) * scale;
        const float cap = width - margin - (x + margin + address_min) - nav_step - 2*margin;
        search_width = std::max(search_width, std::min(search_min_dip*scale, cap));
    }
    out.search = {width-margin-search_width,top+4*scale,width-margin,top+40*scale};
    out.address = {x+margin,top+4*scale,std::max(x+margin+60*scale,out.search.left-margin),top+40*scale};
    const bool overflow = available < 540*scale;
    if (available < 480*scale) {
        out.search.left=out.search.right-32*scale;
    }
    out.navigation[3] = NavigationButtonAt(out.search.left - margin - nav_step, top + 6 * scale, scale, nav_step);
    out.address.right = out.navigation[3].left-margin;
    out.create = {left+margin,top+50*scale,left+margin+create_width,top+82*scale};
    x = out.create.right + 12*scale;
    for (int i=0;i<6;++i) {
        out.commands[i] = {x,top+50*scale,x+30*scale,top+82*scale};
        x += 34*scale;
    }
    if (overflow) { for (auto& command : out.commands) command = {}; x = out.create.right; }
    x += 8*scale;
    // sort_width: measured "icon + label + chevron" width (0 = legacy 88 DIP).
    const float sort_w = available >= 700*scale ? (sort_width > 0.0f ? sort_width : 88*scale) : 32*scale;
    out.sort = {x,top+50*scale,x+sort_w,top+82*scale};
    x = out.sort.right + 6*scale;
    // filter_width: measured "icon + label" width (0 = legacy 88 DIP); kept above the
    // 60 DIP icon-only threshold. Expanding grows the field from here.
    const float filter_base = available >= 700*scale
        ? (filter_width > 0.0f ? std::max(filter_width, 64*scale) : 88*scale) : 32*scale;
    const float group_room = group_width > 0.0f ? group_width + 6*scale : 0.0f;
    const float filter_max = std::clamp(width-margin-(overflow ? 44 : 114)*scale-x-group_room,filter_base,220*scale);
    const float filter_w = filter_base + (filter_max-filter_base)*std::clamp(filter_expand,0.0f,1.0f);
    out.filter = {x,top+50*scale,x+filter_w,top+82*scale};
    float tail = out.filter.right;
    if (group_width > 0.0f) {
        out.group = {tail+6*scale,top+50*scale,tail+6*scale+group_width,top+82*scale};
        tail = out.group.right;
    }
    const float right_group = std::max(tail+12*scale,width-margin-102*scale);
    for (int i=6;i<9;++i) {
        x = right_group+(i-6)*34*scale;
        out.commands[i] = {x,top+50*scale,x+30*scale,top+82*scale};
    }
    if (overflow) {
        out.commands[5] = out.commands[6] = out.commands[7] = {};
        out.overflow = {width-margin-32*scale,top+50*scale,width-margin,top+82*scale};
    }
    return out;
}

// Trailing "x" of the active group chip (removes grouping).
inline D2D1_RECT_F ToolbarGroupClearRect(const D2D1_RECT_F& chip, float scale) {
    return {chip.right-28*scale,chip.top+5*scale,chip.right-6*scale,chip.bottom-5*scale};
}
}