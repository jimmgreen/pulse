// tile_liquid_glass.hlsl - Single-pass macOS Tahoe Liquid Glass pixel shader (ps_4_0) for desktop tiles.
// Samples native-resolution sharp & 1/4-res frosted inputs directly (ZERO D2D12DAffineTransform
// intermediate 4K upscale passes!), executing convex superellipse bezel Snell refraction,
// 3-channel RGB prism chromatic dispersion, crisp Gaussian frost + soft frosted blend, Apple Vibrancy
// saturation boost, tint + white veil + monochrome anti-banding dither, and pixel-accurate SDF
// directional + interactive cursor-tracking specular rim in one GPU pass.
//
// Input 0: Native sharp wallpaper / live capture bitmap [0..sharp_w, 0..sharp_h]
// Input 1: Optional 1/4-res frosted backdrop bitmap [0..frost_w, 0..frost_h]

#define D2D_INPUT_COUNT 2
#define D2D_INPUT0_COMPLEX
#define D2D_INPUT1_COMPLEX
#define D2D_REQUIRES_SCENE_POSITION
#include "d2d1effecthelpers.hlsli"

cbuffer Constants : register(b0) {
    float4 c_rect_xywh;       // [0] left_px, top_px, width_px, height_px (target physical pixels)
    float4 c_optics;          // [1] radius_px, bezel_px, ior, bezel_slope
    float4 c_refract;         // [2] dmax_px, inv_snell_max, chromatic_spread, dpi_scale
    float4 c_blur_vib;        // [3] sigma_px, frost_amount, vibrancy, overall_opacity
    float4 c_tint;            // [4] tint.r, tint.g, tint.b, tint.a
    float4 c_surface;         // [5] white_veil_alpha, dither_alpha, rim_gain, has_frost_tex
    float4 c_bounds_px;       // [6] valid_min_x, valid_min_y, valid_max_x, valid_max_y (target px)
    float4 c_src_scale;       // [7] sharp_scale_x, sharp_scale_y, frost_scale_x, frost_scale_y (src_px / logical_px)
    float4 c_src_max_px;      // [8] sharp_w - 0.5, sharp_h - 0.5, frost_w - 0.5, frost_h - 0.5
    float4 c_rim_light;       // [9] light_norm_x, light_norm_y, rim_stroke_strength, rim_flags (bit0=dark, bit1=clear)
};

float2 ClampScenePos(float2 pos, float2 off) {
    return clamp(pos + off, c_bounds_px.xy + 0.5, c_bounds_px.zw - 0.5);
}

float3 SampleSharp(float2 pos, float2 off, float4 uv0) {
    float2 scene_p = ClampScenePos(pos, off);
    float2 src_px = clamp((scene_p - c_bounds_px.xy) * c_src_scale.xy, float2(0.5, 0.5), c_src_max_px.xy);
    return InputTexture0.SampleLevel(InputSampler0, uv0.xy + (src_px - pos) * uv0.zw, 0.0).rgb;
}

float3 SampleFrost(float2 pos, float2 off, float4 uv1) {
    float2 scene_p = ClampScenePos(pos, off);
    float2 frost_px = clamp((scene_p - c_bounds_px.xy) * c_src_scale.zw, float2(0.5, 0.5), c_src_max_px.zw);
    return InputTexture1.SampleLevel(InputSampler1, uv1.xy + (frost_px - pos) * uv1.zw, 0.0).rgb;
}

float3 SampleFrosted9Tap(float2 pos, float2 off, float4 uv0, float4 uv1, float sigma, float frost_mix) {
    float3 sharp_col = float3(0.0, 0.0, 0.0);
    if (sigma <= 0.35) {
        sharp_col = SampleSharp(pos, off, uv0);
    } else {
        float step_px = clamp(sigma * 1.10, 0.6, 3.2);
        const float ws = 0.23;
        const float wc = 1.0 - 2.0 * ws;
        const float w00 = wc * wc;
        const float w10 = wc * ws;
        const float w11 = ws * ws;
        sharp_col =
            SampleSharp(pos, off, uv0) * w00 +
            (SampleSharp(pos, off + float2( step_px, 0.0), uv0) +
             SampleSharp(pos, off + float2(-step_px, 0.0), uv0) +
             SampleSharp(pos, off + float2(0.0,  step_px), uv0) +
             SampleSharp(pos, off + float2(0.0, -step_px), uv0)) * w10 +
            (SampleSharp(pos, off + float2( step_px,  step_px), uv0) +
             SampleSharp(pos, off + float2(-step_px,  step_px), uv0) +
             SampleSharp(pos, off + float2( step_px, -step_px), uv0) +
             SampleSharp(pos, off + float2(-step_px, -step_px), uv0)) * w11;
    }
    float3 result = sharp_col;
    if (frost_mix > 0.002 && c_surface.w > 0.5) {
        float3 soft_col = SampleFrost(pos, off, uv1);
        result = lerp(sharp_col, soft_col, frost_mix);
    }
    return result;
}

float3 SampleBezel5Tap(float2 pos, float2 off, float4 uv0, float4 uv1, float sigma, float frost_mix) {
    float3 sharp_col = float3(0.0, 0.0, 0.0);
    if (sigma <= 0.45) {
        sharp_col = SampleSharp(pos, off, uv0);
    } else {
        float step_px = clamp(sigma * 1.05, 0.6, 3.0);
        sharp_col =
            SampleSharp(pos, off, uv0) * 0.36 +
            (SampleSharp(pos, off + float2( step_px, 0.0), uv0) +
             SampleSharp(pos, off + float2(-step_px, 0.0), uv0) +
             SampleSharp(pos, off + float2(0.0,  step_px), uv0) +
             SampleSharp(pos, off + float2(0.0, -step_px), uv0)) * 0.16;
    }
    float3 result = sharp_col;
    if (frost_mix > 0.002 && c_surface.w > 0.5) {
        float3 soft_col = SampleFrost(pos, off, uv1);
        result = lerp(sharp_col, soft_col, frost_mix);
    }
    return result;
}

D2D_PS_ENTRY(main) {
    float2 pos = D2DGetScenePosition().xy;
    float2 half_sz = c_rect_xywh.zw * 0.5;
    float2 center = c_rect_xywh.xy + half_sz;
    float2 p = pos - center;
    float r = min(c_optics.x, min(half_sz.x, half_sz.y));

    // Exact rounded-rectangle SDF and outward unit normal.
    float2 q = abs(p) - (half_sz - r);
    float sdf;
    float2 n;
    if (q.x > 0.0 && q.y > 0.0) {
        float len = length(q);
        sdf = len - r;
        n = (len > 1e-6) ? (q / len) : float2(0.70710678, 0.70710678);
    } else if (q.x > q.y) {
        sdf = q.x - r;
        n = float2(1.0, 0.0);
    } else {
        sdf = q.y - r;
        n = float2(0.0, 1.0);
    }
    if (p.x < 0.0) n.x = -n.x;
    if (p.y < 0.0) n.y = -n.y;

    float cov = saturate(0.5 - sdf);
    float4 final_out = float4(0.0, 0.0, 0.0, 0.0);
    if (cov > 0.0005) {
        // Convex superellipse bezel h(x) = (1 - (1 - x)^4)^(1/4) + Snell's Law refraction.
        float depth = max(0.0, -sdf);
        float bezel = max(1.0, c_optics.y);
        float t = saturate(depth / bezel);
        float mag = 0.0;
        if (t < 1.0) {
            float x = max(t, 1e-4);
            float u = 1.0 - x;
            float u3 = u * u * u;
            float u4 = u3 * u;
            float dh = u3 / pow(max(1e-6, 1.0 - u4), 0.75) * max(0.1, c_optics.w);
            float theta_in = atan(dh);
            float sin_out = clamp(sin(theta_in) / max(1.01, c_optics.z), -0.9999, 0.9999);
            float theta_out = asin(sin_out);
            float snell = tan(theta_in - theta_out) * c_refract.y;
            mag = c_refract.x * saturate(snell);
        }
        float2 disp = -n * mag;

        float4 uv0 = D2DGetInputCoordinate(0);
        float4 uv1 = D2DGetInputCoordinate(1);
        float sigma = c_blur_vib.x;
        float px = max(0.5, c_refract.w);
        float frost_mix = saturate((sigma - 2.6 * px) / (7.5 * px));
        float spread = c_refract.z;

        float3 rgb = float3(0.0, 0.0, 0.0);
        if (mag > 0.25 && spread > 0.001) {
            // 3-channel RGB prism chromatic dispersion along curved bezel.
            rgb.r = SampleBezel5Tap(pos, disp * (1.0 + spread), uv0, uv1, sigma, frost_mix).r;
            rgb.g = SampleBezel5Tap(pos, disp,                  uv0, uv1, sigma, frost_mix).g;
            rgb.b = SampleBezel5Tap(pos, disp * (1.0 - spread), uv0, uv1, sigma, frost_mix).b;
        } else {
            rgb = SampleFrosted9Tap(pos, disp, uv0, uv1, sigma, frost_mix);
        }

        // Apple Vibrancy saturation boost (Rec.709 luminance).
        float lum = dot(rgb, float3(0.2126, 0.7152, 0.0722));
        rgb = saturate(lerp(float3(lum, lum, lum), rgb, c_blur_vib.z));

        // Body tint overlay.
        rgb = lerp(rgb, c_tint.rgb, c_tint.a);

        // Subtle interior white veil lifting glass luminosity.
        float veil = c_surface.x * min(1.0, 2.0 * max(0.15, c_blur_vib.y));
        rgb = lerp(rgb, float3(1.0, 1.0, 1.0), veil);

        // Monochrome anti-banding dither noise (zero-memory deterministic per-pixel hash).
        if (c_surface.y > 0.0005) {
            float2 cell = floor(pos);
            float h = frac(sin(dot(cell, float2(12.9898, 78.233))) * 43758.5453);
            rgb = lerp(rgb, float3(h, h, h), c_surface.y);
        }

        // Pixel-accurate SDF directional specular rim + GPU cursor-tracking dual-corner highlight
        // (replaces per-frame CPU DrawTileSpecularRim brushes & strokes with 0 CPU overhead).
        float rim_gain = c_surface.z;
        float rim_stroke = c_rim_light.z;
        if (rim_gain > 0.001 || rim_stroke > 0.001) {
            float band = clamp(min(c_rect_xywh.z, c_rect_xywh.w) * 0.08, 2.5 * px, 7.0 * px);
            if (depth <= band) {
                float lx = c_rim_light.x;
                float ly = c_rim_light.y;
                float2 key_dir = float2(-0.70710678, -0.70710678);
                float spec_mul = 0.70;
                if (lx >= 0.0 && ly >= 0.0) {
                    float2 v = float2((lx - 0.5) * c_rect_xywh.z, (ly - 0.5) * c_rect_xywh.w);
                    float vlen = length(v);
                    if (vlen > 0.5) key_dir = v / vlen;
                    spec_mul = 1.0;
                }

                if (rim_gain > 0.001) {
                    float soft = 1.0 - depth / band;
                    soft = soft * soft * soft;
                    float edge_profile = 0.84 * exp(-depth / (0.65 * px)) + 0.18 * soft;

                    float top_v = max(-n.y, 0.0);
                    float bot_v = max(n.y, 0.0);
                    float diag_key = max(dot(n, key_dir), 0.0);
                    float diag_fill = max(dot(n, -key_dir), 0.0);
                    float angular = 0.16 + 0.42 * (top_v * top_v) + 0.36 * (bot_v * bot_v) +
                                    0.52 * (diag_key * diag_key * diag_key) +
                                    0.32 * (diag_fill * diag_fill * diag_fill);
                    float rim_a = saturate(edge_profile * angular * rim_gain);
                    rgb = rgb * (1.0 - rim_a) + rim_a;
                }

                if (rim_stroke > 0.001 && depth <= 2.6 * px) {
                    float flags = c_rim_light.w;
                    bool is_dark = (flags >= 0.5 && fmod(floor(flags + 0.25), 2.0) >= 0.5);
                    bool is_clear = (flags >= 1.5);
                    if (!is_clear && depth <= 1.2 * px) {
                        float outer_ring = saturate(1.0 - abs(depth - 0.5 * px) / (0.65 * px));
                        float dark_a = (is_dark ? 0.30 : 0.10) * rim_stroke * outer_ring;
                        rgb = rgb * (1.0 - dark_a);
                    }
                    float ring1 = saturate(1.0 - abs(depth - 1.0 * px) / (0.70 * px));
                    float ring2 = saturate(1.0 - abs(depth - 2.0 * px) / (0.70 * px));
                    float gain = (is_dark ? 0.75 : 0.95) * rim_stroke;
                    float span = min(c_rect_xywh.z, c_rect_xywh.w) * 0.50 + max(c_rect_xywh.z, c_rect_xywh.w) * 0.18;
                    float2 abs_dir = max(abs(key_dir), float2(1e-4, 1e-4));
                    float t_edge = min(half_sz.x / abs_dir.x, half_sz.y / abs_dir.y);
                    float2 key_pt = center + key_dir * t_edge;
                    float2 fill_pt = center - key_dir * t_edge;
                    float dk = saturate(1.0 - length(pos - key_pt) / max(1.0, span * 0.70710678));
                    float df = saturate(1.0 - length(pos - fill_pt) / max(1.0, span * 0.70710678));
                    float corner_a = (0.42 * dk * dk + 0.20 * df * df) * gain * spec_mul;
                    float amb_a = (is_clear ? (is_dark ? 0.24 : 0.42) : (is_dark ? 0.16 : 0.22)) * rim_stroke;
                    float stroke_a = saturate((corner_a * (0.85 * ring1 + 0.10 * ring2)) + amb_a * ring1);
                    rgb = rgb * (1.0 - stroke_a) + stroke_a;
                }
            }
        }

        float out_a = cov * c_blur_vib.w;
        final_out = float4(saturate(rgb) * out_a, out_a);
    }
    return final_out;
}
