#pragma once
#include <algorithm>
#include <d2d1_1.h>

namespace pulse::ui {
enum class WindowActionIcon { Manage, New, Overview, Detach, Merge };

// Shared geometry keeps caption and menu icons independent of the system icon font.
inline void DrawWindowActionIcon(ID2D1DeviceContext* dc, D2D1_RECT_F bounds,
                                 WindowActionIcon kind, ID2D1SolidColorBrush* brush) {
    const float size = (std::min)(bounds.right - bounds.left, bounds.bottom - bounds.top);
    const float u = size / 20.0f;
    const float x = bounds.left + (bounds.right - bounds.left - size) * 0.5f;
    const float y = bounds.top + (bounds.bottom - bounds.top - size) * 0.5f;
    auto point = [&](float a, float b) { return D2D1::Point2F(x + a*u, y + b*u); };
    auto line = [&](float a, float b, float c, float d) { dc->DrawLine(point(a,b), point(c,d), brush, 1.4f*u); };
    auto box = [&](float a, float b, float c, float d) {
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x+a*u,y+b*u,x+c*u,y+d*u),1.5f*u,1.5f*u),brush,1.4f*u);
    };
    switch (kind) {
    case WindowActionIcon::Manage:
        box(2.5f,6,12.5f,16);
        line(6,6,6,3.5f); line(6,3.5f,17.5f,3.5f);
        line(17.5f,3.5f,17.5f,13); line(17.5f,13,12.5f,13);
        break;
    case WindowActionIcon::New:
        box(2.5f,3.5f,17.5f,16.5f);
        line(2.5f,7,17.5f,7); line(10,9,10,14); line(7.5f,11.5f,12.5f,11.5f);
        break;
    case WindowActionIcon::Overview:
        box(2.5f,3.5f,8.5f,8.5f); box(11.5f,3.5f,17.5f,8.5f);
        box(2.5f,11.5f,8.5f,16.5f); box(11.5f,11.5f,17.5f,16.5f);
        break;
    case WindowActionIcon::Detach:
        line(10,6,2.5f,6); line(2.5f,6,2.5f,17); line(2.5f,17,14,17); line(14,17,14,10);
        line(11,3,17,3); line(17,3,17,9); line(17,3,9,11);
        break;
    case WindowActionIcon::Merge:
        box(2,4,8,16); box(12,4,18,16); line(8,10,12,10);
        break;
    }
}
} // namespace pulse::ui
