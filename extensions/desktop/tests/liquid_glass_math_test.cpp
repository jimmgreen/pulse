// liquid_glass_math_test.cpp - Deterministic optical, coordinate, contrast, fusion,
// and live-wallpaper detector verification + reference PNG showcase generator.
#include "host/render/shaders/liquid_glass_optics.hlsli"
#include "host/render/liquid_glass_params.h"
#include "host/live_wallpaper_detector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

using namespace pulse::desktop;
using namespace pulse::desktop::render;
namespace lg = pulse::desktop::render::lg_optics;

// Minimal valid uncompressed PNG writer (zlib stored deflate blocks + CRC32 + Adler32)
// so tests can emit inspectable PNGs with zero Win32/WIC/USER32 dependencies.
uint32_t Crc32(const uint8_t* data, size_t len) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void AppendChunk(std::vector<uint8_t>& png, const char type[4], const uint8_t* payload, size_t len) {
    AppendBe32(png, static_cast<uint32_t>(len));
    const size_t type_pos = png.size();
    png.insert(png.end(), reinterpret_cast<const uint8_t*>(type), reinterpret_cast<const uint8_t*>(type) + 4);
    if (len > 0) png.insert(png.end(), payload, payload + len);
    const uint32_t crc = Crc32(png.data() + type_pos, 4 + len);
    AppendBe32(png, crc);
}

bool WriteRgbaPng(const std::filesystem::path& path, const uint8_t* rgba, uint32_t w, uint32_t h) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (1 + static_cast<size_t>(w) * 4));
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0); // filter = None
        const uint8_t* row = rgba + static_cast<size_t>(y) * w * 4;
        raw.insert(raw.end(), row, row + static_cast<size_t>(w) * 4);
    }

    std::vector<uint8_t> zstream;
    zstream.push_back(0x78);
    zstream.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t chunk = std::min<size_t>(raw.size() - pos, 65535u);
        const bool last = (pos + chunk == raw.size());
        zstream.push_back(last ? 0x01u : 0x00u);
        const uint16_t len16 = static_cast<uint16_t>(chunk);
        const uint16_t nlen16 = static_cast<uint16_t>(~len16);
        zstream.push_back(static_cast<uint8_t>(len16 & 0xFFu));
        zstream.push_back(static_cast<uint8_t>((len16 >> 8) & 0xFFu));
        zstream.push_back(static_cast<uint8_t>(nlen16 & 0xFFu));
        zstream.push_back(static_cast<uint8_t>((nlen16 >> 8) & 0xFFu));
        zstream.insert(zstream.end(), raw.begin() + static_cast<ptrdiff_t>(pos),
                       raw.begin() + static_cast<ptrdiff_t>(pos + chunk));
        pos += chunk;
    }
    uint32_t s1 = 1, s2 = 0;
    for (uint8_t b : raw) {
        s1 = (s1 + b) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    AppendBe32(zstream, (s2 << 16) | s1);

    std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<uint8_t> ihdr;
    AppendBe32(ihdr, w);
    AppendBe32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA
    AppendChunk(png, "IHDR", ihdr.data(), ihdr.size());
    AppendChunk(png, "IDAT", zstream.data(), zstream.size());
    AppendChunk(png, "IEND", nullptr, 0);

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return f.good();
}

lg::float4 EvalWithPlan(const GlassDrawPlan& plan, lg::float2 scene_pos,
                        auto&& sample_fn) {
    const auto& c = plan.constants;
    return lg::LgShadePixel(
        scene_pos,
        lg::float4(c.rect_x, c.rect_y, c.rect_w, c.rect_h),
        lg::float4(c.radius, c.bezel, c.ref_ior, c.lens_scale),
        lg::float4(c.dispersion_gain, c.frost, c.glare_angle_rad, c.refraction_pad),
        lg::float4(c.glare_factor, c.fresnel_factor, c.clear_interior, c.feather),
        lg::float4(c.glare_range, c.fresnel_range, c.dark_mode, c.smooth_k),
        lg::float4(c.neighbor_count, c.opposite_factor, c.tint_r, c.tint_g),
        lg::float4(c.tint_b, c.tint_opacity, c.overall_alpha, c.rim_alpha),
        lg::float4(c.neighbor_radii[0], c.neighbor_radii[1], c.neighbor_radii[2], c.neighbor_radii[3]),
        lg::float4(c.neighbor_rects[0][0], c.neighbor_rects[0][1], c.neighbor_rects[0][2], c.neighbor_rects[0][3]),
        lg::float4(c.neighbor_rects[1][0], c.neighbor_rects[1][1], c.neighbor_rects[1][2], c.neighbor_rects[1][3]),
        lg::float4(c.neighbor_rects[2][0], c.neighbor_rects[2][1], c.neighbor_rects[2][2], c.neighbor_rects[2][3]),
        lg::float4(c.neighbor_rects[3][0], c.neighbor_rects[3][1], c.neighbor_rects[3][2], c.neighbor_rects[3][3]),
        sample_fn);
}

lg::float3 BlendPremulOver(lg::float3 dst, lg::float4 src_premul) {
    const float inv_a = 1.0f - lg::lg_saturate(src_premul.w);
    return {
        src_premul.x + dst.x * inv_a,
        src_premul.y + dst.y * inv_a,
        src_premul.z + dst.z * inv_a,
    };
}

} // namespace

int main() {
    int passed = 0;
    int failed = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (ok) ++passed;
        else ++failed;
    };

    // -----------------------------------------------------------------------
    // 1. G1: Snell meniscus edge refraction bends lines strongly (>=28..48px) & vanishes inside
    // -----------------------------------------------------------------------
    {
        GlassCardRequest req{};
        req.left_dip = 100.0f; req.top_dip = 100.0f;
        req.right_dip = 340.0f; req.bottom_dip = 260.0f;
        req.radius_dip = 16.0f;
        req.thickness_dip = 28.0f;
        req.ior = 1.52f;
        req.dispersion_gain = 0.0f;
        req.frost_percent = 0;
        req.opacity_percent = 60;
        const auto plan = MakeGlassDrawPlan(req, 1.0f);

        float captured_offsets[5]{};
        const float probe_xs[5] = {
            101.5f,                           // outer bezel (inner = 1.5)
            106.0f,                           // mid-outer bezel (inner = 6)
            114.0f,                           // mid-inner bezel (inner = 14)
            100.0f + plan.constants.bezel - 1.0f, // near inner boundary
            180.0f,                           // flat interior
        };
        for (int i = 0; i < 5; ++i) {
            float dx_seen = 0.0f;
            (void)EvalWithPlan(plan, lg::float2(probe_xs[i], 180.0f),
                [&](lg::float2 off, float /*frost*/) {
                    dx_seen = off.x;
                    return lg::float4(0.5f, 0.5f, 0.5f, 1.0f);
                });
            captured_offsets[i] = dx_seen;
        }

        GlassCardRequest prism = req;
        prism.thickness_dip = 44.0f;
        prism.ior = 1.68f;
        const auto prism_plan = MakeGlassDrawPlan(prism, 1.0f);
        float prism_edge_dx = 0.0f;
        (void)EvalWithPlan(prism_plan, lg::float2(101.5f, 180.0f),
            [&](lg::float2 off, float /*frost*/) {
                prism_edge_dx = off.x;
                return lg::float4(0.5f, 0.5f, 0.5f, 1.0f);
            });

        // On left edge (`n.x < 0`), convex Snell meniscus gives `off.x = -n.x * d > 0` (samples inward!).
        check(captured_offsets[0] >= 28.0f &&
              prism_edge_dx >= 48.0f &&
              captured_offsets[0] > captured_offsets[1] &&
              captured_offsets[1] > captured_offsets[2] &&
              captured_offsets[2] > captured_offsets[3] &&
              captured_offsets[3] > 0.0f &&
              std::fabs(captured_offsets[4]) < 1e-5f,
              "G1: Snell meniscus pulls inward (>=28px standard, >=48px prism) and vanishes in interior");
    }

    // -----------------------------------------------------------------------
    // 2. G2: Clear-mode interior passthrough & seamless inner bezel boundary
    // -----------------------------------------------------------------------
    {
        GlassCardRequest req{};
        req.left_dip = 50.0f; req.top_dip = 50.0f;
        req.right_dip = 250.0f; req.bottom_dip = 200.0f;
        req.thickness_dip = 28.0f;
        req.ior = 1.52f;
        req.dispersion_gain = 0.0f;
        req.frost_percent = 4;
        req.opacity_percent = 60;
        req.allow_clear_passthrough = true;
        auto plan = MakeGlassDrawPlan(req, 1.0f);
        plan.constants.tint_opacity = 0.0f;

        const lg::float3 bg(0.42f, 0.58f, 0.76f);
        auto sample_bg = [&](lg::float2 /*off*/, float /*frost*/) {
            return lg::float4(bg.x, bg.y, bg.z, 1.0f);
        };

        const lg::float4 center_out = EvalWithPlan(plan, lg::float2(150.0f, 125.0f), sample_bg);
        const lg::float3 center_comp = BlendPremulOver(bg, center_out);
        const float max_center_diff = std::max({
            std::fabs(center_comp.x - bg.x),
            std::fabs(center_comp.y - bg.y),
            std::fabs(center_comp.z - bg.z)});
        check(center_out.w == 0.0f && max_center_diff <= (2.0f / 255.0f),
              "G2a: Clear interior outputs alpha=0 and matches external wallpaper within <= 2/255");

        const float bx = 50.0f + plan.constants.bezel;
        const lg::float3 just_inside_bezel = BlendPremulOver(bg, EvalWithPlan(plan, lg::float2(bx - 0.5f, 125.0f), sample_bg));
        const lg::float3 just_flat = BlendPremulOver(bg, EvalWithPlan(plan, lg::float2(bx + 0.5f, 125.0f), sample_bg));
        const float step_diff = std::max({
            std::fabs(just_inside_bezel.x - just_flat.x),
            std::fabs(just_inside_bezel.y - just_flat.y),
            std::fabs(just_inside_bezel.z - just_flat.z)});
        check(step_diff < (4.0f / 255.0f),
              "G2b: Bezel-to-interior transition is feathered C1-continuously with no visible seam");
    }

    // -----------------------------------------------------------------------
    // 3. G3: Chromatic dispersion shifts R/B at bezel, 0 inside, energy-conserving on gray
    // -----------------------------------------------------------------------
    {
        GlassCardRequest req_off{};
        req_off.left_dip = 40.0f; req_off.top_dip = 40.0f;
        req_off.right_dip = 240.0f; req_off.bottom_dip = 180.0f;
        req_off.thickness_dip = 28.0f;
        req_off.ior = 1.52f;
        req_off.frost_percent = 0;
        req_off.opacity_percent = 60;
        req_off.dispersion_gain = 0.0f;

        GlassCardRequest req_strong = req_off;
        req_strong.dispersion_gain = 22.0f;

        auto plan_off = MakeGlassDrawPlan(req_off, 1.0f);
        auto plan_strong = MakeGlassDrawPlan(req_strong, 1.0f);
        plan_off.constants.glare_factor = plan_off.constants.fresnel_factor = 0.0f;
        plan_off.constants.rim_alpha = plan_off.constants.tint_opacity = 0.0f;
        plan_strong.constants.glare_factor = plan_strong.constants.fresnel_factor = 0.0f;
        plan_strong.constants.rim_alpha = plan_strong.constants.tint_opacity = 0.0f;

        // Sharp vertical black/white edge placed right where the refracted ray lands (`43 + off.x`):
        float base_off_x = 0.0f;
        (void)EvalWithPlan(plan_off, lg::float2(43.0f, 110.0f), [&](lg::float2 off, float) {
            base_off_x = off.x;
            return lg::float4(0.5f, 0.5f, 0.5f, 1.0f);
        });
        const float edge_x = 43.0f + base_off_x;
        auto step_backdrop = [&](lg::float2 off, float /*frost*/) {
            const float sx = 43.0f + off.x;
            const float v = (sx >= edge_x) ? 0.92f : 0.08f;
            return lg::float4(v, v, v, 1.0f);
        };

        const lg::float4 c_off = EvalWithPlan(plan_off, lg::float2(43.0f, 110.0f), step_backdrop);
        const lg::float4 c_strong = EvalWithPlan(plan_strong, lg::float2(43.0f, 110.0f), step_backdrop);
        check(std::fabs(c_off.x - c_off.z) < 1e-4f && std::fabs(c_strong.x - c_strong.z) > 0.15f,
              "G3a: Chromatic dispersion separates R/B across high-contrast bezel edges when enabled");

        auto gray_backdrop = [&](lg::float2 /*off*/, float /*frost*/) {
            return lg::float4(0.5f, 0.5f, 0.5f, 1.0f);
        };
        const lg::float4 c_gray = EvalWithPlan(plan_strong, lg::float2(43.0f, 110.0f), gray_backdrop);
        const lg::float4 c_in = EvalWithPlan(plan_strong, lg::float2(140.0f, 110.0f), [&](lg::float2 off, float) {
            const float v = (140.0f + off.x >= 140.0f) ? 0.9f : 0.1f;
            return lg::float4(v, v, v, 1.0f);
        });
        check(std::fabs(c_gray.x - c_gray.y) < 1e-4f && std::fabs(c_gray.y - c_gray.z) < 1e-4f &&
              std::fabs(c_in.x - c_in.z) < 1e-4f,
              "G3b: Spectral dispersion weights sum to 1.0 (zero tint cast) and interior dispersion is 0");
    }

    // -----------------------------------------------------------------------
    // 4. G4 & G7: Dual-lobe specular glare, Fresnel rim, and visible depth on flat bg
    // -----------------------------------------------------------------------
    {
        GlassCardRequest req{};
        req.left_dip = 60.0f; req.top_dip = 60.0f;
        req.right_dip = 260.0f; req.bottom_dip = 260.0f;
        req.radius_dip = 24.0f;
        req.thickness_dip = 28.0f;
        req.glare_percent = 92;
        req.glare_angle_deg = -45.0f;
        req.frost_percent = 0;
        req.opacity_percent = 60;
        req.dark_mode = true;
        req.draw_rim_in_shader = false;
        auto plan = MakeGlassDrawPlan(req, 1.0f);
        plan.constants.tint_opacity = 0.0f;
        plan.constants.frost = 0.0f;

        auto dark_flat = [&](lg::float2, float) { return lg::float4(0.12f, 0.12f, 0.12f, 1.0f); };
        // Sample ~2.1 px inside the outer n=4 Squircle corner arc:
        const lg::float4 tl = EvalWithPlan(plan, lg::float2(65.6f, 65.6f), dark_flat);
        const lg::float4 br = EvalWithPlan(plan, lg::float2(254.4f, 254.4f), dark_flat);
        const lg::float4 tr = EvalWithPlan(plan, lg::float2(254.4f, 65.6f), dark_flat);
        const lg::float4 center = EvalWithPlan(plan, lg::float2(160.0f, 160.0f), dark_flat);

        GlassCardRequest rot = req;
        rot.glare_angle_deg = 135.0f;
        auto plan_rot = MakeGlassDrawPlan(rot, 1.0f);
        plan_rot.constants.tint_opacity = 0.0f;
        plan_rot.constants.frost = 0.0f;
        const lg::float4 tl_rot = EvalWithPlan(plan_rot, lg::float2(65.6f, 65.6f), dark_flat);
        const lg::float4 br_rot = EvalWithPlan(plan_rot, lg::float2(254.4f, 254.4f), dark_flat);

        check(tl.x > br.x && br.x > tr.x && tr.x > center.x &&
              br_rot.x > tl_rot.x,
              "G4/G7: Dual-lobe caustic glare (-45deg TL>BR>TR>center) rotates smoothly with glare_angle_deg");
    }

    // -----------------------------------------------------------------------
    // 5. Crease-free corner normal when `bezel > radius`
    // -----------------------------------------------------------------------
    {
        const lg::float2 half_sz(120.0f, 90.0f);
        const float radius = 8.0f;
        const float bezel = 22.0f;
        float max_angle_jump = 0.0f;
        float prev_angle = 0.0f;
        for (int deg = 20; deg <= 70; ++deg) {
            const float rad = static_cast<float>(deg) * 3.14159265f / 180.0f;
            // Walk an arc deep inside the corner where naive SDF gradients jump 90 degrees!
            const lg::float2 p(half_sz.x - bezel + 8.0f * std::cos(rad),
                               half_sz.y - bezel + 8.0f * std::sin(rad));
            const lg::float2 n = lg::LgGradRoundRectSmooth(p, half_sz, radius, bezel);
            const float ang = std::atan2(n.y, n.x);
            if (deg > 20) {
                max_angle_jump = std::max(max_angle_jump, std::fabs(ang - prev_angle));
            }
            prev_angle = ang;
        }
        check(max_angle_jump < 0.08f,
              "Corner normal rotates smoothly across 45deg diagonal when bezel > radius (no mitre crease)");
    }

    // -----------------------------------------------------------------------
    // 6. G5: DPI (100/125/150/200%) + multi-monitor + VirtualSurface atlas alignment
    // -----------------------------------------------------------------------
    {
        bool all_aligned = true;
        for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
            const uint32_t screen_w = static_cast<uint32_t>(1920.0f * scale);
            const uint32_t screen_h = static_cast<uint32_t>(1080.0f * scale);
            const uint32_t src_w = screen_w / 2;
            const uint32_t src_h = screen_h / 2;
            const float origin_x_px = 72.0f;  // e.g. docked left appbar
            const float origin_y_px = 48.0f;  // e.g. top taskbar
            const float atlas_shift_x = -256.0f;
            const float atlas_shift_y = 128.0f;

            const auto m = ComputeGlassSourceMatrix(
                src_w, src_h, screen_w, screen_h, origin_x_px, origin_y_px, atlas_shift_x, atlas_shift_y);

            // Scene DIP `(200, 150)` lands on target pixel `p_target = p_dip * scale + atlas_shift`.
            const float target_x = 200.0f * scale + atlas_shift_x;
            const float target_y = 150.0f * scale + atlas_shift_y;
            // Inverse of `target = src * m_scale + m_trans`:
            const float sampled_src_x = (target_x - m.dx) / m.m11;
            const float sampled_src_y = (target_y - m.dy) / m.m22;
            const float expected_monitor_px_x = 200.0f * scale + origin_x_px;
            const float expected_monitor_px_y = 150.0f * scale + origin_y_px;
            const float actual_monitor_px_x = sampled_src_x * (static_cast<float>(screen_w) / src_w);
            const float actual_monitor_px_y = sampled_src_y * (static_cast<float>(screen_h) / src_h);
            if (std::fabs(actual_monitor_px_x - expected_monitor_px_x) > 0.01f ||
                std::fabs(actual_monitor_px_y - expected_monitor_px_y) > 0.01f) {
                all_aligned = false;
            }
        }
        check(all_aligned, "G5: 100/125/150/200% DPI + offset + VirtualSurface atlas align within < 0.01 px");
    }

    // -----------------------------------------------------------------------
    // 7. G6: WCAG >= 4.5:1 foreground text contrast across extreme & saddle wallpapers
    // -----------------------------------------------------------------------
    {
        const text_contrast::Srgb test_bgs[] = {
            {0.0f, 0.0f, 0.0f},       // pitch black
            {1.0f, 1.0f, 1.0f},       // pure white
            {0.46f, 0.46f, 0.46f},    // ~0.18 relative luminance WCAG saddle
            {0.53f, 0.53f, 0.53f},    // ~0.24 relative luminance WCAG saddle
            {0.85f, 0.12f, 0.18f},    // vivid crimson
            {0.10f, 0.55f, 0.92f},    // vivid azure
        };
        float worst_contrast = 21.0f;
        for (bool dark : {true, false}) {
            for (uint8_t op : {uint8_t{60}, uint8_t{80}, uint8_t{100}}) {
                for (const auto& bg : test_bgs) {
                    const auto surf = TransformLiquidGlassSample(bg, dark, op, 4);
                    const auto ink = text_contrast::ChooseInk(
                        text_contrast::Fill(surf), 0xFAFFFFFFu, 0xFA1B2028u, dark);
                    const float chosen_ratio = ink.light ? ink.light_contrast : ink.dark_contrast;
                    worst_contrast = std::min(worst_contrast, chosen_ratio);
                }
            }
        }
        check(worst_contrast >= 4.5f,
              "G6: Adaptive Liquid Glass tint + ChooseInk achieves >= 4.5:1 WCAG contrast on all test wallpapers");
    }

    // -----------------------------------------------------------------------
    // 8. G9: Hover light easing settles to 0-Present + press elasticity + Voronoi fusion
    // -----------------------------------------------------------------------
    {
        HoverLightTracker tracker{};
        tracker.SetCursorOnCard(220.0f, 80.0f, 100.0f, 100.0f, 260.0f, 220.0f);
        bool moved_initially = tracker.Step(16.0f);
        int steps = 1;
        while (tracker.animating && steps < 200) {
            tracker.Step(16.0f);
            ++steps;
        }
        check(moved_initially && !tracker.animating && steps < 80 && !tracker.Step(16.0f),
              "G9a: HoverLightTracker eases smoothly toward cursor and converges cleanly to idle (0 redraws)");

        GlassCardRequest rest{};
        rest.left_dip = 0; rest.top_dip = 0; rest.right_dip = 180; rest.bottom_dip = 180;
        GlassCardRequest pressed = rest;
        pressed.press_amount = 0.85f;
        GlassCardRequest rebound = rest;
        rebound.press_amount = -0.20f;
        const auto p_rest = MakeGlassDrawPlan(rest, 1.0f);
        const auto p_press = MakeGlassDrawPlan(pressed, 1.0f);
        const auto p_reb = MakeGlassDrawPlan(rebound, 1.0f);
        check(p_press.constants.bezel > p_rest.constants.bezel &&
              p_press.constants.lens_scale > p_rest.constants.lens_scale &&
              p_reb.constants.bezel < p_rest.constants.bezel,
              "G9b: Elastic press swells meniscus bezel/lens scale and spring rebound relaxes it");

        // Collapsed 40px header card safety:
        GlassCardRequest header_card{};
        header_card.left_dip = 0; header_card.top_dip = 0; header_card.right_dip = 320; header_card.bottom_dip = 40;
        const auto p_hdr = MakeGlassDrawPlan(header_card, 1.0f);
        check(p_hdr.constants.bezel <= 19.5f && p_hdr.visible,
              "Narrow/collapsed 40px group header clamps bezel safely below half-height");

        // Smooth-min Voronoi fusion between two adjacent tiles separated by a 12px gap:
        const GlassNeighborDip nb_b{212.0f, 100.0f, 332.0f, 220.0f, 18.0f};
        const GlassNeighborDip nb_a{80.0f, 100.0f, 200.0f, 220.0f, 18.0f};

        GlassCardRequest card_a{};
        card_a.left_dip = 80.0f; card_a.top_dip = 100.0f; card_a.right_dip = 200.0f; card_a.bottom_dip = 220.0f;
        card_a.radius_dip = 18.0f; card_a.opacity_percent = 60;
        const auto plan_a_isolated = MakeGlassDrawPlan(card_a, 1.0f);
        card_a.neighbors = std::span<const GlassNeighborDip>(&nb_b, 1);
        const auto plan_a_fused = MakeGlassDrawPlan(card_a, 1.0f);

        GlassCardRequest card_b{};
        card_b.left_dip = 212.0f; card_b.top_dip = 100.0f; card_b.right_dip = 332.0f; card_b.bottom_dip = 220.0f;
        card_b.radius_dip = 18.0f; card_b.opacity_percent = 60;
        card_b.neighbors = std::span<const GlassNeighborDip>(&nb_a, 1);
        const auto plan_b_fused = MakeGlassDrawPlan(card_b, 1.0f);

        auto uni_bg = [&](lg::float2, float) { return lg::float4(0.3f, 0.5f, 0.8f, 1.0f); };
        // Midpoint of the 12px gap `(206, 160)` is outside both isolated rects (`x=200` and `x=212`):
        const lg::float4 iso_mid = EvalWithPlan(plan_a_isolated, lg::float2(206.0f, 160.0f), uni_bg);
        const lg::float4 fused_a = EvalWithPlan(plan_a_fused, lg::float2(205.2f, 160.0f), uni_bg);
        const lg::float4 fused_b = EvalWithPlan(plan_b_fused, lg::float2(205.2f, 160.0f), uni_bg);
        const lg::float4 fused_a_r = EvalWithPlan(plan_a_fused, lg::float2(206.8f, 160.0f), uni_bg);
        const lg::float4 fused_b_r = EvalWithPlan(plan_b_fused, lg::float2(206.8f, 160.0f), uni_bg);

        check(iso_mid.w == 0.0f &&
              fused_a.w > 0.99f && fused_b.w == 0.0f &&
              fused_b_r.w > 0.99f && fused_a_r.w == 0.0f &&
              std::fabs(fused_a.x - fused_b_r.x) < (2.0f / 255.0f) &&
              std::fabs(fused_a.y - fused_b_r.y) < (2.0f / 255.0f) &&
              std::fabs(fused_a.z - fused_b_r.z) < (2.0f / 255.0f),
              "G9c: Smooth-min Voronoi fusion bridges 12px gap seamlessly (<2/255 saddle continuity, 0 double-draw)");
    }

    // -----------------------------------------------------------------------
    // 9. LiveWallpaperDetector topology matrix
    // -----------------------------------------------------------------------
    {
        host::LiveWallpaperHierarchy h{};
        h.mode = 1; // ClassicWorkerW
        h.progman = 0x1001;
        h.defview_parent = 0x1002;
        h.shelldll_defview = 0x1003;
        h.workerw_wallpaper = 0x1004;
        h.explorer_pid = 4000;
        h.self_pid = 5000;
        h.primary_work_w = 1920;
        h.primary_work_h = 1040;

        // Explorer's own slideshow WorkerW child -> MUST NOT trigger live mode
        const host::LiveWallpaperCandidate explorer_child{
            0x2001, 0x1004, 0x1004, 4000, 1920, 1080, true, false, false, false};
        check(!host::EvaluateLiveWallpaper(h, std::span(&explorer_child, 1)).active,
              "Detector rejects Explorer's own WorkerW/slideshow children");

        // Cloaked or small helper third-party window -> MUST NOT trigger
        const host::LiveWallpaperCandidate cloaked_or_tiny[] = {
            {0x2002, 0x1004, 0x1004, 7777, 1920, 1080, true, true, false, false},
            {0x2003, 0x1004, 0x1004, 7777, 320, 240, true, false, false, false},
            {0x2004, 0x1004, 0x1004, 5000, 1920, 1080, true, false, false, false},
        };
        check(!host::EvaluateLiveWallpaper(h, cloaked_or_tiny).active,
              "Detector rejects cloaked, tiny (<40%), and self-owned windows");

        // Lively child under workerw_wallpaper -> active, capture_target = workerw_wallpaper
        const host::LiveWallpaperCandidate lively{
            0x3001, 0x1004, 0x1004, 8888, 1920, 1080, true, false, false, false};
        const auto d_lively = host::EvaluateLiveWallpaper(h, std::span(&lively, 1));
        check(d_lively.active && d_lively.capture_target_hwnd == 0x1004 && d_lively.wallpaper_pid == 8888,
              "Detector resolves ClassicWorkerW child to top-level workerw_wallpaper capture target");

        // Win11 24H2 / Wallpaper Engine Progman-parented mode -> capture_target = progman
        h.mode = 2;
        const host::LiveWallpaperCandidate wpe_24h2{
            0x3002, 0x1001, 0x1001, 9999, 1920, 1080, true, false, false, false};
        const auto d_24h2 = host::EvaluateLiveWallpaper(h, std::span(&wpe_24h2, 1));
        check(d_24h2.active && d_24h2.capture_target_hwnd == 0x1001 && d_24h2.wallpaper_pid == 9999,
              "Detector resolves Win11 24H2 Progman-parented wallpaper to Progman capture target");
    }

    // -----------------------------------------------------------------------
    // 10. Render 960x540 visual showcase PNG for human/agent inspection
    // -----------------------------------------------------------------------
    {
        constexpr uint32_t W = 960, H = 540;
        std::vector<lg::float3> sharp_bg(W * H);
        std::vector<lg::float3> frost_bg(W * H);

        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                const float u = static_cast<float>(x) / W;
                const float v = static_cast<float>(y) / H;
                // Rich sapphire/magenta/cyan aurora + crisp geometric grid & diagonal light bars
                float r = 0.07f + 0.42f * std::pow(1.0f - u, 1.4f) * v + 0.35f * std::exp(-18.0f * ((u - 0.72f) * (u - 0.72f) + (v - 0.28f) * (v - 0.28f)));
                float g = 0.10f + 0.48f * u * (1.0f - 0.5f * v) + 0.30f * std::exp(-20.0f * ((u - 0.30f) * (u - 0.30f) + (v - 0.65f) * (v - 0.65f)));
                float b = 0.22f + 0.58f * (1.0f - 0.4f * v) + 0.22f * std::sin(u * 9.0f);

                // Crisp architectural grid & bright diagonal ribbons so refraction + dispersion are unmistakable!
                if ((x % 48) < 2 || (y % 48) < 2) {
                    r = lg::lg_lerp(r, 0.96f, 0.45f);
                    g = lg::lg_lerp(g, 0.98f, 0.45f);
                    b = lg::lg_lerp(b, 1.00f, 0.45f);
                }
                const float diag = std::fmod(static_cast<float>(x) * 0.75f + static_cast<float>(y), 110.0f);
                if (diag < 7.0f) {
                    r = lg::lg_lerp(r, 1.0f, 0.78f);
                    g = lg::lg_lerp(g, 0.88f, 0.78f);
                    b = lg::lg_lerp(b, 0.65f, 0.78f);
                }
                sharp_bg[y * W + x] = lg::lg_saturate3({r, g, b});
            }
        }

        // Box-blur sharp_bg (radius 10) to synthesize frost_bg:
        std::vector<lg::float3> temp_bg(W * H);
        constexpr int R = 10;
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                lg::float3 acc{};
                int cnt = 0;
                for (int dx = -R; dx <= R; ++dx) {
                    const int sx = std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(W) - 1);
                    acc = acc + sharp_bg[y * W + sx];
                    ++cnt;
                }
                temp_bg[y * W + x] = acc * (1.0f / static_cast<float>(cnt));
            }
        }
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                lg::float3 acc{};
                int cnt = 0;
                for (int dy = -R; dy <= R; ++dy) {
                    const int sy = std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(H) - 1);
                    acc = acc + temp_bg[sy * W + x];
                    ++cnt;
                }
                frost_bg[y * W + x] = acc * (1.0f / static_cast<float>(cnt));
            }
        }

        auto sample_bilinear = [](const std::vector<lg::float3>& img, float px, float py) {
            px = std::clamp(px - 0.5f, 0.0f, static_cast<float>(W - 1));
            py = std::clamp(py - 0.5f, 0.0f, static_cast<float>(H - 1));
            const int x0 = static_cast<int>(px), y0 = static_cast<int>(py);
            const int x1 = std::min(x0 + 1, static_cast<int>(W - 1));
            const int y1 = std::min(y0 + 1, static_cast<int>(H - 1));
            const float fx = px - x0, fy = py - y0;
            const lg::float3 top = lg::lg_lerp3(img[y0 * W + x0], img[y0 * W + x1], fx);
            const lg::float3 bot = lg::lg_lerp3(img[y1 * W + x0], img[y1 * W + x1], fx);
            return lg::lg_lerp3(top, bot, fy);
        };

        std::vector<lg::float3> canvas = sharp_bg;

        auto composite_plan = [&](const GlassDrawPlan& plan) {
            const int x0 = std::max(0, plan.output_rect_px.left);
            const int y0 = std::max(0, plan.output_rect_px.top);
            const int x1 = std::min(static_cast<int>(W), plan.output_rect_px.right);
            const int y1 = std::min(static_cast<int>(H), plan.output_rect_px.bottom);
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const lg::float2 sp(x + 0.5f, y + 0.5f);
                    const lg::float4 out = EvalWithPlan(plan, sp, [&](lg::float2 off, float frost_amt) {
                        const float sx = sp.x + off.x;
                        const float sy = sp.y + off.y;
                        const lg::float3 cs = sample_bilinear(sharp_bg, sx, sy);
                        if (frost_amt <= 0.002f) return lg::float4(cs, 1.0f);
                        const lg::float3 cf = sample_bilinear(frost_bg, sx, sy);
                        return lg::float4(lg::lg_lerp3(cs, cf, frost_amt), 1.0f);
                    });
                    if (out.w > 0.0f) {
                        canvas[y * W + x] = BlendPremulOver(canvas[y * W + x], out);
                    }
                }
            }
        };

        // Card 1 (Top-Left): Clear Droplet preset (thick 28px bezel, IOR 1.52, crisp frost 4%)
        GlassCardRequest c1{};
        c1.left_dip = 48; c1.top_dip = 44; c1.right_dip = 446; c1.bottom_dip = 258;
        c1.radius_dip = 24; c1.thickness_dip = 28; c1.ior = 1.52f;
        c1.dispersion_gain = 9.0f; c1.glare_percent = 92; c1.glare_angle_deg = -45.0f;
        c1.frost_percent = 4; c1.opacity_percent = 70; c1.dark_mode = true;
        composite_plan(MakeGlassDrawPlan(c1, 1.0f));

        // Card 2 (Bottom-Left): Deep Prism preset (44px lens, IOR 1.88, dispersion 22, glare 108%, -38deg)
        GlassCardRequest c2{};
        c2.left_dip = 48; c2.top_dip = 282; c2.right_dip = 446; c2.bottom_dip = 496;
        c2.radius_dip = 26; c2.thickness_dip = 44; c2.ior = 1.88f;
        c2.dispersion_gain = 22.0f; c2.glare_percent = 108; c2.glare_angle_deg = -38.0f;
        c2.frost_percent = 0; c2.opacity_percent = 75; c2.dark_mode = true;
        c2.elevated_hover = true;
        composite_plan(MakeGlassDrawPlan(c2, 1.0f));

        // Cards 3 & 4 (Top-Right): Two fusing Apple Regular Liquid Glass Tiles (14px gap bridged smoothly!)
        const GlassNeighborDip nb4{706, 44, 912, 258, 22};
        const GlassNeighborDip nb3{488, 44, 692, 258, 22};

        GlassCardRequest c3{};
        c3.left_dip = 488; c3.top_dip = 44; c3.right_dip = 692; c3.bottom_dip = 258;
        c3.radius_dip = 22; c3.thickness_dip = 22; c3.ior = 1.48f;
        c3.dispersion_gain = 7.0f; c3.glare_percent = 80; c3.glare_angle_deg = -45.0f;
        c3.frost_percent = 28; c3.opacity_percent = 72; c3.dark_mode = true;
        c3.elevated_hover = true;
        c3.neighbors = std::span<const GlassNeighborDip>(&nb4, 1);
        composite_plan(MakeGlassDrawPlan(c3, 1.0f));

        GlassCardRequest c4 = c3;
        c4.left_dip = 706; c4.top_dip = 44; c4.right_dip = 912; c4.bottom_dip = 258;
        c4.neighbors = std::span<const GlassNeighborDip>(&nb3, 1);
        composite_plan(MakeGlassDrawPlan(c4, 1.0f));

        // Card 5 (Bottom-Right): Satin Gel + cursor light pull (rotated glare, soft 62% internal frost)
        GlassCardRequest c5{};
        c5.left_dip = 488; c5.top_dip = 282; c5.right_dip = 912; c5.bottom_dip = 496;
        c5.radius_dip = 24; c5.thickness_dip = 18; c5.ior = 1.42f;
        c5.dispersion_gain = 4.0f; c5.glare_percent = 84; c5.glare_angle_deg = -35.0f;
        c5.frost_percent = 62; c5.opacity_percent = 80; c5.dark_mode = true;
        c5.elevated_hover = true;
        c5.cursor_light_active = true;
        c5.light_dir_x = 0.65f; c5.light_dir_y = -0.76f;
        composite_plan(MakeGlassDrawPlan(c5, 1.0f));

        std::vector<uint8_t> rgba(W * H * 4);
        for (size_t i = 0; i < W * H; ++i) {
            rgba[i * 4 + 0] = static_cast<uint8_t>(lg::lg_saturate(canvas[i].x) * 255.0f + 0.5f);
            rgba[i * 4 + 1] = static_cast<uint8_t>(lg::lg_saturate(canvas[i].y) * 255.0f + 0.5f);
            rgba[i * 4 + 2] = static_cast<uint8_t>(lg::lg_saturate(canvas[i].z) * 255.0f + 0.5f);
            rgba[i * 4 + 3] = 255u;
        }
        const std::filesystem::path out_png =
            "bench_data/liquid-glass-20261116/optics_showcase.png";
        check(WriteRgbaPng(out_png, rgba.data(), W, H),
              "Rendered 960x540 optical showcase PNG (ClearDroplet + DeepPrism + Fused AppleRegular + SatinGel)");
    }

    std::printf("LIQUID_GLASS_MATH_RESULT passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
