// transcode_manager.h - Supervises `pulse_wallpaper_transcoder.exe` in a memory-capped,
// KILL_ON_JOB_CLOSE Job Object, receives 1 Hz progress via Named Pipe, supports pause/resume/cancel,
// and atomically publishes completed transcoded videos into WallpaperCacheManager.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "host/video/wallpaper_policy.h"
#include "ipc/desktop_ipc.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace pulse::desktop::video {

enum class TranscodeOutcome : uint8_t {
    Succeeded = 0,
    Cancelled = 1,
    OutOfMemory = 2,
    CodecError = 3,
    Failed = 4,
};

struct TranscodeJobSpec {
    uint64_t job_id = 0;
    std::wstring source_path;
    std::wstring cache_key;
    std::wstring temp_output_path;
    std::wstring final_output_path;
    uint32_t target_width = 1920;
    uint32_t target_height = 1080;
    TranscodeCodecChoice codec = TranscodeCodecChoice::HevcMainHw;
    uint64_t memory_limit_bytes = kMaxTranscodeJobMemoryBytes;
    std::wstring transcoder_exe_override; // optional; empty resolves sibling pulse_wallpaper_transcoder.exe
};

struct TranscodeCallbacks {
    std::function<void(uint64_t job_id, float progress)> on_progress;
    std::function<void(uint64_t job_id, TranscodeOutcome outcome, const std::wstring& final_path)> on_finished;
};

class TranscodeManager {
public:
    TranscodeManager() = default;
    ~TranscodeManager();
    TranscodeManager(const TranscodeManager&) = delete;
    TranscodeManager& operator=(const TranscodeManager&) = delete;

    bool StartJob(const TranscodeJobSpec& spec, TranscodeCallbacks callbacks);
    void SetPaused(bool paused);
    void Cancel();
    void WaitUntilIdle();

    bool IsActive() const noexcept { return active_.load(std::memory_order_acquire); }
    uint64_t ActiveJobId() const noexcept { return active_job_id_.load(std::memory_order_acquire); }
    float Progress() const noexcept { return progress_.load(std::memory_order_acquire); }

    static std::wstring ResolveSiblingTranscoderExe();

private:
    void CleanupJobHandlesLocked() noexcept;

    mutable std::mutex mtx_;
    std::thread watch_thread_;
    ipc::AsyncPipeEndpoint pipe_;
    HANDLE job_handle_ = nullptr;
    HANDLE proc_handle_ = nullptr;
    std::wstring temp_path_;

    std::atomic<bool> active_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<uint64_t> active_job_id_{0};
    std::atomic<float> progress_{0.0f};
    std::atomic<uint64_t> next_req_id_{1};
};

} // namespace pulse::desktop::video
