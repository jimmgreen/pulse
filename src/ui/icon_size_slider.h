#pragma once
#include "FluentTokens.h"
#include "view_layout.h"
#include <d2d1_1.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace pulse::ui {
inline constexpr float kIconSizeSliderWidth = 122.0f;
inline constexpr float kIconSizeTrackLeft = 15.0f;
inline constexpr float kIconSizeTrackRight = 20.0f;

inline float IconSizeSliderPosition(float dip) noexcept {
    return std::log(std::clamp(dip, kMinIconDip, kMaxIconDip) / kMinIconDip) /
        std::log(kMaxIconDip / kMinIconDip);
}
inline std::array<float, 3> IconSizeSliderStops(float scale) noexcept {
    return {IconPresetDip(ViewMode::MediumIcons, scale), IconPresetDip(ViewMode::LargeIcons, scale),
        std::max(IconPresetDip(ViewMode::ExtraLargeIcons, scale), 120.0f)};
}
inline float IconSizeSliderValueAt(const D2D1_RECT_F& bounds, float x, float scale, float preset_scale) noexcept {
    if (bounds.right <= bounds.left) return 0;
    const float x0 = bounds.left + kIconSizeTrackLeft * scale;
    const float x1 = bounds.right - kIconSizeTrackRight * scale;
    for (const float stop : IconSizeSliderStops(preset_scale)) {
        if (stop <= kMaxIconDip && std::fabs(x - (x0 + (x1 - x0) * IconSizeSliderPosition(stop))) <= 5 * scale) return stop;
    }
    const float t = std::clamp((x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
    return std::round(kMinIconDip * std::pow(kMaxIconDip / kMinIconDip, t));
}
// Shared pane-header control. Geometry scale and preset scale are separate for
// windows that draw logical DIPs through their render target's DPI.
inline void DrawIconSizeSlider(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush,
    const D2D1_RECT_F& bounds, float dip, float scale, float preset_scale,
    const Theme& theme, bool hovered, float alpha = 1) {
    if (!dc || !brush || bounds.right - bounds.left < 36 * scale) return;
    const auto color = [&](D2D1_COLOR_F value, float opacity = 1.0f) { value.a = alpha * opacity; brush->SetColor(value); };
    const auto fill = [&](D2D1_RECT_F rect, float radius, D2D1_COLOR_F value, float opacity = 1.0f) {
        color(value, opacity); dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
    };
    const float cy = (bounds.top + bounds.bottom) * .5f;
    const float x0 = bounds.left + kIconSizeTrackLeft * scale, x1 = bounds.right - kIconSizeTrackRight * scale;
    const float tx = x0 + (x1 - x0) * IconSizeSliderPosition(dip), sw = 1.4f * scale;
    color(theme.text_secondary, .85f);
    dc->DrawRoundedRectangle(D2D1::RoundedRect({bounds.left + .5f * sw, cy - 3.5f * scale,
        bounds.left + .5f * sw + 7 * scale, cy + 3.5f * scale}, 2 * scale, 2 * scale), brush, sw);
    dc->DrawRoundedRectangle(D2D1::RoundedRect({bounds.right - .5f * sw - 12 * scale, cy - 6 * scale,
        bounds.right - .5f * sw, cy + 6 * scale}, 3 * scale, 3 * scale), brush, sw);
    fill({x0, cy - scale, x1, cy + scale}, scale, theme.text_secondary, .4f);
    fill({x0, cy - scale, tx, cy + scale}, scale, theme.accent);
    for (const float stop : IconSizeSliderStops(preset_scale)) {
        if (stop > kMaxIconDip) continue;
        const float sx = x0 + (x1 - x0) * IconSizeSliderPosition(stop);
        fill({sx - scale, cy - 3 * scale, sx + scale, cy + 3 * scale}, scale,
            dip >= stop - .5f ? theme.accent : theme.text_secondary, dip >= stop - .5f ? 1 : .6f);
    }
    const float radius = 7 * scale;
    color(D2D1::ColorF(0, 0, 0), .16f);
    dc->FillEllipse(D2D1::Ellipse({tx, cy + scale}, radius + scale, radius + scale), brush);
    const auto thumb = D2D1::Ellipse({tx, cy}, radius, radius);
    color(theme.surface_card); dc->FillEllipse(thumb, brush);
    color(D2D1::ColorF(.5f, .5f, .5f), .32f); dc->DrawEllipse(thumb, brush, scale);
    const float dot = (hovered ? 4.5f : 3.5f) * scale;
    color(theme.accent); dc->FillEllipse(D2D1::Ellipse({tx, cy}, dot, dot), brush);
}
}
