// wallpaper_policy.h - Pure deterministic policy evaluator, pause/freeze state machine,
// and SHA-256 + LRU cache manager for Pulse Desktop video wallpapers.
#pragma once

#include "host/video/video_wallpaper_types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::desktop::video {

inline constexpr uint32_t kWallpaperCacheSchemeVersion = 1u;
inline constexpr uint64_t kDefaultCacheBudgetBytes = 2ull * 1024ull * 1024ull * 1024ull; // 2 GB
inline constexpr uint64_t kDefaultPauseFreezeDelayMs = 30000ull;                         // 30 s
inline constexpr uint64_t kHeavyTranscodeConfirmThresholdBytes = 1024ull * 1024ull * 1024ull; // 1 GB
inline constexpr uint64_t kMaxTranscodeJobMemoryBytes = 3ull * 1024ull * 1024ull * 1024ull;   // 3 GB

struct GpuCapabilities {
    uint64_t dedicated_vram_bytes = 0;
    uint64_t system_ram_bytes = 0;
    uint64_t available_ram_bytes = 0;
    bool is_integrated_gpu = false;
    bool has_hevc_decoder = false;
    bool has_hevc_hw_encoder = false;
    bool has_h264_hw_encoder = false;
    bool has_h264_sw_encoder = true;
};

enum class TranscodeCodecChoice : uint8_t {
    None = 0,
    HevcMain10Hw = 1,
    HevcMainHw = 2,
    H264HighHw = 3,
    H264HighSw = 4,
};

enum class PolicyDecisionKind : uint8_t {
    PlayOriginal = 0,
    TranscodeThenSwitch = 1,
    RejectMissingFile = 2,
    RejectHdr = 3,
    RejectMissingHevc = 4,
    RejectMissingAv1 = 5,
    RejectUnsupportedCodec = 6,
    RejectTooLarge = 7,
};

struct PolicyDecision {
    PolicyDecisionKind kind = PolicyDecisionKind::RejectUnsupportedCodec;
    bool play_original_while_transcoding = false;
    bool requires_software_decode = false;
    bool require_heavy_confirm = false;
    uint32_t target_width = 0;
    uint32_t target_height = 0;
    TranscodeCodecChoice encoder = TranscodeCodecChoice::None;
    uint64_t estimated_play_vram_bytes = 0;
    uint64_t vram_budget_bytes = 0;
    uint64_t estimated_transcode_ram_bytes = 0;
    uint64_t job_memory_limit_bytes = 0;
    std::wstring store_uri;
    std::wstring user_message;
};

const wchar_t* CodecChoiceArgName(TranscodeCodecChoice choice) noexcept;
uint64_t ComputeTranscodeJobMemoryLimit(uint64_t available_ram_bytes) noexcept;
uint64_t ComputePlayOriginalVramBudget(const GpuCapabilities& gpu) noexcept;
uint64_t EstimatePlaybackVramBytes(const VideoProbeInfo& probe, uint32_t surface_w, uint32_t surface_h) noexcept;
uint64_t EstimateSoftwareDecodePeakRamBytes(const VideoProbeInfo& probe) noexcept;

PolicyDecision EvaluateWallpaperPolicy(const VideoProbeInfo& probe,
                                       uint32_t screen_target_w,
                                       uint32_t screen_target_h,
                                       const GpuCapabilities& gpu);

// ---------------------------------------------------------------------------
// Pause / 30-second Freeze-Release State Machine
// ---------------------------------------------------------------------------
struct PauseSignals {
    bool manual = false;
    bool fullscreen = false;
    bool battery_saver = false;
    bool session_locked = false;
    bool display_off = false;
    bool desktop_occluded = false;

    bool AnyPaused() const noexcept {
        return manual || fullscreen || battery_saver || session_locked || display_off || desktop_occluded;
    }

    VideoPauseReason PrimaryReason() const noexcept {
        if (manual) return VideoPauseReason::Manual;
        if (session_locked) return VideoPauseReason::SessionLocked;
        if (display_off) return VideoPauseReason::DisplayOff;
        if (fullscreen) return VideoPauseReason::Fullscreen;
        if (battery_saver) return VideoPauseReason::BatterySaver;
        if (desktop_occluded) return VideoPauseReason::DesktopOccluded;
        return VideoPauseReason::None;
    }
};

enum class PauseLifecycleState : uint8_t {
    Playing = 0,
    PausedHot = 1,       // Decoder + swap chain kept warm (< 30 s)
    FrozenReleased = 2,  // Decoder + swap chain released, static last-frame surface retained
};

struct PauseStepResult {
    PauseLifecycleState state = PauseLifecycleState::Playing;
    VideoPauseReason reason = VideoPauseReason::None;
    bool entered_pause = false;
    bool should_freeze_release = false;
    bool resumed_from_hot = false;
    bool resumed_from_frozen = false;
};

class VideoPauseStateMachine {
public:
    explicit VideoPauseStateMachine(uint64_t freeze_delay_ms = kDefaultPauseFreezeDelayMs) noexcept
        : freeze_delay_ms_(freeze_delay_ms) {}

    void Reset() noexcept {
        state_ = PauseLifecycleState::Playing;
        reason_ = VideoPauseReason::None;
        paused_since_ms_ = 0;
    }

    PauseStepResult Step(const PauseSignals& signals, uint64_t now_ms) noexcept;

    PauseLifecycleState State() const noexcept { return state_; }
    VideoPauseReason Reason() const noexcept { return reason_; }
    uint64_t PausedSinceMs() const noexcept { return paused_since_ms_; }
    uint64_t FreezeDelayMs() const noexcept { return freeze_delay_ms_; }

private:
    uint64_t freeze_delay_ms_ = kDefaultPauseFreezeDelayMs;
    PauseLifecycleState state_ = PauseLifecycleState::Playing;
    VideoPauseReason reason_ = VideoPauseReason::None;
    uint64_t paused_since_ms_ = 0;
};

// ---------------------------------------------------------------------------
// Wallpaper Cache (SHA-256 Key + 2 GB LRU Eviction + Temp Cleanup)
// ---------------------------------------------------------------------------
struct CacheKeyInput {
    std::wstring source_path;
    uint64_t file_size_bytes = 0;
    uint64_t file_mtime_100ns = 0;
    uint32_t target_width = 0;
    uint32_t target_height = 0;
    std::wstring codec_pref; // e.g. L"hevc_main10", L"hevc_main", L"h264"
    uint32_t scheme_version = kWallpaperCacheSchemeVersion;
};

struct CacheEntry {
    std::wstring key;
    std::wstring video_path;
    std::wstring poster_path;
    std::wstring source_path;
    uint64_t size_bytes = 0;
    uint64_t last_used_ms = 0;
};

std::wstring ComputeSha256HexWide(std::string_view data);
std::wstring ComputeWallpaperCacheKey(const CacheKeyInput& input);
std::wstring DefaultWallpaperCacheDir();

class WallpaperCacheManager {
public:
    explicit WallpaperCacheManager(std::wstring cache_dir = {},
                                   uint64_t budget_bytes = kDefaultCacheBudgetBytes);

    const std::wstring& CacheDir() const noexcept { return cache_dir_; }
    uint64_t BudgetBytes() const noexcept { return budget_bytes_; }
    void SetBudgetBytes(uint64_t budget_bytes) noexcept { budget_bytes_ = budget_bytes; }

    bool EnsureDirectory();
    bool LoadIndex();
    bool SaveIndex() const;

    std::wstring VideoFilePathForKey(std::wstring_view key) const;
    std::wstring TempVideoFilePathForKey(std::wstring_view key) const;
    std::wstring PosterFilePathForKey(std::wstring_view key) const;

    std::optional<CacheEntry> LookupValidVideo(std::wstring_view key) const;
    std::optional<CacheEntry> FindEntry(std::wstring_view key) const;
    bool Touch(std::wstring_view key, uint64_t now_ms);
    bool UpsertEntry(const CacheEntry& entry, std::wstring_view protected_active_key = {});
    size_t EvictToBudget(std::wstring_view protected_active_key = {});
    size_t CleanupStaleTempFiles();
    uint64_t TotalTrackedBytes() const noexcept;
    const std::vector<CacheEntry>& Entries() const noexcept { return entries_; }

private:
    std::wstring IndexPath() const;
    void RefreshEntrySizes();

    std::wstring cache_dir_;
    uint64_t budget_bytes_ = kDefaultCacheBudgetBytes;
    std::vector<CacheEntry> entries_;
};

// Update MRU recent video list (dedup case-insensitively by path, cap at max_items = 3).
void PushRecentVideo(std::vector<VideoWallpaperRecentItem>& recent,
                     const VideoWallpaperRecentItem& item,
                     size_t max_items = 3);

} // namespace pulse::desktop::video
