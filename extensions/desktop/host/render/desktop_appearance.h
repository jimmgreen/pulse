#pragma once

#include "host/model/desktop_model.h"
#include "../../../../src/ui/FluentTokens.h"
#include "../../../../src/ui/window_material.h"

namespace pulse::desktop::render {

inline uint32_t ColorToArgb(D2D1_COLOR_F color) noexcept {
    const auto channel = [](float value) {
        return static_cast<uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return (channel(color.a) << 24) | (channel(color.r) << 16) |
           (channel(color.g) << 8) | channel(color.b);
}

struct CardMaterialProfile {
    float tint_opacity;
    float luminosity_opacity;
};

// Thin desktop surfaces control color and luminosity independently, rather
// than covering the live backdrop with a high-alpha ordinary solid fill.
inline CardMaterialProfile CardAcrylicProfile(bool dark, uint8_t opacity_percent) noexcept {
    const float surface = std::clamp(static_cast<float>(opacity_percent) / 100.0f, 0.30f, 1.0f);
    const float strength = 0.08f + 0.92f * surface;
    constexpr float intensity = 0.65f;
    return dark ? CardMaterialProfile{(0.04f + 0.38f * intensity) * strength,
                                      (0.16f + 0.40f * intensity) * strength}
                : CardMaterialProfile{(0.02f + 0.32f * intensity) * strength,
                                      (0.22f + 0.42f * intensity) * strength};
}

// Desktop cards and tiles sit on the wallpaper all day and are much larger than a flyout, so they keep
// more of the wallpaper than CardAcrylicProfile (the flyout recipe): a light luminosity layer and almost
// no tint. The same 界面透明度 setting still scales them.
inline CardMaterialProfile CardSurfaceProfile(bool dark, uint8_t opacity_percent) noexcept {
    const float surface = std::clamp(static_cast<float>(opacity_percent) / 100.0f, 0.30f, 1.0f);
    const float strength = 0.10f + 0.90f * surface;
    return dark ? CardMaterialProfile{0.10f * strength, 0.34f * strength}
                : CardMaterialProfile{0.02f * strength, 0.26f * strength};
}

inline D2D1_COLOR_F CardMaterialTint(bool dark) noexcept {
    return ui::HexColor(dark ? 0x202226u : 0xFFFFFFu);
}

inline CardMaterialProfile CardMicaProfile(bool alternate, bool dark) noexcept {
    constexpr float intensity = 0.65f;
    if (alternate) return {0.28f + 0.54f * intensity,
                           dark ? 0.34f + 0.38f * intensity : 0.42f + 0.34f * intensity};
    return {0.04f + 0.42f * intensity,
            dark ? 0.78f + 0.16f * intensity : 0.82f + 0.14f * intensity};
}

struct SystemAppearance {
    bool dark = false;
    bool transparency = true;
    uint32_t accent_argb = 0xFF0078D4u;
};

// Only call at initialization or system notifications, never in the drawing loop.
inline SystemAppearance ReadSystemAppearance() noexcept {
    SystemAppearance result;
    result.dark = ui::ShouldUseDarkMode(ui::ThemeMode::Auto);
    result.accent_argb = ColorToArgb(ui::GetAccentColor());
    result.transparency = ui::SystemAllowsPopupAcrylic();
    return result;
}

} // namespace pulse::desktop::render
