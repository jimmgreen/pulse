#pragma once
#include <algorithm>

namespace pulse::ui::drag_layout {
inline constexpr float card_height = 34.0f;
inline constexpr float card_max_width = 236.0f;
inline constexpr float offset_y = 14.0f;
inline constexpr float fan_step_y = 4.0f;
// Conservatively include the entire rotated card (13 degrees maximum tilt),
// rear-card fan and shadow, rather than only the lead card's resting rectangle.
inline constexpr float stack_bottom = offset_y + card_height + 2 * fan_step_y +
    card_max_width * 0.225f + 12.0f;
inline float BadgeTop(float cursor_y, float height, float scale,
                      float top, float bottom, float margin) {
    float y = cursor_y - height - (card_max_width * 0.225f / 2 + 12.0f + 8.0f) * scale;
    if (y < top + margin) y = cursor_y + (stack_bottom + 8.0f) * scale;
    return std::max(top + margin, std::min(y, bottom - height - margin));
}
}
