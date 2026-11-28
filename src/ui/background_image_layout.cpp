#include "background_image_layout.h"

#include <algorithm>

namespace pulse::ui {
namespace {

// d2d1.h defines the struct; the helpers header is not part of its guarantee.
D2D1_RECT_F Rect(float left, float top, float right, float bottom) noexcept {
    return D2D1_RECT_F{left, top, right, bottom};
}

float AlignedStart(float start, float extent, float length, int align) noexcept {
    switch (align) {
    case 0: return start;
    case 1: return start + (extent - length) * 0.5f;
    default: return start + extent - length;
    }
}

} // namespace

D2D1_RECT_F ImagePlacementRect(const D2D1_RECT_F& area, int position, int ratio_percent) noexcept {
    const float dw = area.right - area.left;
    const float dh = area.bottom - area.top;
    if (position < 0 || position > 8 || position == 4 || dw <= 0.0f || dh <= 0.0f) return area;

    const float ratio = static_cast<float>(std::clamp(ratio_percent, 0, 100)) / 100.0f;
    const float w = dw * ratio;
    const float h = dh * ratio;
    const float x = AlignedStart(area.left, dw, w, position % 3);
    const float y = AlignedStart(area.top, dh, h, position / 3);
    return Rect(x, y, x + w, y + h);
}

D2D1_RECT_F ImageDrawRect(float image_w, float image_h, const D2D1_RECT_F& area,
                          int position, int ratio_percent, ImageFit fit) noexcept {
    const D2D1_RECT_F placement = ImagePlacementRect(area, position, ratio_percent);
    const float pw = placement.right - placement.left;
    const float ph = placement.bottom - placement.top;
    if (image_w <= 0.0f || image_h <= 0.0f || pw <= 0.0f || ph <= 0.0f) return placement;

    float w = pw;
    float h = ph;
    switch (fit) {
    case ImageFit::Stretch:
        break;
    case ImageFit::Original:
        w = image_w;
        h = image_h;
        break;
    case ImageFit::Contain: {
        const float scale = (std::min)(pw / image_w, ph / image_h);
        w = image_w * scale;
        h = image_h * scale;
        break;
    }
    case ImageFit::Cover:
    default: {
        const float scale = (std::max)(pw / image_w, ph / image_h);
        w = image_w * scale;
        h = image_h * scale;
        break;
    }
    }
    const float x = placement.left + (pw - w) * 0.5f;
    const float y = placement.top + (ph - h) * 0.5f;
    return Rect(x, y, x + w, y + h);
}

} // namespace pulse::ui
