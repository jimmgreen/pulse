// liquid_glass_params.h - Pure C++20 optical parameter mapping, coordinate transforms,
// hover light spring/easing state, and WCAG text-contrast model for Liquid Glass surfaces.
#pragma once
#include "glass_options.h"

#include "host/render/text_contrast.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace pulse::desktop::render {

// 192-byte (12 x float4) constant buffer matching `cbuffer Constants : register(b0)` in liquid_glass.hlsl.
struct alignas(16) LiquidGlassConstants {
    // [0] Card bounds in target pixel space: (x, y, width, height)
    float rect_x = 0.0f;
    float rect_y = 0.0f;
    float rect_w = 0.0f;
    float rect_h = 0.0f;

    // [1] Shape & Snell meniscus optics (target pixels)
    float radius = 14.0f;
    float bezel = 28.0f;
    float ref_ior = 1.52f;
    float lens_scale = 79.8f;

    // [2] Dispersion gain, frost [0..1], primary glare angle (radians), refraction pad hint (px)
    float dispersion_gain = 9.0f;
    float frost = 0.04f;
    float glare_angle_rad = -2.3561945f; // -135 deg in D2D space -> top-left primary highlight
    float refraction_pad = 96.0f;

    // [3] Glare factor, Fresnel factor, interior passthrough flag, inner feather width (px)
    float glare_factor = 0.92f;
    float fresnel_factor = 0.34f;
    float clear_interior = 0.0f;
    float feather = 5.0f;

    // [4] Glare falloff range (px), Fresnel range (px), dark mode flag, smooth-min radius (px)
    float glare_range = 12.0f;
    float fresnel_range = 8.0f;
    float dark_mode = 1.0f;
    float smooth_k = 0.0f;

    // [5] Active neighbor count (0..4), opposite glare strength, tint R & G
    float neighbor_count = 0.0f;
    float opposite_factor = 0.80f;
    float tint_r = 0.10f;
    float tint_g = 0.12f;

    // [6] Tint B, tint opacity, overall card alpha, outer rim hairline alpha
    float tint_b = 0.16f;
    float tint_opacity = 0.06f;
    float overall_alpha = 1.0f;
    float rim_alpha = 0.34f;

    // [7] Corner radii (px) for up to 4 fusion neighbors
    float neighbor_radii[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    // [8..11] Neighbor bounds in target pixel space: (center_x, center_y, half_w, half_h)
    float neighbor_rects[4][4] = {};
    // [12] Studio mode, source viewport height in px, DPI scale, blur radius.
    float reference_mode = 0, reference_height = 1080, reference_dpi = 1, blur_radius = 1;
    // [13] Fresnel hardness, glare hardness, convergence, blur-edge.
    float fresnel_hardness = 0.2f, glare_hardness = 0.2f, convergence = 0.5f, blur_edge = 1;
};
static_assert(sizeof(LiquidGlassConstants) == 14 * 16, "LiquidGlassConstants must be 224 bytes (14 float4s)");

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
inline GlassIntRect BoundedGlassInputRect(const GlassIntRect& output, const GlassIntRect& background, int32_t pad) noexcept {
    pad=std::clamp(pad,0,kGlassMaxInputHalo);
    const auto bound=[](int64_t value,int32_t low,int32_t high) { return static_cast<int32_t>(std::clamp<int64_t>(value,low,high)); };
    return {bound(int64_t(output.left)-pad,background.left,background.right),
        bound(int64_t(output.top)-pad,background.top,background.bottom),
        bound(int64_t(output.right)+pad,background.left,background.right),
        bound(int64_t(output.bottom)+pad,background.top,background.bottom)};
}
inline bool GlassInputWithinBudget(const GlassIntRect& input) noexcept {
    return !input.Empty() && input.Width()<=8192 && input.Height()<=8192 &&
        static_cast<uint64_t>(input.Width())*input.Height()<=4096ull*4096ull;
}

struct GlassMatrix3x2 {
    float m11 = 1.0f, m12 = 0.0f;
    float m21 = 0.0f, m22 = 1.0f;
    float dx = 0.0f, dy = 0.0f;
};

struct GlassNeighborDip {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float radius_dip = 12.0f;
};

struct GlassCardRequest {
    float left_dip = 0.0f;
    float top_dip = 0.0f;
    float right_dip = 0.0f;
    float bottom_dip = 0.0f;
    float radius_dip = 14.0f;

    // Decoupled Liquid Glass Studio optical controls (defaults = ClearDroplet preset)
    GlassOpticalSettings glass_options{};
    bool reference_mode = false;
    uint8_t blur_radius = 1;
    bool blur_edge = true;
    float thickness_dip = 28.0f;      // 6..64 DIP meniscus bezel thickness
    float ior = 1.52f;                // 1.05..2.40 index of refraction
    float dispersion_gain = 9.0f;     // 0..35 chromatic separation gain
    uint8_t glare_percent = 92;       // 0..120 dual-lobe specular intensity
    float glare_angle_deg = -45.0f;   // -180..180 primary highlight azimuth (-45 = top-left)
    uint8_t frost_percent = 4;        // 0..100 internal backdrop blur mix (decoupled from opacity)
    uint8_t opacity_percent = 60;     // 0..100 global card fade (<30 fades out cleanly)

    bool dark_mode = true;
    bool elevated_hover = false;
    bool allow_clear_passthrough = false; // true on sparse DComp surface when interior reveals live desktop
    bool live_source = false;             // true when refracting live dynamic wallpaper frames
    bool energy_saver = false;            // battery saver: forces frost=1, dispersion=0
    bool draw_rim_in_shader = true;       // false when emphasized accent border is drawn on top
    bool cursor_light_active = false;     // true when cursor is hovering this card for angular pull

    float background_luminance = -1.0f;   // 0..1 local relative luminance, or <0 when unknown
    float background_opacity = 1.0f;      // scene-level multiplier (e.g. Focus scene 0.40)
    float press_amount = 0.0f;            // 0=rest, >0=pressed sink, <0=spring rebound
    float light_dir_x = 0.70710678f;      // normalized light travel vector (default from top-left)
    float light_dir_y = 0.70710678f;

    std::span<const GlassNeighborDip> neighbors{};
};

struct GlassDrawPlan {
    LiquidGlassConstants constants{};
    GlassIntRect output_rect_px{};
    int32_t refraction_pad_px = 0;
    bool visible = false;
};

inline float GlassFrostFromPercent(uint8_t frost_percent, bool energy_saver) noexcept {
    if (energy_saver) return 1.0f;
    return std::clamp(static_cast<float>(frost_percent) / 100.0f, 0.0f, 1.0f);
}

struct GlassTintRecipe {
    text_contrast::Srgb rgb{};
    float opacity = 0.0f;
};

// Computes subtle crystal body tinting:
// Keeps Clear/Crystal glass translucent and luminous rather than painting muddy fog,
// gently deepening only as frost_percent / opacity increases to protect readability.
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
    // Clear crystal keeps tint gossamer-thin (0.02..0.10); frosted gel rises smoothly up to ~0.34.
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

inline GlassDrawPlan MakeGlassDrawPlan(const GlassCardRequest& req,
                                       float dip_scale,
                                       float atlas_shift_x = 0.0f,
                                       float atlas_shift_y = 0.0f) noexcept {
    GlassDrawPlan plan{};
    const float s = (std::isfinite(dip_scale) && dip_scale > 0.05f) ? dip_scale : 1.0f;
    const float w_dip = req.right_dip - req.left_dip;
    const float h_dip = req.bottom_dip - req.top_dip;
    if (!std::isfinite(w_dip) || !std::isfinite(h_dip) || w_dip <= 0.5f || h_dip <= 0.5f) {
        return plan;
    }

    const float fade = std::clamp(static_cast<float>(req.opacity_percent) / 30.0f, 0.0f, 1.0f);
    const float overall_alpha = std::clamp(fade * req.background_opacity, 0.0f, 1.0f);
    if (overall_alpha <= 1e-4f) {
        return plan;
    }

    auto& cb = plan.constants;
    cb.rect_x = req.left_dip * s + atlas_shift_x;
    cb.rect_y = req.top_dip * s + atlas_shift_y;
    cb.rect_w = w_dip * s;
    cb.rect_h = h_dip * s;

    const float min_dim_dip = std::min(w_dip, h_dip);
    const float max_rad_dip = std::max(0.5f, min_dim_dip * 0.5f);
    const float rad_dip = std::clamp(req.radius_dip, 0.5f, max_rad_dip);
    cb.radius = rad_dip * s;

    // Bezel width respects user's `thickness_dip` (6..64), safely clamped on narrow/collapsed headers.
    const float max_bezel_dip = std::max(2.0f, min_dim_dip * 0.46f - 0.5f);
    const float base_bezel_dip = std::min(std::clamp(req.thickness_dip, 6.0f, 64.0f), max_bezel_dip);
    const float press = std::clamp(std::isfinite(req.press_amount) ? req.press_amount : 0.0f, -0.35f, 1.0f);
    const float bezel_scale = std::clamp(1.0f + 0.16f * press, 0.82f, 1.25f);
    cb.bezel = std::clamp(base_bezel_dip * s * bezel_scale, 2.0f * s, std::max(2.0f * s, std::min(cb.rect_w, cb.rect_h) * 0.48f));

    // Real Snell refraction with press-coupled IOR swell
    const float refr_scale = std::clamp(1.0f + 0.25f * press, 0.72f, 1.35f);
    cb.ref_ior = std::clamp(req.ior + 0.10f * press, 1.04f, 2.45f);
    cb.lens_scale = cb.bezel * 2.35f * refr_scale;

    cb.dispersion_gain = req.energy_saver ? 0.0f : std::clamp(req.dispersion_gain, 0.0f, 35.0f);
    cb.frost = GlassFrostFromPercent(req.frost_percent, req.energy_saver);

    // Dual-lobe specular glare azimuth: -45 deg in user UI = -135 deg in D2D (+Y down) -> top-left primary!
    constexpr float kDegToRad = 0.01745329252f;
    float base_rad = (std::clamp(req.glare_angle_deg, -180.0f, 180.0f) - 90.0f) * kDegToRad;
    if (req.cursor_light_active && std::isfinite(req.light_dir_x) && std::isfinite(req.light_dir_y)) {
        const float l_len = std::sqrt(req.light_dir_x * req.light_dir_x + req.light_dir_y * req.light_dir_y);
        if (l_len > 1e-4f) {
            const float cursor_rad = std::atan2(-req.light_dir_y, -req.light_dir_x);
            float delta = std::atan2(std::sin(cursor_rad - base_rad), std::cos(cursor_rad - base_rad));
            delta = std::clamp(delta, -0.56f, 0.56f); // +/- 32 deg cursor attraction around base highlight
            base_rad += delta;
        }
    }
    cb.glare_angle_rad = base_rad;

    const bool clear_mode = req.allow_clear_passthrough && (cb.frost <= 0.06f) && !req.energy_saver;
    cb.clear_interior = clear_mode ? 1.0f : 0.0f;
    cb.feather = (req.live_source ? 6.5f : 5.0f) * s;

    const float glare_base = std::clamp(static_cast<float>(req.glare_percent) / 100.0f, 0.0f, 1.25f);
    cb.glare_factor = glare_base * (req.elevated_hover ? 1.15f : 1.0f);
    cb.fresnel_factor = std::clamp(0.20f + 0.16f * glare_base, 0.08f, 0.52f) * (req.elevated_hover ? 1.15f : 1.0f);

    cb.glare_range = std::clamp(cb.bezel * 0.42f, 7.0f * s, 18.0f * s);
    cb.fresnel_range = std::clamp(cb.bezel * 0.28f, 5.5f * s, 14.0f * s);
    cb.dark_mode = req.dark_mode ? 1.0f : 0.0f;
    cb.opposite_factor = 0.80f;

    const auto tint = ComputeGlassTint(req.dark_mode, req.opacity_percent, req.frost_percent,
                                       req.background_luminance, clear_mode);
    cb.tint_r = tint.rgb.r;
    cb.tint_g = tint.rgb.g;
    cb.tint_b = tint.rgb.b;
    cb.tint_opacity = tint.opacity;
    cb.overall_alpha = overall_alpha;
    cb.rim_alpha = req.draw_rim_in_shader
                       ? (req.dark_mode ? (req.elevated_hover ? 0.42f : 0.30f)
                                        : (req.elevated_hover ? 0.38f : 0.28f))
                       : 0.0f;

    if (req.reference_mode) {
        auto optical=req.glass_options;
        for(const auto& field:kGlassOpticalFields) optical.*(field.member)=ClampGlassValue(field,optical.*(field.member));
        cb.reference_mode = 1; cb.reference_dpi = s;
        cb.blur_radius = static_cast<float>(optical.blur_radius);
        cb.blur_edge = optical.blur_edge ? 1.0f : 0.0f;
        cb.bezel = static_cast<float>(optical.thickness) * s;
        cb.ref_ior = static_cast<float>(optical.ior);
        cb.dispersion_gain = req.energy_saver ? 0 : static_cast<float>(optical.dispersion);
        cb.glare_angle_rad = static_cast<float>(optical.glare_angle) * kDegToRad;
        cb.glare_factor = static_cast<float>(optical.glare_factor / 100);
        cb.fresnel_factor = static_cast<float>(optical.fresnel_factor / 100);
        cb.glare_range = static_cast<float>(optical.glare_range);
        cb.fresnel_range = static_cast<float>(optical.fresnel_range);
        cb.fresnel_hardness = static_cast<float>(optical.fresnel_hardness / 100);
        cb.glare_hardness = static_cast<float>(optical.glare_hardness / 100);
        cb.convergence = static_cast<float>(optical.glare_convergence / 100);
        cb.opposite_factor = static_cast<float>(optical.glare_opposite / 100);
        cb.tint_r = static_cast<float>(optical.tint_r / 255);
        cb.tint_g = static_cast<float>(optical.tint_g / 255);
        cb.tint_b = static_cast<float>(optical.tint_b / 255);
        cb.tint_opacity = static_cast<float>(optical.tint_a / 100);
        cb.rim_alpha = 0; cb.clear_interior = 0; cb.frost = 1;
    }

    // Collect up to 4 nearby cards within smooth-min bridge reach (`2.25 * base_bezel_dip`).
    float out_l = cb.rect_x - 2.0f;
    float out_t = cb.rect_y - 2.0f;
    float out_r = cb.rect_x + cb.rect_w + 2.0f;
    float out_b = cb.rect_y + cb.rect_h + 2.0f;

    float union_l = out_l;
    float union_t = out_t;
    float union_r = out_r;
    float union_b = out_b;

    const float reach_dip = base_bezel_dip * 2.25f;
    uint32_t nb_count = 0;
    for (const auto& nb : req.neighbors) {
        if (nb_count >= 4) break;
        const float nw = nb.right - nb.left;
        const float nh = nb.bottom - nb.top;
        if (nw <= 0.5f || nh <= 0.5f) continue;

        const float gap_x = std::max({0.0f, req.left_dip - nb.right, nb.left - req.right_dip});
        const float gap_y = std::max({0.0f, req.top_dip - nb.bottom, nb.top - req.bottom_dip});
        const float gap_dist = std::sqrt(gap_x * gap_x + gap_y * gap_y);
        if (gap_dist > reach_dip) continue;

        const float n_x = nb.left * s + atlas_shift_x;
        const float n_y = nb.top * s + atlas_shift_y;
        const float n_w = nw * s;
        const float n_h = nh * s;
        cb.neighbor_rects[nb_count][0] = n_x + n_w * 0.5f;
        cb.neighbor_rects[nb_count][1] = n_y + n_h * 0.5f;
        cb.neighbor_rects[nb_count][2] = n_w * 0.5f;
        cb.neighbor_rects[nb_count][3] = n_h * 0.5f;
        cb.neighbor_radii[nb_count] = std::clamp(nb.radius_dip, 0.5f, std::min(nw, nh) * 0.5f) * s;
        ++nb_count;

        union_l = std::min(union_l, n_x - 2.0f);
        union_t = std::min(union_t, n_y - 2.0f);
        union_r = std::max(union_r, n_x + n_w + 2.0f);
        union_b = std::max(union_b, n_y + n_h + 2.0f);
    }

    cb.neighbor_count = static_cast<float>(nb_count);
    cb.smooth_k = (nb_count > 0) ? (cb.bezel * 1.45f) : 0.0f;
    if (nb_count > 0) {
        const float bridge_pad = cb.bezel * 1.20f + cb.smooth_k * 0.55f + 4.0f;
        out_l = std::max(union_l, out_l - bridge_pad);
        out_t = std::max(union_t, out_t - bridge_pad);
        out_r = std::min(union_r, out_r + bridge_pad);
        out_b = std::min(union_b, out_b + bridge_pad);
    }

    const float theta_i_max = std::asin(0.996f);
    const float theta_t_max = std::asin(0.996f / std::max(cb.ref_ior, 1.01f));
    const float max_snell = std::max(0.0f, -std::tan(theta_t_max - theta_i_max));
    const float disp_k = std::clamp(cb.dispersion_gain * 0.022f, 0.0f, 0.72f);
    plan.refraction_pad_px = static_cast<int32_t>(
        std::ceil(cb.lens_scale * max_snell * (1.0f + disp_k))) + 6;
    cb.refraction_pad = static_cast<float>(plan.refraction_pad_px);

    plan.output_rect_px = {
        static_cast<int32_t>(std::floor(out_l)),
        static_cast<int32_t>(std::floor(out_t)),
        static_cast<int32_t>(std::ceil(out_r)),
        static_cast<int32_t>(std::ceil(out_b)),
    };
    plan.visible = !plan.output_rect_px.Empty();
    return plan;
}

// Smooth exponential easing (`tau = 120 ms`) for cursor-driven specular light direction.
// Converges deterministically and sets `animating = false` once within 0.004 rad so idle stays 0-Present.
struct HoverLightTracker {
    float current_x = 0.70710678f;
    float current_y = 0.70710678f;
    float target_x = 0.70710678f;
    float target_y = 0.70710678f;
    bool animating = false;

    void SetDefaultTarget() noexcept {
        SetTarget(0.70710678f, 0.70710678f);
    }

    void SetCursorOnCard(float cursor_x, float cursor_y,
                         float card_left, float card_top, float card_right, float card_bottom) noexcept {
        const float cx = (card_left + card_right) * 0.5f;
        const float cy = (card_top + card_bottom) * 0.5f;
        // Light travels FROM cursor INTO card center (`-light_dir` points toward cursor).
        float dx = cx - cursor_x;
        float dy = cy - cursor_y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 6.0f) {
            SetDefaultTarget();
            return;
        }
        SetTarget(dx / len, dy / len);
    }

    void SetTarget(float lx, float ly) noexcept {
        const float len = std::sqrt(lx * lx + ly * ly);
        if (len <= 1e-4f) return;
        target_x = lx / len;
        target_y = ly / len;
        const float diff = std::hypot(target_x - current_x, target_y - current_y);
        if (diff > 0.005f) {
            animating = true;
        }
    }

    bool Step(float dt_ms) noexcept {
        if (!animating) return false;
        const float clamped_dt = std::clamp(std::isfinite(dt_ms) ? dt_ms : 16.0f, 1.0f, 200.0f);
        const float alpha = 1.0f - std::exp(-clamped_dt / 120.0f);
        current_x += (target_x - current_x) * alpha;
        current_y += (target_y - current_y) * alpha;
        const float len = std::hypot(current_x, current_y);
        if (len > 1e-4f) {
            current_x /= len;
            current_y /= len;
        } else {
            current_x = 0.70710678f;
            current_y = 0.70710678f;
        }
        if (std::hypot(target_x - current_x, target_y - current_y) <= 0.004f) {
            current_x = target_x;
            current_y = target_y;
            animating = false;
        }
        return true;
    }
};

} // namespace pulse::desktop::render
