#pragma once

// FFmpeg media preview pack: video thumbnails, playing time and media details
// for files Windows cannot decode itself (HEVC / AV1 / VP9 without the Store
// extensions, FLV, RMVB, MPEG-TS, ProRes...).
//
// Performance rules (see docs/design/preview_packs_plan.md):
//  - callers try the native path first; the pack is the fallback only;
//  - one short-lived ffmpeg per request, below-normal priority, two decode
//    threads, inside a kill-on-close Job Object with a memory cap;
//  - hard timeouts; a pack that keeps failing to start is skipped for a while.

#include "preview_properties.h"
#include <windows.h>
#include <shellapi.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::preview {

// Video containers the media pack is offered for (lower case, with the dot).
bool IsMediaPackVideoExtension(std::wstring_view extension);

// Audio formats whose details the media pack can fill in.
bool IsMediaPackAudioExtension(std::wstring_view extension);

// True when the pack (or a user-chosen FFmpeg) is installed and enabled.
// Cached; costs one tick-count compare on the hot path.
bool MediaPackAvailable();

struct MediaFrame {
    std::vector<uint8_t> pixels;        // 32bpp BGRA, opaque, top-down
    UINT width = 0, height = 0, stride = 0;
    UINT source_width = 0, source_height = 0;
    uint32_t duration_ms = 0;           // 0 when unknown
};

// One representative frame scaled to fit `cap` x `cap` (never enlarged).
// `known_duration_ms` (0 if unknown) picks the seek position.
bool MediaPackFrame(const std::wstring& path, UINT cap, bool grid, uint32_t known_duration_ms,
                    MediaFrame& frame);

struct AvifSequenceFrame {
    MediaFrame bitmap;                  // Premultiplied BGRA (not the opaque video thumbnail contract).
    uint32_t count = 1;
    uint32_t delay_ms = 0;
};

// An interactive AVIF sequence with the optional Media Pack. The bounded cache
// is request-thread-local. False leaves the ordinary still decode available.
bool MediaPackAvifFrame(const std::wstring& path, UINT cap, uint32_t index, HANDLE client_pipe,
                        AvifSequenceFrame& frame);
void RetireAvifSequence(const std::wstring& path);

// Details-pane rows from ffprobe (duration, resolution, frame rate, codecs).
// Appends at most `max_rows` rows not already present in `rows`.
bool MediaPackProperties(const std::wstring& path, std::vector<PreviewPropertyValue>& rows,
                         size_t max_rows);

// ---- Exposed for tests -------------------------------------------------------

// "Duration: 00:42:18.12," in ffmpeg's log; 0 when absent or N/A.
uint32_t ParseFfmpegDurationMs(std::string_view log);
// First "Video: ... 3840x2160" stream size in ffmpeg's log.
bool ParseFfmpegVideoSize(std::string_view log, UINT& width, UINT& height);
// 24/32bpp BMP (as ffmpeg's bmp encoder writes it) -> top-down BGRA.
bool DecodeBmpToBgra(const std::vector<uint8_t>& bmp, std::vector<uint8_t>& out,
                     UINT& width, UINT& height, UINT& stride);
// Seek position for the thumbnail frame, in milliseconds.
uint32_t ThumbnailSeekMs(uint32_t duration_ms);
// Command-line quoting for CreateProcess (CommandLineToArgvW rules).
std::wstring QuoteArgument(const std::wstring& argument);
// ffprobe "key=value" output -> display rows.
std::vector<PreviewPropertyValue> ParseProbeRows(std::string_view output);

} // namespace pulse::preview
