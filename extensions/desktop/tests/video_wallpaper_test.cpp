// video_wallpaper_test.cpp - Unit + real hardware verification suite for video wallpapers:
//   1. Policy decision table (§4 + §11 Q2/Q3, including 1.1x threshold, VRAM budget, SW RAM limit, HDR, missing codecs)
//   2. Cache key invalidation (path/size/mtime/target/codec) & LRU eviction preserving active key
//   3. Pause/freeze/resume state machine (§3.3, 30s release, reason priority)
//   4. Empirical GPU verification (§2.2): independent D3D11 device swap chain bound into renderer's DComp tree
//   5. Empirical GPU verification (§2.2/§3.4): cross-device shared NT handle + KeyedMutex 1/4-res glass texture
//   6. Empirical hardware decode probe (§4): CheckHardwareDecodeSupport & ProbeVideoFile on real clips
//   7. Real VideoWallpaperPlayer open/loop/pause/freeze-release/reopen + poster JPEG + TranscodeManager cancel/finish
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <dcomp.h>
#include <wrl/client.h>

#include "host/model/desktop_model.h"
#include "host/overlays/desktop_overlays.h"
#include "host/render/desktop_renderer.h"
#include "host/video/transcode_manager.h"
#include "host/video/video_prober.h"
#include "host/video/video_wallpaper_player.h"
#include "host/video/wallpaper_policy.h"
#include "ipc/desktop_ipc.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace pulse::desktop::video;
namespace fs = std::filesystem;

namespace pulse::desktop::overlays {
struct DesktopBeautifyTestPeer {
    static void Prepare(DesktopOverlayController& overlay,
                        const model::BeautifySettings& settings,
                        const video::VideoWallpaperUiState& video_state,
                        DesktopOverlayController::BeautifyTab tab,
                        bool dark,
                        OverlayCallbacks callbacks,
                        bool drop_hover = false,
                        const std::wstring& drop_name = {}) {
        overlay.callbacks_ = std::move(callbacks);
        overlay.active_kind_ = OverlayKind::BeautifyDrawer;
        overlay.dark_mode_ = dark;
        overlay.accent_argb_ = settings.accent_argb;
        overlay.card_w_ = 372;
        overlay.card_h_ = (tab == DesktopOverlayController::BeautifyTab::Wall) ? 688
                        : (tab == DesktopOverlayController::BeautifyTab::Fx)   ? 648
                        : (tab == DesktopOverlayController::BeautifyTab::Desk) ? 460
                                                                               : 560;
        overlay.card_screen_x_ = 0;
        overlay.card_screen_y_ = 0;
        overlay.beautify_settings_ = settings;
        overlay.beautify_tab_ = tab;
        overlay.beautify_video_ = video_state;
        overlay.beautify_scroll_y_ = 0.0f;
        overlay.beautify_drop_hover_ = drop_hover;
        overlay.beautify_drop_filename_ = drop_name;
    }

    static bool ClickAction(DesktopOverlayController& overlay, const std::string& action_id) {
        for (const auto& region : overlay.click_regions_) {
            if (region.action_id != action_id) continue;
            const RECT b = region.bounds;
            overlay.HandleLeftClick((b.left + b.right) / 2, (b.top + b.bottom) / 2);
            return true;
        }
        return false;
    }

    static bool CheckRegionsInBounds(const DesktopOverlayController& overlay) {
        if (overlay.click_regions_.empty()) return false;
        for (const auto& r : overlay.click_regions_) {
            if (r.bounds.right <= r.bounds.left || r.bounds.bottom <= r.bounds.top) return false;
            if (r.bounds.left < -2 || r.bounds.top < -2 ||
                r.bounds.right > overlay.card_w_ + 2 || r.bounds.bottom > overlay.card_h_ + 2) {
                std::printf("[WARN] click region out of card bounds: %s (%ld,%ld,%ld,%ld) card=%dx%d\n",
                            r.action_id.c_str(), r.bounds.left, r.bounds.top,
                            r.bounds.right, r.bounds.bottom, overlay.card_w_, overlay.card_h_);
                return false;
            }
        }
        return true;
    }

    static int CardW(const DesktopOverlayController& overlay) { return overlay.card_w_; }
    static int CardH(const DesktopOverlayController& overlay) { return overlay.card_h_; }
    static DesktopOverlayController::BeautifyTab CurrentTab(const DesktopOverlayController& overlay) {
        return overlay.beautify_tab_;
    }
};
} // namespace pulse::desktop::overlays

namespace {

int g_failed = 0;

bool Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failed;
    return ok;
}

LRESULT CALLBACK TestWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

void WriteDummyFile(const fs::path& p, size_t bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    std::vector<char> buf(std::min<size_t>(bytes, 65536), 'V');
    size_t rem = bytes;
    while (rem > 0) {
        const size_t chunk = std::min(rem, buf.size());
        f.write(buf.data(), static_cast<std::streamsize>(chunk));
        rem -= chunk;
    }
}

void TestPolicyTable() {
    constexpr uint64_t kGB = 1024ull * 1024ull * 1024ull;
    GpuCapabilities gpu{};
    gpu.dedicated_vram_bytes = 3ull * kGB; // GTX 1060 3GB
    gpu.system_ram_bytes = 16ull * kGB;
    gpu.available_ram_bytes = 10ull * kGB;
    gpu.is_integrated_gpu = false;
    gpu.has_hevc_decoder = true;
    gpu.has_hevc_hw_encoder = true;
    gpu.has_h264_hw_encoder = true;
    gpu.has_h264_sw_encoder = true;

    // 1. Missing file
    {
        VideoProbeInfo p{};
        p.file_exists = false;
        const auto d = EvaluateWallpaperPolicy(p, 3840, 2160, gpu);
        Check(d.kind == PolicyDecisionKind::RejectMissingFile, "policy: missing file rejected cleanly");
    }

    // 2. HDR rejected explicitly (never silently tone-mapped)
    {
        VideoProbeInfo p{};
        p.file_exists = true;
        p.open_ok = true;
        p.has_mf_decoder = true;
        p.hw_decode_supported = true;
        p.codec = VideoCodecKind::HEVC;
        p.coded_width = p.display_width = 3840;
        p.coded_height = p.display_height = 2160;
        p.color.is_hdr = true;
        const auto d = EvaluateWallpaperPolicy(p, 3840, 2160, gpu);
        Check(d.kind == PolicyDecisionKind::RejectHdr, "policy: HDR video rejected with explicit message");
    }

    // 3. Missing HEVC decoder -> Store prompt
    {
        VideoProbeInfo p{};
        p.file_exists = true;
        p.open_ok = true;
        p.has_mf_decoder = false;
        p.codec = VideoCodecKind::HEVC;
        p.coded_width = p.display_width = 3840;
        p.coded_height = p.display_height = 2160;
        const auto d = EvaluateWallpaperPolicy(p, 3840, 2160, gpu);
        Check(d.kind == PolicyDecisionKind::RejectMissingHevc && !d.store_uri.empty(),
              "policy: missing HEVC decoder yields Store prompt");
    }

    // 4. Missing AV1 decoder -> Store prompt
    {
        VideoProbeInfo p{};
        p.file_exists = true;
        p.open_ok = true;
        p.has_mf_decoder = false;
        p.codec = VideoCodecKind::AV1;
        p.coded_width = p.display_width = 3840;
        p.coded_height = p.display_height = 2160;
        const auto d = EvaluateWallpaperPolicy(p, 3840, 2160, gpu);
        Check(d.kind == PolicyDecisionKind::RejectMissingAv1 && !d.store_uri.empty(),
              "policy: missing AV1 decoder yields Store prompt");
    }

    // 5. HW decodeable <= 1.1x target -> PlayOriginal (including exact 1.1x edge)
    {
        VideoProbeInfo p{};
        p.file_exists = true;
        p.open_ok = true;
        p.has_mf_decoder = true;
        p.hw_decode_supported = true;
        p.codec = VideoCodecKind::H264;
        p.coded_width = p.display_width = 4224; // 3840 * 1.10
        p.coded_height = p.display_height = 2376; // 2160 * 1.10
        const auto d = EvaluateWallpaperPolicy(p, 3840, 2160, gpu);
        Check(d.kind == PolicyDecisionKind::PlayOriginal, "policy: HW decode at exact 1.1x threshold plays original");

        p.coded_width = p.display_width = 4226; // slightly > 1.1x
        const auto d2 = EvaluateWallpaperPolicy(p, 3840, 2160, gpu);
        Check(d2.kind == PolicyDecisionKind::TranscodeThenSwitch && d2.target_width <= 3840,
              "policy: HW decode above 1.1x threshold schedules transcode");
    }

    // 6. 8K HEVC (HW decodeable on 3 GB card): transcode to 4K, plays original meanwhile (551 MB <= 768 MB budget)
    {
        VideoProbeInfo p{};
        p.file_exists = true;
        p.open_ok = true;
        p.has_mf_decoder = true;
        p.hw_decode_supported = true;
        p.codec = VideoCodecKind::HEVC;
        p.bit_depth = 8;
        p.coded_width = p.display_width = 7680;
        p.coded_height = p.display_height = 4320;
        const auto d = EvaluateWallpaperPolicy(p, 3840, 2160, gpu);
        Check(d.kind == PolicyDecisionKind::TranscodeThenSwitch &&
              d.target_width == 3840 && d.target_height == 2160 &&
              d.encoder == TranscodeCodecChoice::HevcMainHw &&
              d.play_original_while_transcoding && !d.require_heavy_confirm,
              "policy: 8K HEVC 8-bit on 3GB GPU transcodes to 4K HEVC and plays original meanwhile");
    }

    // 7. 8K 10-bit HEVC on 3 GB card: exceeds 25% VRAM budget -> poster only while transcoding to HEVC Main10
    {
        VideoProbeInfo p{};
        p.file_exists = true;
        p.open_ok = true;
        p.has_mf_decoder = true;
        p.hw_decode_supported = true;
        p.codec = VideoCodecKind::HEVC;
        p.bit_depth = 10;
        p.coded_width = p.display_width = 7680;
        p.coded_height = p.display_height = 4320;
        const auto d = EvaluateWallpaperPolicy(p, 3840, 2160, gpu);
        Check(d.kind == PolicyDecisionKind::TranscodeThenSwitch &&
              d.encoder == TranscodeCodecChoice::HevcMain10Hw &&
              !d.play_original_while_transcoding,
              "policy: 8K HEVC 10-bit exceeds 25% VRAM budget -> poster while transcoding Main10");
    }

    // 8. 8K H.264 (cannot HW decode): SW decode transcode -> poster only + heavy confirmation (> 1 GB)
    {
        VideoProbeInfo p{};
        p.file_exists = true;
        p.open_ok = true;
        p.has_mf_decoder = true;
        p.hw_decode_supported = false;
        p.codec = VideoCodecKind::H264;
        p.bit_depth = 8;
        p.coded_width = p.display_width = 7680;
        p.coded_height = p.display_height = 4320;
        const auto d = EvaluateWallpaperPolicy(p, 3840, 2160, gpu);
        Check(d.kind == PolicyDecisionKind::TranscodeThenSwitch &&
              d.requires_software_decode && !d.play_original_while_transcoding &&
              d.require_heavy_confirm,
              "policy: 8K H.264 SW decode triggers poster-only and heavy confirmation");

        // Tight memory machine (2 GB avail -> 1 GB job cap) -> RejectTooLarge
        GpuCapabilities low_ram = gpu;
        low_ram.available_ram_bytes = 2ull * kGB;
        const auto d_oom = EvaluateWallpaperPolicy(p, 3840, 2160, low_ram);
        Check(d_oom.kind == PolicyDecisionKind::RejectTooLarge,
              "policy: 8K H.264 SW decode on low-RAM PC rejected as too large");
    }

    // 9. Encoder fallback: HEVC encoder without HEVC decoder must choose H.264 (§4)
    {
        GpuCapabilities no_hevc_dec = gpu;
        no_hevc_dec.has_hevc_decoder = false;
        VideoProbeInfo p{};
        p.file_exists = true;
        p.open_ok = true;
        p.has_mf_decoder = true;
        p.hw_decode_supported = true;
        p.codec = VideoCodecKind::H264;
        p.coded_width = p.display_width = 3840;
        p.coded_height = p.display_height = 2160;
        const auto d = EvaluateWallpaperPolicy(p, 1920, 1080, no_hevc_dec);
        Check(d.kind == PolicyDecisionKind::TranscodeThenSwitch &&
              d.encoder == TranscodeCodecChoice::H264HighHw,
              "policy: machine with HEVC encoder but no HEVC decoder outputs H.264");
    }
}

void TestCacheAndEviction() {
    CacheKeyInput a{L"C:\\Videos\\Sunset.mp4", 1000, 50000, 3840, 2160, L"hevc_main", 1};
    CacheKeyInput a_case{L"c:/videos/sunset.mp4", 1000, 50000, 3840, 2160, L"hevc_main", 1};
    CacheKeyInput b_mtime = a; b_mtime.file_mtime_100ns = 50001;
    CacheKeyInput b_size = a; b_size.file_size_bytes = 1001;
    CacheKeyInput b_res = a; b_res.target_width = 2560;
    CacheKeyInput b_codec = a; b_codec.codec_pref = L"h264_hw";

    const auto ka = ComputeWallpaperCacheKey(a);
    Check(ka.size() == 64 && ka == ComputeWallpaperCacheKey(a_case),
          "cache key: deterministic 64-hex SHA-256 and case/slash normalized");
    Check(ka != ComputeWallpaperCacheKey(b_mtime) &&
          ka != ComputeWallpaperCacheKey(b_size) &&
          ka != ComputeWallpaperCacheKey(b_res) &&
          ka != ComputeWallpaperCacheKey(b_codec),
          "cache key: invalidates on mtime, size, resolution, or codec change");

    const fs::path tmp_dir = fs::temp_directory_path() / L"pulse_vw_cache_test";
    std::error_code ec;
    fs::remove_all(tmp_dir, ec);

    WallpaperCacheManager mgr(tmp_dir.wstring(), 10000); // 10 KB budget
    mgr.EnsureDirectory();

    // Create stale temp file and verify cleanup
    WriteDummyFile(tmp_dir / L"orphan.tmp.mp4", 512);
    Check(mgr.CleanupStaleTempFiles() == 1 && !fs::exists(tmp_dir / L"orphan.tmp.mp4"),
          "cache: CleanupStaleTempFiles removes leftover .tmp.mp4");

    auto add_entry = [&](const std::wstring& key, size_t bytes, uint64_t ts, std::wstring_view active) {
        CacheEntry e{};
        e.key = key;
        e.video_path = mgr.VideoFilePathForKey(key);
        e.poster_path = mgr.PosterFilePathForKey(key);
        e.source_path = L"C:\\src\\" + key + L".mp4";
        e.last_used_ms = ts;
        WriteDummyFile(e.video_path, bytes);
        WriteDummyFile(e.poster_path, 200);
        mgr.UpsertEntry(e, active);
    };

    add_entry(L"k1", 4000, 100, L"k1"); // oldest, but protected active!
    add_entry(L"k2", 4000, 200, L"k1");
    add_entry(L"k3", 4000, 300, L"k1"); // total 12600 > 10000 -> k2 evicted, k1 protected!

    Check(mgr.LookupValidVideo(L"k1").has_value() &&
          !mgr.LookupValidVideo(L"k2").has_value() &&
          mgr.LookupValidVideo(L"k3").has_value(),
          "cache: LRU evicts oldest non-active entry while protecting active wallpaper");

    // Verify index reload from disk
    WallpaperCacheManager mgr2(tmp_dir.wstring(), 10000);
    Check(mgr2.LoadIndex() && mgr2.Entries().size() == 2 && mgr2.LookupValidVideo(L"k1").has_value(),
          "cache: index.json persists and reloads cleanly");

    fs::remove_all(tmp_dir, ec);
}

void TestPauseStateMachine() {
    VideoPauseStateMachine sm(30000);
    PauseSignals sig{};

    auto r = sm.Step(sig, 1000);
    Check(r.state == PauseLifecycleState::Playing && !r.entered_pause, "pause SM: initial playing");

    // Fullscreen enters hot pause
    sig.fullscreen = true;
    r = sm.Step(sig, 2000);
    Check(r.state == PauseLifecycleState::PausedHot && r.entered_pause && r.reason == VideoPauseReason::Fullscreen,
          "pause SM: fullscreen enters PausedHot");

    // Still paused at +29.9s -> stays hot
    r = sm.Step(sig, 31900);
    Check(r.state == PauseLifecycleState::PausedHot && !r.should_freeze_release,
          "pause SM: stays PausedHot before 30s");

    // At +30.0s -> triggers freeze release
    r = sm.Step(sig, 32000);
    Check(r.state == PauseLifecycleState::FrozenReleased && r.should_freeze_release,
          "pause SM: triggers FrozenReleased at 30s");

    // Subsequent ticks while frozen don't re-fire freeze_release
    r = sm.Step(sig, 40000);
    Check(r.state == PauseLifecycleState::FrozenReleased && !r.should_freeze_release,
          "pause SM: remains FrozenReleased without duplicate signal");

    // Clear fullscreen -> resumed_from_frozen
    sig.fullscreen = false;
    r = sm.Step(sig, 41000);
    Check(r.state == PauseLifecycleState::Playing && r.resumed_from_frozen && !r.resumed_from_hot,
          "pause SM: clearing signal after freeze returns resumed_from_frozen");

    // Brief display-off -> resumed_from_hot
    sig.display_off = true;
    r = sm.Step(sig, 50000);
    Check(r.entered_pause && r.reason == VideoPauseReason::DisplayOff, "pause SM: display_off pauses");
    sig.display_off = false;
    r = sm.Step(sig, 55000);
    Check(r.resumed_from_hot && !r.resumed_from_frozen, "pause SM: short pause resumes hot");
}

void TestCrossDeviceDCompAndKeyedMutex() {
    // §2.2 empirical verification: independent D3D11 device swap chain inside renderer's DComp tree
    // + cross-device shared NT handle + KeyedMutex glass texture!
    WNDCLASSW wc{};
    wc.lpfnWndProc = TestWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseCrossDeviceDCompTestWnd";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
                                wc.lpszClassName, L"cross-device-test", WS_POPUP,
                                -3200, 0, 256, 256, nullptr, nullptr, wc.hInstance, nullptr);
    Check(hwnd != nullptr, "cross-device DComp: test window created");
    if (!hwnd) return;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ComPtr<ID3D11Device> dev_a, dev_b;
    ComPtr<ID3D11DeviceContext> ctx_a, ctx_b;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, 2, D3D11_SDK_VERSION, &dev_a, nullptr, &ctx_a);
    Check(SUCCEEDED(hr) && dev_a, "cross-device DComp: renderer device A created");
    if (!dev_a) { DestroyWindow(hwnd); return; }

    ComPtr<IDXGIDevice> dxgi_a;
    ComPtr<IDXGIAdapter> adapter_a;
    dev_a.As(&dxgi_a);
    dxgi_a->GetAdapter(&adapter_a);

    hr = D3D11CreateDevice(adapter_a.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                           levels, 2, D3D11_SDK_VERSION, &dev_b, nullptr, &ctx_b);
    Check(SUCCEEDED(hr) && dev_b, "cross-device DComp: independent player device B created on same adapter");

    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> root, video_vis, top_vis;
    hr = DCompositionCreateDevice(dxgi_a.Get(), IID_PPV_ARGS(&dcomp));
    bool dcomp_ok = SUCCEEDED(hr) &&
                    SUCCEEDED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &target)) &&
                    SUCCEEDED(dcomp->CreateVisual(&root)) &&
                    SUCCEEDED(dcomp->CreateVisual(&video_vis)) &&
                    SUCCEEDED(dcomp->CreateVisual(&top_vis)) &&
                    SUCCEEDED(root->AddVisual(video_vis.Get(), FALSE, nullptr)) &&
                    SUCCEEDED(root->AddVisual(top_vis.Get(), TRUE, video_vis.Get())) &&
                    SUCCEEDED(target->SetRoot(root.Get()));
    Check(dcomp_ok, "cross-device DComp: DComp device & visual tree created on device A");

    ComPtr<IDXGIFactory2> factory;
    adapter_a->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 256;
    sd.Height = 256;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGISwapChain1> swap_b;
    hr = factory->CreateSwapChainForComposition(dev_b.Get(), &sd, nullptr, &swap_b);
    Check(SUCCEEDED(hr) && swap_b, "cross-device DComp: waitable composition swap chain created on device B");

    // Clear backbuffer on device B to solid teal (R=0x20, G=0xC0, B=0xE0) and Present
    ComPtr<ID3D11Texture2D> back;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (swap_b && SUCCEEDED(swap_b->GetBuffer(0, IID_PPV_ARGS(&back))) &&
        SUCCEEDED(dev_b->CreateRenderTargetView(back.Get(), nullptr, &rtv))) {
        const float teal[4] = {0x20 / 255.0f, 0xC0 / 255.0f, 0xE0 / 255.0f, 1.0f};
        ctx_b->ClearRenderTargetView(rtv.Get(), teal);
        swap_b->Present(0, 0);
    }

    const HRESULT bind_hr = video_vis ? video_vis->SetContent(swap_b.Get()) : E_FAIL;
    const HRESULT commit_hr = dcomp ? dcomp->Commit() : E_FAIL;
    std::printf("[INFO] cross-device SetContent hr=0x%08lX Commit hr=0x%08lX\n",
                static_cast<unsigned long>(bind_hr), static_cast<unsigned long>(commit_hr));
    Check(SUCCEEDED(bind_hr) && SUCCEEDED(commit_hr),
          "cross-device DComp: SetContent(deviceB swap chain) + Commit succeeds on deviceA DComp tree");

    // Verify visual pixel via PrintWindow(PW_RENDERFULLCONTENT)
    Sleep(50);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = 256;
    bi.bmiHeader.biHeight = -256;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP hbmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mdc, hbmp);
    const BOOL pw_ok = PrintWindow(hwnd, mdc, PW_RENDERFULLCONTENT);
    bool pixel_ok = false;
    if (pw_ok && bits) {
        const auto* px = static_cast<const uint8_t*>(bits) + (128 * 256 + 128) * 4;
        std::printf("[INFO] cross-device DComp center pixel BGRA=(%u,%u,%u,%u)\n", px[0], px[1], px[2], px[3]);
        pixel_ok = (std::abs(int(px[0]) - 0xE0) <= 4 &&
                    std::abs(int(px[1]) - 0xC0) <= 4 &&
                    std::abs(int(px[2]) - 0x20) <= 4);
    }
    Check(pixel_ok, "cross-device DComp: PrintWindow reads exact Device B teal pixels through Device A DComp tree");
    SelectObject(mdc, old);
    DeleteObject(hbmp);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);

    // Cross-device shared NT handle + KeyedMutex test
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 64;
    td.Height = 64;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    ComPtr<ID3D11Texture2D> shared_b;
    ComPtr<IDXGIKeyedMutex> km_b;
    ComPtr<IDXGIResource1> res_b;
    HANDLE nt_handle = nullptr;
    bool km_ok = SUCCEEDED(dev_b->CreateTexture2D(&td, nullptr, &shared_b)) &&
                 SUCCEEDED(shared_b.As(&km_b)) &&
                 SUCCEEDED(shared_b.As(&res_b)) &&
                 SUCCEEDED(res_b->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                     nullptr, &nt_handle)) &&
                 nt_handle != nullptr;

    if (km_ok) {
        ComPtr<ID3D11RenderTargetView> rtv_s;
        dev_b->CreateRenderTargetView(shared_b.Get(), nullptr, &rtv_s);
        const float orange[4] = {0xF2 / 255.0f, 0x54 / 255.0f, 0x2D / 255.0f, 1.0f};
        km_b->AcquireSync(0, 100);
        ctx_b->ClearRenderTargetView(rtv_s.Get(), orange);
        km_b->ReleaseSync(0);

        ComPtr<ID3D11Device1> dev_a1;
        ComPtr<ID3D11Texture2D> shared_a;
        ComPtr<IDXGIKeyedMutex> km_a;
        bool open_ok = SUCCEEDED(dev_a.As(&dev_a1)) &&
                       SUCCEEDED(dev_a1->OpenSharedResource1(nt_handle, IID_PPV_ARGS(&shared_a))) &&
                       SUCCEEDED(shared_a.As(&km_a));
        bool copy_match = false;
        if (open_ok) {
            D3D11_TEXTURE2D_DESC stgd = td;
            stgd.Usage = D3D11_USAGE_STAGING;
            stgd.BindFlags = 0;
            stgd.MiscFlags = 0;
            stgd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> stg;
            if (SUCCEEDED(dev_a->CreateTexture2D(&stgd, nullptr, &stg)) && km_a->AcquireSync(0, 100) == S_OK) {
                ctx_a->CopyResource(stg.Get(), shared_a.Get());
                km_a->ReleaseSync(0);
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(ctx_a->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
                    const auto* p = static_cast<const uint8_t*>(m.pData);
                    copy_match = (std::abs(int(p[0]) - 0x2D) <= 2 &&
                                  std::abs(int(p[1]) - 0x54) <= 2 &&
                                  std::abs(int(p[2]) - 0xF2) <= 2);
                    ctx_a->Unmap(stg.Get(), 0);
                }
            }
        }
        CloseHandle(nt_handle);
        Check(copy_match, "cross-device KeyedMutex: shared NT handle texture transfers exact pixels B -> A");
    } else {
        Check(false, "cross-device KeyedMutex: failed to create shared NT handle texture");
    }

    DestroyWindow(hwnd);
}

void TestRealClipsHardwareProbeAndPipeline() {
    const fs::path clips_dir = L"bench_data\\video-wallpaper-proto\\clips";
    if (!fs::exists(clips_dir)) {
        std::printf("[INFO] clips dir not found, skipping real clip tests\n");
        return;
    }

    const auto gpu = ProbeGpuCapabilities();
    std::printf("[INFO] GPU caps: dedicatedVRAM=%.0f MB iGPU=%d availRAM=%.0f MB hevcDec=%d hevcEncHW=%d h264EncHW=%d\n",
                gpu.dedicated_vram_bytes / (1024.0 * 1024.0), gpu.is_integrated_gpu ? 1 : 0,
                gpu.available_ram_bytes / (1024.0 * 1024.0),
                gpu.has_hevc_decoder ? 1 : 0, gpu.has_hevc_hw_encoder ? 1 : 0, gpu.has_h264_hw_encoder ? 1 : 0);

    const wchar_t* clips[] = {
        L"h264-1080p.mp4",
        L"h264-4k.mp4",
        L"h264-8k.mp4",
        L"hevc-1440p.mp4",
        L"hevc-4k.mp4",
        L"hevc-4k-10bit.mp4",
        L"hevc-8k.mp4",
        L"av1-4k.mp4",
    };

    for (const wchar_t* name : clips) {
        const fs::path p = clips_dir / name;
        if (!fs::exists(p)) continue;
        const auto info = ProbeVideoFile(p.wstring());
        const auto dec = EvaluateWallpaperPolicy(info, 3840, 2160, gpu);
        std::printf("[INFO] probe %-18ls: open=%d codec=%ls %ux%u (coded %ux%u) %ubit %.1ffps dur=%.1fs hdr=%d mfDec=%d hwDec=%d yuvMat=%u range=%u -> policy=%d\n",
                    name, info.open_ok ? 1 : 0, info.codec_name.c_str(),
                    info.display_width, info.display_height, info.coded_width, info.coded_height,
                    info.bit_depth, info.Fps(), info.DurationMs() / 1000.0,
                    info.color.is_hdr ? 1 : 0, info.has_mf_decoder ? 1 : 0, info.hw_decode_supported ? 1 : 0,
                    info.color.yuv_matrix, info.color.nominal_range, static_cast<int>(dec.kind));
    }

    // Specific assertions on GTX 1060 / standard clips
    const auto p_1080 = ProbeVideoFile((clips_dir / L"h264-1080p.mp4").wstring());
    Check(p_1080.open_ok && p_1080.display_width == 1920 && p_1080.display_height == 1080 &&
          p_1080.has_mf_decoder && p_1080.hw_decode_supported,
          "real clip probe: h264-1080p detected as 1920x1080 HW-decodeable");

    const auto p_h264_8k = ProbeVideoFile((clips_dir / L"h264-8k.mp4").wstring());
    Check(p_h264_8k.open_ok && p_h264_8k.display_width == 7680 && !p_h264_8k.hw_decode_supported,
          "real clip probe: h264-8k accurately detected as NOT hardware-decodeable on 4096-capped H.264 GPU");

    if (gpu.has_hevc_decoder) {
        const auto p_hevc_10 = ProbeVideoFile((clips_dir / L"hevc-4k-10bit.mp4").wstring());
        Check(p_hevc_10.open_ok && p_hevc_10.bit_depth == 10 && p_hevc_10.hw_decode_supported,
              "real clip probe: hevc-4k-10bit detected as 10-bit HW-decodeable");
    }

    // Poster extraction test
    const fs::path poster_path = fs::temp_directory_path() / L"pulse_vw_test_poster.jpg";
    DeleteFileW(poster_path.c_str());
    uint32_t accent = 0;
    const bool poster_ok = ExtractPosterJpeg((clips_dir / L"h264-1080p.mp4").wstring(),
                                             poster_path.wstring(), 1920, 1080, &accent);
    Check(poster_ok && fs::exists(poster_path) && fs::file_size(poster_path) > 4096 && (accent & 0xFF000000u) == 0xFF000000u,
          "poster extraction: decodes first frame with color conversion, extracts accent, writes valid JPEG");
    DeleteFileW(poster_path.c_str());

    // Live VideoWallpaperPlayer lifecycle: open -> play -> glass frame -> pause -> freeze-release -> reopen
    std::atomic<bool> got_swap{false}, got_glass{false}, got_frozen{false};
    ComPtr<IDXGISwapChain1> held_swap;
    std::mutex swap_mtx;
    FrozenFrameSnapshot frozen_snap{};

    VideoPlayerCallbacks cb{};
    cb.on_swap_chain_ready = [&](uint64_t, ComPtr<IDXGISwapChain1> sc, uint32_t, uint32_t, uint32_t, uint32_t) {
        std::lock_guard<std::mutex> lk(swap_mtx);
        held_swap = std::move(sc);
        got_swap.store(true);
    };
    cb.on_glass_frame_ready = [&](uint64_t, uint64_t, HANDLE, uint32_t w, uint32_t h) {
        if (w > 0 && h > 0) got_glass.store(true);
    };
    cb.on_frozen_frame_ready = [&](uint64_t, FrozenFrameSnapshot snap) {
        std::lock_guard<std::mutex> lk(swap_mtx);
        frozen_snap = std::move(snap);
        got_frozen.store(true);
    };

    VideoWallpaperPlayer player;
    Check(player.Start(LUID{0, 0}, std::move(cb)), "player: started independent playback thread");
    player.SetGlassConsumerActive(true);
    player.SetGlassLiveRate(1);
    player.OpenVideo((clips_dir / L"h264-1080p.mp4").wstring(), 1920, 1080);

    for (int i = 0; i < 100 && !got_swap.load(); ++i) Sleep(20);
    Sleep(350); // let a few frames play
    const auto st_play = player.Stats();
    std::printf("[INFO] player playing: indepDev=%d frames=%llu dropped=%llu glass=%llu first=%.1fms fmt=%d\n",
                st_play.using_independent_device ? 1 : 0,
                static_cast<unsigned long long>(st_play.frames_shown),
                static_cast<unsigned long long>(st_play.frames_dropped),
                static_cast<unsigned long long>(st_play.glass_frames_published),
                st_play.first_frame_ms, static_cast<int>(st_play.swap_format));
    Check(got_swap.load() && got_glass.load() && st_play.frames_shown >= 5 && st_play.using_independent_device,
          "player: opens on independent D3D11 device, presents frames, publishes 1/4-res glass texture");

    player.FreezeAndRelease();
    for (int i = 0; i < 100 && !got_frozen.load(); ++i) Sleep(20);
    Sleep(80);
    {
        std::lock_guard<std::mutex> lk(swap_mtx);
        held_swap.Reset();
    }
    const auto st_frozen = player.Stats();
    Check(got_frozen.load() && st_frozen.frozen_released && frozen_snap.width == 1920 && frozen_snap.height == 1080,
          "player: FreezeAndRelease captures full-res last frame and releases decoder/swapchain");

    got_swap.store(false);
    player.ResumeFromFrozen();
    for (int i = 0; i < 100 && !got_swap.load(); ++i) Sleep(20);
    const auto st_reopen = player.Stats();
    std::printf("[INFO] player reopen first frame: %.1f ms\n", st_reopen.reopen_first_frame_ms);
    Check(got_swap.load() && !st_reopen.frozen_released && st_reopen.reopen_first_frame_ms >= 0.0,
          "player: ResumeFromFrozen reopens cleanly and binds fresh swap chain");

    player.Shutdown();

    // TranscodeManager cancellation test (must leave zero temp files)
    {
        const fs::path tc_dir = fs::temp_directory_path() / L"pulse_vw_tc_test";
        std::error_code ec;
        fs::remove_all(tc_dir, ec);
        fs::create_directories(tc_dir, ec);

        TranscodeJobSpec spec{};
        spec.job_id = 1;
        spec.source_path = (clips_dir / L"hevc-4k.mp4").wstring();
        spec.cache_key = L"testcancel";
        spec.temp_output_path = (tc_dir / L"testcancel.tmp.mp4").wstring();
        spec.final_output_path = (tc_dir / L"testcancel.mp4").wstring();
        spec.target_width = 1920;
        spec.target_height = 1080;
        spec.codec = TranscodeCodecChoice::HevcMainHw;
        spec.memory_limit_bytes = 2ull * 1024ull * 1024ull * 1024ull;

        std::atomic<bool> done{false};
        TranscodeOutcome outcome = TranscodeOutcome::Failed;
        TranscodeManager tm;
        const bool started = tm.StartJob(spec, {
            nullptr,
            [&](uint64_t, TranscodeOutcome o, const std::wstring&) {
                outcome = o;
                done.store(true);
            }
        });
        Sleep(250);
        tm.Cancel();
        tm.WaitUntilIdle();
        Check(started && done.load() && outcome == TranscodeOutcome::Cancelled &&
              !fs::exists(spec.temp_output_path) && !fs::exists(spec.final_output_path),
              "transcoder: mid-job cancellation terminates cleanly and leaves no temp/partial files");
        fs::remove_all(tc_dir, ec);
    }
}

void TestModelVideoWallpaperPersistence() {
    using namespace pulse::desktop::model;
    DesktopModelState orig{};
    orig.video_wallpaper.active = true;
    orig.video_wallpaper.source_path = L"C:\\Videos\\Ocean_4K.mp4";
    orig.video_wallpaper.fill_mode = VideoFillMode::Fit;
    orig.video_wallpaper.cache_key = L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    orig.video_wallpaper.cached_video_path = L"C:\\Cache\\0123.mp4";
    orig.video_wallpaper.poster_path = L"C:\\Cache\\0123.jpg";
    orig.video_wallpaper.source_width = 7680;
    orig.video_wallpaper.source_height = 4320;
    orig.video_wallpaper.target_width = 3840;
    orig.video_wallpaper.target_height = 2160;
    orig.video_wallpaper.duration_ms = 18500;
    orig.video_wallpaper.optimized = true;
    orig.video_wallpaper.recent.push_back({L"C:\\Videos\\Ocean_4K.mp4", L"C:\\Cache\\0123.jpg", 7680, 4320, 18500});
    orig.video_wallpaper.recent.push_back({L"C:\\Videos\\City_Night.mov", L"C:\\Cache\\4567.jpg", 3840, 2160, 12000});

    const std::string json = SerializeModelStateJson(orig);
    DesktopModelState restored{};
    const bool ok = DeserializeModelStateJson(json, restored);
    Check(ok &&
          restored.video_wallpaper.active == true &&
          restored.video_wallpaper.source_path == orig.video_wallpaper.source_path &&
          restored.video_wallpaper.fill_mode == VideoFillMode::Fit &&
          restored.video_wallpaper.cache_key == orig.video_wallpaper.cache_key &&
          restored.video_wallpaper.cached_video_path == orig.video_wallpaper.cached_video_path &&
          restored.video_wallpaper.poster_path == orig.video_wallpaper.poster_path &&
          restored.video_wallpaper.source_width == 7680 &&
          restored.video_wallpaper.source_height == 4320 &&
          restored.video_wallpaper.target_width == 3840 &&
          restored.video_wallpaper.target_height == 2160 &&
          restored.video_wallpaper.duration_ms == 18500 &&
          restored.video_wallpaper.optimized == true &&
          restored.video_wallpaper.recent.size() == 2 &&
          restored.video_wallpaper.recent[1].path == L"C:\\Videos\\City_Night.mov" &&
          restored.video_wallpaper.recent[1].duration_ms == 12000,
          "model persistence: video_wallpaper state & recent list roundtrip cleanly");

    // Backward compatibility: older JSON without video_wallpaper field loads safely
    DesktopModelState legacy{};
    const bool legacy_ok = DeserializeModelStateJson("{\"schema_version\":3,\"settings\":{\"dark_mode\":true}}", legacy);
    Check(legacy_ok && !legacy.video_wallpaper.active && legacy.video_wallpaper.recent.empty(),
          "model persistence: legacy JSON without video_wallpaper deserializes safely");
}

void TestRendererVideoWallpaperAndGlassIntegration(const fs::path& snap_dir) {
    const fs::path clip = L"bench_data\\video-wallpaper-proto\\clips\\h264-1080p.mp4";
    if (!fs::exists(clip)) {
        std::printf("[INFO] skipping renderer video integration test (clip missing)\n");
        return;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = TestWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseRendererVideoTestWnd";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
                                wc.lpszClassName, L"renderer-video-test", WS_POPUP,
                                -3200, 0, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);
    Check(hwnd != nullptr, "renderer video: host window created");
    if (!hwnd) return;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    pulse::desktop::render::DesktopRenderer renderer;
    const bool init_ok = renderer.InitializeForHwnd(hwnd, 640, 360, true, false);
    Check(init_ok && renderer.UsingComposition(), "renderer video: DesktopRenderer initialized with DComp");
    if (!init_ok) { DestroyWindow(hwnd); return; }

    std::atomic<bool> got_swap{false}, got_glass{false}, got_frozen{false};
    std::mutex mtx;
    ComPtr<IDXGISwapChain1> sc_held;
    uint32_t sc_w = 0, sc_h = 0;
    HANDLE glass_handle = nullptr;
    uint32_t glass_w = 0, glass_h = 0;
    FrozenFrameSnapshot frozen{};

    VideoPlayerCallbacks cb{};
    cb.on_swap_chain_ready = [&](uint64_t, ComPtr<IDXGISwapChain1> sc, uint32_t w, uint32_t h, uint32_t, uint32_t) {
        std::lock_guard<std::mutex> lk(mtx);
        sc_held = std::move(sc);
        sc_w = w;
        sc_h = h;
        got_swap.store(true);
    };
    cb.on_glass_frame_ready = [&](uint64_t, uint64_t, HANDLE h, uint32_t w, uint32_t h_px) {
        std::lock_guard<std::mutex> lk(mtx);
        glass_handle = h;
        glass_w = w;
        glass_h = h_px;
        got_glass.store(true);
    };
    cb.on_frozen_frame_ready = [&](uint64_t, FrozenFrameSnapshot snap) {
        std::lock_guard<std::mutex> lk(mtx);
        frozen = std::move(snap);
        got_frozen.store(true);
    };

    VideoWallpaperPlayer player;
    player.Start(renderer.AdapterLuid(), std::move(cb));
    player.SetGlassConsumerActive(true);
    player.SetGlassLiveRate(1);
    player.OpenVideo(clip.wstring(), 1280, 720);

    for (int i = 0; i < 120 && (!got_swap.load() || !got_glass.load()); ++i) Sleep(20);
    Sleep(150);

    bool bound_ok = false;
    bool glass_ok = false;
    {
        std::lock_guard<std::mutex> lk(mtx);
        if (sc_held) {
            bound_ok = renderer.SetVideoWallpaperSwapChain(sc_held.Get(), sc_w, sc_h, VideoFillMode::Fill);
        }
        if (glass_handle && glass_w > 0 && glass_h > 0) {
            glass_ok = renderer.OnInternalVideoGlassFrame(glass_handle, glass_w, glass_h);
        }
    }
    Check(bound_ok && renderer.IsVideoWallpaperActive(),
          "renderer video: SetVideoWallpaperSwapChain binds player swap chain at bottom of DComp tree");
    Check(glass_ok && renderer.LiveGlassStats().frames_imported >= 1,
          "renderer video: OnInternalVideoGlassFrame imports shared KeyedMutex texture into LiveWallpaperSource");

    // Verify surgical Dock Bar live glass tracking (even with 0 tiles & 0 groups!)
    {
        pulse::desktop::model::DesktopModelState dock_st{};
        dock_st.settings.dark_mode = true;
        dock_st.settings.default_material = pulse::desktop::model::CardMaterial::LiquidGlass;
        dock_st.settings.dock_enabled = true;
        pulse::desktop::render::LiveTileRuntimeData dock_live{};
        dock_live.dock.pill_rect = D2D1::RectF(120.0f, 290.0f, 520.0f, 350.0f);
        dock_live.dock.pill_radius = 22.0f;
        dock_live.dock.items.push_back({});
        pulse::desktop::render::DesktopInteractionState dock_inter{};
        dock_inter.dock_bar_reveal = 1.0f;
        renderer.InvalidateAll();
        renderer.Tick(dock_st, {}, {}, dock_live, dock_inter);
        Check(renderer.HasLiveGlassConsumers(),
              "renderer video: Liquid Glass Dock bar registers as live glass consumer");
        bool dock_glass_ok = false;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (glass_handle && glass_w > 0 && glass_h > 0) {
                dock_glass_ok = renderer.OnInternalVideoGlassFrame(glass_handle, glass_w, glass_h, true);
            }
        }
        Check(dock_glass_ok && renderer.LiveWallpaper().FrostBitmap() != nullptr && renderer.HasDirtyWork(),
              "renderer video: OnInternalVideoGlassFrame bakes frost for overlays and marks surgical Dock dirty work");
    }

    // Render a Liquid Glass tile over the video wallpaper and verify DComp compositing via PrintWindow!
    pulse::desktop::model::DesktopModelState st{};
    st.settings.dark_mode = true;
    st.settings.default_material = pulse::desktop::model::CardMaterial::LiquidGlass;
    st.settings.tiles_enabled = true;
    pulse::desktop::model::DesktopTile tile{};
    tile.id = "t-glass";
    tile.kind = pulse::desktop::model::TileKind::Clock;
    tile.title = L"时钟";
    tile.size = {2, 2};
    tile.free_placed = true;
    tile.x = 24; tile.y = 24; tile.w = 180; tile.h = 160;
    tile.material = pulse::desktop::model::CardMaterial::LiquidGlass;
    st.tiles.push_back(tile);
    pulse::desktop::render::LiveTileRuntimeData live{};
    pulse::desktop::render::DesktopInteractionState inter{};
    renderer.InvalidateAll();
    renderer.Tick(st, {}, {}, live, inter);
    Sleep(60);

    // Check PrintWindow reads non-black video pixels in the open area (right half of 640x360)
    auto sample_window_nonblack = [&]() -> int {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = 640;
        bi.bmiHeader.biHeight = -360;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC sdc = GetDC(nullptr);
        HDC mdc = CreateCompatibleDC(sdc);
        HBITMAP hbmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mdc, hbmp);
        int nonblack = 0;
        if (PrintWindow(hwnd, mdc, PW_RENDERFULLCONTENT) && bits) {
            const auto* p = static_cast<const uint8_t*>(bits);
            for (int y = 40; y < 320; y += 8) {
                for (int x = 260; x < 600; x += 8) {
                    const uint8_t* px = p + (y * 640 + x) * 4;
                    if (px[0] > 8 || px[1] > 8 || px[2] > 8) ++nonblack;
                }
            }
        }
        SelectObject(mdc, old);
        DeleteObject(hbmp);
        DeleteDC(mdc);
        ReleaseDC(nullptr, sdc);
        return nonblack;
    };

    const int fill_nonblack = sample_window_nonblack();
    Check(fill_nonblack > 200, "renderer video: PrintWindow captures live video wallpaper under Liquid Glass tile");

    // Exercise Fit fill mode transform update
    renderer.UpdateVideoWallpaperFillMode(VideoFillMode::Fit);
    Sleep(40);
    Check(sample_window_nonblack() > 150, "renderer video: Fit mode updates DComp visual transform cleanly");

    // Exercise FreezeAndRelease -> SetVideoWallpaperFrozenBgra -> keeps non-black frame with swapchain released
    player.FreezeAndRelease();
    for (int i = 0; i < 100 && !got_frozen.load(); ++i) Sleep(20);
    bool frozen_bound = false;
    {
        std::lock_guard<std::mutex> lk(mtx);
        sc_held.Reset();
        if (!frozen.bgra.empty()) {
            frozen_bound = renderer.SetVideoWallpaperFrozenBgra(
                frozen.bgra.data(), frozen.width, frozen.height, frozen.stride, VideoFillMode::Fill);
        }
    }
    Sleep(40);
    const int frozen_nonblack = sample_window_nonblack();
    Check(frozen_bound && frozen_nonblack > 200,
          "renderer video: SetVideoWallpaperFrozenBgra preserves static frame after swap chain release");

    renderer.ClearVideoWallpaper();
    Check(!renderer.IsVideoWallpaperActive(), "renderer video: ClearVideoWallpaper cleanly exits video mode");

    if (!snap_dir.empty()) {
        fs::create_directories(snap_dir);
    }

    player.Shutdown();
    renderer.Shutdown();
    DestroyWindow(hwnd);
}

void TestBeautifyDrawerVisualAndInteractions(const fs::path& snap_dir) {
    using namespace pulse::desktop;
    using namespace pulse::desktop::overlays;

    const fs::path clips_dir = L"bench_data\\video-wallpaper-proto\\clips";
    const fs::path work_tmp = snap_dir.empty() ? (fs::temp_directory_path() / L"pulse_vw_drawer_test") : snap_dir;
    std::error_code ec;
    fs::create_directories(work_tmp, ec);

    // Extract real posters from real clips so the drawer renders genuine video thumbnails
    const fs::path p1 = work_tmp / L"_poster1.jpg";
    const fs::path p2 = work_tmp / L"_poster2.jpg";
    const fs::path p3 = work_tmp / L"_poster3.jpg";
    if (fs::exists(clips_dir / L"h264-1080p.mp4")) {
        ExtractPosterJpeg((clips_dir / L"h264-1080p.mp4").wstring(), p1.wstring(), 640, 360);
    }
    if (fs::exists(clips_dir / L"hevc-1440p.mp4")) {
        ExtractPosterJpeg((clips_dir / L"hevc-1440p.mp4").wstring(), p2.wstring(), 640, 360);
    }
    if (fs::exists(clips_dir / L"hevc-4k.mp4")) {
        ExtractPosterJpeg((clips_dir / L"hevc-4k.mp4").wstring(), p3.wstring(), 640, 360);
    }

    // Build a synthetic vivid gradient BGRA live preview frame (320x180)
    std::vector<uint8_t> live_bgra(320u * 180u * 4u, 255);
    for (uint32_t y = 0; y < 180; ++y) {
        for (uint32_t x = 0; x < 320; ++x) {
            const float u = float(x) / 319.0f;
            const float v = float(y) / 179.0f;
            uint8_t* px = live_bgra.data() + (y * 320u + x) * 4u;
            px[0] = static_cast<uint8_t>(std::clamp(int(180 + 60 * u - 80 * v), 0, 255)); // B
            px[1] = static_cast<uint8_t>(std::clamp(int(110 + 90 * (1.0f - v) + 30 * u), 0, 255)); // G
            px[2] = static_cast<uint8_t>(std::clamp(int(40 + 180 * v + 30 * (1.0f - u)), 0, 255)); // R
            px[3] = 255;
        }
    }

    render::DesktopRenderer renderer;
    Check(renderer.InitializeOffscreen(404, 724), "drawer visual: offscreen renderer initialized");

    model::DesktopModelState state{};
    state.settings.wallpaper_preset = model::WallpaperPreset::Bloom;
    state.settings.accent_argb = 0xFF4CC2FFu;
    state.settings.default_material = model::CardMaterial::LiquidGlass;
    render::LiveTileRuntimeData live{};
    render::DesktopInteractionState inter{};
    inter.force_opaque_wallpaper = true;

    auto make_recent = [&]() {
        std::vector<VideoWallpaperRecentItem> r;
        r.push_back({L"C:\\Videos\\ocean_waves_8k.mp4", p1.wstring(), 7680, 4320, 18000});
        r.push_back({L"C:\\Videos\\city_rain_4k.mp4", p2.wstring(), 3840, 2160, 10000});
        r.push_back({L"C:\\Videos\\hevc_sample.mp4", p3.wstring(), 3840, 2160, 12000});
        return r;
    };

    // Interactive callback verification across all video actions + tabs
    int cb_pick = 0, cb_pause = 0, cb_cancel = 0, cb_confirm = 0, cb_takeover = 0, cb_store = 0;
    std::wstring cb_set_path;
    VideoFillMode cb_fill = VideoFillMode::Fill;
    auto make_cbs = [&]() {
        OverlayCallbacks cbs{};
        cbs.on_pick_video_wallpaper = [&]() { ++cb_pick; };
        cbs.on_toggle_video_pause = [&]() { ++cb_pause; };
        cbs.on_set_video_wallpaper = [&](const std::wstring& path) { cb_set_path = path; };
        cbs.on_set_video_fill_mode = [&](VideoFillMode m) { cb_fill = m; };
        cbs.on_cancel_video_optimize = [&]() { ++cb_cancel; };
        cbs.on_confirm_heavy_video_optimize = [&]() { ++cb_confirm; };
        cbs.on_open_codec_store = [&](const std::wstring&) { ++cb_store; };
        cbs.on_enable_icon_takeover = [&]() { ++cb_takeover; };
        return cbs;
    };

    auto render_case = [&](const wchar_t* filename,
                           DesktopOverlayController::BeautifyTab tab,
                           bool dark,
                           const VideoWallpaperUiState& vs,
                           bool with_live_frame,
                           bool drop_hover,
                           DesktopOverlayController* out_overlay = nullptr) -> bool {
        DesktopOverlayController local_ov;
        DesktopOverlayController& ov = out_overlay ? *out_overlay : local_ov;
        state.settings.dark_mode = dark;
        DesktopBeautifyTestPeer::Prepare(ov, state.settings, vs, tab, dark, make_cbs(),
                                         drop_hover, drop_hover ? L"sunset_8k.mp4" : L"");
        if (with_live_frame) {
            ov.UpdateBeautifyVideoLiveFrame(live_bgra.data(), 320, 180, 320 * 4);
        }
        const fs::path out_png = work_tmp / filename;
        const bool ok = renderer.RenderCompositePng(
            out_png.wstring(), state, {}, {}, live, inter,
            [&](ID2D1DeviceContext* dc) { ov.CompositeOnto(dc, -16, -16); });
        const bool bounds_ok = DesktopBeautifyTestPeer::CheckRegionsInBounds(ov);
        if (snap_dir.empty()) DeleteFileW(out_png.c_str());
        return ok && bounds_ok;
    };

    // 1. Default image wallpaper (none) - Dark & Light
    {
        VideoWallpaperUiState vs{};
        vs.phase = VideoWallpaperPhase::Inactive;
        vs.icon_takeover_active = true;
        vs.recent = make_recent();
        Check(render_case(L"wall_none_dark.png", DesktopOverlayController::BeautifyTab::Wall, true, vs, false, false),
              "drawer visual: Wall tab (none / Dark) renders with all hit regions in bounds");
        Check(render_case(L"wall_none_light.png", DesktopOverlayController::BeautifyTab::Wall, false, vs, false, false),
              "drawer visual: Wall tab (none / Light) renders with all hit regions in bounds");
    }

    // 2. Active optimized video + live preview + Fill/Fit + pause + recent clicks
    {
        VideoWallpaperUiState vs{};
        vs.phase = VideoWallpaperPhase::PlayingOptimized;
        vs.icon_takeover_active = true;
        vs.active_source_path = L"C:\\Videos\\ocean_waves_8k.mp4";
        vs.active_display_name = L"ocean_waves_8k.mp4";
        vs.active_poster_path = p1.wstring();
        vs.source_w = 7680; vs.source_h = 4320;
        vs.target_w = 3840; vs.target_h = 2160;
        vs.duration_ms = 18000;
        vs.fill_mode = VideoFillMode::Fill;
        vs.status_text = L"已优化为 4K";
        vs.sub_text = L"原片 8K 30fps · 缓存命中，静音循环中";
        vs.recent = make_recent();

        DesktopOverlayController ov;
        Check(render_case(L"wall_optimized_dark.png", DesktopOverlayController::BeautifyTab::Wall, true, vs, true, false, &ov),
              "drawer visual: Wall tab (optimized active / Dark) renders cleanly");
        Check(render_case(L"wall_optimized_light.png", DesktopOverlayController::BeautifyTab::Wall, false, vs, true, false),
              "drawer visual: Wall tab (optimized active / Light) renders cleanly");

        Check(DesktopBeautifyTestPeer::ClickAction(ov, "b-vid-pause") && cb_pause == 1,
              "drawer click: b-vid-pause dispatches on_toggle_video_pause");
        Check(DesktopBeautifyTestPeer::ClickAction(ov, "b-vfill:1") && cb_fill == VideoFillMode::Fit,
              "drawer click: b-vfill:1 dispatches on_set_video_fill_mode(Fit)");
        Check(DesktopBeautifyTestPeer::ClickAction(ov, "b-vid-pick") && cb_pick == 1,
              "drawer click: b-vid-pick dispatches on_pick_video_wallpaper");
        Check(DesktopBeautifyTestPeer::ClickAction(ov, "b-vid-recent:1") && cb_set_path == L"C:\\Videos\\city_rain_4k.mp4",
              "drawer click: b-vid-recent:1 dispatches on_set_video_wallpaper");
        Check(DesktopBeautifyTestPeer::ClickAction(ov, "b-tab:2") &&
              DesktopBeautifyTestPeer::CurrentTab(ov) == DesktopOverlayController::BeautifyTab::Desk,
              "drawer click: b-tab:2 switches to Desk tab");
    }

    // 3. Optimizing 42% + cancel button
    {
        VideoWallpaperUiState vs{};
        vs.phase = VideoWallpaperPhase::Optimizing;
        vs.icon_takeover_active = true;
        vs.active_source_path = L"C:\\Videos\\ocean_waves_8k.mp4";
        vs.active_display_name = L"ocean_waves_8k.mp4";
        vs.active_poster_path = p1.wstring();
        vs.source_w = 7680; vs.source_h = 4320;
        vs.target_w = 3840; vs.target_h = 2160;
        vs.optimize_progress = 0.42f;
        vs.status_text = L"正在优化为 4K · 42%";
        vs.sub_text = L"当前显示第一帧，完成后自动切换流畅播放";
        vs.recent = make_recent();
        DesktopOverlayController ov;
        Check(render_case(L"wall_optimizing_dark.png", DesktopOverlayController::BeautifyTab::Wall, true, vs, false, false, &ov),
              "drawer visual: Wall tab (optimizing 42%) renders cleanly");
        Check(DesktopBeautifyTestPeer::ClickAction(ov, "b-vid-cancel") && cb_cancel == 1,
              "drawer click: b-vid-cancel dispatches on_cancel_video_optimize");
    }

    // 4. Confirm heavy SW transcode
    {
        VideoWallpaperUiState vs{};
        vs.phase = VideoWallpaperPhase::ConfirmHeavyTranscode;
        vs.icon_takeover_active = true;
        vs.tried_source_path = L"C:\\Videos\\h264_8k.mp4";
        vs.tried_display_name = L"h264_8k.mp4";
        vs.tried_poster_path = p2.wstring();
        vs.status_text = L"视频较大，首次优化约需 2–5 分钟";
        vs.sub_text = L"临时占用约 1.8 GB 内存，优化后每次直接秒开";
        vs.recent = make_recent();
        DesktopOverlayController ov;
        Check(render_case(L"wall_confirm_heavy_dark.png", DesktopOverlayController::BeautifyTab::Wall, true, vs, false, false, &ov),
              "drawer visual: Wall tab (confirm heavy transcode) renders cleanly");
        Check(DesktopBeautifyTestPeer::ClickAction(ov, "b-vid-confirm-heavy") && cb_confirm == 1,
              "drawer click: b-vid-confirm-heavy dispatches on_confirm_heavy_video_optimize");
    }

    // 5. Paused (fullscreen)
    {
        VideoWallpaperUiState vs{};
        vs.phase = VideoWallpaperPhase::PausedAuto;
        vs.icon_takeover_active = true;
        vs.pause_reason = VideoPauseReason::Fullscreen;
        vs.active_source_path = L"C:\\Videos\\ocean_waves_8k.mp4";
        vs.active_display_name = L"ocean_waves_8k.mp4";
        vs.active_poster_path = p1.wstring();
        vs.status_text = L"已暂停 · 检测到全屏应用";
        vs.sub_text = L"退出全屏后自动恢复播放";
        vs.recent = make_recent();
        Check(render_case(L"wall_paused_dark.png", DesktopOverlayController::BeautifyTab::Wall, true, vs, false, false),
              "drawer visual: Wall tab (paused / fullscreen) renders cleanly");
    }

    // 6. Missing HEVC codec -> Store button + ! badge
    {
        VideoWallpaperUiState vs{};
        vs.phase = VideoWallpaperPhase::MissingCodecHevc;
        vs.icon_takeover_active = true;
        vs.tried_source_path = L"C:\\Videos\\hevc_sample.mp4";
        vs.tried_display_name = L"hevc_sample.mp4";
        vs.store_uri = L"ms-windows-store://pdp/?ProductId=9NMZLZ57R3T7";
        vs.status_text = L"需要系统 HEVC 视频扩展";
        vs.sub_text = L"安装后即可硬件解码此视频并设为动态壁纸";
        vs.recent = make_recent();
        DesktopOverlayController ov;
        Check(render_case(L"wall_hevc_dark.png", DesktopOverlayController::BeautifyTab::Wall, true, vs, false, false, &ov),
              "drawer visual: Wall tab (missing HEVC codec) renders cleanly");
        Check(DesktopBeautifyTestPeer::ClickAction(ov, "b-vid-store") && cb_store == 1,
              "drawer click: b-vid-store dispatches on_open_codec_store");
    }

    // 7. Missing file + Requires icon takeover + Drag-over state
    {
        VideoWallpaperUiState vs{};
        vs.phase = VideoWallpaperPhase::FileMissing;
        vs.icon_takeover_active = true;
        vs.active_source_path = L"C:\\Videos\\deleted.mp4";
        vs.active_display_name = L"deleted.mp4";
        vs.status_text = L"原视频文件已移动或删除";
        vs.sub_text = L"请重新选择视频文件，或切回静态壁纸";
        vs.recent = make_recent();
        Check(render_case(L"wall_missing_dark.png", DesktopOverlayController::BeautifyTab::Wall, true, vs, false, false),
              "drawer visual: Wall tab (missing file) renders cleanly");

        vs.phase = VideoWallpaperPhase::RequiresIconTakeover;
        vs.icon_takeover_active = false;
        vs.status_text = L"视频壁纸需要由桌面整理显示图标";
        vs.sub_text = L"开启后桌面图标绘制在视频上方，不遮挡动态画面";
        DesktopOverlayController ov;
        Check(render_case(L"wall_takeover_dark.png", DesktopOverlayController::BeautifyTab::Wall, true, vs, false, false, &ov),
              "drawer visual: Wall tab (requires icon takeover) renders cleanly");
        Check(DesktopBeautifyTestPeer::ClickAction(ov, "b-vid-takeover") && cb_takeover == 1,
              "drawer click: b-vid-takeover dispatches on_enable_icon_takeover");

        vs.phase = VideoWallpaperPhase::PlayingOptimized;
        vs.icon_takeover_active = true;
        vs.active_poster_path = p1.wstring();
        Check(render_case(L"wall_drop_dark.png", DesktopOverlayController::BeautifyTab::Wall, true, vs, true, true),
              "drawer visual: Wall tab (drag-over dashed overlay) renders cleanly");
    }

    // 8. Fx, Desk, Dock tabs (Dark & Light Fx)
    {
        VideoWallpaperUiState vs{};
        vs.icon_takeover_active = true;
        Check(render_case(L"fx_dark.png", DesktopOverlayController::BeautifyTab::Fx, true, vs, false, false),
              "drawer visual: Fx tab (Dark) renders cleanly");
        Check(render_case(L"fx_light.png", DesktopOverlayController::BeautifyTab::Fx, false, vs, false, false),
              "drawer visual: Fx tab (Light) renders cleanly");
        Check(render_case(L"desk_dark.png", DesktopOverlayController::BeautifyTab::Desk, true, vs, false, false),
              "drawer visual: Desk tab (Dark) renders cleanly");
        Check(render_case(L"dock_dark.png", DesktopOverlayController::BeautifyTab::Dock, true, vs, false, false),
              "drawer visual: Dock tab (Dark) renders cleanly");
    }

    DeleteFileW(p1.c_str());
    DeleteFileW(p2.c_str());
    DeleteFileW(p3.c_str());
    if (snap_dir.empty()) fs::remove_all(work_tmp, ec);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    fs::path snap_dir;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wcscmp(argv[i], L"--render-drawer") == 0) {
            snap_dir = argv[i + 1];
        }
    }

    TestPolicyTable();
    TestCacheAndEviction();
    TestPauseStateMachine();
    TestModelVideoWallpaperPersistence();
    TestCrossDeviceDCompAndKeyedMutex();
    TestRealClipsHardwareProbeAndPipeline();
    TestRendererVideoWallpaperAndGlassIntegration(snap_dir);
    TestBeautifyDrawerVisualAndInteractions(snap_dir);

    CoUninitialize();
    std::printf("VIDEO_WALLPAPER_TEST failed=%d\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
