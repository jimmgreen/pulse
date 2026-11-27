// tile_liquid_glass.h - macOS Tahoe-style Liquid Glass renderer for desktop tiles.
// Implements convex superellipse bezel Snell refraction, 3-channel RGB prism chromatic
// dispersion, crisp Gaussian frost + vibrancy saturation boost, tint + white veil +
// anti-banding dither noise, pixel-accurate SDF directional specular rim, interactive
// cursor-tracking HighlightSpring + DrawSpecularRim, and hollowed erf ambient+contact shadow.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <wrl/client.h>

#include "host/model/desktop_model.h"
#include "host/render/text_contrast.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::desktop::render {

struct GlassIntRect {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;

    constexpr bool Empty() const noexcept { return right <= left || bottom <= top; }
    constexpr int32_t Width() const noexcept { return right - left; }
    constexpr int32_t Height() const noexcept { return bottom - top; }
    constexpr bool operator==(const GlassIntRect&) const noexcept = default;
};

inline constexpr int32_t kGlassMaxInputHalo = 384;

inline GlassIntRect BoundedGlassInputRect(const GlassIntRect& output,
                                          const GlassIntRect& background,
                                          int32_t pad) noexcept {
    pad = std::clamp(pad, 0, kGlassMaxInputHalo);
    const auto bound = [](int64_t value, int32_t low, int32_t high) {
        return static_cast<int32_t>(std::clamp<int64_t>(value, low, high));
    };
    return {
        bound(int64_t(output.left) - pad, background.left, background.right),
        bound(int64_t(output.top) - pad, background.top, background.bottom),
        bound(int64_t(output.right) + pad, background.left, background.right),
        bound(int64_t(output.bottom) + pad, background.top, background.bottom),
    };
}

inline bool GlassInputWithinBudget(const GlassIntRect& input) noexcept {
    return !input.Empty() && input.Width() <= 8192 && input.Height() <= 8192 &&
           static_cast<uint64_t>(input.Width()) * input.Height() <= 4096ull * 4096ull;
}

struct GlassMatrix3x2 {
    float m11 = 1.0f, m12 = 0.0f;
    float m21 = 0.0f, m22 = 1.0f;
    float dx = 0.0f, dy = 0.0f;
};

// Maps a source wallpaper/capture bitmap `[0, src_w] x [0, src_h]` (96 DPI) onto target bitmap pixels.
inline GlassMatrix3x2 ComputeGlassSourceMatrix(uint32_t src_w, uint32_t src_h,
                                               uint32_t screen_w, uint32_t screen_h,
                                               float origin_x_px, float origin_y_px,
                                               float atlas_shift_x, float atlas_shift_y) noexcept {
    if (src_w == 0 || src_h == 0 || screen_w == 0 || screen_h == 0) {
        return {};
    }
    const float sx = static_cast<float>(screen_w) / static_cast<float>(src_w);
    const float sy = static_cast<float>(screen_h) / static_cast<float>(src_h);
    return GlassMatrix3x2{
        sx, 0.0f,
        0.0f, sy,
        atlas_shift_x - origin_x_px,
        atlas_shift_y - origin_y_px,
    };
}

struct GlassTintRecipe {
    text_contrast::Srgb rgb{};
    float opacity = 0.0f;
};

inline GlassTintRecipe ComputeGlassTint(bool dark_mode, uint8_t opacity_percent, uint8_t frost_percent,
                                        float bg_luminance, bool clear_mode) noexcept {
    const float pct = std::clamp(static_cast<float>(opacity_percent), 0.0f, 100.0f);
    if (pct <= 0.0f) {
        return {dark_mode ? text_contrast::Srgb{0.10f, 0.12f, 0.16f}
                          : text_contrast::Srgb{0.97f, 0.98f, 1.00f},
                0.0f};
    }
    const float frost_f = std::clamp(static_cast<float>(frost_percent) / 100.0f, 0.0f, 1.0f);
    const float op_f = std::clamp((pct - 25.0f) / 75.0f, 0.0f, 1.0f);
    const float body_strength = frost_f * (0.55f + 0.45f * op_f);

    text_contrast::Srgb tint = dark_mode ? text_contrast::Srgb{0.08f, 0.10f, 0.15f}
                                         : text_contrast::Srgb{0.97f, 0.98f, 1.00f};
    float op = clear_mode ? (0.02f + 0.06f * op_f)
                          : (dark_mode ? (0.04f + 0.06f * op_f + 0.24f * body_strength)
                                       : (0.03f + 0.05f * op_f + 0.22f * body_strength));

    if (bg_luminance >= 0.0f && std::isfinite(bg_luminance)) {
        const float lum = std::clamp(bg_luminance, 0.0f, 1.0f);
        const float mid_saddle = std::clamp(1.0f - std::fabs(lum - 0.21f) / 0.10f, 0.0f, 1.0f);
        if (mid_saddle > 0.0f) {
            if (lum < 0.21f) {
                tint = {0.04f, 0.05f, 0.08f};
                op = std::clamp(std::max(op, 0.24f * mid_saddle), 0.0f, 0.44f);
            } else {
                tint = {0.99f, 0.995f, 1.00f};
                op = std::clamp(std::max(op, 0.22f * mid_saddle), 0.0f, 0.48f);
            }
        }
    }
    return {tint, op};
}

// Used by DesktopRenderer::CardBackgroundSamples for foreground WCAG ink selection.
inline text_contrast::Srgb TransformLiquidGlassSample(text_contrast::Srgb wallpaper,
                                                      bool dark_mode,
                                                      uint8_t opacity_percent,
                                                      uint8_t frost_percent = 4) noexcept {
    const float fade = std::clamp(static_cast<float>(opacity_percent) / 30.0f, 0.0f, 1.0f);
    if (fade <= 0.0f) return wallpaper;
    const float lum_lin = text_contrast::RelativeLuminance(wallpaper);
    const float enc_lum = 0.2126f * wallpaper.r + 0.7152f * wallpaper.g + 0.0722f * wallpaper.b;
    const text_contrast::Srgb vibrant{
        text_contrast::Unit(enc_lum + (wallpaper.r - enc_lum) * 1.12f),
        text_contrast::Unit(enc_lum + (wallpaper.g - enc_lum) * 1.12f),
        text_contrast::Unit(enc_lum + (wallpaper.b - enc_lum) * 1.12f),
    };
    const bool clear_mode = (frost_percent <= 6);
    const auto recipe = ComputeGlassTint(dark_mode, opacity_percent, frost_percent, lum_lin, clear_mode);
    const auto tinted = text_contrast::SourceOver(vibrant, recipe.rgb, recipe.opacity);
    return text_contrast::SourceOver(wallpaper, tinted, fade);
}

// Optical & visual parameter set aligned to macOS Tahoe Liquid Glass.
struct TileGlassConfig {
    float corner_radius_dip = 16.0f;

    // Convex superellipse bezel h(x) = (1 - (1 - x)^4)^(1/4) + Snell refraction
    float index_of_refraction = 1.50f;
    float bezel_slope = 2.50f;
    float refraction_strength = 0.74f;
    float bezel_ratio = 0.25f;
    float bezel_min_dip = 6.0f;
    float bezel_max_dip = 26.0f;
    float max_displacement_dip = 64.0f;

    // 3-channel RGB prism chromatic dispersion along curved bezel
    bool enable_chromatic_aberration = true;
    float chromatic_spread = 0.13f;

    // Subtle Gaussian frost + Apple Vibrancy saturation boost
    float frost_amount = 0.08f;
    float min_blur_dip = 0.0f;
    float vibrancy = 1.92f;

    // Body tint, interior white veil, anti-banding monochrome dither noise
    D2D1_COLOR_F tint{1.0f, 1.0f, 1.0f, 0.09f};
    float white_veil_alpha = 0.04f;
    float dither_noise_alpha = 0.025f;

    // Pixel-accurate SDF directional specular rim + interactive cursor specular rim
    bool enable_surface_rim = true;
    bool dark_appearance = true;
    float rim_intensity = 0.95f;

    // Hollowed ambient + tight contact drop shadow
    bool enable_drop_shadow = true;
    float shadow_blur_dip = 20.0f;
    float shadow_offset_y_dip = 14.0f;
    float shadow_opacity = 0.45f;
};

// Critically damped spring for smooth cursor-tracking specular highlights (response 0.30s).
struct TileHighlightSpring {
    float x = -1.0f;
    float y = -1.0f;
    float vx = 0.0f;
    float vy = 0.0f;

    // Advances toward normalized target (tx, ty) in [0, 1]; negative resets to default -45 deg light.
    bool Step(float tx, float ty, float dt_sec, float response_sec = 0.30f) noexcept {
        if (tx < 0.0f || ty < 0.0f) {
            const bool had = (x >= 0.0f || y >= 0.0f);
            x = y = -1.0f;
            vx = vy = 0.0f;
            return had;
        }
        if (x < 0.0f || y < 0.0f) {
            x = tx;
            y = ty;
            vx = vy = 0.0f;
            return true;
        }
        const float omega = 2.0f * 3.14159265f / std::max(0.05f, response_sec);
        const float clamped_dt = std::clamp(dt_sec, 0.0f, 0.05f);
        constexpr int kSubsteps = 4;
        const float h = clamped_dt / static_cast<float>(kSubsteps);
        for (int i = 0; i < kSubsteps; ++i) {
            const float ax = omega * omega * (tx - x) - 2.0f * omega * vx;
            const float ay = omega * omega * (ty - y) - 2.0f * omega * vy;
            vx += ax * h;
            vy += ay * h;
            x += vx * h;
            y += vy * h;
        }
        const bool moving = std::fabs(tx - x) > 0.001f || std::fabs(ty - y) > 0.001f ||
                            std::fabs(vx) > 0.01f || std::fabs(vy) > 0.01f;
        if (!moving) {
            x = tx;
            y = ty;
            vx = vy = 0.0f;
        }
        return moving;
    }

    void Reset() noexcept {
        x = y = -1.0f;
        vx = vy = 0.0f;
    }
};

// Maps Pulse's BeautifySettings + theme/hover/power state into reference-aligned TileGlassConfig.
TileGlassConfig MakeTileGlassConfig(const model::BeautifySettings& settings,
                                    float corner_radius_dip,
                                    bool elevated_hover = false,
                                    bool energy_saver = false) noexcept;

// Evaluates normalized convex superellipse bezel Snell refraction profile in [0, 1].
// t = 0 at outer border (max bend), t = 1 at inner bezel edge (flat glass, 0 bend).
float EvaluateTileSnellProfile(float t, float ior, float slope) noexcept;

// Computes rounded-rectangle signed distance (negative inside) and outward unit normal.
void TileRoundedRectSdfAndNormal(float px, float py, float hx, float hy, float r,
                                 float& out_sdf, float& out_nx, float& out_ny) noexcept;

// Computes background sampling padding margin in physical pixels.
int ComputeTileGlassPaddingMargin(const TileGlassConfig& cfg, int w_px, int h_px, float dpi_scale) noexcept;

// Draws dual-ring directional specular rim with optional cursor-tracking light origin (lx, ly in [0,1]).
void DrawTileSpecularRim(ID2D1RenderTarget* rt,
                         const D2D1_RECT_F& rect_dip,
                         float corner_radius_dip,
                         float stroke_width_dip,
                         bool dark_appearance,
                         float strength = 1.0f,
                         bool clear_style = true,
                         float specular_intensity = 0.85f,
                         float light_norm_x = -1.0f,
                         float light_norm_y = -1.0f);

// Stateful GPU renderer for desktop tile Liquid Glass.
// Executes exact macOS Tahoe optical pipeline via a single-pass custom HLSL D2D1 effect
// (zero intermediate blur/displacement/composite surfaces, < 250 KB persistent atlas).
class TileLiquidGlassRenderer {
public:
    TileLiquidGlassRenderer() = default;
    ~TileLiquidGlassRenderer() = default;
    TileLiquidGlassRenderer(const TileLiquidGlassRenderer&) = delete;
    TileLiquidGlassRenderer& operator=(const TileLiquidGlassRenderer&) = delete;

    void BeginFrame() noexcept;
    void EndFrame() noexcept;
    void Reset() noexcept;
    void TrimCaches() noexcept;

    size_t EffectSlotCount() const noexcept { return slots_.size(); }
    bool Unavailable() const noexcept { return unavailable_; }

    // Draws hollowed ambient+contact drop shadow + single-pass Tahoe Liquid Glass + specular rim.
    // `rect_dip` is in scene DIPs (matching `dc`'s transform). `source_bmp` covers `(logical_w x logical_h)`
    // monitor physical pixels; `origin_px_x/y` maps surface pixel (0,0) into monitor physical pixels.
    bool DrawTile(ID2D1Factory1* factory,
                  ID2D1DeviceContext* dc,
                  ID2D1Bitmap1* source_bmp,
                  uint32_t src_w, uint32_t src_h,
                  uint32_t logical_w, uint32_t logical_h,
                  float origin_px_x, float origin_px_y,
                  float dip_scale,
                  const D2D1_RECT_F& rect_dip,
                  const TileGlassConfig& cfg,
                  float overall_opacity = 1.0f,
                  bool draw_rim = true,
                  float light_norm_x = -1.0f,
                  float light_norm_y = -1.0f,
                  ID2D1Bitmap1* frost_bmp = nullptr,
                  uint32_t frost_w = 0,
                  uint32_t frost_h = 0);

private:
    struct EffectSlot {
        Microsoft::WRL::ComPtr<ID2D1Effect> tile_glass;
    };

    void BindContext(ID2D1DeviceContext* dc) noexcept;
    EffectSlot* AcquireSlot(ID2D1Factory1* factory, ID2D1DeviceContext* dc) noexcept;
    // The hollowed shadow of a w x h DIP card (rounded-rect interior baked to alpha=0, exterior pads included).
    ID2D1Bitmap1* EnsureAmbientShadow(ID2D1DeviceContext* dc, int w_dip, int h_dip,
                                      float radius_dip, float blur_dip, float offset_y_dip);
    void DrawHollowedAmbientShadow(ID2D1DeviceContext* dc,
                                   const D2D1_RECT_F& rect_dip,
                                   float radius_dip,
                                   float opacity,
                                   float blur_dip,
                                   float offset_y_dip);

    ID2D1DeviceContext* bound_dc_ = nullptr;
    bool unavailable_ = false;
    std::vector<EffectSlot> slots_;
    size_t next_slot_ = 0;

    struct ShadowEntry {
        int w = 0, h = 0;
        int radius_q = 0;              // half DIPs
        int blur_q = 0, offset_q = 0;  // quarter DIPs
        uint64_t last_use = 0;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
    };
    std::vector<ShadowEntry> shadow_cache_;
    uint64_t shadow_clock_ = 0;
};

} // namespace pulse::desktop::render
