#pragma once
#include <algorithm>
#include <d2d1.h>

namespace pulse::ui {
struct PreviewFooterLayout {
    D2D1_RECT_F caption{}, zoom{};
    D2D1_POINT_2F chevron{};
};

inline PreviewFooterLayout MakePreviewFooterLayout(const D2D1_RECT_F& bounds,
                                                   float scale) {
    PreviewFooterLayout result;
    result.chevron = D2D1::Point2F((bounds.left + bounds.right) * .5f,
                                 (bounds.top + bounds.bottom) * .5f);
    const float right = bounds.right - 4 * scale;
    result.zoom = D2D1::RectF(std::max(result.chevron.x + 14 * scale, right - 58 * scale), bounds.top,
                             right, bounds.bottom);
    const float left = bounds.left + 4 * scale;
    result.caption = D2D1::RectF(left, bounds.top,
        std::max(left, result.chevron.x - 14 * scale), bounds.bottom);
    return result;
}
} // namespace pulse::ui
