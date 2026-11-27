// video_wallpaper_player.cpp - Dedicated-thread hardware video wallpaper player.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <psapi.h>
#include <wrl/client.h>

#include "host/video/video_prober.h"
#include "host/video/video_wallpaper_player.h"
#include "host/video/video_decode_format_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace pulse::desktop::video {
namespace {

using Microsoft::WRL::ComPtr;

double QpcNowMs() noexcept {
    static const double inv_freq_ms = []() noexcept {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? (1000.0 / static_cast<double>(f.QuadPart)) : 0.001;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * inv_freq_ms;
}

bool SupportsNv12Overlay(IDXGIAdapter* adapter, ID3D11Device* dev) noexcept {
    if (!adapter || !dev) return false;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIOutput> out;
        if (FAILED(adapter->EnumOutputs(i, &out)) || !out) break;
        ComPtr<IDXGIOutput3> out3;
        UINT flags = 0;
        if (SUCCEEDED(out.As(&out3)) &&
            SUCCEEDED(out3->CheckOverlaySupport(DXGI_FORMAT_NV12, dev, &flags)) &&
            flags != 0) {
            return true;
        }
    }
    return false;
}

} // namespace

struct VideoWallpaperPlayer::Pipeline {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIDevice1> dxgi_dev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IMFDXGIDeviceManager> mf_mgr;
    UINT mf_token = 0;
    bool independent_device = false;
    bool nv12_overlay_ok = false;

    // Active media resources (released completely on Close / FreezeAndRelease).
    ComPtr<IMFSourceReader> reader;
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    ComPtr<ID3D11VideoProcessorEnumerator> venum;
    ComPtr<ID3D11VideoProcessor> vp;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<IDXGISwapChain2> swap2;
    HANDLE frame_waitable = nullptr;
    ComPtr<ID3D11Texture2D> upload_tex;
    std::map<std::pair<ID3D11Texture2D*, UINT>, ComPtr<ID3D11VideoProcessorInputView>> in_views;

    // 1/4-res shared BGRA texture for Liquid Glass sampling & drawer preview.
    ComPtr<ID3D11VideoProcessorEnumerator> glass_venum;
    ComPtr<ID3D11VideoProcessor> glass_vp;
    std::map<std::pair<ID3D11Texture2D*, UINT>, ComPtr<ID3D11VideoProcessorInputView>> glass_in_views;
    ComPtr<ID3D11Texture2D> glass_tex;
    ComPtr<IDXGIKeyedMutex> glass_mutex;
    ComPtr<ID3D11VideoProcessorOutputView> glass_ov;
    ComPtr<ID3D11Texture2D> glass_staging;
    HANDLE glass_shared_handle = nullptr;
    uint32_t glass_w = 0;
    uint32_t glass_h = 0;
    uint64_t glass_seq = 0;
    double last_glass_ms = 0.0;

    // Last decoded texture for immediate glass refresh / freeze capture.
    ComPtr<IMFSample> last_shown_sample;
    ComPtr<ID3D11Texture2D> last_shown_tex;
    UINT last_shown_slice = 0;

    // Pending sample waiting for its QPC presentation timestamp.
    ComPtr<IMFSample> pending_sample;
    LONGLONG pending_pts = 0;

    // Stream metadata & clock state.
    uint64_t active_gen = 0;
    std::wstring current_path;
    uint32_t target_w = 1920;
    uint32_t target_h = 1080;
    uint32_t coded_w = 0;
    uint32_t coded_h = 0;
    uint32_t video_w = 0;
    uint32_t video_h = 0;
    uint32_t surface_w = 0;
    uint32_t surface_h = 0;
    RECT src_rect{};
    VideoColorInfo color{};
    DXGI_FORMAT in_fmt = DXGI_FORMAT_NV12;
    DXGI_FORMAT out_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    double frame_ms = 33.3333;
    LONGLONG frame_100ns = 333333;
    bool sw_decode = false;

    bool is_open = false;
    bool paused = false;
    bool frozen_released = false;
    bool have_clock_base = false;
    LONGLONG base_pts = 0;
    LONGLONG loop_pts_offset = 0;
    LONGLONG last_raw_pts = 0;
    LONGLONG saved_pause_pts = 0;
    double base_ms = 0.0;
    double first_frame_ms = -1.0;
    double reopen_first_frame_ms = -1.0;
    uint64_t frames_shown = 0;
    uint64_t frames_dropped = 0;
    uint64_t glass_frames = 0;
    uint64_t loop_count = 0;
    std::wstring last_error;
    bool profile = false;
    bool memory_profile = false;
    double profile_read_ms = 0.0, profile_wait_ms = 0.0, profile_blt_ms = 0.0;
    double profile_present_ms = 0.0, profile_glass_ms = 0.0, profile_total_ms = 0.0;
    double profile_callback_ms = 0.0;
    double profile_seek_ms = 0.0, profile_wake_late_ms = 0.0, profile_present_end = 0.0;
    LONGLONG profile_pts = 0;
    DWORD profile_wait_result = 0;
    unsigned profile_preroll_frames = 0;
    std::vector<std::string> profile_events;
    unsigned profile_omitted = 0;

    void TraceMemory(const char* stage) const {
        if (!memory_profile) return;
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> base;
        ComPtr<IDXGIAdapter3> memory_adapter;
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
        if (dev && SUCCEEDED(dev.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&base)) && SUCCEEDED(base.As(&memory_adapter))) {
            memory_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
            memory_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal);
        }
        std::printf("PIPELINE_MEMORY stage=%s working_set_bytes=%zu private_commit_bytes=%zu gpu_local_bytes=%llu gpu_nonlocal_bytes=%llu\n",
                    stage, memory.WorkingSetSize, memory.PrivateUsage, local.CurrentUsage, nonlocal.CurrentUsage);
    }

    void RecordProfile(const char* event, double late_ms = 0.0) {
        if (!profile) return;
        if (profile_events.size() >= 512) { ++profile_omitted; return; }
        char line[640]{};
        std::snprintf(line, sizeof(line),
            "VIDEO_PROFILE event=%s qpc_ms=%.3f pts=%lld loops=%llu late_ms=%.3f read_ms=%.3f last_wait_ms=%.3f wait_result=%lu last_blt_ms=%.3f last_present_ms=%.3f last_glass_ms=%.3f last_callback_ms=%.3f last_total_ms=%.3f seek_ms=%.3f wake_late_ms=%.3f present_age_ms=%.3f preroll_frames=%u",
            event, QpcNowMs(), profile_pts, loop_count, late_ms, profile_read_ms,
            profile_wait_ms, profile_wait_result, profile_blt_ms, profile_present_ms,
            profile_glass_ms, profile_callback_ms, profile_total_ms, profile_seek_ms, profile_wake_late_ms,
            profile_present_end > 0 ? QpcNowMs() - profile_present_end : -1.0, profile_preroll_frames);
        profile_events.emplace_back(line);
    }

    ~Pipeline() {
        if (frame_waitable) {
            CloseHandle(frame_waitable);
            frame_waitable = nullptr;
        }
        if (glass_shared_handle) {
            CloseHandle(glass_shared_handle);
            glass_shared_handle = nullptr;
        }
    }
};

VideoWallpaperPlayer::~VideoWallpaperPlayer() {
    Shutdown();
}

bool VideoWallpaperPlayer::Start(LUID adapter_luid,
                                 VideoPlayerCallbacks callbacks,
                                 ID3D11Device* shared_fallback_device,
                                 bool force_shared_device) {
    Shutdown();
    callbacks_ = std::move(callbacks);
    cmd_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!cmd_event_) return false;
    wait_timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
    if (!wait_timer_) {
        wait_timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    running_.store(true, std::memory_order_release);
    ComPtr<ID3D11Device> fallback_ref(shared_fallback_device);
    thread_ = std::thread([this, adapter_luid, fallback_ref, force_shared_device]() mutable {
        ThreadMain(adapter_luid, std::move(fallback_ref), force_shared_device);
    });
    return true;
}

void VideoWallpaperPlayer::Shutdown() noexcept {
    if (running_.exchange(false, std::memory_order_acq_rel)) {
        EnqueueCommand(Command{Command::Kind::Stop});
    }
    if (thread_.joinable()) {
        if (thread_.get_id() != std::this_thread::get_id()) {
            thread_.join();
        } else {
            thread_.detach();
        }
    }
    if (cmd_event_) {
        CloseHandle(cmd_event_);
        cmd_event_ = nullptr;
    }
    if (wait_timer_) {
        CloseHandle(wait_timer_);
        wait_timer_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(cmd_mtx_);
        cmd_queue_.clear();
    }
}

void VideoWallpaperPlayer::EnqueueCommand(Command cmd) {
    {
        std::lock_guard<std::mutex> lk(cmd_mtx_);
        cmd_queue_.push_back(std::move(cmd));
    }
    if (cmd_event_) SetEvent(cmd_event_);
}

uint64_t VideoWallpaperPlayer::OpenVideo(const std::wstring& video_path,
                                         uint32_t target_w,
                                         uint32_t target_h,
                                         bool start_paused,
                                         int64_t start_pos_100ns) {
    const uint64_t gen = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    Command cmd{};
    cmd.kind = Command::Kind::Open;
    cmd.generation = gen;
    cmd.path = video_path;
    cmd.target_w = std::max<uint32_t>(64u, target_w);
    cmd.target_h = std::max<uint32_t>(64u, target_h);
    cmd.start_paused = start_paused;
    cmd.start_pos_100ns = std::max<int64_t>(0, start_pos_100ns);
    EnqueueCommand(std::move(cmd));
    return gen;
}

void VideoWallpaperPlayer::CloseVideo() {
    const uint64_t gen = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    Command cmd{};
    cmd.kind = Command::Kind::Close;
    cmd.generation = gen;
    EnqueueCommand(std::move(cmd));
}

void VideoWallpaperPlayer::SetPaused(bool paused) {
    Command cmd{};
    cmd.kind = paused ? Command::Kind::Pause : Command::Kind::Resume;
    cmd.generation = generation_.load(std::memory_order_acquire);
    EnqueueCommand(std::move(cmd));
}

void VideoWallpaperPlayer::FreezeAndRelease() {
    Command cmd{};
    cmd.kind = Command::Kind::FreezeRelease;
    cmd.generation = generation_.load(std::memory_order_acquire);
    EnqueueCommand(std::move(cmd));
}

void VideoWallpaperPlayer::ResumeFromFrozen() {
    Command cmd{};
    cmd.kind = Command::Kind::ResumeFromFrozen;
    cmd.generation = generation_.load(std::memory_order_acquire);
    EnqueueCommand(std::move(cmd));
}

void VideoWallpaperPlayer::SetGlassLiveRate(uint8_t rate_mode) noexcept {
    glass_live_rate_.store(std::min<uint8_t>(rate_mode, 2u), std::memory_order_release);
}

void VideoWallpaperPlayer::SetGlassConsumerActive(bool active) noexcept {
    glass_consumer_active_.store(active, std::memory_order_release);
}

void VideoWallpaperPlayer::SetPreviewConsumerActive(bool active) noexcept {
    preview_consumer_active_.store(active, std::memory_order_release);
}

void VideoWallpaperPlayer::RequestGlassRefresh() {
    Command cmd{};
    cmd.kind = Command::Kind::RefreshGlass;
    cmd.generation = generation_.load(std::memory_order_acquire);
    EnqueueCommand(std::move(cmd));
}

void VideoWallpaperPlayer::TrimDeviceMemory() {
    Command cmd{};
    cmd.kind = Command::Kind::TrimMemory;
    EnqueueCommand(std::move(cmd));
}

bool VideoWallpaperPlayer::CopyLatestPreviewFrame(FrozenFrameSnapshot& out_frame) const {
    std::lock_guard<std::mutex> lk(preview_mtx_);
    if (latest_preview_.width == 0 || latest_preview_.height == 0 || latest_preview_.bgra.empty()) {
        return false;
    }
    out_frame = latest_preview_;
    return true;
}

VideoPlayerStats VideoWallpaperPlayer::Stats() const {
    std::lock_guard<std::mutex> lk(stats_mtx_);
    return stats_;
}

void VideoWallpaperPlayer::UpdateStatsLocked(const Pipeline& p) {
    std::lock_guard<std::mutex> lk(stats_mtx_);
    stats_.open = p.is_open;
    stats_.playing = p.is_open && !p.paused && !p.frozen_released;
    stats_.paused = p.paused;
    stats_.frozen_released = p.frozen_released;
    stats_.software_decode = p.sw_decode;
    stats_.using_independent_device = p.independent_device;
    stats_.input_format = p.in_fmt;
    stats_.swap_format = p.out_fmt;
    stats_.video_width = p.video_w;
    stats_.video_height = p.video_h;
    stats_.surface_width = p.surface_w;
    stats_.surface_height = p.surface_h;
    stats_.glass_width = p.glass_w;
    stats_.glass_height = p.glass_h;
    stats_.first_frame_ms = p.first_frame_ms;
    stats_.reopen_first_frame_ms = p.reopen_first_frame_ms;
    stats_.frames_shown = p.frames_shown;
    stats_.frames_dropped = p.frames_dropped;
    stats_.glass_frames_published = p.glass_frames;
    stats_.loop_count = p.loop_count;
    stats_.video_input_views = static_cast<uint32_t>(p.in_views.size());
    stats_.glass_input_views = static_cast<uint32_t>(p.glass_in_views.size());
    if (p.memory_profile) {
        std::set<ID3D11Texture2D*> textures;
        for (const auto& entry : p.in_views) textures.insert(entry.first.first);
        for (const auto& entry : p.glass_in_views) textures.insert(entry.first.first);
        uint64_t nominal_bytes = 0;
        for (auto* texture : textures) {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            const uint64_t pixels = uint64_t{desc.Width} * desc.Height * desc.ArraySize;
            nominal_bytes += desc.Format == DXGI_FORMAT_NV12 ? pixels * 3 / 2 :
                             desc.Format == DXGI_FORMAT_P010 ? pixels * 3 : pixels * 4;
        }
        stats_.retained_decode_textures = static_cast<uint32_t>(textures.size());
        stats_.retained_decode_nominal_bytes = nominal_bytes;
    }
    stats_.position_100ns = p.last_raw_pts;
    stats_.last_error = p.last_error;
}

bool VideoWallpaperPlayer::InitDevice(Pipeline& p, LUID adapter_luid,
                                      ID3D11Device* fallback_dev, bool force_shared) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    if (!force_shared) {
        ComPtr<IDXGIAdapter1> matched;
        ComPtr<IDXGIFactory1> fac1;
        if ((adapter_luid.LowPart != 0 || adapter_luid.HighPart != 0) &&
            SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac1))) && fac1) {
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> cand;
                if (FAILED(fac1->EnumAdapters1(i, &cand)) || !cand) break;
                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(cand->GetDesc1(&desc)) &&
                    desc.AdapterLuid.LowPart == adapter_luid.LowPart &&
                    desc.AdapterLuid.HighPart == adapter_luid.HighPart) {
                    matched = cand;
                    break;
                }
            }
        }
        const HRESULT hr = D3D11CreateDevice(
            matched.Get(),
            matched ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, flags, levels, 2, D3D11_SDK_VERSION, &p.dev, nullptr, &p.ctx);
        if (SUCCEEDED(hr) && p.dev && p.ctx) {
            p.independent_device = true;
        }
    }

    if (!p.dev && fallback_dev) {
        p.dev = fallback_dev;
        p.dev->GetImmediateContext(&p.ctx);
        p.independent_device = false;
    }
    if (!p.dev || !p.ctx) return false;

    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(p.dev.As(&mt)) && mt) mt->SetMultithreadProtected(TRUE);

    if (FAILED(p.dev.As(&p.dxgi_dev)) || !p.dxgi_dev) return false;
    if (FAILED(p.dxgi_dev->GetAdapter(&p.adapter)) || !p.adapter) return false;
    if (FAILED(p.adapter->GetParent(IID_PPV_ARGS(&p.factory))) || !p.factory) return false;

    if (FAILED(MFCreateDXGIDeviceManager(&p.mf_token, &p.mf_mgr)) ||
        FAILED(p.mf_mgr->ResetDevice(p.dev.Get(), p.mf_token))) {
        return false;
    }

    p.nv12_overlay_ok = SupportsNv12Overlay(p.adapter.Get(), p.dev.Get());
    return true;
}

void VideoWallpaperPlayer::ReleaseMediaPipeline(Pipeline& p) noexcept {
    p.pending_sample.Reset();
    p.last_shown_sample.Reset();
    p.last_shown_tex.Reset();
    p.last_shown_slice = 0;

    p.in_views.clear();
    p.glass_in_views.clear();
    p.glass_ov.Reset();
    p.glass_vp.Reset();
    p.glass_venum.Reset();
    p.glass_mutex.Reset();
    p.glass_tex.Reset();
    p.glass_staging.Reset();
    if (p.glass_shared_handle) {
        CloseHandle(p.glass_shared_handle);
        p.glass_shared_handle = nullptr;
    }
    p.glass_w = 0;
    p.glass_h = 0;

    if (p.frame_waitable) {
        CloseHandle(p.frame_waitable);
        p.frame_waitable = nullptr;
    }
    p.swap2.Reset();
    p.swap.Reset();
    p.upload_tex.Reset();
    p.vp.Reset();
    p.venum.Reset();
    p.vctx.Reset();
    p.vdev.Reset();
    p.reader.Reset();

    if (p.ctx) {
        p.ctx->ClearState();
        p.ctx->Flush();
    }
    if (p.dxgi_dev) {
        ComPtr<IDXGIDevice3> dxgi3;
        if (SUCCEEDED(p.dxgi_dev.As(&dxgi3)) && dxgi3) {
            dxgi3->Trim();
        }
    }
}

bool VideoWallpaperPlayer::EnsureVideoProcessorAndSwapChain(Pipeline& p) {
    if (p.vp && p.swap) return true;
    if (FAILED(p.dev.As(&p.vdev)) || FAILED(p.ctx.As(&p.vctx)) || !p.vdev || !p.vctx) {
        p.last_error = L"no D3D11 video device";
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = p.coded_w;
    cd.InputHeight = p.coded_h;
    cd.OutputWidth = p.surface_w;
    cd.OutputHeight = p.surface_h;
    cd.InputFrameRate = {30, 1};
    cd.OutputFrameRate = {30, 1};
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(p.vdev->CreateVideoProcessorEnumerator(&cd, &p.venum)) ||
        FAILED(p.vdev->CreateVideoProcessor(p.venum.Get(), 0, &p.vp))) {
        p.last_error = L"CreateVideoProcessor failed";
        return false;
    }

    auto make_swap = [&](DXGI_FORMAT fmt) -> bool {
        UINT support = 0;
        if (FAILED(p.venum->CheckVideoProcessorFormat(fmt, &support)) ||
            !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            return false;
        }
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = p.surface_w;
        sd.Height = p.surface_h;
        sd.Format = fmt;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        p.swap.Reset();
        p.swap2.Reset();
        if (p.frame_waitable) {
            CloseHandle(p.frame_waitable);
            p.frame_waitable = nullptr;
        }
        if (FAILED(p.factory->CreateSwapChainForComposition(p.dev.Get(), &sd, nullptr, &p.swap)) || !p.swap) {
            return false;
        }
        if (SUCCEEDED(p.swap.As(&p.swap2)) && p.swap2) {
            p.swap2->SetMaximumFrameLatency(1);
            p.frame_waitable = p.swap2->GetFrameLatencyWaitableObject();
        }
        if (fmt == DXGI_FORMAT_NV12) {
            ComPtr<IDXGISwapChain3> sc3;
            if (SUCCEEDED(p.swap.As(&sc3)) && sc3) {
                sc3->SetColorSpace1(DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
            }
        }
        p.out_fmt = fmt;
        return true;
    };

    bool created = false;
    if (p.nv12_overlay_ok) {
        created = make_swap(DXGI_FORMAT_NV12);
    }
    if (!created) {
        created = make_swap(DXGI_FORMAT_B8G8R8A8_UNORM);
    }
    if (!created) {
        p.last_error = L"CreateSwapChainForComposition failed";
        return false;
    }

    ComPtr<ID3D11VideoContext1> vctx1;
    if (SUCCEEDED(p.vctx.As(&vctx1)) && vctx1) {
        const DXGI_COLOR_SPACE_TYPE in_cs = MapMfToDxgiColorSpace(p.color, p.video_w, p.video_h);
        const DXGI_COLOR_SPACE_TYPE out_cs = (p.out_fmt == DXGI_FORMAT_NV12)
                                                 ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709
                                                 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        vctx1->VideoProcessorSetStreamColorSpace1(p.vp.Get(), 0, in_cs);
        vctx1->VideoProcessorSetOutputColorSpace1(p.vp.Get(), out_cs);
    }
    p.vctx->VideoProcessorSetStreamSourceRect(p.vp.Get(), 0, TRUE, &p.src_rect);
    const RECT out_rect{0, 0, static_cast<LONG>(p.surface_w), static_cast<LONG>(p.surface_h)};
    p.vctx->VideoProcessorSetStreamDestRect(p.vp.Get(), 0, TRUE, &out_rect);
    p.vctx->VideoProcessorSetOutputTargetRect(p.vp.Get(), TRUE, &out_rect);
    p.vctx->VideoProcessorSetStreamAutoProcessingMode(p.vp.Get(), 0, FALSE);
    return true;
}

bool VideoWallpaperPlayer::EnsureGlassSharedTexture(Pipeline& p) {
    const uint32_t gw = std::max<uint32_t>(64u, (p.surface_w / 4u) & ~1u);
    const uint32_t gh = std::max<uint32_t>(36u, (p.surface_h / 4u) & ~1u);
    if (p.glass_tex && p.glass_vp && p.glass_ov && p.glass_w == gw && p.glass_h == gh) {
        return true;
    }

    p.glass_in_views.clear();
    p.glass_ov.Reset();
    p.glass_vp.Reset();
    p.glass_venum.Reset();
    p.glass_mutex.Reset();
    p.glass_tex.Reset();
    p.glass_staging.Reset();
    if (p.glass_shared_handle) {
        CloseHandle(p.glass_shared_handle);
        p.glass_shared_handle = nullptr;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = p.coded_w;
    cd.InputHeight = p.coded_h;
    cd.OutputWidth = gw;
    cd.OutputHeight = gh;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(p.vdev->CreateVideoProcessorEnumerator(&cd, &p.glass_venum)) ||
        FAILED(p.vdev->CreateVideoProcessor(p.glass_venum.Get(), 0, &p.glass_vp))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = gw;
    td.Height = gh;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (FAILED(p.dev->CreateTexture2D(&td, nullptr, &p.glass_tex)) || !p.glass_tex) {
        return false;
    }

    if (FAILED(p.glass_tex.As(&p.glass_mutex)) || !p.glass_mutex) {
        p.glass_tex.Reset();
        return false;
    }

    ComPtr<IDXGIResource1> res1;
    if (FAILED(p.glass_tex.As(&res1)) ||
        FAILED(res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                        nullptr, &p.glass_shared_handle)) ||
        !p.glass_shared_handle) {
        p.glass_mutex.Reset();
        p.glass_tex.Reset();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (FAILED(p.vdev->CreateVideoProcessorOutputView(p.glass_tex.Get(), p.glass_venum.Get(), &ovd, &p.glass_ov))) {
        CloseHandle(p.glass_shared_handle);
        p.glass_shared_handle = nullptr;
        p.glass_mutex.Reset();
        p.glass_tex.Reset();
        return false;
    }

    ComPtr<ID3D11VideoContext1> vctx1;
    if (SUCCEEDED(p.vctx.As(&vctx1)) && vctx1) {
        vctx1->VideoProcessorSetStreamColorSpace1(p.glass_vp.Get(), 0,
                                                  MapMfToDxgiColorSpace(p.color, p.video_w, p.video_h));
        vctx1->VideoProcessorSetOutputColorSpace1(p.glass_vp.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    }
    const RECT dst{0, 0, static_cast<LONG>(gw), static_cast<LONG>(gh)};
    p.vctx->VideoProcessorSetStreamSourceRect(p.glass_vp.Get(), 0, TRUE, &p.src_rect);
    p.vctx->VideoProcessorSetStreamDestRect(p.glass_vp.Get(), 0, TRUE, &dst);
    p.vctx->VideoProcessorSetOutputTargetRect(p.glass_vp.Get(), TRUE, &dst);
    p.vctx->VideoProcessorSetStreamAutoProcessingMode(p.glass_vp.Get(), 0, FALSE);

    p.glass_w = gw;
    p.glass_h = gh;
    return true;
}

bool VideoWallpaperPlayer::PublishGlassFrame(Pipeline& p, ID3D11Texture2D* decoded_tex, UINT slice, bool force) {
    if (!decoded_tex) return false;
    const uint8_t rate = glass_live_rate_.load(std::memory_order_acquire);
    const bool consumer = glass_consumer_active_.load(std::memory_order_acquire);
    const bool preview = preview_consumer_active_.load(std::memory_order_acquire);
    if (!force) {
        double min_ms = 0.0;
        if (consumer && rate > 0) {
            // Use 28 ms / 14 ms debounce (instead of 33 / 16) so 30/60 fps videos with +/-1.5 ms
            // timer jitter never hit a beat-frequency skip that halves Dock/glass refresh rate.
            min_ms = (rate >= 2) ? 14.0 : 28.0;
        } else if (preview) {
            // Drawer live preview stays smooth (~20 fps) even when glass refraction is set to Static.
            min_ms = 48.0;
        } else {
            return false;
        }
        const double now = QpcNowMs();
        if (now - p.last_glass_ms < min_ms) return false;
    }
    if (!EnsureGlassSharedTexture(p)) return false;

    auto key = std::make_pair(decoded_tex, slice);
    ID3D11VideoProcessorInputView* iv = nullptr;
    if (auto it = p.glass_in_views.find(key); it != p.glass_in_views.end()) {
        iv = it->second.Get();
    } else {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.ArraySlice = slice;
        ComPtr<ID3D11VideoProcessorInputView> created;
        if (FAILED(p.vdev->CreateVideoProcessorInputView(decoded_tex, p.glass_venum.Get(), &ivd, &created))) {
            return false;
        }
        iv = created.Get();
        p.glass_in_views[key] = std::move(created);
    }

    // Glass is an optional consumer, not a reason to delay the next video frame.
    // If the UI device still uses this surface, publish the next available frame.
    if (p.glass_mutex->AcquireSync(0, 0) != S_OK) {
        return false;
    }
    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = iv;
    const HRESULT blt_hr = p.vctx->VideoProcessorBlt(p.glass_vp.Get(), p.glass_ov.Get(), 0, 1, &st);
    p.glass_mutex->ReleaseSync(0);
    if (FAILED(blt_hr)) return false;

    p.last_glass_ms = QpcNowMs();
    ++p.glass_seq;
    ++p.glass_frames;

    // On forced updates (first frame / pause / switch), also cache a CPU copy for the BeautifyDrawer preview.
    if (force) {
        if (!p.glass_staging) {
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = p.glass_w;
            sd.Height = p.glass_h;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            p.dev->CreateTexture2D(&sd, nullptr, &p.glass_staging);
        }
        if (p.glass_staging && p.glass_mutex->AcquireSync(0, 10) == S_OK) {
            p.ctx->CopyResource(p.glass_staging.Get(), p.glass_tex.Get());
            p.glass_mutex->ReleaseSync(0);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(p.ctx->Map(p.glass_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData) {
                FrozenFrameSnapshot snap{};
                snap.width = p.glass_w;
                snap.height = p.glass_h;
                snap.stride = p.glass_w * 4u;
                snap.bgra.resize(static_cast<size_t>(snap.stride) * snap.height);
                for (uint32_t y = 0; y < snap.height; ++y) {
                    const uint8_t* srow = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
                    uint8_t* drow = snap.bgra.data() + static_cast<size_t>(y) * snap.stride;
                    std::memcpy(drow, srow, snap.stride);
                    for (uint32_t x = 0; x < snap.width; ++x) drow[x * 4u + 3u] = 255u;
                }
                p.ctx->Unmap(p.glass_staging.Get(), 0);
                std::lock_guard<std::mutex> lk(preview_mtx_);
                latest_preview_ = std::move(snap);
            }
        }
    }

    if (callbacks_.on_glass_frame_ready && p.glass_shared_handle) {
        const double profile_callback_start = p.profile ? QpcNowMs() : 0.0;
        callbacks_.on_glass_frame_ready(p.active_gen, p.glass_seq, p.glass_shared_handle, p.glass_w, p.glass_h);
        if (p.profile) p.profile_callback_ms = QpcNowMs() - profile_callback_start;
    }
    return true;
}

bool VideoWallpaperPlayer::CaptureFrozenSnapshot(Pipeline& p, FrozenFrameSnapshot& out) {
    if (!p.last_shown_tex || !p.vdev || !p.vctx || p.surface_w == 0 || p.surface_h == 0) {
        return CopyLatestPreviewFrame(out);
    }

    // Blit the last shown decoded frame to a temporary BGRA texture at surface_w x surface_h
    // and read it back once so DesktopRenderer can hold a single static DComp surface while
    // we destroy the entire decoder + VideoProcessor + swap chain.
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = p.coded_w;
    cd.InputHeight = p.coded_h;
    cd.OutputWidth = p.surface_w;
    cd.OutputHeight = p.surface_h;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;

    ComPtr<ID3D11VideoProcessorEnumerator> venum;
    ComPtr<ID3D11VideoProcessor> vp;
    if (FAILED(p.vdev->CreateVideoProcessorEnumerator(&cd, &venum)) ||
        FAILED(p.vdev->CreateVideoProcessor(venum.Get(), 0, &vp))) {
        return CopyLatestPreviewFrame(out);
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = p.surface_w;
    td.Height = p.surface_h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> gpu_tex;
    if (FAILED(p.dev->CreateTexture2D(&td, nullptr, &gpu_tex))) return CopyLatestPreviewFrame(out);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.ArraySlice = p.last_shown_slice;
    ComPtr<ID3D11VideoProcessorInputView> iv;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> ov;
    if (FAILED(p.vdev->CreateVideoProcessorInputView(p.last_shown_tex.Get(), venum.Get(), &ivd, &iv)) ||
        FAILED(p.vdev->CreateVideoProcessorOutputView(gpu_tex.Get(), venum.Get(), &ovd, &ov))) {
        return CopyLatestPreviewFrame(out);
    }

    ComPtr<ID3D11VideoContext1> vctx1;
    if (SUCCEEDED(p.vctx.As(&vctx1)) && vctx1) {
        vctx1->VideoProcessorSetStreamColorSpace1(vp.Get(), 0,
                                                  MapMfToDxgiColorSpace(p.color, p.video_w, p.video_h));
        vctx1->VideoProcessorSetOutputColorSpace1(vp.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    }
    const RECT dst{0, 0, static_cast<LONG>(p.surface_w), static_cast<LONG>(p.surface_h)};
    p.vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &p.src_rect);
    p.vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dst);
    p.vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &dst);
    p.vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);

    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = iv.Get();
    if (FAILED(p.vctx->VideoProcessorBlt(vp.Get(), ov.Get(), 0, 1, &st))) {
        return CopyLatestPreviewFrame(out);
    }

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stg;
    if (FAILED(p.dev->CreateTexture2D(&td, nullptr, &stg))) return CopyLatestPreviewFrame(out);
    p.ctx->CopyResource(stg.Get(), gpu_tex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(p.ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
        return CopyLatestPreviewFrame(out);
    }
    out.width = p.surface_w;
    out.height = p.surface_h;
    out.stride = p.surface_w * 4u;
    out.bgra.resize(static_cast<size_t>(out.stride) * out.height);
    for (uint32_t y = 0; y < out.height; ++y) {
        const uint8_t* srow = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        uint8_t* drow = out.bgra.data() + static_cast<size_t>(y) * out.stride;
        std::memcpy(drow, srow, out.stride);
        for (uint32_t x = 0; x < out.width; ++x) drow[x * 4u + 3u] = 255u;
    }
    p.ctx->Unmap(stg.Get(), 0);
    return true;
}

bool VideoWallpaperPlayer::PresentSample(Pipeline& p, IMFSample* sample, bool force_glass) {
    if (!sample) return false;
    const double profile_start = p.profile ? QpcNowMs() : 0.0;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->GetBufferByIndex(0, &buf)) || !buf) return false;

    ComPtr<ID3D11Texture2D> tex;
    UINT slice = 0;
    ComPtr<IMFDXGIBuffer> dxgi_buf;
    if (SUCCEEDED(buf.As(&dxgi_buf)) && SUCCEEDED(dxgi_buf->GetResource(IID_PPV_ARGS(&tex))) && tex) {
        dxgi_buf->GetSubresourceIndex(&slice);
    } else {
        p.sw_decode = true;
        if (!p.upload_tex) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = p.coded_w;
            td.Height = p.coded_h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = p.in_fmt;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(p.dev->CreateTexture2D(&td, nullptr, &p.upload_tex))) return false;
        }
        ComPtr<IMF2DBuffer> b2;
        BYTE* data = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&data, &pitch))) {
            p.ctx->UpdateSubresource(p.upload_tex.Get(), 0, nullptr, data, static_cast<UINT>(pitch), 0);
            b2->Unlock2D();
        } else {
            DWORD len = 0;
            if (FAILED(buf->Lock(&data, nullptr, &len))) return false;
            const UINT bpp = (p.in_fmt == DXGI_FORMAT_P010) ? 2u : 1u;
            p.ctx->UpdateSubresource(p.upload_tex.Get(), 0, nullptr, data, p.coded_w * bpp, 0);
            buf->Unlock();
        }
        tex = p.upload_tex;
    }

    if (!EnsureVideoProcessorAndSwapChain(p)) return false;

    // Wait on waitable swap chain object (non-infinite timeout so hidden/offscreen windows never hang).
    const double profile_wait_start = p.profile ? QpcNowMs() : 0.0;
    if (p.frame_waitable) {
        p.profile_wait_result = WaitForSingleObjectEx(p.frame_waitable, 50, FALSE);
    }
    const double profile_blt_start = p.profile ? QpcNowMs() : 0.0;
    if (p.profile) p.profile_wait_ms = profile_blt_start - profile_wait_start;

    auto key = std::make_pair(tex.Get(), slice);
    ID3D11VideoProcessorInputView* iv = nullptr;
    if (auto it = p.in_views.find(key); it != p.in_views.end()) {
        iv = it->second.Get();
    } else {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.ArraySlice = slice;
        ComPtr<ID3D11VideoProcessorInputView> created;
        if (FAILED(p.vdev->CreateVideoProcessorInputView(tex.Get(), p.venum.Get(), &ivd, &created))) {
            return false;
        }
        iv = created.Get();
        p.in_views[key] = std::move(created);
    }

    ComPtr<ID3D11Texture2D> back;
    if (FAILED(p.swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> ov;
    if (FAILED(p.vdev->CreateVideoProcessorOutputView(back.Get(), p.venum.Get(), &ovd, &ov))) return false;

    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = iv;
    if (FAILED(p.vctx->VideoProcessorBlt(p.vp.Get(), ov.Get(), 0, 1, &st))) return false;

    const double profile_present_start = p.profile ? QpcNowMs() : 0.0;
    if (p.profile) p.profile_blt_ms = profile_present_start - profile_blt_start;
    const HRESULT pres_hr = p.swap->Present(1, 0);
    if (p.profile) p.profile_present_ms = QpcNowMs() - profile_present_start;
    if (pres_hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        return true;
    }
    if (FAILED(pres_hr)) {
        wchar_t err_buf[64]{};
        std::swprintf(err_buf, ARRAYSIZE(err_buf), L"SwapChain Present failed (0x%08lX)",
                      static_cast<unsigned long>(pres_hr));
        p.last_error = err_buf;
        return false;
    }

    p.last_shown_sample = sample;
    p.last_shown_tex = tex;
    p.last_shown_slice = slice;
    ++p.frames_shown;

    const double profile_glass_start = p.profile ? QpcNowMs() : 0.0;
    PublishGlassFrame(p, tex.Get(), slice, force_glass);
    if (p.profile) {
        p.profile_present_end = QpcNowMs();
        p.profile_glass_ms = p.profile_present_end - profile_glass_start;
        p.profile_total_ms = p.profile_present_end - profile_start;
        if (p.profile_total_ms > 33.0) p.RecordProfile("slow_present");
    }
    return true;
}

bool VideoWallpaperPlayer::OpenPipeline(Pipeline& p, const std::wstring& path,
                                        uint32_t target_w, uint32_t target_h,
                                        int64_t start_pos_100ns, bool is_reopen) {
    ReleaseMediaPipeline(p);
    p.last_error.clear();
    p.sw_decode = false;
    p.have_clock_base = false;
    p.loop_pts_offset = 0;
    p.last_raw_pts = start_pos_100ns;
    p.current_path = path;
    p.target_w = target_w;
    p.target_h = target_h;
    if (!is_reopen) {
        p.frames_shown = 0;
        p.frames_dropped = 0;
        p.glass_frames = 0;
        p.loop_count = 0;
    }

    const double open_t0 = QpcNowMs();
    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 3))) {
        p.last_error = L"MFCreateAttributes failed";
        return false;
    }
    attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, p.mf_mgr.Get());
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), attrs.Get(), &p.reader);
    p.TraceMemory("source_reader_created");
    if (FAILED(hr) || !p.reader) {
        p.last_error = L"MFCreateSourceReaderFromURL failed";
        return false;
    }

    const DWORD vs = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    p.reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    p.reader->SetStreamSelection(vs, TRUE);

    GUID formats[]{MFVideoFormat_P010, MFVideoFormat_NV12};
    ComPtr<IMFMediaType> native_type;
    GUID native_subtype{};
    UINT32 native_profile = 0;
    bool nv12_first = false;
    if (SUCCEEDED(p.reader->GetNativeMediaType(vs, 0, &native_type)) &&
        SUCCEEDED(native_type->GetGUID(MF_MT_SUBTYPE, &native_subtype))) {
        native_type->GetUINT32(MF_MT_MPEG2_PROFILE, &native_profile);
        if (!native_profile) native_type->GetUINT32(MF_MT_VIDEO_PROFILE, &native_profile);
        nv12_first = (native_subtype == MFVideoFormat_H264 || native_subtype == MFVideoFormat_H264_ES) &&
            IsKnownEightBitAvcProfile(native_profile);
        if (nv12_first) std::swap(formats[0], formats[1]);
    }
    if (p.memory_profile) std::printf("DECODER_FORMAT_ORDER native_profile=%u nv12_first=%d\n", native_profile, nv12_first ? 1 : 0);
    bool typed = false;
    for (const GUID& want : formats) {
        ComPtr<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, want);
        if (SUCCEEDED(p.reader->SetCurrentMediaType(vs, nullptr, t.Get()))) {
            p.in_fmt = (want == MFVideoFormat_P010) ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
            typed = true;
            break;
        }
    }
    if (!typed) {
        p.last_error = L"no NV12/P010 decoder output";
        ReleaseMediaPipeline(p);
        return false;
    }
    p.TraceMemory("output_type_selected");
    auto read_type_info = [&]() {
        ComPtr<IMFMediaType> cur;
        if (FAILED(p.reader->GetCurrentMediaType(vs, &cur)) || !cur) return false;
        UINT32 cw = 0, ch = 0;
        MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &cw, &ch);
        if (cw == 0 || ch == 0) return false;
        p.coded_w = cw;
        p.coded_h = ch;
        p.video_w = cw;
        p.video_h = ch;
        p.src_rect = {0, 0, static_cast<LONG>(cw), static_cast<LONG>(ch)};
        MFVideoArea area{};
        if (SUCCEEDED(cur->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr)) &&
            area.Area.cx > 0 && area.Area.cy > 0) {
            p.src_rect.left = static_cast<LONG>(area.OffsetX.value);
            p.src_rect.top = static_cast<LONG>(area.OffsetY.value);
            p.src_rect.right = p.src_rect.left + area.Area.cx;
            p.src_rect.bottom = p.src_rect.top + area.Area.cy;
            p.video_w = static_cast<uint32_t>(area.Area.cx);
            p.video_h = static_cast<uint32_t>(area.Area.cy);
        }
        UINT32 num = 30, den = 1;
        if (SUCCEEDED(MFGetAttributeRatio(cur.Get(), MF_MT_FRAME_RATE, &num, &den)) && num > 0 && den > 0) {
            const double fps = static_cast<double>(num) / static_cast<double>(den);
            if (fps >= 1.0 && fps <= 240.0) {
                p.frame_ms = 1000.0 / fps;
                p.frame_100ns = static_cast<LONGLONG>(1e7 / fps + 0.5);
            }
        }
        p.color = {};
        UINT32 v = 0;
        if (SUCCEEDED(cur->GetUINT32(MF_MT_YUV_MATRIX, &v))) p.color.yuv_matrix = v;
        if (SUCCEEDED(cur->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &v))) p.color.nominal_range = v;
        if (SUCCEEDED(cur->GetUINT32(MF_MT_VIDEO_PRIMARIES, &v))) p.color.primaries = v;
        if (SUCCEEDED(cur->GetUINT32(MF_MT_TRANSFER_FUNCTION, &v))) p.color.transfer_func = v;

        // Also check native type if decoder stripped color matrix metadata.
        if (p.color.yuv_matrix == 0 || p.color.nominal_range == 0) {
            ComPtr<IMFMediaType> nat;
            if (SUCCEEDED(p.reader->GetNativeMediaType(vs, 0, &nat)) && nat) {
                if (p.color.yuv_matrix == 0 && SUCCEEDED(nat->GetUINT32(MF_MT_YUV_MATRIX, &v))) p.color.yuv_matrix = v;
                if (p.color.nominal_range == 0 && SUCCEEDED(nat->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &v))) p.color.nominal_range = v;
            }
        }

        ComputeAspectFitEvenSize(p.video_w, p.video_h, p.target_w, p.target_h, p.surface_w, p.surface_h);
        return true;
    };

    if (!read_type_info()) {
        p.last_error = L"GetCurrentMediaType failed";
        ReleaseMediaPipeline(p);
        return false;
    }

    if (start_pos_100ns > 0) {
        PROPVARIANT pv{};
        PropVariantInit(&pv);
        pv.vt = VT_I8;
        pv.hVal.QuadPart = start_pos_100ns;
        p.reader->SetCurrentPosition(GUID_NULL, pv);
    }

    // Decode first frame before notifying UI so the swap chain is never bound blank.
    ComPtr<IMFSample> first;
    LONGLONG first_ts = 0;
    p.profile_preroll_frames = 0;
    unsigned empty_samples = 0;
    const auto cancelled = [&] {
        return !running_.load(std::memory_order_acquire) ||
               generation_.load(std::memory_order_acquire) != p.active_gen;
    };
    while (empty_samples < 64 && !first) {
        if (cancelled()) {
            p.last_error.clear();
            ReleaseMediaPipeline(p);
            return false;
        }
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        hr = p.reader->ReadSample(vs, 0, &idx, &flags, &ts, &s);
        if (cancelled()) {
            p.last_error.clear();
            ReleaseMediaPipeline(p);
            return false;
        }
        if (FAILED(hr)) {
            p.last_error = L"ReadSample first frame failed";
            ReleaseMediaPipeline(p);
            return false;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            read_type_info();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (start_pos_100ns > 0) {
                start_pos_100ns = 0;
                PROPVARIANT pv{};
                PropVariantInit(&pv);
                pv.vt = VT_I8;
                pv.hVal.QuadPart = 0;
                p.reader->SetCurrentPosition(GUID_NULL, pv);
                continue;
            }
            break;
        }
        if (s) {
            // Source Reader seeks to a preceding keyframe. When reopening in paused state
            // (e.g. deterministic seek verification), decode full preroll to retain exact PTS.
            // When resuming live wallpaper playback from FrozenReleased (!p.paused), cap
            // preroll to <= 6 frames / 80ms so long-GOP 4K60 clips resume within ~100ms
            // instead of hanging frozen for 1-2+ seconds decoding 200+ 4K frames.
            if (start_pos_100ns > 0 && ts < start_pos_100ns) {
                const bool fast_live_resume = is_reopen && !p.paused;
                const bool within_preroll_budget = !fast_live_resume ||
                    (p.profile_preroll_frames < 6u && (QpcNowMs() - open_t0) < 80.0);
                if (within_preroll_budget) {
                    ++p.profile_preroll_frames;
                    continue;
                }
            }
            first = std::move(s);
            first_ts = ts;
        } else {
            ++empty_samples;
        }
    }
    if (!first) {
        p.last_error = L"no first video frame";
        ReleaseMediaPipeline(p);
        return false;
    }

    if (cancelled()) {
        p.last_error.clear();
        ReleaseMediaPipeline(p);
        return false;
    }
    if (p.profile) p.profile_pts = first_ts;
    p.TraceMemory("first_sample_decoded");
    if (!PresentSample(p, first.Get(), /*force_glass=*/true)) {
        if (p.last_error.empty()) p.last_error = L"present first frame failed";
        ReleaseMediaPipeline(p);
        return false;
    }
    p.TraceMemory("first_sample_presented");
    if (cancelled()) {
        p.last_error.clear();
        ReleaseMediaPipeline(p);
        return false;
    }

    const double elapsed = QpcNowMs() - open_t0;
    if (is_reopen) p.reopen_first_frame_ms = elapsed;
    else p.first_frame_ms = elapsed;

    p.is_open = true;
    p.frozen_released = false;
    p.last_raw_pts = first_ts;
    p.base_pts = first_ts;
    p.base_ms = QpcNowMs();
    // Anchor the pacing clock on the first live playback frame after UI binds the swap chain
    // so swap-chain binding latency never triggers an initial dropped-frame burst.
    p.have_clock_base = false;

    if (callbacks_.on_swap_chain_ready) {
        callbacks_.on_swap_chain_ready(p.active_gen, p.swap, p.surface_w, p.surface_h, p.video_w, p.video_h);
    }
    return true;
}

void VideoWallpaperPlayer::ThreadMain(LUID adapter_luid,
                                      ComPtr<ID3D11Device> fallback_dev,
                                      bool force_shared) {
    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT mf_hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    Pipeline p;
    wchar_t profile_enabled[2]{};
    p.profile = GetEnvironmentVariableW(L"PULSE_VIDEO_PROFILE", profile_enabled, 2) == 1 && profile_enabled[0] == L'1';
    p.memory_profile = GetEnvironmentVariableW(L"PULSE_VIDEO_MEMORY_PROFILE", profile_enabled, 2) == 1 && profile_enabled[0] == L'1';
    if (p.profile) p.profile_events.reserve(512);
    const bool dev_ok = InitDevice(p, adapter_luid, fallback_dev.Get(), force_shared);
    p.TraceMemory("device_initialized");
    if (!dev_ok) {
        p.last_error = L"Failed to initialize D3D11 video device";
    }
    UpdateStatsLocked(p);

    const DWORD vs = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

    while (running_.load(std::memory_order_acquire)) {
        // 1. Drain queued commands.
        std::vector<Command> cmds;
        {
            std::lock_guard<std::mutex> lk(cmd_mtx_);
            cmds.swap(cmd_queue_);
        }
        bool should_exit = false;
        for (auto& cmd : cmds) {
            switch (cmd.kind) {
            case Command::Kind::Stop:
                should_exit = true;
                break;

            case Command::Kind::Close:
                p.active_gen = cmd.generation;
                p.is_open = false;
                p.paused = false;
                p.frozen_released = false;
                p.current_path.clear();
                ReleaseMediaPipeline(p);
                UpdateStatsLocked(p);
                break;

            case Command::Kind::Open:
                p.active_gen = cmd.generation;
                p.paused = cmd.start_paused;
                p.frozen_released = false;
                if (!dev_ok) {
                    if (callbacks_.on_playback_error) callbacks_.on_playback_error(p.active_gen, p.last_error);
                } else if (!OpenPipeline(p, cmd.path, cmd.target_w, cmd.target_h, cmd.start_pos_100ns, false)) {
                    p.is_open = false;
                    if (!p.last_error.empty() && callbacks_.on_playback_error)
                        callbacks_.on_playback_error(p.active_gen, p.last_error);
                }
                UpdateStatsLocked(p);
                break;

            case Command::Kind::Pause:
                if (cmd.generation == p.active_gen && p.is_open && !p.paused) {
                    p.paused = true;
                    p.saved_pause_pts = p.last_raw_pts;
                    p.have_clock_base = false;
                    // Publish the paused frame to Liquid Glass even in Static mode (§3.4).
                    if (p.last_shown_tex) {
                        PublishGlassFrame(p, p.last_shown_tex.Get(), p.last_shown_slice, true);
                    }
                    UpdateStatsLocked(p);
                }
                break;

            case Command::Kind::Resume:
                if (cmd.generation == p.active_gen && p.is_open) {
                    if (p.frozen_released) {
                        p.paused = false;
                        if (!OpenPipeline(p, p.current_path, p.target_w, p.target_h, p.saved_pause_pts, true)) {
                            p.is_open = false;
                            if (!p.last_error.empty() && callbacks_.on_playback_error)
                                callbacks_.on_playback_error(p.active_gen, p.last_error);
                        }
                    } else if (p.paused) {
                        p.paused = false;
                        p.have_clock_base = false;
                    }
                    UpdateStatsLocked(p);
                }
                break;

            case Command::Kind::FreezeRelease:
                if (cmd.generation == p.active_gen && p.is_open && !p.frozen_released) {
                    p.paused = true;
                    p.saved_pause_pts = p.last_raw_pts;
                    FrozenFrameSnapshot snap{};
                    const bool got_snap = CaptureFrozenSnapshot(p, snap);
                    if (got_snap && callbacks_.on_frozen_frame_ready) {
                        callbacks_.on_frozen_frame_ready(p.active_gen, std::move(snap));
                    }
                    ReleaseMediaPipeline(p);
                    p.frozen_released = true;
                    UpdateStatsLocked(p);
                }
                break;

            case Command::Kind::ResumeFromFrozen:
                if (cmd.generation == p.active_gen && (p.frozen_released || !p.is_open) && !p.current_path.empty()) {
                    p.paused = false;
                    if (!OpenPipeline(p, p.current_path, p.target_w, p.target_h, p.saved_pause_pts, true)) {
                        p.is_open = false;
                        if (!p.last_error.empty() && callbacks_.on_playback_error)
                            callbacks_.on_playback_error(p.active_gen, p.last_error);
                    }
                    UpdateStatsLocked(p);
                }
                break;

            case Command::Kind::RefreshGlass:
                if (cmd.generation == p.active_gen && p.is_open && !p.frozen_released && p.last_shown_tex) {
                    PublishGlassFrame(p, p.last_shown_tex.Get(), p.last_shown_slice, true);
                    UpdateStatsLocked(p);
                }
                break;

            case Command::Kind::TrimMemory:
                if (p.ctx) {
                    p.ctx->ClearState();
                    p.ctx->Flush();
                }
                if (p.dxgi_dev) {
                    ComPtr<IDXGIDevice3> dxgi3;
                    if (SUCCEEDED(p.dxgi_dev.As(&dxgi3)) && dxgi3) {
                        dxgi3->Trim();
                    }
                }
                break;
            }
        }
        if (should_exit) break;

        // 2. If idle/paused/frozen, block with zero CPU usage until next command.
        if (!p.is_open || p.paused || p.frozen_released || !p.reader) {
            WaitForSingleObject(cmd_event_, INFINITE);
            continue;
        }

        // 3. Read next sample if none is pending.
        if (!p.pending_sample) {
            DWORD idx = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> smp;
            const double profile_read_start = p.profile ? QpcNowMs() : 0.0;
            const HRESULT hr = p.reader->ReadSample(vs, 0, &idx, &flags, &ts, &smp);
            if (p.profile) {
                p.profile_read_ms = QpcNowMs() - profile_read_start;
                p.profile_pts = ts;
                if (p.profile_read_ms > 33.0) p.RecordProfile("slow_read");
            }
            if (FAILED(hr)) {
                p.last_error = L"ReadSample failed during playback";
                p.is_open = false;
                ReleaseMediaPipeline(p);
                UpdateStatsLocked(p);
                if (callbacks_.on_playback_error) callbacks_.on_playback_error(p.active_gen, p.last_error);
                continue;
            }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                ComPtr<IMFMediaType> cur;
                if (SUCCEEDED(p.reader->GetCurrentMediaType(vs, &cur)) && cur) {
                    UINT32 cw = 0, ch = 0;
                    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &cw, &ch);
                    if (cw > 0 && ch > 0 && (cw != p.coded_w || ch != p.coded_h)) {
                        p.coded_w = cw;
                        p.coded_h = ch;
                        p.in_views.clear();
                        p.glass_in_views.clear();
                        p.vp.Reset();
                        p.venum.Reset();
                    }
                }
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                // Gapless loop: seek back to 0 and continue virtual PTS seamlessly.
                PROPVARIANT pv{};
                PropVariantInit(&pv);
                pv.vt = VT_I8;
                pv.hVal.QuadPart = 0;
                const double profile_seek_start = p.profile ? QpcNowMs() : 0.0;
                const HRESULT seek_hr = p.reader->SetCurrentPosition(GUID_NULL, pv);
                if (p.profile) {
                    p.profile_seek_ms = QpcNowMs() - profile_seek_start;
                    if (p.profile_seek_ms > 33.0) p.RecordProfile("slow_loop_seek");
                }
                if (SUCCEEDED(seek_hr)) {
                    p.loop_pts_offset += p.last_raw_pts + p.frame_100ns;
                    p.last_raw_pts = 0;
                    ++p.loop_count;
                    UpdateStatsLocked(p);
                } else {
                    p.have_clock_base = false;
                }
                continue;
            }
            if (!smp) continue;
            p.pending_sample = std::move(smp);
            p.last_raw_pts = ts;
            p.pending_pts = ts + p.loop_pts_offset;
        }

        // 4. QPC pacing until pending sample is due.
        const double now_ms = QpcNowMs();
        if (!p.have_clock_base) {
            p.base_pts = p.pending_pts;
            p.base_ms = now_ms;
            p.have_clock_base = true;
        }
        const double due_ms = p.base_ms + static_cast<double>(p.pending_pts - p.base_pts) / 10000.0;
        const double wait_ms = due_ms - now_ms;
        if (wait_ms > 0.75) {
            if (wait_timer_) {
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>(std::min(wait_ms, 500.0) * 10000.0);
                SetWaitableTimer(wait_timer_, &due, 0, nullptr, nullptr, FALSE);
                HANDLE hs[2] = {cmd_event_, wait_timer_};
                WaitForMultipleObjects(2, hs, FALSE, static_cast<DWORD>(std::ceil(std::min(wait_ms, 500.0))) + 5u);
            } else {
                WaitForSingleObject(cmd_event_, static_cast<DWORD>(std::min(wait_ms, 500.0)));
            }
            if (p.profile) p.profile_wake_late_ms = QpcNowMs() - due_ms;
            continue;
        }

        // 5. Drop if more than two frames behind; otherwise present.
        if (now_ms - due_ms > 2.0 * p.frame_ms) {
            ++p.frames_dropped;
            p.RecordProfile("drop", now_ms - due_ms);
        } else {
            if (!PresentSample(p, p.pending_sample.Get(), false)) {
                p.last_error = L"PresentSample failed";
                p.is_open = false;
                ReleaseMediaPipeline(p);
                UpdateStatsLocked(p);
                if (callbacks_.on_playback_error) callbacks_.on_playback_error(p.active_gen, p.last_error);
                continue;
            }
        }
        p.pending_sample.Reset();
        if (p.profile) { p.profile_seek_ms = 0.0; p.profile_wake_late_ms = 0.0; }
        UpdateStatsLocked(p);
    }

    ReleaseMediaPipeline(p);
    if (p.profile) {
        for (const auto& event : p.profile_events) std::fprintf(stderr, "%s\n", event.c_str());
        std::fprintf(stderr, "VIDEO_PROFILE_END recorded=%zu omitted=%u\n", p.profile_events.size(), p.profile_omitted);
    }
    p.mf_mgr.Reset();
    p.factory.Reset();
    p.adapter.Reset();
    p.dxgi_dev.Reset();
    p.ctx.Reset();
    p.dev.Reset();

    if (SUCCEEDED(mf_hr)) MFShutdown();
    if (SUCCEEDED(co_hr)) CoUninitialize();
}

} // namespace pulse::desktop::video
