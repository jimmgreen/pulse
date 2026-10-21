#pragma once
#include <d2d1.h>
#include <algorithm>
#include <cmath>

namespace pulse::ui {

// Cursor offset is not eased: a drag must track the pointer without lag.
inline D2D1_RECT_F ItemDragPreviewRect(D2D1_RECT_F bounds, float x, float y,
                                      float width, float height, float scale) {
    width = (std::min)(width, (std::max)(0.0f, bounds.right - bounds.left));
    height = (std::min)(height, (std::max)(0.0f, bounds.bottom - bounds.top));
    float left = x + 16.0f * scale;
    float top = y + 20.0f * scale;
    if (left + width > bounds.right) left = x - width - 12.0f * scale;
    if (top + height > bounds.bottom) top = y - height - 12.0f * scale;
    left = (std::clamp)(std::round(left), bounds.left, bounds.right - width);
    top = (std::clamp)(std::round(top), bounds.top, bounds.bottom - height);
    return D2D1::RectF(left, top, left + width, top + height);
}

} // namespace pulse::ui
