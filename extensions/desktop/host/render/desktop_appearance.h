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

// Desktop flyout / menu Acrylic profile (Windows.UI.Composition HostBackdropBrush graph).
// Note: in HostBackdropLayer's WinUI-aligned WUC BlendEffect graph, `luminosity_opacity` drives
// D2D1_BLEND_MODE_COLOR (Win2D/WUC Luminosity — clamps backdrop brightness into the theme's
// readable band) while `tint_opacity` drives D2D1_BLEND_MODE_LUMINOSITY (Win2D/WUC Color — chroma tinting).
// Keeping `luminosity_opacity` in Win11's readability range guarantees WCAG AA/AAA contrast for
// theme text over pitch-black or pure-white behind-window content while preserving live blur.
inline CardMaterialProfile CardAcrylicProfile(bool dark, uint8_t opacity_percent) noexcept {
    const float surface = std::clamp(static_cast<float>(opacity_percent) / 100.0f, 0.30f, 1.0f);
    return dark ? CardMaterialProfile{0.22f + 0.24f * surface,
                                      0.72f + 0.18f * surface}
                : CardMaterialProfile{0.16f + 0.24f * surface,
                                      0.68f + 0.20f * surface};
}

// Desktop Acrylic cards sit on the wallpaper all day and remain visibly translucent, while
// locking enough luminosity (Win11 WUC Luminosity Blend) that theme text stays readable across
// black/white wallpapers without per-card black/white ink flipping.
inline CardMaterialProfile CardSurfaceProfile(bool dark, uint8_t opacity_percent) noexcept {
    const float surface = std::clamp(static_cast<float>(opacity_percent) / 100.0f, 0.30f, 1.0f);
    const float op_norm = (surface - 0.30f) / 0.70f;
    return dark ? CardMaterialProfile{0.16f + 0.22f * op_norm,
                                      0.70f + 0.18f * op_norm}
                : CardMaterialProfile{0.12f + 0.22f * op_norm,
                                      0.66f + 0.20f * op_norm};
}

inline D2D1_COLOR_F CardMaterialTint(bool dark) noexcept {
    return ui::HexColor(dark ? 0x202226u : 0xFFFFFFu);
}

// Desktop Mica / MicaAlt keep wallpaper hue and spatial variation visible while clamping
// luminance into the active theme's readable band (matching Win11 Fluent 2 theme ink).
inline CardMaterialProfile CardMicaProfile(bool alternate, bool dark) noexcept {
    if (alternate) return {dark ? 0.86f : 0.84f,
                           dark ? 0.34f : 0.44f};
    return {dark ? 0.78f : 0.72f,
            dark ? 0.58f : 0.52f};
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
