// video_wallpaper_controller.h - Host-side coordinator connecting VideoProber,
// WallpaperPolicy, WallpaperCacheManager, VideoWallpaperPlayer, TranscodeManager,
// VideoPauseStateMachine, DesktopRenderer, and BeautifyDrawer UI state.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "host/render/desktop_renderer.h"
#include "host/video/transcode_manager.h"
#include "host/video/video_glass_retry.h"
#include "host/video/video_prober.h"
#include "host/video/video_ui_mailbox.h"
#include "host/video/video_wallpaper_player.h"
#include "host/video/video_wallpaper_types.h"
#include "host/video/wallpaper_policy.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulse::desktop::video {

struct VideoWallpaperControllerCallbacks {
    std::function<void(const VideoWallpaperUiState& ui_state)> on_ui_state_changed;
    std::function<void(const VideoWallpaperPersistedState& persisted)> on_persisted_changed;
    std::function<void(uint32_t accent_argb)> on_accent_extracted;
    std::function<void(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride)> on_drawer_preview_frame;
    std::function<void(bool force)> on_live_glass_updated;
    std::function<void()> on_video_active_changed;
};

class VideoWallpaperController {
    friend struct VideoWallpaperControllerTestPeer;
public:
    VideoWallpaperController() = default;
    ~VideoWallpaperController();
    VideoWallpaperController(const VideoWallpaperController&) = delete;
    VideoWallpaperController& operator=(const VideoWallpaperController&) = delete;

    bool Initialize(HWND notify_hwnd,
                    UINT notify_msg,
                    render::DesktopRenderer* renderer,
                    VideoWallpaperControllerCallbacks callbacks);
    void Shutdown() noexcept;

    void RestorePersisted(const VideoWallpaperPersistedState& persisted,
                          bool icon_takeover_active,
                          uint32_t target_w,
                          uint32_t target_h,
                          uint8_t glass_live_rate);

    void SetVideoWallpaper(const std::wstring& source_path,
                           bool icon_takeover_active,
                           uint32_t target_w,
                           uint32_t target_h,
                           uint8_t glass_live_rate);

    void StopVideoWallpaper(bool persist_inactive = true);
    void ToggleManualPause();
    void SetFillMode(VideoFillMode fill);
    void SetGlassLiveRate(uint8_t rate_mode);
    void SetGlassConsumers(bool desktop_glass_active, bool overlay_active) noexcept;
    void SetIconTakeoverActive(bool active);
    void ConfirmHeavyOptimize();
    void CancelOptimize();

    void OnDisplayOffChanged(bool display_off);
    void OnSessionLockChanged(bool locked);
    void OnMonitorGeometryChanged(uint32_t target_w, uint32_t target_h);

    void DrainUiEvents();

    void TickSecond(bool auto_sleep_enabled,
                    HWND desktop_hwnd,
                    HWND overlay_hwnd,
                    bool drawer_open_on_wall_tab);

    bool IsVideoActive() const noexcept { return persisted_.active; }
    bool IsOptimizing() const noexcept { return transcoder_.IsActive(); }
    const VideoWallpaperPersistedState& PersistedState() const noexcept { return persisted_; }
    VideoWallpaperUiState BuildUiState() const;
    std::string StatusSummaryUtf8() const;
    std::string DisplayNameUtf8() const;

    static std::wstring PickVideoFileDialog(HWND owner);
    static bool IsDesktopOccludedByForeground(HWND desktop_hwnd, HWND overlay_hwnd) noexcept;

private:
    struct PendingProbeOutcome {
        uint64_t generation = 0;
        std::wstring source_path;
        uint32_t target_w = 1920;
        uint32_t target_h = 1080;
        uint8_t glass_rate = 1;
        GpuCapabilities gpu{};
        VideoProbeInfo probe{};
        PolicyDecision policy{};
        std::wstring cache_key;
        std::wstring cached_hit_path;
        std::wstring poster_path;
        uint32_t accent_argb = 0;
        bool poster_ok = false;
    };

    struct PendingTranscodeJob {
        std::wstring source_path;
        std::wstring temp_output_path;
        std::wstring final_output_path;
        std::wstring cache_key;
        uint32_t target_w = 1920;
        uint32_t target_h = 1080;
        uint32_t source_w = 0;
        uint32_t source_h = 0;
        uint32_t duration_ms = 0;
        PolicyDecision policy{};
    };

    struct UiEvent {
        enum class Kind : uint8_t {
            ProbeDone,
            PlayerSwapChainReady,
            PlayerFrozenReady,
            PlayerGlassFrame,
            PlayerError,
            TranscodeProgress,
            TranscodeFinished,
        };
        Kind kind = Kind::ProbeDone;
        uint64_t generation = 0;
        uint64_t sequence = 0;
        PendingProbeOutcome probe;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain;
        FrozenFrameSnapshot frozen;
        uint32_t u32_a = 0;
        uint32_t u32_b = 0;
        HANDLE shared_handle = nullptr;
        float progress = 0.0f;
        TranscodeOutcome transcode_outcome = TranscodeOutcome::Failed;
        std::wstring text;
    };

    void EnsurePlayerStarted();
    void PostUiEvent(UiEvent ev);
    void RetryUiWakeLocked();
    void HandleProbeOutcome(const PendingProbeOutcome& out);
    void StartPlayerOnPath(const std::wstring& play_path, uint32_t cap_w, uint32_t cap_h, bool is_optimized);
    void StartTranscodeJob(const PendingTranscodeJob& job);
    void ApplyPauseStep(const PauseStepResult& tr);
    void NotifyUiChanged();
    void NotifyPersistedChanged();
    void JoinProbeThread() noexcept;

    HWND notify_hwnd_ = nullptr;
    UINT notify_msg_ = 0;
    render::DesktopRenderer* renderer_ = nullptr;
    VideoWallpaperControllerCallbacks callbacks_;

    WallpaperCacheManager cache_;
    VideoWallpaperPlayer player_;
    bool player_started_ = false;
    TranscodeManager transcoder_;
    VideoPauseStateMachine pause_sm_;

    VideoWallpaperPersistedState persisted_{};
    bool icon_takeover_active_ = true;
    uint32_t target_w_ = 1920;
    uint32_t target_h_ = 1080;
    uint8_t glass_live_rate_ = 1;

    VideoWallpaperPhase phase_ = VideoWallpaperPhase::Inactive;
    VideoPauseReason active_pause_reason_ = VideoPauseReason::None;
    bool user_paused_ = false;
    bool display_off_ = false;
    bool session_locked_ = false;
    bool overlay_glass_active_ = false;
    bool playing_original_while_transcoding_ = false;
    float optimize_progress_ = 0.0f;
    std::wstring status_text_;
    std::wstring sub_text_;
    std::wstring store_uri_;
    std::wstring tried_source_path_;
    std::wstring tried_display_name_;
    uint32_t tried_duration_ms_ = 0;
    uint32_t active_source_w_ = 0;
    uint32_t active_source_h_ = 0;
    uint32_t active_play_w_ = 0;
    uint32_t active_play_h_ = 0;
    bool has_pending_heavy_ = false;
    PendingTranscodeJob pending_heavy_job_{};
    PendingTranscodeJob active_transcode_job_{};
    uint64_t active_player_gen_ = 0;
    uint64_t next_job_id_ = 1;

    std::atomic<uint64_t> generation_{0};
    std::thread probe_thread_;

    std::mutex ev_mtx_;
    VideoUiMailbox<UiEvent> ev_mailbox_;
    struct PendingGlassFrame {
        uint64_t generation = 0;
        HANDLE shared_handle = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    VideoGlassRetry<PendingGlassFrame> glass_retry_;
    bool glass_frames_suspended_ = true;
    bool last_auto_sleep_enabled_ = false;
    HWND last_desktop_hwnd_ = nullptr;
    HWND last_overlay_hwnd_ = nullptr;
    uint32_t error_recovery_attempts_ = 0;
    uint64_t next_error_recovery_ms_ = 0;
};

} // namespace pulse::desktop::video
