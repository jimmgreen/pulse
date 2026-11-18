#pragma once
#include <d2d1.h>

namespace pulse::ui {
struct TabPagerLayout {
    D2D1_RECT_F up{}, label{}, down{};
};

inline TabPagerLayout MakeTabPagerLayout(float right, float row_height, int rows, float scale) {
    const float left = right - 26.0f * scale;
    const float edge = right - 2.0f * scale;
    const float top = row_height + 4.0f * scale;
    const float bottom = row_height * rows;
    const float middle = (top + bottom) * 0.5f;
    return {{left, top, edge, middle - 7.0f * scale},
            {left, middle - 7.0f * scale, edge, middle + 7.0f * scale},
            {left, middle + 7.0f * scale, edge, bottom}};
}
}
