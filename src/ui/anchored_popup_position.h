#pragma once
#include <windows.h>
#include <algorithm>

namespace pulse::ui {
inline RECT AnchoredPopupPosition(RECT anchor, RECT work, int width, int height,
                                  int shadow, int gap, bool above) {
    int x = above ? anchor.left + (anchor.right - anchor.left - width) / 2 : anchor.left - shadow;
    const int upper = anchor.top - gap + shadow - height;
    const int lower = anchor.bottom + gap - shadow;
    int y = above ? upper : lower;
    if (above && y < work.top) y = lower;
    else if (!above && y + height > work.bottom) y = upper;
    x = std::max<int>(work.left, std::min<int>(x, work.right - width));
    y = std::max<int>(work.top, std::min<int>(y, work.bottom - height));
    return {x, y, x + width, y + height};
}
}
