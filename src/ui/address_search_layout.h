#pragma once
#include <d2d1.h>
#include <algorithm>

namespace pulse::ui {

struct AddressSearchLayout {
    D2D1_RECT_F scope;
    D2D1_RECT_F input;
    D2D1_RECT_F clear;
    D2D1_RECT_F close;
    bool scope_label = false;
};

inline AddressSearchLayout LayoutAddressSearch(D2D1_RECT_F field, float scale) {
    AddressSearchLayout out;
    const float width = (field.right - field.left) / scale;
    const float button = (width < 180.0f ? 24.0f : 28.0f) * scale;
    const float top = field.top + 3.0f * scale;
    const float bottom = field.bottom - 3.0f * scale;
    out.scope_label = width >= 420.0f;
    const float left = field.left + 4.0f * scale;
    out.scope = D2D1::RectF(left, top, left + (out.scope_label ? 158.0f * scale : button), bottom);
    out.close = D2D1::RectF(field.right - button - 4.0f * scale, top,
                          field.right - 4.0f * scale, bottom);
    const bool show_clear = width >= 260.0f;
    out.clear = D2D1::RectF(out.close.left - (show_clear ? button : 0.0f), top, out.close.left, bottom);
    out.input = D2D1::RectF(out.scope.right + 4.0f * scale, field.top + 2.0f * scale,
                           out.clear.left - 4.0f * scale, field.bottom - 2.0f * scale);
    return out;
}

} // namespace pulse::ui
