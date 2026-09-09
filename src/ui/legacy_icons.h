#pragma once
#include "typography.h"
#include "../common/windows_compat.h"
#include <d2d1_1.h>
#include <algorithm>
#include <cmath>

namespace pulse::ui {
// Font-independent outlines for the PUA icons used by Pulse on pre-MDL2 Windows.
inline bool DrawLegacyIcon(ID2D1DeviceContext* dc, IDWriteFactory2* factory,
                           std::wstring_view glyph, const D2D1_RECT_F& bounds,
                           ID2D1Brush* brush, float nominal_size = 16.0f) {
    if (!dc || !factory || !brush || glyph.size() != 1 ||
        glyph[0] < 0xE000 || glyph[0] > 0xEFFF) return false;
    if (!compat::LegacyMode() && typography::HasIconFont(factory)) return false;
    const float size = std::min(nominal_size,
        std::min(bounds.right - bounds.left, bounds.bottom - bounds.top) * 0.72f);
    if (size <= 0) return true;
    D2D1_MATRIX_3X2_F previous;
    dc->GetTransform(&previous);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(size / 24, size / 24) *
        D2D1::Matrix3x2F::Translation((bounds.left + bounds.right - size) / 2,
                                      (bounds.top + bounds.bottom - size) / 2) * previous);
    const auto line = [&](float x, float y, float a, float b) {
        dc->DrawLine(D2D1::Point2F(x,y), D2D1::Point2F(a,b), brush, 1.7f);
    };
    const auto box = [&](float x, float y, float a, float b) {
        dc->DrawRectangle(D2D1::RectF(x,y,a,b), brush, 1.7f);
    };
    const auto circle = [&](float x, float y, float r) {
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x,y),r,r),brush,1.7f);
    };
    const auto folder = [&] { line(2,6,9,6); line(9,6,12,9); line(12,9,22,9);
        line(22,9,22,21); line(22,21,2,21); line(2,21,2,6); };
    const auto arrow = [&](bool left) { const float x = left ? 4.0f : 20.0f;
        line(4,12,20,12); line(x,12,12,4); line(x,12,12,20); };
    const auto check = [&] { line(4,12,9,17); line(9,17,20,5); };
    const wchar_t c = glyph[0];
    switch (c) {
    case 0xE711: case 0xE8BB: line(5,5,19,19); line(5,19,19,5); break;
    case 0xE710: line(12,3,12,21); line(3,12,21,12); break;
    case 0xE921: line(4,17,20,17); break;
    case 0xE922: box(4,4,20,20); break;
    case 0xE923: box(3,7,17,21); line(7,7,7,3); line(7,3,21,3); line(21,3,21,17); line(21,17,17,17); break;
    case 0xE70D: line(4,8,12,16); line(12,16,20,8); break;
    case 0xE70E: line(4,16,12,8); line(12,8,20,16); break;
    case 0xE76C: line(8,4,16,12); line(16,12,8,20); break;
    case 0xE72B: case 0xE7A7: arrow(true); break;
    case 0xE72A: case 0xE7C2: case 0xE8E5: arrow(false); break;
    case 0xE898: line(12,3,12,21); line(12,3,4,11); line(12,3,20,11); break;
    case 0xE896: line(12,3,12,16); line(12,16,6,10); line(12,16,18,10); line(3,17,3,21); line(3,21,21,21); line(21,21,21,17); break;
    case 0xE73E: case 0xE777: check(); break;
    case 0xE721: circle(10,10,7); line(15,15,22,22); break;
    case 0xE712: for (int x : {5,12,19}) dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(static_cast<float>(x),12),1.4f,1.4f),brush); break;
    case 0xE700: for (int y : {5,12,19}) line(3,static_cast<float>(y),21,static_cast<float>(y)); break;
    case 0xE8B7: folder(); break;
    case 0xE8F4: folder(); line(9,15,17,15); line(13,11,13,19); break;
    case 0xE8C8: case 0xE8EF: box(7,7,21,21); line(17,7,17,3); line(17,3,3,3); line(3,3,3,17); line(3,17,7,17); break;
    case 0xE74D: case 0xE75C: line(3,6,21,6); box(5,6,19,21); box(9,3,15,6); line(9,10,9,18); line(15,10,15,18); break;
    case 0xE8A5: case 0xE8FD: box(5,2,19,22); line(8,9,16,9); line(8,13,16,13); line(8,17,14,17); break;
    case 0xE8AC: line(4,17,16,3); line(16,3,21,7); line(21,7,9,21); line(9,21,4,22); line(4,22,4,17); break;
    case 0xE8C6: circle(6,17,4); circle(18,17,4); line(9,14,19,2); line(15,14,5,2); break;
    case 0xE77F: box(5,5,19,22); box(9,2,15,7); break;
    case 0xE734: case 0xE735: {
        D2D1_POINT_2F first{}, last{};
        for (int i=0;i<=10;++i) {
            const float angle = static_cast<float>(i % 10)*0.62831853f-1.57079633f;
            const float r = i%2 ? 4.5f : 10.0f;
            const auto p = D2D1::Point2F(12+std::cos(angle)*r,12+std::sin(angle)*r);
            if (i==0) first=p; else dc->DrawLine(last,i==10?first:p,brush,1.7f);
            last=p;
        } break;
    }
    case 0xE718: case 0xE841: case 0xE77A: box(8,3,16,10); line(8,10,5,15); line(16,10,19,15); line(5,15,19,15); line(12,15,12,23); if(c==0xE77A) line(2,2,22,22); break;
    case 0xE946: circle(12,12,10); line(12,10,12,18); circle(12,6,0.7f); break;
    case 0xE7BA: case 0xEA39: line(12,2,23,21); line(23,21,1,21); line(1,21,12,2); line(12,9,12,15); circle(12,18,0.6f); break;
    case 0xE823: circle(12,12,9); line(12,5,12,12); line(12,12,18,14); break;
    case 0xE72C: circle(12,12,9); line(19,2,21,9); line(21,9,14,9); break;
    case 0xE8A7: box(3,7,17,21); line(12,12,22,2); line(15,2,22,2); line(22,2,22,9); break;
    case 0xE7F4: box(2,3,22,17); line(12,17,12,22); line(7,22,17,22); break;
    case 0xE7F1: box(2,7,22,19); circle(18,15,1); line(4,11,20,11); break;
    case 0xE80F: line(1,11,12,2); line(12,2,23,11); line(5,9,5,22); line(5,22,19,22); line(19,22,19,9); box(10,15,14,22); break;
    case 0xE8A0: case 0xE89F: box(2,3,22,21); line(15,3,15,21); break;
    case 0xE8A1: case 0xE8F1: line(3,6,3,21); line(3,21,21,21); line(21,21,21,6); line(3,15,8,15); line(8,15,10,18); line(10,18,14,18); line(14,18,16,15); line(16,15,21,15); break;
    case 0xE738: case 0xE8B3: box(3,3,21,21); if(c!=0xE738) check(); break;
    case 0xE7A1: circle(12,12,9); dc->FillRectangle(D2D1::RectF(5,5,12,19),brush); break;
    case 0xE71C: line(2,3,22,3); line(22,3,15,12); line(15,12,15,22); line(15,22,9,18); line(9,18,9,12); line(9,12,2,3); break;
    case 0xE8EC: case 0xE8D2: line(3,3,13,3); line(13,3,22,12); line(22,12,12,22); line(12,22,3,13); line(3,13,3,3); circle(8,8,1.4f); break;
    case 0xE71B: circle(8,9,5); circle(16,15,5); line(8,9,16,15); break;
    case 0xE8CB: line(5,3,5,21); line(1,17,5,21); line(5,21,9,17); line(12,5,23,5); line(12,11,20,11); line(12,17,17,17); break;
    case 0xE756: box(1,3,23,21); line(5,8,9,12); line(9,12,5,16); line(12,16,19,16); break;
    case 0xE968: box(9,2,15,8); box(1,17,7,23); box(17,17,23,23); line(12,8,12,13); line(4,13,20,13); line(4,13,4,17); line(20,13,20,17); break;
    case 0xE8A9: case 0xECA5: case 0xEA37: box(2,2,10,10); box(14,2,22,10); box(2,14,10,22); box(14,14,22,22); break;
    case 0xE713: circle(12,12,8); circle(12,12,3); for(int i=0;i<8;++i){float a=static_cast<float>(i)*0.785398f;line(12+8*std::cos(a),12+8*std::sin(a),12+11*std::cos(a),12+11*std::sin(a));} break;
    case 0xE706: circle(12,12,5); for(int i=0;i<8;++i){float a=static_cast<float>(i)*0.785398f;line(12+8*std::cos(a),12+8*std::sin(a),12+11*std::cos(a),12+11*std::sin(a));} break;
    default: dc->SetTransform(previous); return false;
    }
    dc->SetTransform(previous);
    return true;
}
}
