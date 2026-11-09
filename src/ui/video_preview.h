#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <future>
#include <vector>
#include <string>

namespace pulse::ui {
struct FfmpegFrame;

// MFPlay is loaded on demand (including on Windows N without Media Foundation).
// All media/provider calls run off the UI thread. Each open has its own mailbox,
// so a retired decoder cannot publish state into the next file's preview.
class VideoPreview {
public:
    struct Track {
        int id = -1;
        std::wstring label;
        bool supported = true;
    };
    struct State {
        std::vector<Track> audio_tracks, subtitle_tracks;
        int audio_track = -1, subtitle_track = -1;
        bool can_select_audio = false, can_select_subtitle = false;
        bool ready = false;
        bool opening = false; // Backend selection/probing is still in progress.
        bool playing = false;
        bool ended = false;
        bool can_seek = false;
        bool busy = false;
        int64_t position = 0; // 100 ns units, not an estimated frame number
        int64_t duration = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t stepped_frames = 0; // successful steps since the last seek/play
        HRESULT error = S_OK;
        HRESULT control_error = S_OK;
        float volume = 1.0f;  // requested (applied once the item is ready)
        bool muted = false;
        float rate = 1.0f;    // actual rate after the last SetRate
        // The video track's format has no installed decoder (e.g. HEVC without
        // the Store extension): MFPlay would only play the sound.
        bool missing_decoder = false;
        bool unsupported_audio = false; // Media Foundation rejected the container or codec.
        std::wstring codec;       // short name ("HEVC"), set with missing_decoder
        std::wstring codec_name;  // display name ("HEVC (H.265)")
        const wchar_t* store_id = nullptr;  // Microsoft Store extension, if any
        // Playing through the FFmpeg preview pack (Media Foundation could not).
        bool ffmpeg = false;
        bool ffmpeg_available = false;
        bool ffmpeg_subtitles_available = false;
    };
    VideoPreview() = default;
    ~VideoPreview();
    VideoPreview(const VideoPreview&) = delete;
    VideoPreview& operator=(const VideoPreview&) = delete;
    // Candidate video/audio extensions; actual support depends on installed
    // Media Foundation sources and decoders, plus the formats the FFmpeg
    // preview pack adds while it is installed and enabled. Audio-only files
    // never show the video child window (the Quick Look draws cover art and a
    // waveform).
    static bool Supports(const std::wstring& path);
    static bool IsAudio(const std::wstring& path);
    // card: the hover preview card's silent loop. Starts muted, lets the
    // pointer through to the owner, and decodes FFmpeg frames at card size.
    void Open(HWND owner, const std::wstring& path, bool card = false);
    void Reset();
    std::vector<std::shared_future<void>> SuspendVolume(const std::wstring& root);
    // corner_radius > 0 clips the video window to a rounded rectangle.
    void Layout(const RECT& bounds, bool visible, int corner_radius = 0);
    void Play(bool playing);
    void Seek(double fraction);
    void Step();
    void SetVolume(float volume);  // 0..1
    void SetMuted(bool muted);
    void SetRate(float rate);
    void EnableTrackPlayback(); // explicitly reload through the installed FFmpeg pack
    void SelectAudioTrack(int id);
    void SelectSubtitleTrack(int id);
    State Snapshot() const;
    // FFmpeg playback (State::ffmpeg): the picture for the owner to draw, already
    // scaled for it; serial changes with every new frame.
    std::shared_ptr<const FfmpegFrame> Frame(uint64_t& serial) const;
    bool active() const noexcept { return state_ != nullptr; }
private:
    struct Shared;
    static void Run(std::shared_ptr<Shared> state, std::wstring path);
    // Playback through the pack's ffmpeg; false when it cannot read the file.
    static bool RunFfmpeg(const std::shared_ptr<Shared>& state, const std::wstring& path,
                          const std::wstring& ffmpeg);
    static LRESULT CALLBACK VideoProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    std::shared_ptr<Shared> state_;
    std::vector<std::weak_ptr<Shared>> retired_;
    HWND child_ = nullptr;
    RECT layout_bounds_{};
    bool layout_valid_ = false;
    bool layout_visible_ = false;
    SIZE region_size_{};
    int region_radius_ = 0;
};

} // namespace pulse::ui
