#pragma once
#include <algorithm>
#include <d2d1_1.h>
namespace pulse::ui {
// Compact toolbar icons use one frame, unlike the filled menu previews.
inline void DrawWindowArrangementIcon(ID2D1DeviceContext* dc, const D2D1_RECT_F& bounds, int layout,
                                      ID2D1SolidColorBrush* brush, D2D1_COLOR_F background) {
    const auto ink = brush->GetColor();
    const float u = (std::min)((bounds.right-bounds.left)/18.0f, (bounds.bottom-bounds.top)/14.0f);
    const float x = (bounds.left+bounds.right-18*u)*0.5f;
    const float y = (bounds.top+bounds.bottom-14*u)*0.5f;
    auto box = [&](float left, float top, float right, float bottom, float radius, bool opaque) {
        const auto rect = D2D1::RoundedRect(D2D1::RectF(x+left*u,y+top*u,x+right*u,y+bottom*u),radius*u,radius*u);
        if (opaque) { brush->SetColor(background); dc->FillRoundedRectangle(rect,brush); brush->SetColor(ink); }
        dc->DrawRoundedRectangle(rect,brush,1.3f*u);
    };
    auto line = [&](float a,float b,float c,float d) {
        dc->DrawLine(D2D1::Point2F(x+a*u,y+b*u),D2D1::Point2F(x+c*u,y+d*u),brush,1.3f*u);
    };
    if (layout == 5) {
        box(1,1,11,8,1,true);
        box(4,3.5f,14,10.5f,1,true);
        box(7,6,17,13,1,true);
        return;
    }
    box(1,1,17,13,1.5f,false);
    switch (layout) {
    case 0: line(9,1,9,13); break;
    case 1: line(1,7,17,7); break;
    case 2: line(6.33f,1,6.33f,13); line(11.67f,1,11.67f,13); break;
    case 3: line(9,1,9,13); line(1,7,17,7); break;
    case 4: line(10,1,10,13); line(10,7,17,7); break;
    default: break;
    }
}

// Shared miniature windows for the management menu and overview toolbar.
inline void DrawWindowLayoutDiagram(ID2D1DeviceContext *dc, const D2D1_RECT_F &bounds, int layout,
                                    ID2D1SolidColorBrush *brush, float scale,
                                    D2D1_COLOR_F accent, bool miniature = false) {
    const float width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    const float gap = 2.0f * scale, line = std::max(1.0f, scale);
    const auto ink = brush->GetColor();
    int index = 0;
    if (miniature) {
        auto outline = ink;
        outline.a *= 0.15f;
        brush->SetColor(outline);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(bounds, 4*scale, 4*scale), brush, line);
    }
    const auto window = [&](float x, float y, float w, float h) {
        const float inset = miniature ? 3*scale : 0;
        const float inner_w = width - 2*inset + gap, inner_h = height - 2*inset + gap;
        const auto box = D2D1::RectF(bounds.left + inset + x * inner_w, bounds.top + inset + y * inner_h,
            bounds.left + inset + (x+w) * inner_w - gap, bounds.top + inset + (y+h) * inner_h - gap);
        const bool current = layout == 5 ? index == 2 : index == 0;
        auto color = current ? accent : ink;
        if (miniature) {
            if (!current) color.a *= 0.22f;
            brush->SetColor(color);
            dc->FillRoundedRectangle(D2D1::RoundedRect(box, 2*scale, 2*scale), brush);
        } else {
            brush->SetColor(color);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(box, 1.5f*scale, 1.5f*scale), brush, line);
        }
        ++index;
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
    case 6:
        window(.4f, 0, .6f, 1);
        window(0, 0, .4f, .5f);
        window(0, .5f, .4f, .5f);
        break;
    case 7:
        window(0, 0, 1, .6f);
        window(0, .6f, .5f, .4f);
        window(.5f, .6f, .5f, .4f);
        break;
    case 8:
        window(0, 0, 1, 1);
        brush->SetColor(ink);
        dc->DrawLine(D2D1::Point2F(bounds.left + 5*scale, bounds.top + 13*scale),
                     D2D1::Point2F(bounds.right - 5*scale, bounds.top + 13*scale), brush, line);
        for (int tab = 1; tab < 3; ++tab) {
            const float x = bounds.left + width * tab / 3;
            dc->DrawLine(D2D1::Point2F(x, bounds.top + 5*scale),
                         D2D1::Point2F(x, bounds.top + 13*scale), brush, line);
        }
        break;
    default:
        break;
    }
    brush->SetColor(ink);
}
} // namespace pulse::ui
