#pragma once
#include "view_layout.h"
#include <algorithm>

namespace pulse::ui {
struct IconZoomTarget {
    ViewMode mode;
    float dip;
};

inline IconZoomTarget ResolveIconZoom(ViewMode mode, float dip, ViewMode last_mode,
                                     float last_dip, int step, float scale, bool leave_icons = true) {
    if (!step) return {mode, dip};
    if (!IsZoomIconMode(mode)) {
        if (!leave_icons) return {mode, dip};
        if (step < 0) return {ViewMode::Details, 0};
        return IsZoomIconMode(last_mode) ? IconZoomTarget{last_mode, last_dip}
                                        : IconZoomTarget{ViewMode::MediumIcons, 0};
    }
    const float next = ZoomIconDip(IconDip(mode, dip, scale), step, scale);
    if (next > 0) return {IconModeForDip(next, scale), next};
    return leave_icons ? IconZoomTarget{ViewMode::Details, 0} : IconZoomTarget{mode, dip};
}

// Preserve touchpad fractions, but limit each message to one layout change.
inline int ConsumeIconZoomWheel(int delta, ULONGLONG now, int& remainder, ULONGLONG& tick) {
    if (!delta) return 0;
    if (now - tick > 400 || (remainder > 0) != (delta > 0)) remainder = 0;
    tick = now;
    remainder += delta;
    const int step = std::clamp(remainder / WHEEL_DELTA, -1, 1);
    if (step) remainder = 0;
    return step;
}
}
