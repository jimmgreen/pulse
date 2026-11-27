// video_wallpaper_player.h - Dedicated-thread Media Foundation D3D11VA hardware decoder +
// ID3D11VideoProcessor + waitable 2-buffer composition swap chain + shared NT-handle
// 1/4-res glass sampling texture + 30s pause-release freeze frame.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "host/video/video_wallpaper_types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulse::desktop::video {

struct VideoPlayerStats {
    bool open = false;
    bool playing = false;
    bool paused = false;
    bool frozen_released = false;
    bool software_decode = false;
    bool using_independent_device = false;
    DXGI_FORMAT input_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT swap_format = DXGI_FORMAT_UNKNOWN;
    uint32_t video_width = 0;
    uint32_t video_height = 0;
    uint32_t surface_width = 0;
    uint32_t surface_height = 0;
    uint32_t glass_width = 0;
    uint32_t glass_height = 0;
    double first_frame_ms = -1.0;
    double reopen_first_frame_ms = -1.0;
    uint64_t frames_shown = 0;
    uint64_t frames_dropped = 0;
    uint64_t glass_frames_published = 0;
    uint64_t loop_count = 0;
    uint32_t video_input_views = 0;
    uint32_t glass_input_views = 0;
    uint32_t retained_decode_textures = 0;
    uint64_t retained_decode_nominal_bytes = 0; // Opt-in diagnostic; not process RAM or driver allocation size.
    int64_t position_100ns = 0;
    std::wstring last_error;
};

struct FrozenFrameSnapshot {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    std::vector<uint8_t> bgra; // opaque BGRA pixels of the last presented frame
};

struct VideoPlayerCallbacks {
    // Called on playback thread (marshaled to UI thread by host) whenever a new swap chain
    // is ready with its first frame already presented.
    std::function<void(uint64_t generation,
                       Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain,
                       uint32_t surface_w,
                       uint32_t surface_h,
                       uint32_t video_w,
                       uint32_t video_h)> on_swap_chain_ready;

    // Called when entering 30s frozen-released state: supplies the last frame so the UI thread
    // can paint a static DComp surface before the swap chain is unbound.
    std::function<void(uint64_t generation, FrozenFrameSnapshot snapshot)> on_frozen_frame_ready;

    // Called when a fresh 1/4-res BGRA frame is written into the shared NT-handle texture.
    std::function<void(uint64_t generation, uint64_t frame_seq, HANDLE shared_nt_handle,
                       uint32_t glass_w, uint32_t glass_h)> on_glass_frame_ready;

    // Called on unrecoverable playback failure (missing file, codec error, device lost).
    std::function<void(uint64_t generation, std::wstring error)> on_playback_error;
};

class VideoWallpaperPlayer {
public:
    VideoWallpaperPlayer() = default;
    ~VideoWallpaperPlayer();
    VideoWallpaperPlayer(const VideoWallpaperPlayer&) = delete;
    VideoWallpaperPlayer& operator=(const VideoWallpaperPlayer&) = delete;

    // Initializes the playback thread and its independent D3D11 device on `adapter_luid`.
    // If `shared_fallback_device` is provided and `force_shared_device` is true, uses that
    // device (with multithread protection) instead of creating an independent device.
    bool Start(LUID adapter_luid,
               VideoPlayerCallbacks callbacks,
               ID3D11Device* shared_fallback_device = nullptr,
               bool force_shared_device = false);

    void Shutdown() noexcept;

    // Opens and starts looping `video_path`, capping swap chain to `target_w x target_h`.
    // Returns a monotonically increasing generation ID.
    uint64_t OpenVideo(const std::wstring& video_path,
                       uint32_t target_w,
                       uint32_t target_h,
                       bool start_paused = false,
                       int64_t start_pos_100ns = 0);

    void CloseVideo();

    // Hot pause/resume (keeps decoder + swap chain warm).
    void SetPaused(bool paused);

    // Captures last frame into FrozenFrameSnapshot, notifies `on_frozen_frame_ready`,
    // then releases SourceReader, VideoProcessor, and SwapChain + Trims DXGI memory.
    void FreezeAndRelease();

    // Reopens the previously opened video at the saved position and presents first frame.
    void ResumeFromFrozen();

    // Updates glass sampling rate: 0=Static (only on open/pause/switch), 1=30Hz, 2=60Hz.
    void SetGlassLiveRate(uint8_t rate_mode) noexcept;
    void SetGlassConsumerActive(bool active) noexcept;
    // Keeps live 1/4-res frames publishing (~20Hz) while BeautifyDrawer preview is visible,
    // even when glass_live_rate == 0 (Static refraction).
    void SetPreviewConsumerActive(bool active) noexcept;

    // Force-publishes the current frame into the shared glass texture (e.g. when glass becomes visible).
    void RequestGlassRefresh();

    // Flushes and trims DXGI driver memory on the player's independent D3D11 device
    // (call after renderer releases its ComPtr<IDXGISwapChain1> on FreezeAndRelease).
    void TrimDeviceMemory();

    // Copies the most recently published 1/4-res frame (or current frame) for drawer preview.
    bool CopyLatestPreviewFrame(FrozenFrameSnapshot& out_frame) const;

    VideoPlayerStats Stats() const;
    bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    struct Command {
        enum class Kind : uint8_t {
            Open,
            Close,
            Pause,
            Resume,
            FreezeRelease,
            ResumeFromFrozen,
            RefreshGlass,
            TrimMemory,
            Stop,
        };
        Kind kind = Kind::Stop;
        uint64_t generation = 0;
        std::wstring path;
        uint32_t target_w = 0;
        uint32_t target_h = 0;
        bool start_paused = false;
        int64_t start_pos_100ns = 0;
    };

    void EnqueueCommand(Command cmd);
    void ThreadMain(LUID adapter_luid,
                    Microsoft::WRL::ComPtr<ID3D11Device> fallback_dev,
                    bool force_shared);

    struct Pipeline;
    bool InitDevice(Pipeline& p, LUID adapter_luid,
                    ID3D11Device* fallback_dev, bool force_shared);
    bool OpenPipeline(Pipeline& p, const std::wstring& path,
                      uint32_t target_w, uint32_t target_h,
                      int64_t start_pos_100ns, bool is_reopen);
    void ReleaseMediaPipeline(Pipeline& p) noexcept;
    bool EnsureVideoProcessorAndSwapChain(Pipeline& p);
    bool EnsureGlassSharedTexture(Pipeline& p);
    bool PublishGlassFrame(Pipeline& p, ID3D11Texture2D* decoded_tex, UINT slice, bool force);
    bool CaptureFrozenSnapshot(Pipeline& p, FrozenFrameSnapshot& out);
    bool PresentSample(Pipeline& p, struct IMFSample* sample, bool force_glass);
    void UpdateStatsLocked(const Pipeline& p);

    VideoPlayerCallbacks callbacks_;
    HANDLE cmd_event_ = nullptr;
    HANDLE wait_timer_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint8_t> glass_live_rate_{1};
    std::atomic<bool> glass_consumer_active_{false};
    std::atomic<bool> preview_consumer_active_{false};

    mutable std::mutex cmd_mtx_;
    std::vector<Command> cmd_queue_;

    mutable std::mutex stats_mtx_;
    VideoPlayerStats stats_{};

    mutable std::mutex preview_mtx_;
    FrozenFrameSnapshot latest_preview_{};
};

} // namespace pulse::desktop::video
