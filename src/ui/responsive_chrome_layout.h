#pragma once
#include <d2d1.h>
#include <algorithm>
#include <array>

namespace pulse::ui {
enum class NavigationTier { Full, Compact, Narrow, Merged };
struct ResponsiveNavigation {
    std::array<D2D1_RECT_F, 4> navigation{};
    D2D1_RECT_F address{}, search{}, address_text{};
    NavigationTier tier = NavigationTier::Full;
    bool refresh_inside = false, merged = false;
    int breadcrumb_limit = 99;
};
inline ResponsiveNavigation MakeResponsiveNavigation(float width, float left, float top,
    float margin, float scale, bool can_forward, float search_min_dip) {
    ResponsiveNavigation out;
    const float available = (width - left) / scale;
    out.tier = available >= 760 ? NavigationTier::Full : available >= 600 ? NavigationTier::Compact :
        available >= 460 ? NavigationTier::Narrow : NavigationTier::Merged;
    out.refresh_inside = out.tier != NavigationTier::Full;
    out.merged = out.tier == NavigationTier::Merged;
    out.breadcrumb_limit = out.merged ? 1 : out.tier == NavigationTier::Narrow ? 2 : 99;
    const bool hidden = search_min_dip < 0, editing = search_min_dip >= 500;
    const float step = (available < 500 ? 26.0f : 34.0f) * scale;
    float x = left + margin;
    for (int i = 0; i < 3; ++i) {
        if (i == 1 && !can_forward && available < 600) continue;
        out.navigation[i] = {x, top + 6 * scale, x + step - 4 * scale, top + 38 * scale};
        x += step;
    }
    const float field_left = x + margin, right = width - margin;
    const float y = top + 4 * scale, bottom = top + 40 * scale;
    out.address = {field_left, y, right, bottom};
    out.search = {right, y, right, bottom};
    if (out.merged && editing && !hidden) {
        out.search = out.address;
        out.address.right = out.address.left;
        out.address_text = out.address;
        return out;
    }
    if (!hidden && !out.merged) {
        float search_width = out.tier == NavigationTier::Narrow ? 112 * scale :
            out.tier == NavigationTier::Compact ? std::clamp(available * 0.27f, 180.0f, 260.0f) * scale :
            std::clamp(available * 0.27f, 140.0f, 360.0f) * scale;
        if (search_min_dip > 0)
            search_width = (std::max)(search_width, (std::min)(search_min_dip * scale,
                right - field_left - 120 * scale - margin - (out.refresh_inside ? 0 : step)));
        out.search.left = right - search_width;
        out.address.right = out.search.left - margin;
    } else if (!hidden) {
        out.search.left = right - 32 * scale;
    }
    const float refresh_right = out.merged && !hidden ? out.search.left : out.address.right;
    out.navigation[3] = {refresh_right - 32 * scale, top + 6 * scale, refresh_right, top + 38 * scale};
    if (!out.refresh_inside) {
        out.navigation[3].left = out.address.right - step;
        out.navigation[3].right = out.navigation[3].left + step - 4 * scale;
        out.address.right = out.navigation[3].left - margin;
    }
    out.address_text = out.address;
    if (out.refresh_inside) out.address_text.right = out.navigation[3].left - 4 * scale;
    out.address_text.right = (std::max)(out.address_text.left, out.address_text.right);
    return out;
}
}
