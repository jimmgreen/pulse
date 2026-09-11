#pragma once
#include "app_model.h"
#include <algorithm>

namespace pulse::app {
inline bool TickFilterAnimation(Pane& pane, bool editing, uint64_t now) {
    const float target = editing || !pane.view.filter_text.empty() ? 1.0f : 0.0f;
    if (pane.filter_animation_target != target) {
        pane.filter_animation_from = pane.filter_expand;
        pane.filter_animation_target = target;
        pane.filter_animation_start = now;
    }
    const float before = pane.filter_expand;
    const float t = std::min(1.0f, static_cast<float>(now - pane.filter_animation_start) / 140.0f);
    const float remaining = 1.0f - t;
    pane.filter_expand = t >= 1.0f ? target :
        pane.filter_animation_from + (target - pane.filter_animation_from) *
        (1.0f - remaining * remaining * remaining);
    return before != pane.filter_expand;
}
}
