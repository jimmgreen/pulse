#pragma once
// snack_style.h - Multi-window M7i: the look the floating capsule
// notifications and the dragged-tab pill and hint share (toast redesign,
// scheme A): an in-window acrylic plate in the window's own theme, with a
// soft flyout shadow and a hairline edge.
#include <d2d1_1.h>

#include "FluentTokens.h"

namespace pulse::ui {
struct SnackPalette {
    bool dark;
    D2D1_COLOR_F plate;           // solid fallback, and the background text is drawn for
    D2D1_COLOR_F veil;            // laid over the acrylic so busy backgrounds stay calm
    D2D1_COLOR_F text;
    D2D1_COLOR_F text_secondary;
    D2D1_COLOR_F accent;          // the accent, made readable on the plate
    D2D1_COLOR_F accent_wash;     // behind the first button
    D2D1_COLOR_F divider;
    D2D1_COLOR_F hover;
    D2D1_COLOR_F stroke;          // the hairline edge
    D2D1_COLOR_F shadow_key;      // the wide, offset shadow
    D2D1_COLOR_F shadow_ambient;  // the tight one around the edge
    D2D1_COLOR_F progress;        // the time line: the theme's accent as it is
};

inline float SnackLuma(const D2D1_COLOR_F& c) noexcept { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

inline D2D1_COLOR_F SnackMix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) noexcept {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.0f);
}

inline D2D1_COLOR_F SnackAlpha(D2D1_COLOR_F c, float a) noexcept {
    c.a = a;
    return c;
}

// The plate takes the window's theme (light on light, dark on dark); high
// contrast keeps the system's flyout colors and draws no material.
inline SnackPalette MakeSnackPalette(const Theme& theme, bool high_contrast) noexcept {
    const bool dark = SnackLuma(theme.bg) < 0.5f;
    const D2D1_COLOR_F clear = D2D1::ColorF(0, 0.0f);
    if (high_contrast)
        return {dark,          theme.surface_flyout, clear,             theme.text,
                theme.text_secondary, theme.accent,  clear,             theme.stroke_card,
                theme.fill_hover,     theme.text,    clear,             clear,
                theme.accent};
    D2D1_COLOR_F accent = theme.accent;
    accent.a = 1.0f;
    const D2D1_COLOR_F progress = accent;
    // Mix the accent toward the text tone until it reads on the plate.
    const D2D1_COLOR_F toward = dark ? D2D1::ColorF(0xFFFFFF) : D2D1::ColorF(0x000000);
    for (int step = 0; step < 8; ++step) {
        const float luma = SnackLuma(accent);
        if (dark ? luma >= 0.58f : luma <= 0.42f) break;
        accent = SnackMix(accent, toward, 0.16f);
    }
    if (!dark)
        return {false,
                D2D1::ColorF(0xF9F9F9),
                D2D1::ColorF(0xFFFFFF, 0.32f),
                theme.text,
                theme.text_secondary,
                accent,
                SnackAlpha(accent, 0.10f),
                D2D1::ColorF(0x000000, 0.10f),
                D2D1::ColorF(0x000000, 0.05f),
                D2D1::ColorF(0x000000, 0.08f),
                D2D1::ColorF(0x000000, 0.16f),
                D2D1::ColorF(0x000000, 0.10f),
                progress};
    return {true,
            D2D1::ColorF(0x2C2C2C),
            D2D1::ColorF(0x2C2C2C, 0.28f),
            theme.text,
            theme.text_secondary,
            accent,
            SnackAlpha(accent, 0.16f),
            D2D1::ColorF(0xFFFFFF, 0.12f),
            D2D1::ColorF(0xFFFFFF, 0.07f),
            D2D1::ColorF(0xFFFFFF, 0.10f),
            D2D1::ColorF(0x000000, 0.36f),
            D2D1::ColorF(0x000000, 0.22f),
            progress};
}

// Paints the plate: the shadow, then acrylic sampled from what the frame
// already holds under `rect` (a solid plate when that cannot be sampled),
// then the hairline edge. Draw it before the content. `opacity` fades the
// whole plate (a capsule coming or going); the backdrop is sampled before the
// fade layer opens, since a target cannot be read with a layer pushed.
void DrawSnackPlate(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, float radius, const SnackPalette& palette,
                    float scale, bool high_contrast, float opacity = 1.0f);

// Fluent flyout elevation for an in-window surface: a real Gaussian shadow
// (CLSID_D2D1Shadow) of the rounded `rect`, `drop` lower; call it once for the
// wide key shadow and once for the tight ambient one, before the surface.
// CSS `0 y b color` = DrawElevationShadow(..., b / 2, y, color). Not with a
// layer or clip pushed: recording the silhouette retargets the context.
void DrawElevationShadow(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, float radius, float deviation, float drop,
                         const D2D1_COLOR_F& color, float opacity = 1.0f);
}  // namespace pulse::ui
