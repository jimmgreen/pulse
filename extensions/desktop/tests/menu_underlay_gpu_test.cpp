#include "host/render/desktop_renderer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pulse::desktop;

using Microsoft::WRL::ComPtr;
struct MenuDevice {
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID3D11Texture2D> texture, staging;
    ComPtr<ID2D1Bitmap1> target;
    bool Initialize(ID3D11Device* source, bool same_device = false) {
        ComPtr<IDXGIDevice> source_dxgi, dxgi;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(source->QueryInterface(IID_PPV_ARGS(&source_dxgi))) ||
            FAILED(source_dxgi->GetAdapter(&adapter))) return false;
        D3D_FEATURE_LEVEL level{};
        if (same_device) {
            d3d = source;
            d3d->GetImmediateContext(&context);
        } else if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, &level, &context))) return false;
        if (FAILED(d3d.As(&dxgi)) ||
            FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) ||
            FAILED(factory->CreateDevice(dxgi.Get(), &device)) ||
            FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return false;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 240; desc.Height = 180; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(d3d->CreateTexture2D(&desc, nullptr, &texture))) return false;
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(d3d->CreateTexture2D(&desc, nullptr, &staging))) return false;
        ComPtr<IDXGISurface> surface;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(texture.As(&surface)) ||
            FAILED(dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target))) return false;
        dc->SetTarget(target.Get());
        dc->SetDpi(96, 96);
        return true;
    }
    bool ReadPixels(std::vector<uint8_t>& result) {
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
        result.resize(240 * 180 * 4);
        for (uint32_t y = 0; y < 180; ++y)
            std::memcpy(result.data() + y * 240 * 4,
                static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch, 240 * 4);
        context->Unmap(staging.Get(), 0);
        return true;
    }
};

#ifndef PULSE_MENU_UNDERLAY_BASELINE
namespace pulse::desktop::render {
struct WallpaperBackdropTestAccess {
    static bool HasFrost(const WallpaperBackdrop& backdrop) { return !!backdrop.menu_underlay_frost_; }
    static ID3D11DeviceContext1* BakeContext(const WallpaperBackdrop& backdrop) {
        return backdrop.menu_underlay_bake_context_.Get();
    }
    static ID3D11PixelShader* Shader(const WallpaperBackdrop& backdrop) {
        return backdrop.menu_underlay_pixel_shader_.Get();
    }
    static bool BakeReleased(const WallpaperBackdrop& backdrop) {
        return !backdrop.menu_underlay_bake_context_ && !backdrop.menu_underlay_bake_state_ &&
            !backdrop.menu_underlay_vertex_shader_ && !backdrop.menu_underlay_pixel_shader_ &&
            !backdrop.menu_underlay_constants_ &&
            !backdrop.menu_underlay_scratch_[0] && !backdrop.menu_underlay_scratch_[1] &&
            !backdrop.menu_underlay_scratch_texture_[0] && !backdrop.menu_underlay_scratch_texture_[1] &&
            !backdrop.menu_underlay_scratch_srv_[0] && !backdrop.menu_underlay_scratch_srv_[1] &&
            !backdrop.menu_underlay_scratch_rtv_[0] && !backdrop.menu_underlay_scratch_rtv_[1];
    }
    static bool FailFrostAllocation(WallpaperBackdrop& backdrop, ID2D1DeviceContext* dc) {
        const auto width = backdrop.menu_underlay_w_;
        backdrop.menu_underlay_frost_.Reset();
        backdrop.menu_underlay_w_ = 0;
        const bool ready = backdrop.EnsureMenuUnderlay(dc);
        backdrop.menu_underlay_w_ = width;
        return !ready && backdrop.MenuUnderlayPending() && !backdrop.menu_underlay_frost_;
    }
    static RECT Bounds(const WallpaperBackdrop& backdrop) { return backdrop.menu_underlay_screen_px_; }
    static ComPtr<IDXGIKeyedMutex> SharedMutex(const WallpaperBackdrop& backdrop) {
        ComPtr<IDXGIKeyedMutex> mutex;
        if (backdrop.menu_underlay_shared_) backdrop.menu_underlay_shared_.As(&mutex);
        return mutex;
    }
};
}
#endif

int main(int argc, char** argv) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    const bool same_device_first = argc >= 3 && std::string(argv[2]) == "--same-device-first";
    std::printf("MODE first_menu_device=%s\n", same_device_first ? "same" : "cross");
    int failures = 0;
    auto check = [&](bool pass, const char* label) {
        std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
        if (!pass) ++failures;
    };
    {
        render::DesktopRenderer desktop;
        MenuDevice menu, replacement, same_device;
        const bool ready = desktop.InitializeOffscreen(800, 600) &&
            menu.Initialize(desktop.D3DDevice(), same_device_first) && replacement.Initialize(desktop.D3DDevice()) &&
            same_device.Initialize(desktop.D3DDevice(), true);
        check(ready, "three independent hardware renderer devices initialize");
        if (!ready) return 3;
        check((desktop.D3DDevice() == menu.d3d.Get()) == same_device_first,
            "first menu device identity matches requested same/cross fixture");
        desktop.SetSurfaceGeometry(96, {0, 0, 800, 600}, {0, 0, 800, 600});
        check(desktop.ReloadWallpaper(model::WallpaperPreset::Bloom, false), "load deterministic Bloom wallpaper");
        model::DesktopModelState state;
        state.settings.default_material = model::CardMaterial::LiquidGlass;
        state.settings.organize_enabled = true;
        state.settings.tiles_enabled = false;
        state.settings.icons_hidden = false;
        state.settings.dark_mode = false;
        state.settings.wallpaper_preset = model::WallpaperPreset::Bloom;
        render::DesktopInteractionState interaction;
        interaction.banner_suppressed = true;
        interaction.host_dark_mode = false;
        model::DesktopGroup group;
        group.id = "underlay"; group.title = L"UNDERLAY CARD";
        group.material = model::CardMaterial::Solid;
        group.x = 80; group.y = 80; group.cols = 4; group.visible_rows = 2;
        model::GroupComputedView view;
        view.group_id = group.id; view.frame = {80, 80, 420, 300};
        auto capture = [&] {
            const std::vector<model::GroupComputedView> views = state.groups.empty() ?
                std::vector<model::GroupComputedView>{} : std::vector<model::GroupComputedView>{view};
            desktop.CaptureMenuUnderlay({100, 100}, state, views, {}, {}, interaction);
        };
        unsigned pending_frames = 0;
        const auto pending = [&] {
#ifdef PULSE_MENU_UNDERLAY_BASELINE
            return false;
#else
            return desktop.Backdrop().MenuUnderlayPending();
#endif
        };
        double last_paint_ms = 0, last_readback_ms = 0;
        auto pixels = [&](MenuDevice& target) {
            std::vector<uint8_t> result;
            const auto paint_start = std::chrono::steady_clock::now();
            target.dc->BeginDraw();
            target.dc->Clear(D2D1::ColorF(0, 0));
            const bool drawn = desktop.Backdrop().DrawMenuMaterial(target.dc.Get(), D2D1::RectF(0, 0, 240, 180),
                {110, 110}, 12, model::CardMaterial::LiquidGlass, false, 78, 1, &state.settings);
            const HRESULT hr = target.dc->EndDraw();
            const auto paint_stop = std::chrono::steady_clock::now();
            if (pending()) ++pending_frames;
            const bool mapped = SUCCEEDED(hr) && target.ReadPixels(result);
            const auto readback_stop = std::chrono::steady_clock::now();
            last_paint_ms = std::chrono::duration<double, std::milli>(paint_stop - paint_start).count();
            last_readback_ms = std::chrono::duration<double, std::milli>(readback_stop - paint_stop).count();
            check(mapped && drawn, "DrawMenuMaterial completes and GPU pixels read back");
            return result;
        };
        auto difference = [](const auto& a, const auto& b) {
            if (a.size() != b.size() || a.empty()) return -1.0;
            double sum = 0;
            for (size_t i = 0; i < a.size(); ++i) sum += std::abs(int(a[i]) - int(b[i]));
            return sum / a.size();
        };
        const auto cold_start = std::chrono::steady_clock::now();
        capture();
        const auto cold_capture_stop = std::chrono::steady_clock::now();
        auto wallpaper = pixels(menu);
        const double first_paint_ms = last_paint_ms, first_readback_ms = last_readback_ms;
        unsigned cold_retries = 0;
        while (pending() && cold_retries < 16) {
            ++cold_retries;
            Sleep(1);
            wallpaper = pixels(menu);
        }
        const auto cold_complete = std::chrono::steady_clock::now();
        check(!pending(), "cold first menu completes GPU underlay within bounded retries");
        std::printf("TIMING cold_capture_ms=%.6f cold_gpu_complete_ms=%.6f retries=%u\n",
            std::chrono::duration<double, std::milli>(cold_capture_stop - cold_start).count(),
            std::chrono::duration<double, std::milli>(cold_complete - cold_start).count(), cold_retries);
        std::printf("TIMING cold_first_menu_paint_enddraw_ms=%.6f readback_wait_ms=%.6f\n",
            first_paint_ms, first_readback_ms);
        state.groups.push_back(group);
        capture();
        pixels(menu);
        const auto card = pixels(menu);
        const double card_delta = difference(wallpaper, card);
        std::printf("PIXELS card_delta=%.6f\n", card_delta);
        check(card_delta > 1.0, "cross-device menu refracts underlying desktop card, not wallpaper only");
        pixels(replacement);
        const auto rebound = pixels(replacement);
        const double rebound_delta = difference(card, rebound);
        std::printf("PIXELS rebound_delta=%.6f\n", rebound_delta);
        check(rebound_delta >= 0 && rebound_delta < 0.05, "same snapshot survives menu device replacement");
        const auto same = pixels(same_device);
        const double same_delta = difference(card, same);
        std::printf("PIXELS same_device_delta=%.6f\n", same_delta);
        check(same_delta >= 0 && same_delta < 0.05, "same-device GPU view matches cross-device pixel content");
#ifndef PULSE_MENU_UNDERLAY_BASELINE
        ComPtr<ID3D11PixelShader> same_shader = render::WallpaperBackdropTestAccess::Shader(desktop.Backdrop());
        MenuDevice another_same;
        check(another_same.Initialize(desktop.D3DDevice(), true), "create replacement D2D DC on same D3D device");
        const auto same_rebound = pixels(another_same);
        check(render::WallpaperBackdropTestAccess::Shader(desktop.Backdrop()) == same_shader.Get() &&
            difference(same, same_rebound) == 0,
            "same-D3D device rebind preserves native shader and exact menu pixels");
        pixels(same_device);
        same_shader.Reset();
        check(render::WallpaperBackdropTestAccess::HasFrost(desktop.Backdrop()),
            "GPU frost bitmap successfully baked, not sharp fallback");
        check(render::WallpaperBackdropTestAccess::FailFrostAllocation(desktop.Backdrop(), same_device.dc.Get()),
            "real frost texture allocation failure defers material instead of sharp fallback");
        const auto recovered_frost = pixels(same_device);
        check(!pending() && render::WallpaperBackdropTestAccess::HasFrost(desktop.Backdrop()) &&
            difference(card, recovered_frost) == 0,
            "frost allocation retry recovers exact pixels without persistent degradation");
        auto held_mutex = render::WallpaperBackdropTestAccess::SharedMutex(desktop.Backdrop());
        const bool held = held_mutex && held_mutex->AcquireSync(1, 1000) == S_OK;
        check(held, "test holds immutable shared snapshot mutex to force pending import");
        if (held) {
            pixels(replacement);
            check(pending(), "busy keyed mutex defers cross-device import without waiting");
            std::printf("TIMING forced_pending_paint_ms=%.6f\n", last_paint_ms);
            check(SUCCEEDED(held_mutex->ReleaseSync(1)), "test releases snapshot mutex");
            for (int attempt = 0; attempt < 16 && pending(); ++attempt) { Sleep(1); pixels(replacement); }
            check(!pending(), "deferred cross-device import completes on repaint");
            check(difference(card, pixels(replacement)) < 0.05, "retry renders same card pixels, not stale fallback");
        }
#endif
        const auto original_glass = state.settings;
        // Studio/reference mode uses glass_options rather than the legacy frost
        // slider; explicitly select its supported custom legacy recipe here.
        state.settings.glass_preset = 4;
        state.settings.glass_reference = false;
        state.settings.glass_frost = 4;
        const auto low_frost = pixels(same_device);
        state.settings.glass_frost = 100;
        const auto frosted = pixels(same_device);
        const double frost_delta = difference(low_frost, frosted);
        std::printf("PIXELS frost_100_delta=%.6f\n", frost_delta);
        check(frost_delta > 0.01, "frost strength changes actual menu pixels");
        state.settings = original_glass;
        {
            // Right-clicked near the bottom-right corner: the root card shifts back inside the surface and its
            // submenu opens on its left, lined up with its row and only clamped to the work area. Every card of
            // that cascade must refract the desktop under it like the root card, not the wallpaper alone.
            const POINT corner_anchor{760, 560};
            auto submenu_pixels = [&](bool with_card) {
                const auto saved_groups = state.groups;
                state.groups.clear();
                if (with_card) state.groups.push_back(group);
                const std::vector<model::GroupComputedView> views = with_card ?
                    std::vector<model::GroupComputedView>{view} : std::vector<model::GroupComputedView>{};
                desktop.CaptureMenuUnderlay(corner_anchor, state, views, {}, {}, interaction);
                std::vector<uint8_t> result;
                for (int attempt = 0; attempt < 17; ++attempt) {
                    same_device.dc->BeginDraw();
                    same_device.dc->Clear(D2D1::ColorF(0, 0));
                    const bool drawn = desktop.Backdrop().DrawMenuMaterial(same_device.dc.Get(), D2D1::RectF(0, 0, 240, 180),
                        {110, 110}, 12, model::CardMaterial::LiquidGlass, false, 78, 1, &state.settings);
                    const bool ended = SUCCEEDED(same_device.dc->EndDraw());
                    if (!pending()) {
                        check(drawn && ended && same_device.ReadPixels(result), "far submenu card draws its material");
                        break;
                    }
                    Sleep(1);
                }
                state.groups = saved_groups;
                return result;
            };
            const auto bare = submenu_pixels(false);
            const auto over_card = submenu_pixels(true);
#ifndef PULSE_MENU_UNDERLAY_BASELINE
            // Two of the widest menu cards (320 DIP) inside the right edge, and the full height.
            const RECT bounds = render::WallpaperBackdropTestAccess::Bounds(desktop.Backdrop());
            std::printf("UNDERLAY corner_anchor_bounds=%ld,%ld,%ld,%ld\n", bounds.left, bounds.top, bounds.right, bounds.bottom);
            check(bounds.left <= 800 - 2 * 320 && bounds.right == 800 && bounds.top == 0 && bounds.bottom == 600,
                  "bottom-right menu snapshot covers the root card and a submenu flipped to its left");
#endif
            const double submenu_delta = difference(bare, over_card);
            std::printf("PIXELS corner_submenu_card_delta=%.6f\n", submenu_delta);
            check(submenu_delta > 1.0, "submenu far from a bottom-right anchor refracts the desktop card, not wallpaper only");
        }
        if (argc >= 2 && std::string(argv[1]) != "-") {
            std::filesystem::create_directories(argv[1]);
            auto save = [&](const char* name, const std::vector<uint8_t>& data) {
                std::ofstream stream(std::filesystem::path(argv[1]) / name, std::ios::binary);
                stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
                check(!!stream, "write 240x180 BGRA menu comparison pixels");
            };
            save("wallpaper.bgra", wallpaper); save("card.bgra", card);
            save("frost4.bgra", low_frost); save("frost100.bgra", frosted);
        }
        std::vector<double> latency, paint_latency, readback_latency, complete_latency;
#ifndef PULSE_MENU_UNDERLAY_BASELINE
        ComPtr<ID3D11DeviceContext1> previous_bake;
#endif
        for (int i = 0; i < 16; ++i) {
            const auto start = std::chrono::steady_clock::now();
            capture();
            const auto stop = std::chrono::steady_clock::now();
            latency.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
            pixels(menu);
            for (int retry = 0; pending() && retry < 16; ++retry) { Sleep(1); pixels(menu); }
            complete_latency.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count());
#ifndef PULSE_MENU_UNDERLAY_BASELINE
            auto* current_bake = render::WallpaperBackdropTestAccess::BakeContext(desktop.Backdrop());
            if (i == 0) previous_bake = current_bake;
            else check(current_bake == previous_bake.Get(), "same-DC captures reuse GPU frost bake pipeline");
#endif
            std::printf("SAMPLE menu_iteration=%d capture_ms=%.6f paint_ms=%.6f readback_ms=%.6f gpu_complete_ms=%.6f\n",
                i, latency.back(), last_paint_ms, last_readback_ms, complete_latency.back());
            paint_latency.push_back(last_paint_ms);
            readback_latency.push_back(last_readback_ms);
        }
        std::printf("TIMING device_rebind_gpu_complete_ms=%.6f\n", complete_latency.front());
        auto steady = std::vector<double>(complete_latency.begin() + 1, complete_latency.end());
        std::sort(steady.begin(), steady.end());
        std::printf("TIMING steady_gpu_complete_n=15 median_ms=%.6f p95_ms=%.6f\n", steady[7], steady[14]);
        std::sort(complete_latency.begin(), complete_latency.end());
        std::printf("TIMING warm_gpu_complete_n=16 median_ms=%.6f p95_ms=%.6f\n",
            complete_latency[8], complete_latency[15]);
        std::sort(paint_latency.begin(), paint_latency.end());
        std::sort(readback_latency.begin(), readback_latency.end());
        std::printf("TIMING first_paint_n=16 median_ms=%.6f p95_ms=%.6f readback_median_ms=%.6f readback_p95_ms=%.6f\n",
            paint_latency[8], paint_latency[15], readback_latency[8], readback_latency[15]);
        std::sort(latency.begin(), latency.end());
        std::printf("TIMING capture_n=16 median_ms=%.6f p95_ms=%.6f max_ms=%.6f\n",
            latency[8], latency[15], latency.back());
        desktop.Backdrop().ReleaseMenuMaterialCache();
        check(desktop.Backdrop().MenuCacheBytes() == 0, "menu teardown releases snapshot and GPU caches");
#ifndef PULSE_MENU_UNDERLAY_BASELINE
        previous_bake.Reset();
        check(render::WallpaperBackdropTestAccess::BakeReleased(desktop.Backdrop()),
            "menu teardown releases cached frost DC, effects and both scratch textures");
#endif
        pixels(menu);
        const auto cleared = pixels(menu);
        const double clear_delta = difference(card, cleared);
        std::printf("PIXELS cleared_delta=%.6f\n", clear_delta);
        check(clear_delta > 1.0, "cleared menu cache never reuses old card snapshot");
        desktop.Backdrop().ReleaseMenuMaterialCache();
        std::printf("PENDING observed_frames=%u remaining=%d\n", pending_frames,
            pending());
        check(!pending(), "cleanup clears pending import state");
    }
    CoUninitialize();
    std::printf("RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}

