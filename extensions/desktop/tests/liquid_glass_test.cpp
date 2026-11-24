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
#include "host/render/liquid_glass_effect.h"
#include "host/render/liquid_glass_params.h"
#include "host/render/live_wallpaper_source.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
        return render::EnsureLiquidGlassEffectRegistered(d2d_factory.Get());
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

    render::LiquidGlassPassPool pool;
    auto render_plan = [&](const render::GlassCardRequest& req, std::vector<uint8_t>& out) {
        const auto plan = render::MakeGlassDrawPlan(req, 1.0f);
        return gpu.RenderAndReadback(kW, kH, [&](ID2D1DeviceContext* dc) {
            dc->DrawBitmap(sharp_bmp.Get(), D2D1::RectF(0, 0, static_cast<float>(kW), static_cast<float>(kH)));
            pool.BeginFrame();
            pool.DrawCard(gpu.d2d_factory.Get(), dc,
                          sharp_bmp.Get(), kW, kH,
                          frost_bmp.Get(), kW / 4u, kH / 4u,
                          kW, kH, 0.0f, 0.0f, 0.0f, 0.0f, plan);
            pool.EndFrame();
        }, out);
    };

    render::GlassCardRequest base{};
    base.left_dip = 96.0f;
    base.top_dip = 64.0f;
    base.right_dip = 416.0f;
    base.bottom_dip = 320.0f;
    base.radius_dip = 20.0f;
    base.dark_mode = true;
    base.opacity_percent = 70;
    base.thickness_dip = 28.0f;
    base.ior = 1.52f;
    base.frost_percent = 0;
    base.dispersion_gain = 22.0f;

    std::vector<uint8_t> clear_disp2, clear_disp0, reg_px, dense_px;
    Check(render_plan(base, clear_disp2), "GPU render LiquidGlass Clear + High Dispersion");

    base.dispersion_gain = 0.0f;
    Check(render_plan(base, clear_disp0), "GPU render LiquidGlass Clear + Zero Dispersion");

    base.dispersion_gain = 9.0f;
    base.frost_percent = 30;
    Check(render_plan(base, reg_px), "GPU render LiquidGlass Regular (30% frost)");

    base.frost_percent = 100;
    Check(render_plan(base, dense_px), "GPU render LiquidGlass Frosted (100% frost)");

    // G1: Edge bezel refracts background vs unwarped background, while deep interior preserves structure
    int edge_diff_count = 0;
    for (int y = 120; y < 260; ++y) {
        for (int x = 100; x < 116; ++x) {
            const size_t p = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
            if (std::abs(int(clear_disp0[p + 1]) - int(sharp_px[p + 1])) > 18) ++edge_diff_count;
        }
    }
    Check(edge_diff_count > 120, "G1 hardware pixel check: meniscus bezel refracts background stripes");

    // G2: Spectral dispersion separates R and B channels on high-contrast edge, and is 0 when disabled
    int max_rb_disp2 = 0;
    int max_rb_disp0 = 0;
    for (int y = 120; y < 260; ++y) {
        for (int x = 98; x < 120; ++x) {
            const size_t p = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
            max_rb_disp2 = std::max(max_rb_disp2, std::abs(int(clear_disp2[p + 2]) - int(clear_disp2[p + 0])));
            max_rb_disp0 = std::max(max_rb_disp0, std::abs(int(clear_disp0[p + 2]) - int(clear_disp0[p + 0])));
        }
    }
    Check(max_rb_disp2 >= 12 && max_rb_disp0 <= 2,
          "G2 hardware pixel check: chromatic dispersion splits R/B at bezel and vanishes when off");

    // G3: Frost monotonicity across Clear -> Regular -> Dense in interior patch
    const double var_clear = PatchSpatialVariance(clear_disp0, kW, 180, 140, 140, 100);
    const double var_reg = PatchSpatialVariance(reg_px, kW, 180, 140, 140, 100);
    const double var_dense = PatchSpatialVariance(dense_px, kW, 180, 140, 140, 100);
    Check(var_clear > var_reg && var_reg > var_dense,
          "G3 hardware pixel check: interior spatial variance decreases monotonically with frost");

    // Studio defaults use the actual shader path, not the legacy optical preset.
    render::GlassCardRequest studio = base;
    studio.reference_mode = true;
    studio.thickness_dip = 20; studio.ior = 1.4f; studio.dispersion_gain = 7;
    studio.glare_percent = 90; studio.glare_angle_deg = -45;
    studio.blur_radius = 1; studio.blur_edge = true;
    std::vector<uint8_t> studio_default, studio_zero, studio_neutral;
    Check(render_plan(studio, studio_default), "Studio default actual GPU render");
    {
        std::ofstream pixels("bench_data/glass_reference_feedback/render/studio_default.bgra",std::ios::binary);
        pixels.write(reinterpret_cast<const char*>(studio_default.data()),static_cast<std::streamsize>(studio_default.size()));
    }

    int center_error = 0, studio_edge_changed = 0;
    for (int y = 160; y < 220; ++y) for (int x = 220; x < 300; ++x) {
        // Exclude the one-texel Gaussian neighborhood of source discontinuities.
        if (x % 12 < 2 || x % 12 > 9 || x % 16 < 2 || x % 16 > 13 || y % 16 < 2 || y % 16 > 13) continue;
        const size_t p = (static_cast<size_t>(y) * kW + x) * 4;
        center_error = std::max(center_error, std::abs(int(studio_default[p]) - int(sharp_px[p])));
    }
    for (int y = 120; y < 260; ++y) for (int x = 100; x < 114; ++x) {
        const size_t p = (static_cast<size_t>(y) * kW + x) * 4;
        studio_edge_changed += std::abs(int(studio_default[p+1]) - int(sharp_px[p+1])) > 12;
    }
    Check(center_error <= 2, "Studio default center preserves source without legacy saturation/tint");
    Check(studio_edge_changed > 120, "Studio default meniscus refracts actual background pixels");
    studio.glass_options.glare_factor = 0; studio.glass_options.dispersion = 0;
    Check(render_plan(studio, studio_zero), "Studio zero dispersion actual GPU render");
    int neutral_error = 0;
    for (int y = 120; y < 260; ++y) for (int x = 98; x < 120; ++x) {
        const size_t p = (static_cast<size_t>(y) * kW + x) * 4;
        neutral_error = std::max(neutral_error, std::abs(int(studio_zero[p+2]) - int(studio_zero[p])));
    }
    std::printf("STUDIO_GPU center_error=%d edge_changed=%d zero_dispersion_rb=%d\n", center_error, studio_edge_changed, neutral_error);
    Check(neutral_error <= 2, "Studio zero dispersion preserves neutral channels without angular glare");

    for (const auto& field : kGlassOpticalFields) {
        render::GlassCardRequest before = studio;
        before.glass_options = {};
        if (field.key.starts_with("glass2_tint_")) before.glass_options.tint_a = 40;
        auto after = before;
        after.glass_options.*(field.member) = field.min + (field.max-field.min)*0.7137;
        std::vector<uint8_t> first, second;
        const bool rendered = render_plan(before,first) && render_plan(after,second);
        size_t changed_pixels=0;
        if(rendered) for(size_t i=0;i<first.size();i+=4)
            changed_pixels += first[i]!=second[i] || first[i+1]!=second[i+1] || first[i+2]!=second[i+2];
        std::printf("STUDIO_CONTINUOUS key=%.*s value=%.8f changed_pixels=%zu\n",
            static_cast<int>(field.key.size()),field.key.data(),after.glass_options.*(field.member),changed_pixels);
        Check(rendered && changed_pixels>0,"continuous optical parameter reaches actual GPU pixels");
    }

    studio.glass_options = {};
    studio.glass_options.blur_radius = 20.375;
    std::vector<uint8_t> edge_blur, edge_sharp;
    bool edge_switch = render_plan(studio,edge_blur);
    studio.glass_options.blur_edge = false;
    edge_switch = edge_switch && render_plan(studio,edge_sharp);
    Check(edge_switch && edge_blur != edge_sharp,"continuous blur-edge toggle reaches actual GPU pixels");

    // Multi-card single-BeginDraw pool reuse check (8 cards x 4 frames -> exactly 8 slots retained)
    std::vector<uint8_t> multi_px;
    pool.Reset();
    for (int frame = 0; frame < 4; ++frame) {
        gpu.RenderAndReadback(kW, kH, [&](ID2D1DeviceContext* dc) {
            pool.BeginFrame();
            for (int i = 0; i < 8; ++i) {
                render::GlassCardRequest r = base;
                r.left_dip = 16.0f + (i % 4) * 120.0f;
                r.top_dip = 16.0f + (i / 4) * 170.0f;
                r.right_dip = 124.0f + (i % 4) * 120.0f;
                r.bottom_dip = 166.0f + (i / 4) * 170.0f;
                pool.DrawCard(gpu.d2d_factory.Get(), dc,
                              sharp_bmp.Get(), kW, kH,
                              frost_bmp.Get(), kW / 4u, kH / 4u,
                              kW, kH, 0.0f, 0.0f, 0.0f, 0.0f, render::MakeGlassDrawPlan(r, 1.0f));
            }
            pool.EndFrame();
        }, multi_px);
    }
    Check(pool.SlotCount() == 8 && !pool.Unavailable(),
          "LiquidGlassPassPool reuses exactly 8 effect slots across multi-card frames with zero growth");
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
    Check(gpu.Init(), "initialize D3D11 + D2D1.1 + CLSID_PulseLiquidGlass");
    if (gpu.dc) {
        RunHardwareEffectChecks(gpu);
        RunRenderer4KBudgetCheck();
        if (GetEnvironmentVariableW(L"PULSE_GLASS_REPEAT_BUDGET", nullptr, 0)) RunRenderer4KBudgetCheck();
    }
    CoUninitialize();
    std::printf("LIQUID_GLASS_GPU_RESULT passed=%d failed=%d\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
