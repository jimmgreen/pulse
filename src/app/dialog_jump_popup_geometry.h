#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace pulse::app {
struct DialogJumpPopupPosition {
    RECT rect{};
    bool above = false;
};

inline DialogJumpPopupPosition PlaceDialogJumpPopup(const RECT& bar, const RECT& work,
    float anchor_dip, float scale, int width, int height, int gap, bool align_right, bool prefer_above) {
    width = std::clamp(width, 1, static_cast<int>((std::max)(1L, work.right-work.left)));
    height = std::clamp(height, 1, static_cast<int>((std::max)(1L, work.bottom-work.top)));
    const int anchor = static_cast<int>(bar.left) + static_cast<int>(std::lround(anchor_dip*scale));
    const int x = std::clamp(anchor-(align_right ? width : 0), static_cast<int>(work.left),
        static_cast<int>(work.right)-width);
    const int upper = static_cast<int>(bar.top)-gap-height;
    const int lower = static_cast<int>(bar.bottom)+gap;
    bool above = prefer_above;
    if (above && upper<work.top && lower+height<=work.bottom) above=false;
    else if (!above && lower+height>work.bottom && upper>=work.top) above=true;
    const int y = std::clamp(above ? upper : lower, static_cast<int>(work.top),
        static_cast<int>(work.bottom)-height);
    return {{x,y,x+width,y+height},above};
}
} // namespace pulse::app
