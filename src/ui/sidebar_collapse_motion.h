#pragma once
#include "ui_motion.h"

namespace pulse::ui {
inline float SidebarCollapseProgress(float elapsed_ms) {
    const float duration = static_cast<float>(motion::SpringSettleMs(motion::spring::Smooth));
    if (elapsed_ms <= 0.0f) return 0.0f;
    if (elapsed_ms >= duration) return 1.0f;
    const float spring = motion::SpringProgress(motion::spring::Smooth, elapsed_ms);
    // The spring still has a visible width residual at its time cutoff. Bring
    // that residual to zero with zero slope instead of snapping the last frame.
    constexpr float tail_ms = 80.0f;
    const float tail = std::clamp((elapsed_ms - (duration - tail_ms)) / tail_ms, 0.0f, 1.0f);
    const float left = 1.0f - tail;
    const float remaining = left * left * left * left * (1.0f + 4.0f * tail);
    return std::clamp(1.0f - (1.0f - spring) * remaining, 0.0f, 1.0f);
}
} // namespace pulse::ui
