#pragma once
#include <algorithm>
#include <d2d1_1.h>
namespace pulse::ui {
// Shared miniature windows for the management menu and overview toolbar.
inline void DrawWindowLayoutDiagram(ID2D1DeviceContext *dc, const D2D1_RECT_F &bounds, int layout,
                                    ID2D1SolidColorBrush *brush, float scale) {
    const float width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    const float gap = 2.0f * scale, line = std::max(1.0f, scale);
    const auto window = [&](float x, float y, float w, float h) {
        const auto box = D2D1::RectF(bounds.left + x * width, bounds.top + y * height,
                                     bounds.left + (x + w) * width - gap, bounds.top + (y + h) * height - gap);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(box, 1.5f * scale, 1.5f * scale), brush, line);
        if (box.bottom - box.top > 8 * scale)
            dc->DrawLine(D2D1::Point2F(box.left, box.top + 3 * scale), D2D1::Point2F(box.right, box.top + 3 * scale),
                         brush, line);
    };
    switch (layout) {
    case 0:
        window(0, 0, .5f, 1);
        window(.5f, 0, .5f, 1);
        break;
    case 1:
        window(0, 0, 1, .5f);
        window(0, .5f, 1, .5f);
        break;
    case 2:
        for (int i = 0; i < 3; ++i)
            window(i / 3.0f, 0, 1 / 3.0f, 1);
        break;
    case 3:
        for (int i = 0; i < 4; ++i)
            window((i % 2) * .5f, (i / 2) * .5f, .5f, .5f);
        break;
    case 4:
        window(0, 0, .6f, 1);
        window(.6f, 0, .4f, .5f);
        window(.6f, .5f, .4f, .5f);
        break;
    case 5:
        for (int i = 0; i < 3; ++i)
            window(i * .16f, i * .16f, .68f, .68f);
        break;
    default:
        break;
    }
}
} // namespace pulse::ui
