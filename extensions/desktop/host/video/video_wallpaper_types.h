// video_wallpaper_types.h - Shared data structures for Pulse Desktop video wallpaper
// policy, playback, transcoding, cache persistence, and Beautify drawer UI state.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::desktop::video {

enum class VideoCodecKind : uint8_t {
    Unknown = 0,
    H264 = 1,
    HEVC = 2,
    VP9 = 3,
    AV1 = 4,
    Other = 5,
};

enum class VideoFillMode : uint8_t {
    Fill = 0, // 铺满（保持比例裁切）
    Fit = 1,  // 适应（保持比例留黑边）
};

enum class VideoWallpaperPhase : uint8_t {
    Inactive = 0,
    PlayingOriginal = 1,
    PlayingOptimized = 2,
    Optimizing = 3,
    ConfirmHeavyTranscode = 4,
    PausedAuto = 5,
    PausedManual = 6,
    MissingCodecHevc = 7,
    MissingCodecAv1 = 8,
    UnsupportedCodec = 9,
    FileMissing = 10,
    TooLargeToTranscode = 11,
    UnsupportedHdr = 12,
    RequiresIconTakeover = 13,
    Error = 14,
};

enum class VideoPauseReason : uint8_t {
    None = 0,
    Manual = 1,
    Fullscreen = 2,
    BatterySaver = 3,
    SessionLocked = 4,
    DisplayOff = 5,
    DesktopOccluded = 6,
};

struct VideoColorInfo {
    uint32_t yuv_matrix = 0;    // MFVideoTransferMatrix (0=Unknown, 1=BT709, 2=BT601, 3=SMPTE240M, 4=BT2020_10)
    uint32_t nominal_range = 0; // MFNominalRange (0=Unknown, 1=0_255, 2=16_235)
    uint32_t primaries = 0;     // MFVideoPrimaries
    uint32_t transfer_func = 0; // MFVideoTransferFunction (15=2084/PQ, 16=HLG)
    bool is_hdr = false;

    bool operator==(const VideoColorInfo&) const noexcept = default;
};

struct VideoProbeInfo {
    bool file_exists = false;
    bool open_ok = false;
    std::wstring source_path;
    uint64_t file_size_bytes = 0;
    uint64_t file_mtime_100ns = 0;

    VideoCodecKind codec = VideoCodecKind::Unknown;
    std::wstring codec_name;
    uint32_t coded_width = 0;
    uint32_t coded_height = 0;
    uint32_t display_width = 0;
    uint32_t display_height = 0;
    int32_t crop_left = 0;
    int32_t crop_top = 0;
    int32_t crop_right = 0;
    int32_t crop_bottom = 0;
    uint32_t fps_num = 30;
    uint32_t fps_den = 1;
    uint64_t duration_100ns = 0;
    uint8_t bit_depth = 8;
    VideoColorInfo color{};

    bool has_mf_decoder = false;
    bool hw_decode_supported = false;
    std::wstring store_uri;
    std::wstring error_detail;

    double Fps() const noexcept {
        if (fps_num == 0 || fps_den == 0) return 30.0;
        const double f = static_cast<double>(fps_num) / static_cast<double>(fps_den);
        return (f >= 1.0 && f <= 240.0) ? f : 30.0;
    }

    uint32_t DurationMs() const noexcept {
        return static_cast<uint32_t>(std::min<uint64_t>(duration_100ns / 10000ull, 0xFFFFFFFFull));
    }
};

struct VideoWallpaperRecentItem {
    std::wstring path;
    std::wstring poster_path;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t duration_ms = 0;

    bool operator==(const VideoWallpaperRecentItem&) const noexcept = default;
};

struct VideoWallpaperPersistedState {
    bool active = false;
    std::wstring source_path;
    VideoFillMode fill_mode = VideoFillMode::Fill;
    std::wstring cache_key;
    std::wstring cached_video_path;
    std::wstring poster_path;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    uint32_t target_width = 0;
    uint32_t target_height = 0;
    uint32_t duration_ms = 0;
    bool optimized = false;
    std::vector<VideoWallpaperRecentItem> recent;

    bool operator==(const VideoWallpaperPersistedState&) const noexcept = default;
};

struct VideoWallpaperUiState {
    VideoWallpaperPhase phase = VideoWallpaperPhase::Inactive;
    VideoPauseReason pause_reason = VideoPauseReason::None;
    VideoFillMode fill_mode = VideoFillMode::Fill;
    bool user_paused = false;
    bool icon_takeover_active = true;
    bool playing_original_while_transcoding = false;

    std::wstring active_source_path;
    std::wstring active_display_name;
    std::wstring active_poster_path;
    uint32_t source_w = 0;
    uint32_t source_h = 0;
    uint32_t target_w = 0;
    uint32_t target_h = 0;
    uint32_t duration_ms = 0;
    float optimize_progress = 0.0f; // 0..1

    // When trying a new video fails (e.g. HDR, missing HEVC, too large), keep active wallpaper
    // intact while highlighting the tried video & showing the error card.
    std::wstring tried_source_path;
    std::wstring tried_display_name;
    std::wstring tried_poster_path;
    uint32_t tried_duration_ms = 0;

    std::wstring store_uri;
    std::wstring status_text;
    std::wstring sub_text;
    std::vector<VideoWallpaperRecentItem> recent;
};

inline bool IsSupportedVideoWallpaperExtension(std::wstring_view ext) noexcept {
    if (ext.empty() || ext.size() > 8) return false;
    wchar_t lower[10]{};
    size_t n = 0;
    if (ext.front() != L'.') {
        lower[n++] = L'.';
    }
    for (wchar_t ch : ext) {
        if (n + 1 >= std::size(lower)) return false;
        lower[n++] = static_cast<wchar_t>(std::towlower(ch));
    }
    const std::wstring_view norm(lower, n);
    return norm == L".mp4" || norm == L".mov" || norm == L".mkv" ||
           norm == L".m4v" || norm == L".webm" || norm == L".wmv";
}

inline bool IsSupportedVideoWallpaperPath(std::wstring_view path) noexcept {
    const size_t dot = path.find_last_of(L'.');
    const size_t sep = path.find_last_of(L"\\/");
    if (dot == std::wstring_view::npos) return false;
    if (sep != std::wstring_view::npos && dot < sep) return false;
    return IsSupportedVideoWallpaperExtension(path.substr(dot));
}

inline std::wstring VideoDisplayName(std::wstring_view path) {
    if (path.empty()) return {};
    const size_t sep = path.find_last_of(L"\\/");
    return std::wstring(sep == std::wstring_view::npos ? path : path.substr(sep + 1));
}

inline std::wstring VideoStemName(std::wstring_view path) {
    std::wstring name = VideoDisplayName(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        name.resize(dot);
    }
    return name;
}

inline std::wstring FormatDurationMmSs(uint32_t duration_ms) {
    const uint32_t total_sec = (duration_ms + 500u) / 1000u;
    const uint32_t hours = total_sec / 3600u;
    const uint32_t mins = (total_sec % 3600u) / 60u;
    const uint32_t secs = total_sec % 60u;
    wchar_t buf[32]{};
    if (hours > 0) {
        std::swprintf(buf, std::size(buf), L"%u:%02u:%02u", hours, mins, secs);
    } else {
        std::swprintf(buf, std::size(buf), L"%u:%02u", mins, secs);
    }
    return buf;
}

inline void ComputeAspectFitEvenSize(uint32_t src_w, uint32_t src_h,
                                     uint32_t max_w, uint32_t max_h,
                                     uint32_t& out_w, uint32_t& out_h) noexcept {
    if (src_w == 0 || src_h == 0 || max_w == 0 || max_h == 0) {
        out_w = 2;
        out_h = 2;
        return;
    }
    const double scale = std::min({1.0,
                                   static_cast<double>(max_w) / static_cast<double>(src_w),
                                   static_cast<double>(max_h) / static_cast<double>(src_h)});
    const uint32_t w = static_cast<uint32_t>(static_cast<double>(src_w) * scale + 0.5) & ~1u;
    const uint32_t h = static_cast<uint32_t>(static_cast<double>(src_h) * scale + 0.5) & ~1u;
    out_w = std::max<uint32_t>(2u, w);
    out_h = std::max<uint32_t>(2u, h);
}

inline std::wstring NormalizePathForCompare(std::wstring_view raw) {
    std::wstring out;
    out.reserve(raw.size());
    for (wchar_t ch : raw) {
        if (ch == L'/') ch = L'\\';
        out.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }
    while (out.size() > 3 && out.back() == L'\\') {
        out.pop_back();
    }
    return out;
}

} // namespace pulse::desktop::video
