#pragma once
#include <algorithm>
namespace pulse::ui {
struct PickerCustomSections {
    float prominent_top = 0;
    float content_height = 0;
};
inline PickerCustomSections LayoutPickerCustomSections(float normal_height, float prominent_height, float viewport_height) {
    const float gap = normal_height > 0 && prominent_height > 0 ? 12.0f : 0.0f;
    const float top = prominent_height > 0 ? std::max(4.0f + normal_height + gap, viewport_height - 4.0f - prominent_height) : 0.0f;
    return {top, prominent_height > 0 ? top + prominent_height + 4.0f : normal_height + 8.0f};
}
inline float PickerProminentLeft(float width) { return std::max(16.0f, width - 28.0f - 360.0f); }
}
