// live_wallpaper_source.cpp - Zero-self-feedback GPU live wallpaper source (WGC + PrintWindow).
#include "host/render/live_wallpaper_source.h"
#include "host/render/text_contrast.h"
#include "../../../../src/common/os_version.h"
#include "../../../../src/ui/window_material.h"

#include <d2d1effects.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <roapi.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace pulse::desktop::render {
namespace {

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgdd = winrt::Windows::Graphics::DirectX::Direct3D11;
using ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

uint32_t IntervalMsForRate(uint8_t rate_mode, bool win10_fallback) noexcept {
    if (win10_fallback) {
        // Cap GDI PrintWindow path to <= 20 fps so CPU stays < 2% of one core.
        return rate_mode == 0 ? 100u : 50u;
    }
    switch (rate_mode) {
    case 0:  return 66u; // 15 fps (power-save)
    case 2:  return 16u; // 60 fps (smooth)
    case 1:
    default: return 33u; // 30 fps (balanced default)
    }
}

RECT QueryWindowCaptureBounds(HWND hwnd) noexcept {
    RECT rc{};
    if (!hwnd || !IsWindow(hwnd)) return rc;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc))) ||
        rc.right <= rc.left || rc.bottom <= rc.top) {
        GetWindowRect(hwnd, &rc);
    }
    return rc;
}

uint64_t SampleChecksum64(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride) noexcept {
    if (!bgra || w == 0 || h == 0) return 0;
    uint64_t hash = 14695981039346656037ull;
    bool all_zero = true;
    for (uint32_t gy = 0; gy < 8; ++gy) {
        const uint32_t y = ((2 * gy + 1) * h) / 16;
        const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
        for (uint32_t gx = 0; gx < 8; ++gx) {
            const uint32_t x = ((2 * gx + 1) * w) / 16;
            uint32_t px = 0;
            std::memcpy(&px, row + static_cast<size_t>(x) * 4u, 4u);
            px &= 0x00FFFFFFu;
            if (px != 0) all_zero = false;
            hash ^= static_cast<uint64_t>(px);
            hash *= 1099511628211ull;
        }
    }
    return all_zero ? 0ull : (hash | 1ull);
}

bool EnsureRoInitialized() noexcept {
    struct ThreadRo {
        HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
        ~ThreadRo() {
            if (SUCCEEDED(hr)) RoUninitialize();
        }
    };
    static thread_local ThreadRo ro;
    return SUCCEEDED(ro.hr) || ro.hr == RPC_E_CHANGED_MODE;
}

} // namespace

struct LiveWallpaperSource::Impl {
    HWND notify_hwnd = nullptr;
    UINT frame_msg = 0;

    ComPtr<ID3D11Device> owner_d3d;
    ComPtr<ID2D1DeviceContext> owner_dc;
    ComPtr<ID2D1DeviceContext> bake_dc;
    ComPtr<ID2D1Effect> scale_fx;
    ComPtr<ID2D1Effect> blur_fx;

    host::LiveWallpaperDecision decision{};
    RECT surface_screen_px{};
    uint8_t rate_mode = 1;
    bool running = false;
    bool paused_state = false;
    uint64_t inactive_since_ms = 0;

    LiveCaptureBackend backend = LiveCaptureBackend::None;

    // GPU retained textures (cropped to `surface_w x surface_h`, 1/1 or 1/2 res, and 1/4 frost)
    uint32_t surf_w = 0, surf_h = 0;
    uint32_t sharp_w = 0, sharp_h = 0;
    uint32_t frost_w = 0, frost_h = 0;
    ComPtr<ID3D11Texture2D> gpu_sharp_tex;
    ComPtr<ID2D1Bitmap1> sharp_bitmap;
    ComPtr<ID2D1Bitmap1> frost_bitmap;
    ComPtr<ID3D11Texture2D> lum_probe_gpu;
    ComPtr<ID3D11Texture2D> lum_probe_staging;
    float avg_luminance = -1.0f;
    uint64_t last_lum_sample_ms = 0;
    bool frost_valid = false;

    uint64_t frames_imported = 0;
    uint64_t frames_skipped = 0;

    // WGC state (Win11+)
    wgdd::IDirect3DDevice winrt_device{nullptr};
    wgc::GraphicsCaptureItem wgc_item{nullptr};
    wgc::Direct3D11CaptureFramePool wgc_pool{nullptr};
    wgc::GraphicsCaptureSession wgc_session{nullptr};
    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker wgc_arrived_revoker;
    winrt::Windows::Graphics::SizeInt32 wgc_pool_size{0, 0};
    bool wgc_permanently_denied = false;

    std::atomic<bool> frame_ready{false};
    std::atomic<uint64_t> last_post_tick_ms{0};
    std::atomic<uint32_t> min_interval_ms{33};

    // Fallback worker state (Win10 / WGC-denied)
    std::mutex fb_mtx;
    std::condition_variable fb_cv;
    std::thread fb_thread;
    bool fb_stop = false;
    uint32_t fb_w = 0, fb_h = 0;
    uint64_t fb_last_hash = 0;
    std::vector<uint8_t> fb_bgra_ready;
    bool fb_has_new = false;

    ~Impl() {
        StopCapture();
        ReleaseGpu();
    }

    void StopWgc() noexcept {
        try {
            wgc_arrived_revoker.revoke();
            if (wgc_session) { wgc_session.Close(); wgc_session = nullptr; }
            if (wgc_pool) { wgc_pool.Close(); wgc_pool = nullptr; }
            wgc_item = nullptr;
            winrt_device = nullptr;
        } catch (...) {}
        wgc_pool_size = {0, 0};
    }

    void StopFallbackWorker() noexcept {
        {
            std::lock_guard<std::mutex> lk(fb_mtx);
            fb_stop = true;
        }
        fb_cv.notify_all();
        if (fb_thread.joinable()) {
            fb_thread.join();
        }
        fb_stop = false;
        fb_has_new = false;
    }

    void StopCapture() noexcept {
        StopWgc();
        StopFallbackWorker();
        running = false;
        backend = LiveCaptureBackend::None;
        frame_ready.store(false, std::memory_order_relaxed);
    }

    void ReleaseGpu() noexcept {
        scale_fx.Reset();
        blur_fx.Reset();
        bake_dc.Reset();
        frost_bitmap.Reset();
        sharp_bitmap.Reset();
        gpu_sharp_tex.Reset();
        lum_probe_gpu.Reset();
        lum_probe_staging.Reset();
        owner_dc.Reset();
        owner_d3d.Reset();
        sharp_w = sharp_h = frost_w = frost_h = surf_w = surf_h = 0;
        frost_valid = false;
        avg_luminance = -1.0f;
    }

    void PostCoalescedFrameWake() noexcept {
        const uint64_t now = GetTickCount64();
        const uint64_t prev = last_post_tick_ms.load(std::memory_order_relaxed);
        const uint32_t interval = min_interval_ms.load(std::memory_order_relaxed);
        if (now - prev < interval) return;
        last_post_tick_ms.store(now, std::memory_order_relaxed);
        if (notify_hwnd && frame_msg != 0 && IsWindow(notify_hwnd)) {
            PostMessageW(notify_hwnd, frame_msg, 0, 0);
        }
    }

    bool TryStartWgc(ID3D11Device* d3d) noexcept {
        if (wgc_permanently_denied || pulse::os::TrueWindowsBuild() < 22000) {
            return false;
        }
        const HWND target = reinterpret_cast<HWND>(decision.capture_target_hwnd);
        if (!d3d || !target || !IsWindow(target) || GetSystemMetrics(SM_REMOTESESSION) != 0) {
            return false;
        }
        if (!EnsureRoInitialized()) return false;

        try {
            if (!wgc::GraphicsCaptureSession::IsSupported()) {
                return false;
            }
            ComPtr<IDXGIDevice> dxgi_dev;
            if (FAILED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi_dev))) || !dxgi_dev) {
                return false;
            }
            winrt::com_ptr<::IInspectable> inspectable_dev;
            if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi_dev.Get(), inspectable_dev.put())) ||
                !inspectable_dev) {
                return false;
            }
            winrt_device = inspectable_dev.as<wgdd::IDirect3DDevice>();

            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            wgc::GraphicsCaptureItem item{nullptr};
            winrt::check_hresult(interop->CreateForWindow(
                target, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                winrt::put_abi(item)));
            if (!item) return false;

            const auto item_sz = item.Size();
            if (item_sz.Width <= 0 || item_sz.Height <= 0) {
                return false;
            }

            // Best-effort borderless capability request (non-blocking).
            try {
                using winrt::Windows::Foundation::Metadata::ApiInformation;
                if (ApiInformation::IsTypePresent(L"Windows.Graphics.Capture.GraphicsCaptureAccess")) {
                    (void)wgc::GraphicsCaptureAccess::RequestAccessAsync(
                        wgc::GraphicsCaptureAccessKind::Borderless);
                }
            } catch (...) {}

            auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                winrt_device,
                wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                1,
                item_sz);
            auto session = pool.CreateCaptureSession(item);

            // Strict requirement: NEVER show a yellow border around the user's desktop!
            session.IsCursorCaptureEnabled(false);
            session.IsBorderRequired(false);

            try {
                using winrt::Windows::Foundation::Metadata::ApiInformation;
                if (ApiInformation::IsPropertyPresent(
                        L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
                    session.MinUpdateInterval(
                        std::chrono::milliseconds(min_interval_ms.load(std::memory_order_relaxed)));
                }
            } catch (...) {}

            wgc_item = item;
            wgc_pool = pool;
            wgc_session = session;
            wgc_pool_size = item_sz;

            wgc_arrived_revoker = wgc_pool.FrameArrived(
                winrt::auto_revoke,
                [this](wgc::Direct3D11CaptureFramePool const&, winrt::Windows::Foundation::IInspectable const&) {
                    frame_ready.store(true, std::memory_order_release);
                    PostCoalescedFrameWake();
                });

            wgc_session.StartCapture();
            backend = LiveCaptureBackend::WgcWindow;
            running = true;
            ui::LogWindowMaterial("liquid glass live WGC active target=%p size=%dx%d",
                                  target, item_sz.Width, item_sz.Height);
            return true;
        } catch (const winrt::hresult_error& err) {
            if (err.code() == E_ACCESSDENIED) {
                wgc_permanently_denied = true;
            }
            ui::LogWindowMaterial("liquid glass live WGC unavailable hr=0x%08lX; trying PrintWindow fallback",
                                  static_cast<unsigned long>(err.code().value));
        } catch (...) {
            ui::LogWindowMaterial("liquid glass live WGC unexpected error; trying PrintWindow fallback");
        }
        StopWgc();
        return false;
    }

    bool TryStartFallbackWorker() noexcept {
        if (GetSystemMetrics(SM_REMOTESESSION) != 0) return false;
        const HWND target = reinterpret_cast<HWND>(decision.capture_target_hwnd);
        const HWND wp_hwnd = reinterpret_cast<HWND>(decision.wallpaper_hwnd);
        if ((!target || !IsWindow(target)) && (!wp_hwnd || !IsWindow(wp_hwnd))) return false;

        const uint32_t sw = static_cast<uint32_t>(std::max<LONG>(64, surface_screen_px.right - surface_screen_px.left));
        const uint32_t sh = static_cast<uint32_t>(std::max<LONG>(64, surface_screen_px.bottom - surface_screen_px.top));
        // Half-resolution capture bounded to 1920x1080 keeps CPU < 1.5% on Windows 10.
        uint32_t cw = std::max(64u, sw / 2u);
        uint32_t ch = std::max(64u, sh / 2u);
        if (cw > 1920u) {
            ch = std::max(64u, static_cast<uint32_t>(static_cast<uint64_t>(ch) * 1920u / cw));
            cw = 1920u;
        }

        StopFallbackWorker();
        fb_w = cw;
        fb_h = ch;
        fb_last_hash = 0;
        min_interval_ms.store(IntervalMsForRate(rate_mode, true), std::memory_order_relaxed);

        try {
            fb_thread = std::thread([this, target, wp_hwnd, cw, ch]() {
                FallbackLoop(target, wp_hwnd, cw, ch);
            });
            backend = LiveCaptureBackend::PrintWindowFallback;
            running = true;
            ui::LogWindowMaterial("liquid glass live PrintWindow active target=%p cap=%ux%u",
                                  target ? target : wp_hwnd, cw, ch);
            return true;
        } catch (...) {
            return false;
        }
    }

    void FallbackLoop(HWND primary_target, HWND child_target, uint32_t cw, uint32_t ch) noexcept {
        HDC screen_dc = GetDC(nullptr);
        HDC full_dc = CreateCompatibleDC(screen_dc);
        HDC scaled_dc = CreateCompatibleDC(screen_dc);
        SetStretchBltMode(scaled_dc, HALFTONE);

        BITMAPINFO bmi_scaled{};
        bmi_scaled.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi_scaled.bmiHeader.biWidth = static_cast<LONG>(cw);
        bmi_scaled.bmiHeader.biHeight = -static_cast<LONG>(ch); // top-down
        bmi_scaled.bmiHeader.biPlanes = 1;
        bmi_scaled.bmiHeader.biBitCount = 32;
        bmi_scaled.bmiHeader.biCompression = BI_RGB;
        void* scaled_bits = nullptr;
        HBITMAP scaled_bmp = CreateDIBSection(screen_dc, &bmi_scaled, DIB_RGB_COLORS, &scaled_bits, nullptr, 0);
        HGDIOBJ old_scaled = scaled_bmp ? SelectObject(scaled_dc, scaled_bmp) : nullptr;

        HBITMAP full_bmp = nullptr;
        HGDIOBJ old_full = nullptr;
        int alloc_fw = 0, alloc_fh = 0;

        ReleaseDC(nullptr, screen_dc);

        auto capture_hwnd = [&](HWND h) -> bool {
            if (!h || !IsWindow(h) || !scaled_bmp || !scaled_bits) return false;
            const RECT wr = QueryWindowCaptureBounds(h);
            const int fw = wr.right - wr.left;
            const int fh = wr.bottom - wr.top;
            if (fw <= 16 || fh <= 16) return false;

            if (fw != alloc_fw || fh != alloc_fh || !full_bmp) {
                if (old_full) { SelectObject(full_dc, old_full); old_full = nullptr; }
                if (full_bmp) { DeleteObject(full_bmp); full_bmp = nullptr; }
                HDC sdc = GetDC(nullptr);
                full_bmp = CreateCompatibleBitmap(sdc, fw, fh);
                ReleaseDC(nullptr, sdc);
                if (!full_bmp) return false;
                old_full = SelectObject(full_dc, full_bmp);
                alloc_fw = fw;
                alloc_fh = fh;
            }

            if (!PrintWindow(h, full_dc, PW_RENDERFULLCONTENT)) return false;

            const int src_x = std::clamp<int>(surface_screen_px.left - wr.left, 0, std::max(0, fw - 1));
            const int src_y = std::clamp<int>(surface_screen_px.top - wr.top, 0, std::max(0, fh - 1));
            const int src_w = std::clamp<int>(surface_screen_px.right - surface_screen_px.left, 1, fw - src_x);
            const int src_h = std::clamp<int>(surface_screen_px.bottom - surface_screen_px.top, 1, fh - src_y);
            StretchBlt(scaled_dc, 0, 0, static_cast<int>(cw), static_cast<int>(ch),
                       full_dc, src_x, src_y, src_w, src_h, SRCCOPY);
            GdiFlush();
            return true;
        };

        while (true) {
            const uint32_t wait_ms = min_interval_ms.load(std::memory_order_relaxed);
            {
                std::unique_lock<std::mutex> lk(fb_mtx);
                if (fb_cv.wait_for(lk, std::chrono::milliseconds(wait_ms), [this] { return fb_stop; })) {
                    break;
                }
            }

            bool ok = capture_hwnd(primary_target);
            uint64_t hash = ok ? SampleChecksum64(static_cast<const uint8_t*>(scaled_bits), cw, ch, cw * 4u) : 0;
            if (hash == 0 && child_target && child_target != primary_target) {
                ok = capture_hwnd(child_target);
                hash = ok ? SampleChecksum64(static_cast<const uint8_t*>(scaled_bits), cw, ch, cw * 4u) : 0;
            }
            if (hash == 0) continue;
            if (hash == fb_last_hash) {
                ++frames_skipped;
                continue;
            }
            fb_last_hash = hash;

            const size_t byte_len = static_cast<size_t>(cw) * ch * 4u;
            uint8_t* p = static_cast<uint8_t*>(scaled_bits);
            for (size_t i = 3; i < byte_len; i += 4) {
                p[i] = 255u; // Force opaque alpha (GDI PrintWindow leaves alpha=0 on some drivers)
            }

            {
                std::lock_guard<std::mutex> lk(fb_mtx);
                fb_bgra_ready.assign(p, p + byte_len);
                fb_has_new = true;
            }
            frame_ready.store(true, std::memory_order_release);
            PostCoalescedFrameWake();
        }

        if (old_full) SelectObject(full_dc, old_full);
        if (full_bmp) DeleteObject(full_bmp);
        if (old_scaled) SelectObject(scaled_dc, old_scaled);
        if (scaled_bmp) DeleteObject(scaled_bmp);
        DeleteDC(scaled_dc);
        DeleteDC(full_dc);
    }

    bool EnsureGpuTargets(ID3D11Device* d3d, ID2D1DeviceContext* dc, uint32_t w, uint32_t h) noexcept {
        if (!d3d || !dc || w == 0 || h == 0) return false;
        if (owner_d3d.Get() != d3d || owner_dc.Get() != dc) {
            ReleaseGpu();
            owner_d3d = d3d;
            owner_dc = dc;
        }
        if (gpu_sharp_tex && sharp_bitmap && sharp_w == w && sharp_h == h) {
            return true;
        }

        frost_bitmap.Reset();
        sharp_bitmap.Reset();
        gpu_sharp_tex.Reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(d3d->CreateTexture2D(&td, nullptr, &gpu_sharp_tex)) || !gpu_sharp_tex) {
            return false;
        }

        ComPtr<IDXGISurface> dxgi_surf;
        if (FAILED(gpu_sharp_tex.As(&dxgi_surf)) || !dxgi_surf) return false;

        const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            96.0f, 96.0f);
        if (FAILED(dc->CreateBitmapFromDxgiSurface(dxgi_surf.Get(), &bp, &sharp_bitmap)) || !sharp_bitmap) {
            gpu_sharp_tex.Reset();
            return false;
        }

        sharp_w = w;
        sharp_h = h;
        frost_valid = false;
        return true;
    }

    void BakeQuarterFrost(ID2D1DeviceContext* dc) noexcept {
        if (!dc || !sharp_bitmap || sharp_w == 0 || sharp_h == 0) return;
        const uint32_t qw = std::max(16u, (sharp_w + 3u) / 4u);
        const uint32_t qh = std::max(16u, (sharp_h + 3u) / 4u);

        if (!bake_dc) {
            ComPtr<ID2D1Device> dev;
            dc->GetDevice(&dev);
            if (!dev || FAILED(dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &bake_dc))) {
                return;
            }
        }
        if (!frost_bitmap || frost_w != qw || frost_h != qh) {
            frost_bitmap.Reset();
            const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            if (FAILED(bake_dc->CreateBitmap(D2D1::SizeU(qw, qh), nullptr, 0, &bp, &frost_bitmap))) {
                return;
            }
            frost_w = qw;
            frost_h = qh;
        }
        if (!scale_fx) {
            if (FAILED(bake_dc->CreateEffect(CLSID_D2D1Scale, &scale_fx))) return;
            scale_fx->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE, D2D1_SCALE_INTERPOLATION_MODE_LINEAR);
            scale_fx->SetValue(D2D1_SCALE_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        }
        if (!blur_fx) {
            if (FAILED(bake_dc->CreateEffect(CLSID_D2D1GaussianBlur, &blur_fx))) return;
            blur_fx->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED);
            blur_fx->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
            blur_fx->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 6.2f);
        }

        scale_fx->SetInput(0, sharp_bitmap.Get());
        scale_fx->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(
            static_cast<float>(qw) / static_cast<float>(sharp_w),
            static_cast<float>(qh) / static_cast<float>(sharp_h)));
        ComPtr<ID2D1Image> scaled_out;
        scale_fx->GetOutput(&scaled_out);
        blur_fx->SetInput(0, scaled_out.Get());

        bake_dc->SetTarget(frost_bitmap.Get());
        bake_dc->SetDpi(96.0f, 96.0f);
        bake_dc->BeginDraw();
        bake_dc->Clear(D2D1::ColorF(0, 0.0f));
        bake_dc->DrawImage(blur_fx.Get(), D2D1::Point2F(0, 0),
                           D2D1::RectF(0, 0, static_cast<float>(qw), static_cast<float>(qh)),
                           D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
        if (SUCCEEDED(bake_dc->EndDraw())) {
            frost_valid = true;
        }
        bake_dc->SetTarget(nullptr);
    }

    void MaybeUpdateAverageLuminance(ID3D11Device* d3d) noexcept {
        const uint64_t now = GetTickCount64();
        if (now - last_lum_sample_ms < 500u || !d3d || !gpu_sharp_tex || sharp_w < 8 || sharp_h < 8) {
            return;
        }
        last_lum_sample_ms = now;

        if (!lum_probe_staging) {
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = 8;
            sd.Height = 8;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(d3d->CreateTexture2D(&sd, nullptr, &lum_probe_staging))) return;
        }

        ComPtr<ID3D11DeviceContext> ctx;
        d3d->GetImmediateContext(&ctx);
        if (!ctx) return;

        // Copy 8 distributed single-pixel probes across the scene into staging[y, x] — 0.01 ms!
        for (uint32_t y = 0; y < 8; ++y) {
            const uint32_t sy = ((2 * y + 1) * sharp_h) / 16;
            for (uint32_t x = 0; x < 8; ++x) {
                const uint32_t sx = ((2 * x + 1) * sharp_w) / 16;
                const D3D11_BOX box{sx, sy, 0, sx + 1, sy + 1, 1};
                ctx->CopySubresourceRegion(lum_probe_staging.Get(), 0, x, y, 0, gpu_sharp_tex.Get(), 0, &box);
            }
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(lum_probe_staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)) &&
            mapped.pData) {
            double sum = 0.0;
            for (uint32_t y = 0; y < 8; ++y) {
                const auto* row = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
                for (uint32_t x = 0; x < 8; ++x) {
                    const uint8_t* p = row + x * 4u;
                    sum += text_contrast::RelativeLuminance({p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f});
                }
            }
            ctx->Unmap(lum_probe_staging.Get(), 0);
            const float inst = static_cast<float>(sum / 64.0);
            avg_luminance = (avg_luminance < 0.0f) ? inst : (avg_luminance * 0.65f + inst * 0.35f);
        }
    }
};

LiveWallpaperSource::LiveWallpaperSource() : impl_(std::make_unique<Impl>()) {}
LiveWallpaperSource::~LiveWallpaperSource() = default;

void LiveWallpaperSource::ConfigureNotification(HWND notify_hwnd, UINT frame_msg) noexcept {
    if (!impl_) return;
    impl_->notify_hwnd = notify_hwnd;
    impl_->frame_msg = frame_msg;
}

void LiveWallpaperSource::SyncState(ID3D11Device* d3d_device,
                                    ID2D1DeviceContext* d2d_dc,
                                    const host::LiveWallpaperDecision& decision,
                                    RECT surface_screen_px,
                                    uint8_t rate_mode,
                                    bool glass_visible,
                                    bool paused) noexcept {
    if (!impl_) return;

    impl_->surf_w = static_cast<uint32_t>(std::max<LONG>(1, surface_screen_px.right - surface_screen_px.left));
    impl_->surf_h = static_cast<uint32_t>(std::max<LONG>(1, surface_screen_px.bottom - surface_screen_px.top));
    impl_->surface_screen_px = surface_screen_px;
    impl_->paused_state = paused;

    const bool should_capture = decision.active && glass_visible && !paused && d3d_device && d2d_dc;
    if (!should_capture) {
        if (impl_->running) {
            // Stop capture immediately (0 idle CPU/GPU!), while retaining last frame in `sharp_bitmap`.
            impl_->StopCapture();
            impl_->inactive_since_ms = GetTickCount64();
        } else if (!glass_visible || !decision.active) {
            if (impl_->sharp_bitmap && impl_->inactive_since_ms != 0 &&
                GetTickCount64() - impl_->inactive_since_ms > 30000ull) {
                impl_->ReleaseGpu();
                impl_->inactive_since_ms = 0;
            }
        }
        impl_->decision = decision;
        impl_->rate_mode = rate_mode;
        return;
    }

    impl_->inactive_since_ms = 0;
    const bool target_changed = !(decision == impl_->decision);
    const bool rate_changed = (rate_mode != impl_->rate_mode);
    impl_->decision = decision;
    impl_->rate_mode = rate_mode;

    if (target_changed && impl_->running) {
        impl_->StopCapture();
    }
    if (rate_changed && impl_->running) {
        const bool is_fb = (impl_->backend == LiveCaptureBackend::PrintWindowFallback);
        impl_->min_interval_ms.store(IntervalMsForRate(rate_mode, is_fb), std::memory_order_relaxed);
        if (impl_->backend == LiveCaptureBackend::WgcWindow && impl_->wgc_session) {
            try {
                using winrt::Windows::Foundation::Metadata::ApiInformation;
                if (ApiInformation::IsPropertyPresent(
                        L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
                    impl_->wgc_session.MinUpdateInterval(
                        std::chrono::milliseconds(impl_->min_interval_ms.load(std::memory_order_relaxed)));
                }
            } catch (...) {}
        }
    }

    if (!impl_->running) {
        impl_->min_interval_ms.store(IntervalMsForRate(rate_mode, false), std::memory_order_relaxed);
        if (!impl_->TryStartWgc(d3d_device)) {
            impl_->TryStartFallbackWorker();
        }
    }
}

bool LiveWallpaperSource::ConsumePendingFrame(ID3D11Device* d3d_device,
                                              ID2D1DeviceContext* d2d_dc,
                                              bool need_frost,
                                              std::span<const RECT> glass_cards_surface_px) noexcept {
    if (!impl_ || !d3d_device || !d2d_dc) return false;
    if (!impl_->frame_ready.exchange(false, std::memory_order_acq_rel)) {
        if (need_frost && impl_->sharp_bitmap && !impl_->frost_valid) {
            impl_->BakeQuarterFrost(d2d_dc);
        }
        return false;
    }

    if (impl_->backend == LiveCaptureBackend::WgcWindow && impl_->wgc_pool) {
        try {
            wgc::Direct3D11CaptureFrame latest{nullptr};
            while (auto next = impl_->wgc_pool.TryGetNextFrame()) {
                if (latest) latest.Close();
                latest = std::move(next);
            }
            if (!latest) return false;

            const auto content_sz = latest.ContentSize();
            if (content_sz.Width > 0 && content_sz.Height > 0 &&
                (content_sz.Width != impl_->wgc_pool_size.Width ||
                 content_sz.Height != impl_->wgc_pool_size.Height)) {
                impl_->wgc_pool_size = content_sz;
                impl_->wgc_pool.Recreate(
                    impl_->winrt_device,
                    wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    1,
                    content_sz);
            }

            const HWND target_hwnd = reinterpret_cast<HWND>(impl_->decision.capture_target_hwnd);
            const RECT target_rc = QueryWindowCaptureBounds(target_hwnd);
            const LONG off_x = impl_->surface_screen_px.left - target_rc.left;
            const LONG off_y = impl_->surface_screen_px.top - target_rc.top;

            // On Win11 24H2+, skip GPU import + redraw when dirty regions miss every visible glass card.
            if (!glass_cards_surface_px.empty() && impl_->sharp_bitmap) {
                try {
                    using winrt::Windows::Foundation::Metadata::ApiInformation;
                    if (ApiInformation::IsPropertyPresent(
                            L"Windows.Graphics.Capture.Direct3D11CaptureFrame", L"DirtyRegions")) {
                        const auto dirty = latest.DirtyRegions();
                        if (dirty.Size() > 0) {
                            bool any_hit = false;
                            for (const auto& dr : dirty) {
                                const RECT dr_surf{
                                    dr.X - off_x, dr.Y - off_y,
                                    dr.X + dr.Width - off_x, dr.Y + dr.Height - off_y};
                                for (const RECT& card : glass_cards_surface_px) {
                                    RECT inter{};
                                    if (IntersectRect(&inter, &dr_surf, &card)) {
                                        any_hit = true;
                                        break;
                                    }
                                }
                                if (any_hit) break;
                            }
                            if (!any_hit) {
                                latest.Close();
                                ++impl_->frames_skipped;
                                return false;
                            }
                        }
                    }
                } catch (...) {}
            }

            auto access = latest.Surface().as<IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> src_tex;
            const HRESULT hr = access->GetInterface(IID_PPV_ARGS(&src_tex));
            if (FAILED(hr) || !src_tex) {
                latest.Close();
                return false;
            }

            D3D11_TEXTURE2D_DESC src_desc{};
            src_tex->GetDesc(&src_desc);
            const uint32_t sx = static_cast<uint32_t>(std::clamp<LONG>(off_x, 0, std::max<LONG>(0, static_cast<LONG>(src_desc.Width) - 1)));
            const uint32_t sy = static_cast<uint32_t>(std::clamp<LONG>(off_y, 0, std::max<LONG>(0, static_cast<LONG>(src_desc.Height) - 1)));
            const uint32_t cw = std::min<uint32_t>(impl_->surf_w, src_desc.Width - sx);
            const uint32_t ch = std::min<uint32_t>(impl_->surf_h, src_desc.Height - sy);
            if (cw == 0 || ch == 0 || !impl_->EnsureGpuTargets(d3d_device, d2d_dc, cw, ch)) {
                latest.Close();
                return false;
            }

            ComPtr<ID3D11DeviceContext> d3d_ctx;
            d3d_device->GetImmediateContext(&d3d_ctx);
            if (d3d_ctx) {
                const D3D11_BOX box{sx, sy, 0, sx + cw, sy + ch, 1};
                d3d_ctx->CopySubresourceRegion(impl_->gpu_sharp_tex.Get(), 0, 0, 0, 0, src_tex.Get(), 0, &box);
            }
            latest.Close();

            impl_->frost_valid = false;
            if (need_frost) {
                impl_->BakeQuarterFrost(d2d_dc);
            }
            impl_->MaybeUpdateAverageLuminance(d3d_device);
            ++impl_->frames_imported;
            return true;
        } catch (...) {
            impl_->StopWgc();
            impl_->TryStartFallbackWorker();
            return false;
        }
    }

    if (impl_->backend == LiveCaptureBackend::PrintWindowFallback) {
        std::vector<uint8_t> pixels;
        uint32_t cw = 0, ch = 0;
        {
            std::lock_guard<std::mutex> lk(impl_->fb_mtx);
            if (!impl_->fb_has_new || impl_->fb_bgra_ready.empty()) return false;
            pixels = std::move(impl_->fb_bgra_ready);
            impl_->fb_has_new = false;
            cw = impl_->fb_w;
            ch = impl_->fb_h;
        }
        if (cw == 0 || ch == 0 || pixels.size() < static_cast<size_t>(cw) * ch * 4u) return false;
        if (!impl_->EnsureGpuTargets(d3d_device, d2d_dc, cw, ch)) return false;

        ComPtr<ID3D11DeviceContext> d3d_ctx;
        d3d_device->GetImmediateContext(&d3d_ctx);
        if (!d3d_ctx) return false;
        d3d_ctx->UpdateSubresource(impl_->gpu_sharp_tex.Get(), 0, nullptr, pixels.data(), cw * 4u, 0);

        impl_->frost_valid = false;
        if (need_frost) {
            impl_->BakeQuarterFrost(d2d_dc);
        }
        impl_->MaybeUpdateAverageLuminance(d3d_device);
        ++impl_->frames_imported;
        return true;
    }

    return false;
}

void LiveWallpaperSource::ReleaseDeviceResources() noexcept {
    if (!impl_) return;
    impl_->StopCapture();
    impl_->ReleaseGpu();
}

bool LiveWallpaperSource::HasFrame() const noexcept {
    return impl_ && impl_->sharp_bitmap != nullptr && impl_->sharp_w > 0 && impl_->sharp_h > 0;
}

ID2D1Bitmap1* LiveWallpaperSource::SharpBitmap() const noexcept {
    return impl_ ? impl_->sharp_bitmap.Get() : nullptr;
}

ID2D1Bitmap1* LiveWallpaperSource::FrostBitmap() const noexcept {
    return (impl_ && impl_->frost_valid) ? impl_->frost_bitmap.Get() : nullptr;
}

uint32_t LiveWallpaperSource::SharpWidth() const noexcept {
    return impl_ ? impl_->sharp_w : 0;
}

uint32_t LiveWallpaperSource::SharpHeight() const noexcept {
    return impl_ ? impl_->sharp_h : 0;
}

uint32_t LiveWallpaperSource::FrostWidth() const noexcept {
    return (impl_ && impl_->frost_valid) ? impl_->frost_w : 0;
}

uint32_t LiveWallpaperSource::FrostHeight() const noexcept {
    return (impl_ && impl_->frost_valid) ? impl_->frost_h : 0;
}

uint32_t LiveWallpaperSource::LogicalScreenWidth() const noexcept {
    return impl_ ? impl_->surf_w : 0;
}

uint32_t LiveWallpaperSource::LogicalScreenHeight() const noexcept {
    return impl_ ? impl_->surf_h : 0;
}

float LiveWallpaperSource::AverageLuminance() const noexcept {
    return impl_ ? impl_->avg_luminance : -1.0f;
}

LiveGlassSourceStats LiveWallpaperSource::Stats() const noexcept {
    if (!impl_) return {};
    const size_t sharp_bytes = impl_->sharp_bitmap ? (static_cast<size_t>(impl_->sharp_w) * impl_->sharp_h * 4u) : 0u;
    const size_t frost_bytes = impl_->frost_bitmap ? (static_cast<size_t>(impl_->frost_w) * impl_->frost_h * 4u) : 0u;
    return LiveGlassSourceStats{
        impl_->backend,
        impl_->running,
        impl_->paused_state,
        HasFrame(),
        impl_->frames_imported,
        impl_->frames_skipped,
        sharp_bytes + frost_bytes,
    };
}

} // namespace pulse::desktop::render
