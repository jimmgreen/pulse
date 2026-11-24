// liquid_glass_optics.hlsli
// Physical optical evaluation core shared by D2D HLSL pixel shader (fxc ps_4_0)
// and deterministic C++20 optical/pixel unit tests.
//
// Third-party algorithm attributions:
// - Rounded-rectangle SDF & chromatic index offsets adapted from
//   Kyant0/AndroidLiquidGlass (Copyright 2025 Kyant, Apache License 2.0).
// - Superellipse squircle SDF, Snell's law meniscus lensing, pow-5 Fresnel rim,
//   dual-lobe angular caustic glare & smooth-min fusion adapted from
//   iyinchao/liquid-glass-studio (MIT License).

#ifndef PULSE_DESKTOP_LIQUID_GLASS_OPTICS_HLSLI
#define PULSE_DESKTOP_LIQUID_GLASS_OPTICS_HLSLI

#ifdef __cplusplus
#include <algorithm>
#include <cmath>

namespace pulse::desktop::render::lg_optics {

struct float2 {
    float x = 0.0f, y = 0.0f;
    constexpr float2() = default;
    constexpr float2(float v) : x(v), y(v) {}
    constexpr float2(float x_, float y_) : x(x_), y(y_) {}
};
struct float3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    constexpr float3() = default;
    constexpr float3(float v) : x(v), y(v), z(v) {}
    constexpr float3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};
struct float4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    constexpr float4() = default;
    constexpr float4(float v) : x(v), y(v), z(v), w(v) {}
    constexpr float4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr float4(float3 rgb, float a) : x(rgb.x), y(rgb.y), z(rgb.z), w(a) {}
    constexpr float3 rgb() const { return {x, y, z}; }
};

inline constexpr float2 operator+(float2 a, float2 b) { return {a.x + b.x, a.y + b.y}; }
inline constexpr float2 operator-(float2 a, float2 b) { return {a.x - b.x, a.y - b.y}; }
inline constexpr float2 operator-(float2 a) { return {-a.x, -a.y}; }
inline constexpr float2 operator*(float2 a, float2 b) { return {a.x * b.x, a.y * b.y}; }
inline constexpr float2 operator*(float2 a, float s) { return {a.x * s, a.y * s}; }
inline constexpr float2 operator*(float s, float2 a) { return {a.x * s, a.y * s}; }
inline constexpr float2 operator/(float2 a, float s) { return {a.x / s, a.y / s}; }

inline constexpr float3 operator+(float3 a, float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline constexpr float3 operator-(float3 a, float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline constexpr float3 operator*(float3 a, float3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline constexpr float3 operator*(float3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline constexpr float3 operator*(float s, float3 a) { return {a.x * s, a.y * s, a.z * s}; }

inline constexpr float4 operator+(float4 a, float4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline constexpr float4 operator-(float4 a, float4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
inline constexpr float4 operator*(float4 a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }

inline float lg_abs(float v) { return std::fabs(v); }
inline float2 lg_abs2(float2 v) { return {std::fabs(v.x), std::fabs(v.y)}; }
inline float lg_min(float a, float b) { return a < b ? a : b; }
inline float lg_max(float a, float b) { return a > b ? a : b; }
inline float2 lg_max2(float2 a, float b) { return {lg_max(a.x, b), lg_max(a.y, b)}; }
inline float lg_clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lg_saturate(float v) { return lg_clamp(v, 0.0f, 1.0f); }
inline float3 lg_saturate3(float3 v) { return {lg_saturate(v.x), lg_saturate(v.y), lg_saturate(v.z)}; }
inline float lg_sqrt(float v) { return std::sqrt(lg_max(v, 0.0f)); }
inline float lg_pow(float b, float e) { return std::pow(lg_max(b, 0.0f), e); }
inline float lg_exp(float v) { return std::exp(v); }
inline float lg_cos(float v) { return std::cos(v); }
inline float lg_sin(float v) { return std::sin(v); }
inline float lg_asin(float v) { return std::asin(lg_clamp(v, -0.9992f, 0.9992f)); }
inline float lg_tan(float v) { return std::tan(lg_clamp(v, -1.56f, 1.56f)); }
inline float lg_dot2(float2 a, float2 b) { return a.x * b.x + a.y * b.y; }
inline float lg_dot3(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float lg_len2(float2 v) { return std::sqrt(lg_dot2(v, v)); }
inline float2 lg_norm2(float2 v) {
    const float l = lg_len2(v);
    return l > 1e-6f ? (v / l) : float2(0.0f, -1.0f);
}
inline float2 lg_sign2(float2 v) {
    return {v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f};
}
inline float lg_lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float3 lg_lerp3(float3 a, float3 b, float t) { return a + (b - a) * t; }
inline float4 lg_lerp4(float4 a, float4 b, float t) { return a + (b - a) * t; }

#define LG_OUT(T) T&
#define LG_RGB(v) (v).rgb()
#define LG_UNROLL
#define LG_LOOP
#define LG_BRANCH
#define LG_SAMPLE(off, fr) sample_fn((off), (fr))
#else
#define lg_abs(v) abs(v)
#define lg_abs2(v) abs(v)
#define lg_min(a, b) min(a, b)
#define lg_max(a, b) max(a, b)
#define lg_max2(a, b) max(a, float2(b, b))
#define lg_clamp(v, lo, hi) clamp(v, lo, hi)
#define lg_saturate(v) saturate(v)
#define lg_saturate3(v) saturate(v)
#define lg_sqrt(v) sqrt(max(v, 0.0))
#define lg_pow(b, e) pow(max(b, 1e-6), e)
#define lg_exp(v) exp(v)
#define lg_cos(v) cos(v)
#define lg_sin(v) sin(v)
#define lg_asin(v) asin(clamp(v, -0.9992, 0.9992))
#define lg_tan(v) tan(clamp(v, -1.56, 1.56))
#define lg_dot2(a, b) dot(a, b)
#define lg_dot3(a, b) dot(a, b)
#define lg_len2(v) length(v)
#define lg_norm2(v) normalize(v)
#define lg_sign2(v) float2((v).x >= 0.0 ? 1.0 : -1.0, (v).y >= 0.0 ? 1.0 : -1.0)
#define lg_lerp(a, b, t) lerp(a, b, t)
#define lg_lerp3(a, b, t) lerp(a, b, t)
#define lg_lerp4(a, b, t) lerp(a, b, t)
#define LG_OUT(T) out T
#define LG_RGB(v) (v).rgb
#define LG_UNROLL [unroll]
#define LG_LOOP [loop]
#define LG_BRANCH [branch]
#define LG_SAMPLE(off, fr) LgSampleBackdrop((off), (fr))
#endif

// Exact C2-continuous Squircle (n=4 Superellipse) signed distance for Apple-style Liquid Glass cards.
inline float LgSdSuperRoundRect(float2 p, float2 half_size, float radius) {
    float max_r = lg_min(half_size.x, half_size.y);
    float r = lg_clamp(radius, 0.5f, lg_max(max_r, 0.5f));
    float2 q = lg_abs2(p) - (half_size - float2(r, r));
    float res = 0.0f;
    if (q.x > 0.0f && q.y > 0.0f) {
        float2 cn = q / r;
        float cx2 = cn.x * cn.x;
        float cy2 = cn.y * cn.y;
        float c_norm = lg_sqrt(lg_sqrt(cx2 * cx2 + cy2 * cy2));
        res = (c_norm - 1.0f) * r;
    } else {
        float2 qp = lg_max2(q, 0.0f);
        res = (qp.x + qp.y) + lg_min(lg_max(q.x, q.y), 0.0f) - r;
    }
    return res;
}

// Legacy alias kept for compatibility with existing optical tests.
inline float LgSdRoundRect(float2 p, float2 half_size, float radius) {
    return LgSdSuperRoundRect(p, half_size, radius);
}

// C1-continuous quadratic smooth minimum for molten liquid droplet merging.
inline float LgSmoothMin(float a, float b, float k) {
    float res = lg_min(a, b);
    if (k > 1e-4f) {
        float h = lg_max(k - lg_abs(a - b), 0.0f) / k;
        res = res - h * h * k * 0.25f;
    }
    return res;
}

// Crease-free outward normal across the entire bezel band for a Squircle rounded rectangle.
// Expanding the gradient corner radius over `bezel` prevents 45-degree mitre discontinuities
// when `bezel > radius`, while blending toward squircle curvature near the perimeter.
inline float2 LgGradSuperRoundRectSmooth(float2 p, float2 half_size, float radius, float bezel) {
    float max_r = lg_max(lg_min(half_size.x, half_size.y), 1.0f);
    float r_eff = lg_clamp(lg_max(radius * 1.35f, bezel * 1.08f), 1.0f, max_r);
    float2 q = lg_abs2(p) - (half_size - float2(r_eff, r_eff));
    float2 s = lg_sign2(p);
    float2 out_n = float2(0.0f, -1.0f);
    if (q.x > 0.0f && q.y > 0.0f) {
        float2 cn = q / r_eff;
        float2 g_circ = cn;
        float2 g_sq = float2(cn.x * cn.x * cn.x, cn.y * cn.y * cn.y);
        // Squircle normal outside/along rim, circularly stabilized deep in small-radius corners:
        float radial = lg_clamp(lg_sqrt(cn.x * cn.x + cn.y * cn.y), 0.0f, 1.0f);
        float gx = lg_lerp(g_circ.x, g_sq.x, radial * radial);
        float gy = lg_lerp(g_circ.y, g_sq.y, radial * radial);
        out_n = lg_norm2(s * (float2(gx, gy) + float2(1e-5f, 1e-5f)));
    } else if (q.x > 0.0f || q.y > 0.0f) {
        out_n = lg_norm2(s * (lg_max2(q, 0.0f) + float2(1e-5f, 1e-5f)));
    } else {
        float m = lg_max(q.x, q.y);
        float wx = lg_exp(lg_clamp((q.x - m) * 0.65f, -20.0f, 0.0f));
        float wy = lg_exp(lg_clamp((q.y - m) * 0.65f, -20.0f, 0.0f));
        out_n = lg_norm2(s * float2(wx, wy));
    }
    return out_n;
}

inline float2 LgGradRoundRectSmooth(float2 p, float2 half_size, float radius, float bezel) {
    return LgGradSuperRoundRectSmooth(p, half_size, radius, bezel);
}

// Physical Snell's law meniscus bending profile matching `liquid-glass-studio`:
//   u = inner / bezel in [0, 1], x_ratio = 1 - u (0 at inner bezel edge -> 1 at outer rim).
//   sin(theta_i) = x_ratio^2 -> zero value and zero slope at inner bezel edge (C1 seamless!).
//   edge_factor = -tan(theta_t - theta_i) = tan(theta_i - theta_t) >= 0.
inline float LgSnellRefractionFactor(float inner_px, float bezel_px, float ref_ior) {
    float x_ratio = lg_saturate(1.0f - inner_px / lg_max(bezel_px, 1.0f));
    float sin_i = lg_clamp(x_ratio * x_ratio, 0.0f, 0.996f);
    float theta_i = lg_asin(sin_i);
    float ior = lg_max(ref_ior, 1.01f);
    float theta_t = lg_asin(lg_clamp(sin_i / ior, 0.0f, 0.996f));
    return lg_max(0.0f, -lg_tan(theta_t - theta_i));
}

// Evaluates unified SDF (single card or smooth-min fusion with up to 4 neighbors)
// and checks deterministic Voronoi pixel ownership so fused bridges never double-blend.
inline float LgEvalFusedSdf(
    float2 scene_pos,
    float2 self_center,
    float2 self_half,
    float self_radius,
    int neighbor_count,
    float smooth_k,
    float4 nb0, float4 nb1, float4 nb2, float4 nb3,
    float4 nb_radii,
    LG_OUT(float) out_sd_self,
    LG_OUT(bool) out_owned) {
    float sd_self = LgSdSuperRoundRect(scene_pos - self_center, self_half, self_radius);
    out_sd_self = sd_self;
    out_owned = true;
    float sd_fused = sd_self;
    LG_BRANCH
    if (neighbor_count > 0) {
        float4 nbs[4] = {nb0, nb1, nb2, nb3};
        float nr[4] = {nb_radii.x, nb_radii.y, nb_radii.z, nb_radii.w};

        LG_LOOP
        for (int i = 0; i < neighbor_count; ++i) {
            float2 nc = float2(nbs[i].x, nbs[i].y);
            float2 nh = float2(nbs[i].z, nbs[i].w);
            float sdi = LgSdSuperRoundRect(scene_pos - nc, nh, nr[i]);
            sd_fused = LgSmoothMin(sd_fused, sdi, smooth_k);
            bool tie_win = (self_center.x < nc.x) || (self_center.x == nc.x && self_center.y < nc.y);
            if (sdi < sd_self - 1e-4f || (lg_abs(sdi - sd_self) <= 1e-4f && !tie_win)) {
                out_owned = false;
            }
        }
    }
    return sd_fused;
}

inline float LgEvalFusedSdfOnly(
    float2 scene_pos,
    float2 self_center,
    float2 self_half,
    float self_radius,
    int neighbor_count,
    float smooth_k,
    float4 nb0, float4 nb1, float4 nb2, float4 nb3,
    float4 nb_radii) {
    float sd = LgSdSuperRoundRect(scene_pos - self_center, self_half, self_radius);
    float4 nbs[4] = {nb0, nb1, nb2, nb3};
    float nr[4] = {nb_radii.x, nb_radii.y, nb_radii.z, nb_radii.w};
    LG_LOOP
    for (int i = 0; i < neighbor_count; ++i) {
        float sdi = LgSdSuperRoundRect(scene_pos - float2(nbs[i].x, nbs[i].y), float2(nbs[i].z, nbs[i].w), nr[i]);
        sd = LgSmoothMin(sd, sdi, smooth_k);
    }
    return sd;
}

#ifdef __cplusplus
template <class SampleFn>
inline float4 LgShadePixel(
#else
inline float4 LgShadePixel(
#endif
    float2 scene_pos,
    float4 rect_xywh,         // [0] x, y, w, h (target pixels)
    float4 shape_bezel,       // [1] radius, bezel, ref_ior, lens_scale
    float4 disp_frost_angle,  // [2] dispersion_gain, frost, glare_angle_rad, refraction_pad
    float4 glare_fresnel_clr, // [3] glare_factor, fresnel_factor, clear_interior, feather
    float4 ranges_dark_k,     // [4] glare_range, fresnel_range, dark_mode, smooth_k
    float4 nb_opp_tint_rg,    // [5] neighbor_count, opposite_factor, tint_r, tint_g
    float4 tint_ba_alpha_rim, // [6] tint_b, tint_opacity, overall_alpha, rim_alpha
    float4 nb_radii,
    float4 nb0, float4 nb1, float4 nb2, float4 nb3
#ifdef __cplusplus
    , SampleFn&& sample_fn
#endif
) {
    float4 final_color = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float overall_alpha = lg_saturate(tint_ba_alpha_rim.z);
    if (overall_alpha > 1e-4f) {
        float2 half_sz = float2(rect_xywh.z * 0.5f, rect_xywh.w * 0.5f);
        float2 center = float2(rect_xywh.x, rect_xywh.y) + half_sz;
        float radius = shape_bezel.x;
        float bezel = lg_max(shape_bezel.y, 1.0f);
        float ref_ior = lg_max(shape_bezel.z, 1.01f);
        float lens_scale = shape_bezel.w;

        int nb_count = (int)(nb_opp_tint_rg.x + 0.5f);
        float smooth_k = ranges_dark_k.w;

        float sd_self = 0.0f;
        bool owned = true;
        float sd = LgEvalFusedSdf(scene_pos, center, half_sz, radius, nb_count, smooth_k,
                                  nb0, nb1, nb2, nb3, nb_radii, sd_self, owned);
        float cov = owned ? lg_saturate(0.5f - sd) : 0.0f;
        LG_BRANCH
        if (cov > 0.0f) {
            float disp_gain = disp_frost_angle.x;
            float frost = lg_saturate(disp_frost_angle.y);
            float glare_rad = disp_frost_angle.z;

            float glare_factor = glare_fresnel_clr.x;
            float fresnel_factor = glare_fresnel_clr.y;
            bool clear_interior = (glare_fresnel_clr.z > 0.5f);
            float feather = lg_max(glare_fresnel_clr.w, 1.0f);

            float glare_range = lg_max(ranges_dark_k.x, 1.0f);
            float fresnel_range = lg_max(ranges_dark_k.y, 1.0f);
            bool dark_mode = (ranges_dark_k.z > 0.5f);

            float opposite_factor = nb_opp_tint_rg.y;
            float3 tint_rgb = float3(nb_opp_tint_rg.z, nb_opp_tint_rg.w, tint_ba_alpha_rim.x);
            float tint_op = lg_saturate(tint_ba_alpha_rim.y);
            float rim_hairline = lg_saturate(tint_ba_alpha_rim.w);

            float inner = lg_max(-sd, 0.0f);
            LG_BRANCH
            if (inner >= bezel) {
                if (clear_interior) {
                    if (tint_op > 1e-4f) {
                        float a = tint_op * overall_alpha * cov;
                        final_color = float4(tint_rgb * a, a);
                    }
                } else {
                    float4 base_in = LG_SAMPLE(float2(0.0f, 0.0f), frost);
                    float3 col_in = LG_RGB(base_in);
                    // Vibrancy lift (liquid-glass crystal saturation boost)
                    float lum_in = lg_dot3(col_in, float3(0.2126f, 0.7152f, 0.0722f));
                    col_in = lg_lerp3(float3(lum_in, lum_in, lum_in), col_in, 1.12f);
                    col_in = lg_lerp3(col_in, tint_rgb, tint_op);
                    float a_in = cov * overall_alpha;
                    final_color = float4(lg_saturate3(col_in) * a_in, a_in);
                }
            } else {
                float2 c = scene_pos - center;
                float2 n;
                float grad_strength = 1.0f;
                LG_BRANCH
                if (nb_count <= 0) {
                    n = LgGradSuperRoundRectSmooth(c, half_sz, radius, bezel);
                } else {
                    float eps = 0.75f;
                    float sx1 = LgEvalFusedSdfOnly(scene_pos + float2(eps, 0.0f), center, half_sz, radius, nb_count, smooth_k, nb0, nb1, nb2, nb3, nb_radii);
                    float sx0 = LgEvalFusedSdfOnly(scene_pos - float2(eps, 0.0f), center, half_sz, radius, nb_count, smooth_k, nb0, nb1, nb2, nb3, nb_radii);
                    float sy1 = LgEvalFusedSdfOnly(scene_pos + float2(0.0f, eps), center, half_sz, radius, nb_count, smooth_k, nb0, nb1, nb2, nb3, nb_radii);
                    float sy0 = LgEvalFusedSdfOnly(scene_pos - float2(0.0f, eps), center, half_sz, radius, nb_count, smooth_k, nb0, nb1, nb2, nb3, nb_radii);
                    float2 raw_grad = float2(sx1 - sx0, sy1 - sy0) * (1.0f / (2.0f * eps));
                    float raw_len = lg_len2(raw_grad);
                    grad_strength = lg_saturate(raw_len);
                    n = raw_len > 1e-4f ? (raw_grad / lg_max(raw_len, 1.0f)) : float2(0.0f, 0.0f);
                }

                float u = lg_saturate(inner / bezel);
                float edge_factor = LgSnellRefractionFactor(inner, bezel, ref_ior) * grad_strength;
                float corner_pinch = 1.0f - 0.24f * (4.0f * n.x * n.x * n.y * n.y);
                float2 off = -n * (edge_factor * lens_scale * corner_pinch);

                // Even at higher interior frost settings, keep the curved meniscus edge clearer so
                // refracted details remain sharp along the rim.
                float rim_frost = frost * lg_lerp(0.32f, 1.0f, u);
                float disp_k = lg_clamp(disp_gain * 0.022f, 0.0f, 0.72f);

                float3 col;
                LG_BRANCH
                if (disp_k > 1e-3f && edge_factor > 1e-3f) {
                    // Tri-band chromatic dispersion matching liquid-glass-studio N_R=0.98 / N_G=1.0 / N_B=1.02
                    float2 off_r = off * (1.0f + disp_k);
                    float2 off_b = off * (1.0f - disp_k);
                    float3 sr = LG_RGB(LG_SAMPLE(off_r, rim_frost));
                    float3 sg = LG_RGB(LG_SAMPLE(off, rim_frost));
                    float3 sb = LG_RGB(LG_SAMPLE(off_b, rim_frost));
                    col = float3(
                        sr.x * 0.78f + sg.x * 0.22f,
                        sr.y * 0.14f + sg.y * 0.72f + sb.y * 0.14f,
                        sg.z * 0.22f + sb.z * 0.78f);
                } else {
                    col = LG_RGB(LG_SAMPLE(off, rim_frost));
                }

                // Crisp crystal vibrance boost along the lens edge
                float lum = lg_dot3(col, float3(0.2126f, 0.7152f, 0.0722f));
                float vib = 0.12f + 0.10f * (1.0f - u);
                col = lg_lerp3(float3(lum, lum, lum), col, 1.0f + vib);

                float local_tint = clear_interior ? (tint_op * lg_lerp(0.30f, 1.0f, u)) : tint_op;
                col = lg_lerp3(col, tint_rgb, local_tint);

                // Subtle volumetric meniscus curvature shading so edges remain visible even over pure white
                float inner_bell = 4.0f * u * (1.0f - u);
                float depth_darken = (dark_mode ? 0.055f : 0.042f) * inner_bell;
                col = col * (1.0f - depth_darken);

                // 1) Sharp pow(..., 5) Fresnel rim matching liquid-glass-studio STEP 9
                float fresnel_geo = lg_saturate(lg_pow(lg_max(0.0f, 1.0f + sd / fresnel_range + 0.18f), 5.0f));
                float fresnel_mix = lg_saturate(fresnel_geo * fresnel_factor * 0.78f * grad_strength);
                float3 fresnel_col = lg_lerp3(float3(0.95f, 0.97f, 1.0f), tint_rgb, tint_op * 0.25f);
                col = lg_lerp3(col, fresnel_col, fresnel_mix);

                // 2) Dual-lobe directional caustic glare matching liquid-glass-studio STEP 9
                float glare_geo = lg_saturate(lg_pow(lg_max(0.0f, 1.0f + sd / glare_range + 0.20f), 5.0f));
                float2 g_dir = float2(lg_cos(glare_rad), lg_sin(glare_rad));
                float ndg = lg_dot2(n, g_dir);
                float side_w = (ndg >= 0.0f) ? 1.20f : (1.20f * opposite_factor);
                float ang_raw = (ndg * ndg) * side_w * glare_factor;
                float glare_ang = lg_saturate(lg_pow(lg_max(ang_raw, 0.0f), 1.05f));
                float glare_mix = lg_saturate(glare_ang * glare_geo * grad_strength);

                // Preserve colorful caustic glow from refracted background colors (LCH lightness + chroma boost approx)
                float3 caustic = lg_saturate3(col * 0.38f + float3(0.88f, 0.91f, 0.95f));
                col = lg_lerp3(col, caustic, glare_mix);

                // 3) 1px crisp outer glass lip hairline
                if (rim_hairline > 0.0f) {
                    float rim_band = lg_saturate(1.0f - lg_abs(sd + 0.55f) / 0.65f);
                    float3 lip_col = lg_lerp3(
                        float3(0.75f, 0.80f, 0.88f),
                        float3(1.0f, 1.0f, 1.0f),
                        lg_saturate(0.35f + 0.65f * lg_saturate(ndg)));
                    float lip_op = rim_hairline * (0.45f + 0.55f * lg_saturate(ndg)) * rim_band;
                    col = lg_lerp3(col, lip_col, lg_saturate(lip_op));
                }

                float out_a = cov * overall_alpha;
                if (clear_interior) {
                    float bezel_alpha = lg_saturate((bezel - inner) / feather);
                    out_a *= lg_max(bezel_alpha, tint_op);
                }

                final_color = float4(lg_saturate3(col) * out_a, out_a);
            }
        }
    }
    return final_color;
}

#include "liquid_glass_studio.hlsli"

#ifdef __cplusplus
#undef LG_OUT
#undef LG_RGB
#undef LG_UNROLL
#undef LG_LOOP
#undef LG_BRANCH
#undef LG_SAMPLE
} // namespace pulse::desktop::render::lg_optics
#endif

#endif // PULSE_DESKTOP_LIQUID_GLASS_OPTICS_HLSLI
