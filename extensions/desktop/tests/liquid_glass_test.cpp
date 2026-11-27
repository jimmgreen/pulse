#include <fstream>
// liquid_glass_test.cpp - Hardware D3D11/D2D1.1/WGC integration & budget test for Liquid Glass.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <psapi.h>
#include <wrl/client.h>

#include "host/render/desktop_renderer.h"
#include "host/render/tile_liquid_glass.h"
#include "host/render/live_wallpaper_source.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace {

using Microsoft::WRL::ComPtr;
using namespace pulse::desktop;

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* name) {
    if (ok) {
        std::printf("[PASS] %s\n", name);
        ++g_passed;
    } else {
        std::printf("[FAIL] %s\n", name);
        ++g_failed;
    }
}

struct GpuHarness {
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> d3d_ctx;
    ComPtr<ID2D1Factory1> d2d_factory;
    ComPtr<ID2D1Device> d2d_device;
    ComPtr<ID2D1DeviceContext> dc;

    bool Init() {
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL actual{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                       ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3d, &actual, &d3d_ctx);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3d, &actual, &d3d_ctx);
        }
        if (FAILED(hr) || !d3d) return false;

        ComPtr<IDXGIDevice> dxgi;
        if (FAILED(d3d.As(&dxgi))) return false;
        D2D1_FACTORY_OPTIONS opts{};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                     &opts, reinterpret_cast<void**>(d2d_factory.GetAddressOf())))) {
            return false;
        }
        if (FAILED(d2d_factory->CreateDevice(dxgi.Get(), &d2d_device))) return false;
        if (FAILED(d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return false;
        return true;
    }

    ComPtr<ID2D1Bitmap1> UploadPbgra(uint32_t w, uint32_t h, const std::vector<uint8_t>& pbgra) {
        ComPtr<ID2D1Bitmap1> bmp;
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        dc->CreateBitmap(D2D1::SizeU(w, h), pbgra.data(), w * 4u, &props, &bmp);
        return bmp;
    }

    bool RenderAndReadback(uint32_t w, uint32_t h,
                           const std::function<void(ID2D1DeviceContext*)>& draw_fn,
                           std::vector<uint8_t>& out_bgra) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> rt_tex;
        if (FAILED(d3d->CreateTexture2D(&td, nullptr, &rt_tex))) return false;

        ComPtr<IDXGISurface> surf;
        if (FAILED(rt_tex.As(&surf))) return false;
        const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        ComPtr<ID2D1Bitmap1> target;
        if (FAILED(dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &target))) return false;

        dc->SetTarget(target.Get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        draw_fn(dc.Get());
        const HRESULT hr = dc->EndDraw();
        dc->SetTarget(nullptr);
        if (FAILED(hr)) return false;

        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(d3d->CreateTexture2D(&td, nullptr, &staging))) return false;
        d3d_ctx->CopyResource(staging.Get(), rt_tex.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(d3d_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        out_bgra.resize(static_cast<size_t>(w) * h * 4u);
        for (uint32_t y = 0; y < h; ++y) {
            const auto* src = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
            std::copy_n(src, w * 4u, out_bgra.data() + static_cast<size_t>(y) * w * 4u);
        }
        d3d_ctx->Unmap(staging.Get(), 0);
        return true;
    }
};

double PatchSpatialVariance(const std::vector<uint8_t>& bgra, uint32_t stride_w,
                            int x0, int y0, int pw, int ph) {
    double sum = 0.0;
    double sq = 0.0;
    const int n = pw * ph;
    for (int y = y0; y < y0 + ph; ++y) {
        for (int x = x0; x < x0 + pw; ++x) {
            const size_t p = (static_cast<size_t>(y) * stride_w + static_cast<size_t>(x)) * 4u;
            const double lum = 0.114 * bgra[p + 0] + 0.587 * bgra[p + 1] + 0.299 * bgra[p + 2];
            sum += lum;
            sq += lum * lum;
        }
    }
    const double mean = sum / n;
    return std::max(0.0, sq / n - mean * mean);
}

void RunHardwareEffectChecks(GpuHarness& gpu) {
    constexpr uint32_t kW = 512;
    constexpr uint32_t kH = 384;
    std::vector<uint8_t> sharp_px(kW * kH * 4u);
    std::vector<uint8_t> frost_px((kW / 4u) * (kH / 4u) * 4u);

    // Sharp checkerboard + high-contrast white/black vertical bars for dispersion & refraction checks
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const bool bar = ((x / 12u) % 2u) == 0u;
            const bool chk = (((x / 16u) + (y / 16u)) % 2u) == 0u;
            const uint8_t v = static_cast<uint8_t>(bar ? (chk ? 245 : 195) : (chk ? 45 : 18));
            const size_t p = (static_cast<size_t>(y) * kW + x) * 4u;
            sharp_px[p + 0] = v;
            sharp_px[p + 1] = v;
            sharp_px[p + 2] = v;
            sharp_px[p + 3] = 255;
        }
    }
    // Smooth frosted quarter-res backdrop
    for (uint32_t y = 0; y < kH / 4u; ++y) {
        for (uint32_t x = 0; x < kW / 4u; ++x) {
            const uint8_t v = static_cast<uint8_t>(124 + 12 * std::sin(x * 0.2f));
            const size_t p = (static_cast<size_t>(y) * (kW / 4u) + x) * 4u;
            frost_px[p + 0] = v;
            frost_px[p + 1] = v;
            frost_px[p + 2] = v;
            frost_px[p + 3] = 255;
        }
    }

    auto sharp_bmp = gpu.UploadPbgra(kW, kH, sharp_px);
    auto frost_bmp = gpu.UploadPbgra(kW / 4u, kH / 4u, frost_px);
    Check(sharp_bmp && frost_bmp, "upload sharp and frost test textures");
    if (!sharp_bmp || !frost_bmp) return;

    // Unified WacOS / macOS Tahoe TileLiquidGlassRenderer GPU optical verification:
    // 1. Neutral white preservation across 3-channel RGB chromatic dispersion (zero 3x darkening).
    // 2. Convex superellipse Snell refraction + RGB prism dispersion on bezel edges.
    // 3. Frost monotonicity across Clear -> Regular -> Frosted in interior patch.
    // 4. Vibrancy saturation boost (1.92x) on tinted backdrop + EffectSlot reuse without growth.
    // 5. Exact circular corner SDF (zero superellipse corner ear outside D2D1::RoundedRect) +
    //    cursor-tracking TileHighlightSpring convergence.
    {
        std::vector<uint8_t> white_px(kW * kH * 4u, 220u);
        for (size_t i = 3; i < white_px.size(); i += 4) white_px[i] = 255u;
        auto white_bmp = gpu.UploadPbgra(kW, kH, white_px);

        std::vector<uint8_t> color_px(kW * kH * 4u);
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const size_t p = (static_cast<size_t>(y) * kW + x) * 4u;
                color_px[p + 0] = 170; // B
                color_px[p + 1] = 120; // G
                color_px[p + 2] = 90;  // R (soft blue-slate to test vibrancy boost)
                color_px[p + 3] = 255;
            }
        }
        auto color_bmp = gpu.UploadPbgra(kW, kH, color_px);

        render::TileLiquidGlassRenderer tile_glass;
        model::BeautifySettings light_s{};
        light_s.dark_mode = false;
        light_s.default_material = model::CardMaterial::LiquidGlass;
        const auto tile_cfg = render::MakeTileGlassConfig(light_s, 16.0f, false, false);
        const D2D1_RECT_F tile_rc = D2D1::RectF(120.0f, 80.0f, 360.0f, 280.0f);

        std::vector<uint8_t> out_white, out_stripe_rgb, out_stripe_mono, out_color;
        std::vector<uint8_t> out_frost_clear, out_frost_dense;
        auto draw_tile = [&](ID2D1Bitmap1* src, const render::TileGlassConfig& cfg, std::vector<uint8_t>& out,
                             ID2D1Bitmap1* frost = nullptr, uint32_t fw = 0, uint32_t fh = 0) {
            return gpu.RenderAndReadback(kW, kH, [&](ID2D1DeviceContext* dc) {
                tile_glass.BeginFrame();
                tile_glass.DrawTile(gpu.d2d_factory.Get(), dc, src, kW, kH, kW, kH, 0.0f, 0.0f, 1.0f,
                                    tile_rc, cfg, 1.0f, true, -1.0f, -1.0f, frost, fw, fh);
                tile_glass.EndFrame();
            }, out);
        };

        auto cfg_no_tint = tile_cfg;
        cfg_no_tint.tint.a = 0.0f;
        cfg_no_tint.white_veil_alpha = 0.0f;
        cfg_no_tint.dither_noise_alpha = 0.0f;
        cfg_no_tint.enable_surface_rim = false;
        cfg_no_tint.enable_drop_shadow = false;
        Check(white_bmp && draw_tile(white_bmp.Get(), cfg_no_tint, out_white),
              "TileLiquidGlass renders neutral white with 3-channel chromatic dispersion");
        const size_t mid = (static_cast<size_t>(180) * kW + 240u) * 4u;
        std::printf("TILE_GLASS_WHITE_CENTER b=%u g=%u r=%u a=%u\n",
                    out_white[mid + 0], out_white[mid + 1], out_white[mid + 2], out_white[mid + 3]);
        Check(out_white[mid + 0] >= 205 && out_white[mid + 1] >= 205 && out_white[mid + 2] >= 205 && out_white[mid + 3] >= 250,
              "TileLiquidGlass 3-channel RGB PLUS composition preserves full luminance without darkening");

        // Exact circular corner SDF check: outside circular corner arc (radius=22) must have alpha == 0
        {
            auto cfg_corner = cfg_no_tint;
            cfg_corner.corner_radius_dip = 22.0f;
            std::vector<uint8_t> out_corner;
            Check(white_bmp && draw_tile(white_bmp.Get(), cfg_corner, out_corner),
                  "TileLiquidGlass circular corner render");
            // Top-left corner center is (120 + 22, 80 + 22) = (142, 102); pixel (124, 84) is at dist=25.46 > 22
            const size_t outer_ear = (static_cast<size_t>(84) * kW + 124u) * 4u;
            // Pixel (128, 88) is at dist=19.80 < 22 -> must be opaque inside circular corner
            const size_t inner_arc = (static_cast<size_t>(88) * kW + 128u) * 4u;
            Check(out_corner[outer_ear + 3] == 0 && out_corner[inner_arc + 3] >= 250,
                  "TileLiquidGlass corner matches exact circular RoundedRect SDF with zero squircle ear");
        }

        // Frost monotonicity: clear vs frosted interior spatial variance
        {
            auto cfg_clear = cfg_no_tint;
            cfg_clear.frost_amount = 0.04f;
            auto cfg_dense = cfg_no_tint;
            cfg_dense.frost_amount = 0.90f;
            Check(draw_tile(sharp_bmp.Get(), cfg_clear, out_frost_clear, frost_bmp.Get(), kW / 4u, kH / 4u) &&
                  draw_tile(sharp_bmp.Get(), cfg_dense, out_frost_dense, frost_bmp.Get(), kW / 4u, kH / 4u),
                  "TileLiquidGlass renders Clear and Frosted modes");
            const double var_clear = PatchSpatialVariance(out_frost_clear, kW, 180, 130, 120, 90);
            const double var_dense = PatchSpatialVariance(out_frost_dense, kW, 180, 130, 120, 90);
            Check(var_clear > var_dense * 2.0,
                  "TileLiquidGlass frost reduces interior spatial variance smoothly");
        }

        // Multi-card slot reuse check (8 cards x 4 frames -> exactly 8 slots retained)
        {
            std::vector<uint8_t> multi_px;
            tile_glass.Reset();
            for (int frame = 0; frame < 4; ++frame) {
                gpu.RenderAndReadback(kW, kH, [&](ID2D1DeviceContext* dc) {
                    tile_glass.BeginFrame();
                    for (int i = 0; i < 8; ++i) {
                        const D2D1_RECT_F r = D2D1::RectF(
                            16.0f + (i % 4) * 120.0f, 16.0f + (i / 4) * 170.0f,
                            124.0f + (i % 4) * 120.0f, 166.0f + (i / 4) * 170.0f);
                        tile_glass.DrawTile(gpu.d2d_factory.Get(), dc, sharp_bmp.Get(), kW, kH, kW, kH,
                                            0.0f, 0.0f, 1.0f, r, tile_cfg, 1.0f, true, -1.0f, -1.0f,
                                            frost_bmp.Get(), kW / 4u, kH / 4u);
                    }
                    tile_glass.EndFrame();
                }, multi_px);
            }
            Check(tile_glass.EffectSlotCount() == 8 && !tile_glass.Unavailable(),
                  "TileLiquidGlassRenderer reuses exactly 8 effect slots across multi-card frames with zero growth");
        }

        Check(draw_tile(sharp_bmp.Get(), tile_cfg, out_stripe_rgb),
              "TileLiquidGlass renders TahoeClearLight over high-contrast stripes");
        auto cfg_mono = tile_cfg;
        cfg_mono.enable_chromatic_aberration = false;
        Check(draw_tile(sharp_bmp.Get(), cfg_mono, out_stripe_mono),
              "TileLiquidGlass renders single-channel Snell refraction when dispersion disabled");

        int tile_refract_px = 0;
        int tile_max_rb_rgb = 0;
        int tile_max_rb_mono = 0;
        for (int y = 120; y < 240; ++y) {
            for (int x = 124; x < 146; ++x) {
                const size_t p = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
                if (std::abs(int(out_stripe_mono[p + 1]) - int(sharp_px[p + 1])) > 18) ++tile_refract_px;
                tile_max_rb_rgb = std::max(tile_max_rb_rgb, std::abs(int(out_stripe_rgb[p + 2]) - int(out_stripe_rgb[p + 0])));
                tile_max_rb_mono = std::max(tile_max_rb_mono, std::abs(int(out_stripe_mono[p + 2]) - int(out_stripe_mono[p + 0])));
            }
        }
        std::printf("TILE_GLASS_OPTICS refract_px=%d rb_rgb=%d rb_mono=%d\n",
                    tile_refract_px, tile_max_rb_rgb, tile_max_rb_mono);
        Check(tile_refract_px > 120,
              "TileLiquidGlass convex superellipse Snell bezel strongly refracts background stripes");
        Check(tile_max_rb_rgb >= 12 && tile_max_rb_mono <= 4,
              "TileLiquidGlass 3-channel prism dispersion splits R/B along bezel and vanishes when off");

        Check(color_bmp && draw_tile(color_bmp.Get(), cfg_no_tint, out_color),
              "TileLiquidGlass vibrancy check render");
        const int chroma_in = std::abs(int(color_px[mid + 0]) - int(color_px[mid + 2]));
        const int chroma_out = std::abs(int(out_color[mid + 0]) - int(out_color[mid + 2]));
        std::printf("TILE_GLASS_VIBRANCY chroma_in=%d chroma_out=%d\n", chroma_in, chroma_out);
        Check(chroma_out > chroma_in + 20,
              "TileLiquidGlass Apple Vibrancy (1.92x saturation) boosts backdrop chroma");

        render::TileHighlightSpring spring;
        Check(spring.Step(0.2f, 0.2f, 0.016f) && spring.Step(0.8f, 0.7f, 0.016f),
              "TileHighlightSpring tracks normalized cursor target");
        bool settled = false;
        for (int i = 0; i < 80; ++i) {
            if (!spring.Step(0.8f, 0.7f, 0.016f)) { settled = true; break; }
        }
        Check(settled && std::fabs(spring.x - 0.8f) < 0.01f && std::fabs(spring.y - 0.7f) < 0.01f,
              "TileHighlightSpring converges cleanly to cursor target and stops ticking when idle");

        // Native 1/4-resolution source sampling (matches 960x540 video glass texture on 4K without
        // D2D12DAffineTransform upscale intermediates) + GPU cursor-tracking specular rim verification.
        {
            const uint32_t qw = kW / 4u;
            const uint32_t qh = kH / 4u;
            std::vector<uint8_t> q_white(qw * qh * 4u, 220u);
            for (size_t i = 3; i < q_white.size(); i += 4) q_white[i] = 255u;
            auto q_white_bmp = gpu.UploadPbgra(qw, qh, q_white);

            std::vector<uint8_t> out_q_white, out_q_rim_left, out_q_rim_right;
            auto draw_q = [&](const render::TileGlassConfig& cfg, bool rim, float lx, float ly, std::vector<uint8_t>& out) {
                return gpu.RenderAndReadback(kW, kH, [&](ID2D1DeviceContext* dc) {
                    tile_glass.BeginFrame();
                    tile_glass.DrawTile(gpu.d2d_factory.Get(), dc, q_white_bmp.Get(), qw, qh, kW, kH, 0.0f, 0.0f, 1.0f,
                                        tile_rc, cfg, 1.0f, rim, lx, ly, frost_bmp.Get(), qw, qh);
                    tile_glass.EndFrame();
                }, out);
            };

            Check(q_white_bmp && draw_q(cfg_no_tint, false, -1.0f, -1.0f, out_q_white),
                  "TileLiquidGlass renders native 1/4-res sharp texture without affine upscale pass");
            const size_t qmid = (static_cast<size_t>(180) * kW + 240u) * 4u;
            Check(out_q_white[qmid + 0] >= 205 && out_q_white[qmid + 1] >= 205 &&
                  out_q_white[qmid + 2] >= 205 && out_q_white[qmid + 3] >= 250,
                  "TileLiquidGlass 1/4-res native sampling preserves neutral white luminance");

            // Dark backdrop + GPU specular rim: cursor near top-left corner must brighten top-left
            // perimeter distinctly more than when cursor moves to top-right corner.
            std::vector<uint8_t> q_dark(qw * qh * 4u, 36u);
            for (size_t i = 3; i < q_dark.size(); i += 4) q_dark[i] = 255u;
            auto q_dark_bmp = gpu.UploadPbgra(qw, qh, q_dark);

            auto cfg_rim = tile_cfg;
            cfg_rim.dark_appearance = true;
            cfg_rim.tint = D2D1::ColorF(0.05f, 0.06f, 0.09f, 0.55f);
            cfg_rim.enable_drop_shadow = false;
            auto draw_q_dark = [&](float lx, float ly, std::vector<uint8_t>& out) {
                return gpu.RenderAndReadback(kW, kH, [&](ID2D1DeviceContext* dc) {
                    tile_glass.BeginFrame();
                    tile_glass.DrawTile(gpu.d2d_factory.Get(), dc, q_dark_bmp.Get(), qw, qh, kW, kH, 0.0f, 0.0f, 1.0f,
                                        tile_rc, cfg_rim, 1.0f, true, lx, ly, frost_bmp.Get(), qw, qh);
                    tile_glass.EndFrame();
                }, out);
            };
            Check(q_dark_bmp && draw_q_dark(0.05f, 0.05f, out_q_rim_left) &&
                  draw_q_dark(0.95f, 0.05f, out_q_rim_right),
                  "TileLiquidGlass GPU specular rim renders with cursor light coordinates");
            const size_t tl_edge = (static_cast<size_t>(85) * kW + 125u) * 4u; // top-left rounded corner perimeter
            const int l_tl = out_q_rim_left[tl_edge + 1];
            const int r_tl = out_q_rim_right[tl_edge + 1];
            std::printf("TILE_GLASS_GPU_RIM tl_left=%d tl_right=%d\n", l_tl, r_tl);
            Check(l_tl > r_tl + 20,
                  "TileLiquidGlass GPU specular rim tracks cursor light along card perimeter");
        }

        // Regression: overwriting a wallpaper file at the SAME path (like Windows TranscodedWallpaper)
        // must invalidate cached wallpaper_pixels_ and glass_sharp_ so Liquid Glass resamples the new image.
        {
            wchar_t tmp_dir[MAX_PATH]{};
            GetTempPathW(ARRAYSIZE(tmp_dir), tmp_dir);
            const std::wstring wp_path = std::wstring(tmp_dir) + L"pulse_lg_samepath_test.bmp";
            auto write_solid_bmp = [&](uint8_t r, uint8_t g, uint8_t b, int w, int h) {
                const uint32_t row_bytes = static_cast<uint32_t>(w * 4);
                const uint32_t img_bytes = row_bytes * static_cast<uint32_t>(h);
                const uint32_t file_bytes = 54u + img_bytes;
                std::vector<uint8_t> buf(file_bytes, 0u);
                buf[0] = 'B'; buf[1] = 'M';
                std::memcpy(buf.data() + 2, &file_bytes, 4);
                const uint32_t off = 54u;
                std::memcpy(buf.data() + 10, &off, 4);
                const uint32_t bi_size = 40u;
                std::memcpy(buf.data() + 14, &bi_size, 4);
                std::memcpy(buf.data() + 18, &w, 4);
                std::memcpy(buf.data() + 22, &h, 4);
                const uint16_t planes = 1u, bpp = 32u;
                std::memcpy(buf.data() + 26, &planes, 2);
                std::memcpy(buf.data() + 28, &bpp, 2);
                std::memcpy(buf.data() + 34, &img_bytes, 4);
                for (size_t i = 54; i < buf.size(); i += 4) {
                    buf[i + 0] = b; buf[i + 1] = g; buf[i + 2] = r; buf[i + 3] = 255u;
                }
                std::ofstream f(wp_path, std::ios::binary | std::ios::trunc);
                f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
            };

            render::WallpaperBackdrop wp_backdrop;
            write_solid_bmp(20, 60, 210, 64, 64); // Blue wallpaper
            Check(wp_backdrop.LoadForScreen(gpu.dc.Get(), 512, 384, model::WallpaperPreset::System, true, wp_path),
                  "WallpaperBackdrop loads initial blue wallpaper");
            uint32_t sw = 0, sh = 0;
            // Hold a COM ref to bmp1 so Windows heap allocator cannot recycle the exact pointer address
            // after ReleaseGlassSource() frees the old bitmap.
            ComPtr<ID2D1Bitmap1> bmp1 = wp_backdrop.GlassSharpBitmap(gpu.dc.Get(), true, sw, sh);
            const auto s1 = wp_backdrop.SampleColors(D2D1::RectF(100, 100, 200, 200));

            // Overwrite the exact same file path with an orange wallpaper (different size + mtime)
            write_solid_bmp(240, 120, 25, 80, 64);
            Check(wp_backdrop.LoadForScreen(gpu.dc.Get(), 512, 384, model::WallpaperPreset::System, true, wp_path),
                  "WallpaperBackdrop reloads overwritten same-path orange wallpaper");
            ComPtr<ID2D1Bitmap1> bmp2 = wp_backdrop.GlassSharpBitmap(gpu.dc.Get(), true, sw, sh);
            const auto s2 = wp_backdrop.SampleColors(D2D1::RectF(100, 100, 200, 200));

            const bool color_updated =
                s1.count > 0 && s2.count > 0 &&
                (s1.colors[0].b > s1.colors[0].r + 0.3f) &&
                (s2.colors[0].r > s2.colors[0].b + 0.3f);
            std::printf("SAMEPATH_WP s1=(%.2f,%.2f,%.2f) s2=(%.2f,%.2f,%.2f) bmp_changed=%d\n",
                        s1.count ? s1.colors[0].r : -1.0f, s1.count ? s1.colors[0].g : -1.0f, s1.count ? s1.colors[0].b : -1.0f,
                        s2.count ? s2.colors[0].r : -1.0f, s2.count ? s2.colors[0].g : -1.0f, s2.count ? s2.colors[0].b : -1.0f,
                        (bmp1.Get() != bmp2.Get()) ? 1 : 0);
            Check(bmp1 != nullptr && bmp2 != nullptr && bmp1.Get() != bmp2.Get() && color_updated,
                  "Same-path wallpaper overwrite invalidates glass_sharp_ and updates sampled backdrop color");
            DeleteFileW(wp_path.c_str());
        }

        // Real-time live window capture regression: verify LiveWallpaperSource captures a live
        // dynamic Win32 window hosted on its own thread (orange -> green live update), releases
        // immediately when deactivated, and guards against all-black Explorer desktop-host frames.
        {
            static std::atomic<COLORREF> g_live_test_color{RGB(245, 110, 20)};
            std::atomic<HWND> live_wnd_atomic{nullptr};
            std::atomic<bool> ready{false};
            std::thread wnd_thread([&]() {
                WNDCLASSW wc{};
                wc.lpfnWndProc = [](HWND hw, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                    if (msg == WM_PAINT) {
                        PAINTSTRUCT ps{};
                        HDC hdc = BeginPaint(hw, &ps);
                        HBRUSH br = CreateSolidBrush(g_live_test_color.load(std::memory_order_acquire));
                        FillRect(hdc, &ps.rcPaint, br);
                        DeleteObject(br);
                        EndPaint(hw, &ps);
                        return 0;
                    }
                    if (msg == WM_ERASEBKGND) return 1;
                    if (msg == WM_CLOSE) { DestroyWindow(hw); return 0; }
                    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
                    return DefWindowProcW(hw, msg, wp, lp);
                };
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = L"PulseLiveGlassTestSourceWnd";
                RegisterClassW(&wc);

                HWND hw = CreateWindowExW(
                    0, wc.lpszClassName, L"LiveSourceTest",
                    WS_POPUP | WS_VISIBLE, 40, 40, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
                if (hw) {
                    ShowWindow(hw, SW_SHOWNOACTIVATE);
                    UpdateWindow(hw);
                }
                live_wnd_atomic.store(hw, std::memory_order_release);
                ready.store(true, std::memory_order_release);
                MSG msg{};
                while (hw && GetMessageW(&msg, nullptr, 0, 0) > 0) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                UnregisterClassW(wc.lpszClassName, wc.hInstance);
            });

            while (!ready.load(std::memory_order_acquire)) Sleep(5);
            HWND live_wnd = live_wnd_atomic.load(std::memory_order_acquire);
            if (live_wnd) {
                RECT wrc{};
                GetWindowRect(live_wnd, &wrc);

                auto read_sharp_center = [&](ID2D1Bitmap1* bmp, uint32_t w, uint32_t h,
                                             uint8_t& out_r, uint8_t& out_g, uint8_t& out_b) -> bool {
                    if (!bmp || w < 4 || h < 4) return false;
                    ComPtr<IDXGISurface> surf;
                    if (FAILED(bmp->GetSurface(&surf)) || !surf) return false;
                    ComPtr<ID3D11Texture2D> tex;
                    if (FAILED(surf.As(&tex)) || !tex) return false;
                    D3D11_TEXTURE2D_DESC td{};
                    tex->GetDesc(&td);
                    D3D11_TEXTURE2D_DESC sd{};
                    sd.Width = 1; sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1;
                    sd.Format = td.Format; sd.SampleDesc.Count = 1;
                    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    ComPtr<ID3D11Texture2D> stg;
                    if (FAILED(gpu.d3d->CreateTexture2D(&sd, nullptr, &stg))) return false;
                    ComPtr<ID3D11DeviceContext> d3d_ctx;
                    gpu.d3d->GetImmediateContext(&d3d_ctx);
                    if (!d3d_ctx) return false;
                    const UINT cx = td.Width / 2u, cy = td.Height / 2u;
                    const D3D11_BOX box{cx, cy, 0, cx + 1u, cy + 1u, 1};
                    d3d_ctx->CopySubresourceRegion(stg.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
                    D3D11_MAPPED_SUBRESOURCE map{};
                    if (FAILED(d3d_ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &map)) || !map.pData) return false;
                    const auto* p = static_cast<const uint8_t*>(map.pData);
                    out_b = p[0]; out_g = p[1]; out_r = p[2];
                    d3d_ctx->Unmap(stg.Get(), 0);
                    return true;
                };

                render::LiveWallpaperSource live_src;
                host::LiveWallpaperDecision dec{};
                dec.active = true;
                dec.wallpaper_hwnd = reinterpret_cast<uintptr_t>(live_wnd);
                dec.capture_target_hwnd = reinterpret_cast<uintptr_t>(live_wnd);
                dec.wallpaper_pid = GetCurrentProcessId();
                dec.is_desktop_host = false;

                live_src.SyncState(gpu.d3d.Get(), gpu.dc.Get(), dec, wrc, 2, true, false);
                bool got_orange = false;
                uint8_t r1 = 0, g1 = 0, b1 = 0;
                for (int i = 0; i < 40 && !got_orange; ++i) {
                    if (live_src.ConsumePendingFrame(gpu.d3d.Get(), gpu.dc.Get(), true, {}) && live_src.HasFrame()) {
                        if (read_sharp_center(live_src.SharpBitmap(), live_src.SharpWidth(), live_src.SharpHeight(), r1, g1, b1) &&
                            r1 > static_cast<uint8_t>(std::min(255, static_cast<int>(b1) + 120))) {
                            got_orange = true;
                        }
                    }
                    Sleep(20);
                }

                // Repaint live window to bright green (simulating live video frame transition)
                g_live_test_color.store(RGB(30, 220, 70), std::memory_order_release);
                InvalidateRect(live_wnd, nullptr, TRUE);
                bool got_green = false;
                uint8_t r2 = 0, g2 = 0, b2 = 0;
                for (int i = 0; i < 40 && !got_green; ++i) {
                    if (live_src.ConsumePendingFrame(gpu.d3d.Get(), gpu.dc.Get(), true, {}) && live_src.HasFrame()) {
                        if (read_sharp_center(live_src.SharpBitmap(), live_src.SharpWidth(), live_src.SharpHeight(), r2, g2, b2) &&
                            g2 > static_cast<uint8_t>(std::min(255, static_cast<int>(r2) + 100))) {
                            got_green = true;
                        }
                    }
                    Sleep(20);
                }

                const auto st = live_src.Stats();
                std::printf("LIVE_CAPTURE backend=%u frames=%llu f1=(%u,%u,%u) f2=(%u,%u,%u) frost=%d\n",
                            static_cast<unsigned>(st.backend),
                            static_cast<unsigned long long>(st.frames_imported),
                            r1, g1, b1, r2, g2, b2, live_src.FrostBitmap() != nullptr ? 1 : 0);
                Check(got_orange && got_green && live_src.FrostBitmap() != nullptr,
                      "LiveWallpaperSource streams live animated window frames (orange -> green) with quarter frost");

                // Deactivating live decision releases frame immediately (no stale frame lingers)
                live_src.SyncState(gpu.d3d.Get(), gpu.dc.Get(), {}, wrc, 2, true, false);
                Check(!live_src.HasFrame(),
                      "LiveWallpaperSource releases stale frame immediately when live decision deactivates");

                // Desktop-host black-frame guard: pure black window with is_desktop_host=true must NOT expose black frames
                g_live_test_color.store(RGB(0, 0, 0), std::memory_order_release);
                InvalidateRect(live_wnd, nullptr, TRUE);
                dec.is_desktop_host = true;
                live_src.SyncState(gpu.d3d.Get(), gpu.dc.Get(), dec, wrc, 2, true, false);
                for (int i = 0; i < 20; ++i) {
                    live_src.ConsumePendingFrame(gpu.d3d.Get(), gpu.dc.Get(), false, {});
                    Sleep(20);
                }
                Check(!live_src.HasFrame(),
                      "LiveWallpaperSource desktop-host 9-point black-frame guard blocks all-black desktop captures");

                live_src.ReleaseDeviceResources();
                PostMessageW(live_wnd, WM_CLOSE, 0, 0);
            }
            if (wnd_thread.joinable()) wnd_thread.join();
        }
    }
}

uint64_t PrivateWorkingSetBytes() {
    PSAPI_WORKING_SET_INFORMATION wsi_probe{};
    QueryWorkingSet(GetCurrentProcess(), &wsi_probe, sizeof(wsi_probe));
    std::vector<ULONG_PTR> buf(static_cast<size_t>(wsi_probe.NumberOfEntries) + 512u);
    uint64_t priv_ws = 0;
    if (QueryWorkingSet(GetCurrentProcess(), buf.data(), static_cast<DWORD>(buf.size() * sizeof(ULONG_PTR)))) {
        const ULONG_PTR count = buf[0];
        uint64_t priv_pages = 0;
        for (ULONG_PTR i = 1; i <= count && i < buf.size(); ++i) {
            if ((buf[i] & 0x100u) == 0) ++priv_pages;
        }
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        priv_ws = priv_pages * si.dwPageSize;
    }
    return priv_ws;
}

void RunRenderer4KBudgetCheck() {
    const uint64_t before = PrivateWorkingSetBytes();
    render::DesktopRenderer renderer;
    if (!renderer.InitializeOffscreen(3840, 2160)) {
        Check(false, "InitializeOffscreen(3840, 2160) for LiquidGlass budget");
        return;
    }
    renderer.Backdrop().SetUseVectorIconsOnly(true);

    model::DesktopModelState state{};
    const model::RectI work{0, 0, 3840, 2112};
    state.monitors.push_back({L"\\\\.\\DISPLAY1", {0, 0, 3840, 2160}, work, 144, true});
    state.settings.default_material = model::CardMaterial::LiquidGlass;
    model::ApplyGlassPreset(static_cast<uint8_t>(model::GlassPreset::ClearDroplet), state.settings);
    state.groups = model::MakeDefaultGroups(work);
    state.tiles = model::MakeDefaultTiles();
    const model::RectI board = render::DesktopRenderer::ComputeBoardRectFallback(work, true);
    const auto gvs = render::DesktopRenderer::BuildSimpleGroupViews(state);
    render::LiveTileRuntimeData live{};
    render::DesktopInteractionState inter{};

    for (int i = 0; i < 4; ++i) {
        renderer.InvalidateAll();
        renderer.Tick(state, gvs, board, live, inter);
    }
    const uint64_t before_idle = renderer.Metrics().total_presents;
    for (int i = 0; i < 12; ++i) {
        renderer.Tick(state, gvs, board, live, inter);
    }
    const uint64_t idle_presents = renderer.Metrics().total_presents - before_idle;
    renderer.TrimDxgiMemory();

    const uint64_t priv_ws = PrivateWorkingSetBytes();
    Check(idle_presents == 0, "G7 hardware check: 0 Presents across 12 idle ticks in LiquidGlass mode");
    std::printf("LIQUID_GLASS_4K_PRIVATE_WS bytes=%llu budget_bytes=%llu\n",
                static_cast<unsigned long long>(priv_ws), 25ull * 1024ull * 1024ull);
    Check(priv_ws > 0 && priv_ws < 25ull * 1024ull * 1024ull,
          "G7 hardware check: 4K LiquidGlass private working set < 25 MB");
    renderer.Shutdown();
    std::printf("LIQUID_GLASS_4K_LIFETIME before=%llu active=%llu after_shutdown=%llu\n",
                static_cast<unsigned long long>(before), static_cast<unsigned long long>(priv_ws),
                static_cast<unsigned long long>(PrivateWorkingSetBytes()));
}

} // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    GpuHarness gpu;
    Check(gpu.Init(), "initialize D3D11 + D2D1.1 for WacOS TileLiquidGlass");
    if (gpu.dc) {
        RunHardwareEffectChecks(gpu);
        RunRenderer4KBudgetCheck();
        if (GetEnvironmentVariableW(L"PULSE_GLASS_REPEAT_BUDGET", nullptr, 0)) RunRenderer4KBudgetCheck();
    }
    CoUninitialize();
    std::printf("LIQUID_GLASS_GPU_RESULT passed=%d failed=%d\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
