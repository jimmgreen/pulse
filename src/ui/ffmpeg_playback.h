#pragma once
// Quick Look playback through the FFmpeg preview pack, for files Media
// Foundation cannot play (missing decoders, FLV / RMVB / TS containers, APE...).
//
// Preview-level playback: one ffmpeg decodes the video to BGRA frames at a
// fixed rate, already scaled to the viewport; another decodes the audio to
// 48 kHz stereo s16 for waveOut. The audio clock paces the frames (a wall
// clock without audio). Seeking or changing the rate restarts both at the new
// position. The pipes give back-pressure, so only a few frames are buffered.
#include "../common/ffmpeg_tool.h"
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace pulse::ui {

struct FfmpegTrack {
    int id = -1; // index within audio/subtitle streams, for -map / subtitles si
    std::wstring label;
    std::string codec;
    bool supported = true;
};

struct FfmpegMediaInfo {
    std::vector<FfmpegTrack> audio_tracks, subtitle_tracks;
    int64_t duration = 0;            // 100 ns, 0 when unknown
    uint32_t width = 0, height = 0;  // display size (square pixels)
    double fps = 0;
    bool video = false, audio = false;
    bool subtitle_filters = false;
};

// Backend-only, bounded/cancellable capability probe, cached by executable identity.
bool FfmpegSubtitleFilters(const std::wstring& ffmpeg, const std::atomic<bool>* stop = nullptr);
bool ParseFfmpegSubtitleFilters(std::string_view output);

// Runs "ffmpeg -i" once (at most 8 s). False when ffmpeg cannot read the file.
bool ProbeFfmpegMedia(const std::wstring& ffmpeg, const std::wstring& path, FfmpegMediaInfo& info,
                      const std::atomic<bool>* stop = nullptr);
// From ffmpeg's input description; exposed for tests.
bool ParseFfmpegMediaInfo(std::string_view log, FfmpegMediaInfo& info);

struct FfmpegFrame {
    std::vector<uint8_t> pixels;     // BGRA, top-down, stride = width * 4
    uint32_t width = 0, height = 0;
    int64_t time = 0;                // 100 ns
};

// A display size fitted into `box`: never enlarged, even, at least 16 px.
SIZE FfmpegFrameSize(uint32_t width, uint32_t height, SIZE box);
// Output frame rate: the source's, at most 30 fps (25 when unknown).
double FfmpegOutputRate(double source_fps);
// "atempo" chain for 0.25x..4x; empty at 1x.
std::wstring FfmpegTempoFilter(float rate);

class FfmpegPlayback {
public:
    FfmpegPlayback(std::wstring ffmpeg, std::wstring path, const FfmpegMediaInfo& info,
                   std::function<void()> wake = {});
    FfmpegPlayback(const FfmpegPlayback&) = delete;
    FfmpegPlayback& operator=(const FfmpegPlayback&) = delete;
    ~FfmpegPlayback();

    // (Re)starts the decoders at `position` (100 ns). When paused, only the
    // first frame is shown and the audio waits, ready to resume at once.
    bool Start(int64_t position, float rate, SIZE box, bool playing);
    bool SelectTracks(int audio, int subtitle); // applied by the next Start
    void SetPlaying(bool playing);
    void SetVolume(float volume, bool muted);
    // The frame due now, or nullptr. Call every few milliseconds.
    std::shared_ptr<const FfmpegFrame> Pump();
    // Paused: queues the next decoded frame for Pump. False when none is ready.
    bool Step();
    int64_t Position();
    bool Ended();
    bool Failed() const noexcept { return failed_; }
    std::string Diagnostic();
    DWORD DecoderExitCode() const noexcept { return decoder_exit_.load(); }
    MMRESULT AudioDeviceError() const noexcept { return audio_device_error_; }
    bool playing() const noexcept { return playing_; }
    float rate() const noexcept { return rate_; }
    bool audio_open() const noexcept { return wave_ != nullptr; }
    SIZE frame_size() const noexcept { return frame_size_; }

private:
    struct Pool;
    void Stop();
    void ReadVideo();
    void WriteAudio();
    void CheckDecoderExit(ffmpeg::Process& process);
    void DrainError(ffmpeg::Process& process);
    int64_t Clock();
    bool AudioDrained();

    const std::wstring ffmpeg_, path_;
    const FfmpegMediaInfo info_;
    std::shared_ptr<Pool> pool_;

    int audio_track_ = 0, subtitle_track_ = -1;

    // Session.
    int64_t start_ = 0;
    float rate_ = 1.0f;
    bool playing_ = false;
    std::atomic<bool> failed_{false};
    std::atomic<DWORD> decoder_exit_{0};
    MMRESULT audio_device_error_ = MMSYSERR_NOERROR;
    std::function<void()> wake_;
    bool first_shown_ = false;
    bool stepped_ = false;
    SIZE frame_size_{};
    double out_fps_ = 25.0;
    int64_t shown_time_ = 0;      // last presented frame
    int64_t paused_at_ = 0;       // clock when paused
    int64_t wall_base_ = 0;       // wall clock: media time at wall_tick_
    ULONGLONG wall_tick_ = 0;
    bool wall_clock_ = false;     // no audio, or the audio has run out
    std::shared_ptr<const FfmpegFrame> stepped_frame_;

    // Video reader.
    std::unique_ptr<ffmpeg::Process> video_;
    std::thread video_reader_;
    std::thread video_errors_, audio_errors_;
    std::mutex error_mutex_;
    std::string diagnostic_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::shared_ptr<const FfmpegFrame>> queue_;
    bool video_eof_ = false;
    std::atomic<bool> stop_{false};

    // Audio writer.
    std::unique_ptr<ffmpeg::Process> audio_;
    std::thread audio_writer_;
    HWAVEOUT wave_ = nullptr;
    HANDLE wave_event_ = nullptr;
    static constexpr int kWaveBuffers = 6;
    WAVEHDR headers_[kWaveBuffers]{};
    std::vector<uint8_t> wave_data_[kWaveBuffers];
    std::atomic<bool> audio_eof_{false};
    std::atomic<float> gain_{1.0f};
};

} // namespace pulse::ui
