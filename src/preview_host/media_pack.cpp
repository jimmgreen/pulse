#include "media_pack.h"
#include "../common/ffmpeg_tool.h"
#include "../common/preview_packs.h"
#include "../common/path_utils.h"
#include "../common/runtime_log.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <charconv>
#include <cstring>
#include <cwchar>
#include <optional>
#include <thread>

namespace pulse::preview {
namespace {

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

// ---- Process runner ----------------------------------------------------------

struct ToolRun {
    bool started = false;
    bool timed_out = false;
    bool output_truncated = false;
    DWORD exit_code = STILL_ACTIVE;
    std::vector<uint8_t> out;
    std::string err;
};

// Launch failures (blocked by antivirus, damaged pack) back off instead of
// costing a CreateProcess per thumbnail.
std::atomic<uint32_t> g_launch_failures{0};
std::atomic<ULONGLONG> g_backoff_until{0};
constexpr uint32_t kLaunchFailureLimit = 3;
constexpr ULONGLONG kLaunchBackoffMs = 60000;

void DrainPipe(ffmpeg::Process& process, bool out, size_t limit, std::vector<uint8_t>* bytes,
               std::string* text, bool* truncated = nullptr) {
    uint8_t buffer[64 * 1024];
    size_t total = 0;
    for (;;) {
        const DWORD got = out ? process.ReadOut(buffer, sizeof(buffer)) : process.ReadErr(buffer, sizeof(buffer));
        if (got == 0) break;
        const size_t keep = total >= limit ? 0 : (std::min)(static_cast<size_t>(got), limit - total);
        total += keep;
        if (truncated && keep < got) *truncated = true;
        if (keep == 0) continue;   // keep reading so the child never blocks on a full pipe
        if (bytes) bytes->insert(bytes->end(), buffer, buffer + keep);
        if (text) text->append(reinterpret_cast<const char*>(buffer), keep);
    }
}

bool PreviewClientConnected(HANDLE pipe) {
    return !pipe || PeekNamedPipe(pipe, nullptr, 0, nullptr, nullptr, nullptr);
}

ToolRun RunTool(const std::wstring& exe, const std::wstring& arguments, DWORD timeout_ms,
                size_t max_stdout, HANDLE client_pipe = nullptr,
                const ffmpeg::LaunchOptions& options = {}, bool reserve_stdout = false) {
    ToolRun run;
    const ULONGLONG now = GetTickCount64();
    if (now < g_backoff_until.load() || !PreviewClientConnected(client_pipe)) return run;
    if (reserve_stdout) run.out.reserve(max_stdout);
    ffmpeg::Process process;
    run.started = process.Start(exe, arguments, options);
    if (!run.started) {
        if (g_launch_failures.fetch_add(1) + 1 >= kLaunchFailureLimit) {
            g_backoff_until = GetTickCount64() + kLaunchBackoffMs;
            g_launch_failures = 0;
            diagnostics::runtime::Event("media_pack_backoff", {{"error", GetLastError()}});
        }
        return run;
    }
    g_launch_failures = 0;
    std::thread err_reader([&] { DrainPipe(process, false, 256 * 1024, nullptr, &run.err); });
    std::thread out_reader([&] { DrainPipe(process, true, max_stdout, &run.out, nullptr, &run.output_truncated); });
    if (client_pipe) {
        const ULONGLONG deadline = now + timeout_ms;
        while (!process.Wait(25)) {
            if (!PreviewClientConnected(client_pipe)) { process.Terminate(); break; }
            if (GetTickCount64() >= deadline) {
                run.timed_out = true;
                process.Terminate(ERROR_TIMEOUT);
                break;
            }
        }
    } else if (!process.Wait(timeout_ms)) {
        run.timed_out = true;
        process.Terminate(ERROR_TIMEOUT);
    }
    run.exit_code = process.ExitCode();
    out_reader.join();
    err_reader.join();
    return run;
}

std::wstring FfmpegInput(const std::wstring& path) { return ffmpeg::InputArgument(path); }

// A bounded still fallback is preferable to starting a tool per displayed frame.
constexpr uint32_t kAvifMaxFrames = 120;
constexpr size_t kAvifMaxInputBytes = 32u << 20;
constexpr size_t kAvifMaxRasterBytes = 64u << 20;
constexpr uint64_t kAvifMaxSourcePixels = 16ull << 20;
constexpr uint32_t kAvifPacketLimit = kAvifMaxFrames * 2 + 4;  // includes the primary still and other tracks

struct AvifIdentity {
    uint64_t file_id = 0, size = 0, modified = 0;
    DWORD volume = 0;
    bool operator==(const AvifIdentity&) const = default;
};

bool ReadAvifIdentity(const std::wstring& path, AvifIdentity& id, bool check_brand) {
    HANDLE file = CreateFileW(path.c_str(), check_brand ? GENERIC_READ : FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    struct Close { HANDLE file; ~Close() { CloseHandle(file); } } close{file};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) return false;
    const auto u64 = [](DWORD high, DWORD low) { return (uint64_t(high) << 32) | low; };
    id = {u64(info.nFileIndexHigh, info.nFileIndexLow), u64(info.nFileSizeHigh, info.nFileSizeLow),
        u64(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime), info.dwVolumeSerialNumber};
    if (!check_brand) return true;
    if (id.size > kAvifMaxInputBytes) return false;
    std::array<uint8_t, 256> ftyp{};
    DWORD bytes = 0;
    if (!ReadFile(file, ftyp.data(), static_cast<DWORD>(ftyp.size()), &bytes, nullptr) || bytes < 16 ||
        memcmp(ftyp.data() + 4, "ftyp", 4) != 0) return false;
    const auto be32 = [&](size_t at) {
        return (uint32_t(ftyp[at]) << 24) | (uint32_t(ftyp[at + 1]) << 16) |
            (uint32_t(ftyp[at + 2]) << 8) | ftyp[at + 3];
    };
    uint64_t size = be32(0);
    size_t brands = 8;
    if (size == 1) {
        if (bytes < 24) return false;
        size = u64(be32(8), be32(12));
        brands = 16;
    }
    if (size < brands + 8 || size > bytes || (size - brands) % 4 != 0) return false;
    if (memcmp(ftyp.data() + brands, "avis", 4) == 0) return true;
    for (size_t at = brands + 8; at < size; at += 4)
        if (memcmp(ftyp.data() + at, "avis", 4) == 0) return true;
    return false;
}

std::string_view CompactValue(std::string_view section, std::string_view key) {
    while (!section.empty()) {
        const size_t end = section.find('|');
        const auto field = section.substr(0, end);
        if (field.size() > key.size() && field.starts_with(key) && field[key.size()] == '=')
            return field.substr(key.size() + 1);
        if (end == section.npos) break;
        section.remove_prefix(end + 1);
    }
    return {};
}

template<class T>
bool FieldNumber(std::string_view section, std::string_view key, T& value) {
    const auto field = CompactValue(section, key);
    if (field.empty()) return false;
    const auto result = std::from_chars(field.data(), field.data() + field.size(), value);
    return result.ec == std::errc{} && result.ptr == field.data() + field.size();
}

struct AvifTrack {
    uint32_t index = 0, count = 0, width = 0, height = 0;
    std::vector<uint32_t> delays;
};

std::optional<AvifTrack> ParseAvifTrack(std::string_view output) {
    struct Packet { uint32_t index = 0; double pts = 0, duration = 0; bool valid = false; };
    std::vector<Packet> packets;
    std::optional<AvifTrack> track;
    while (!output.empty()) {
        const size_t end = output.find('\n');
        auto line = output.substr(0, end);
        if (end == output.npos) output = {};
        else output.remove_prefix(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with("packet|")) {
            if (packets.size() >= kAvifPacketLimit) return {};
            Packet packet;
            if (!FieldNumber(line, "stream_index", packet.index)) return {};
            packet.valid = FieldNumber(line, "pts_time", packet.pts) &&
                FieldNumber(line, "duration_time", packet.duration) &&
                std::isfinite(packet.pts) && std::isfinite(packet.duration) &&
                packet.duration > 0 && packet.duration <= 60;
            packets.push_back(packet);
        } else if (line.starts_with("stream|") && CompactValue(line, "codec_type") == "video") {
            if (CompactValue(line, "disposition:attached_pic") == "1") continue;
            AvifTrack candidate;
            if (!FieldNumber(line, "nb_frames", candidate.count)) return {};
            if (candidate.count <= 1) continue;   // AVIF primary item, not the sequence.
            // Multiple timed tracks (e.g. a separate alpha plane) need a
            // container-aware compositor; never animate a guessed colour plane.
            if (track || candidate.count > kAvifMaxFrames || CompactValue(line, "codec_name") != "av1" ||
                (!CompactValue(line, "rotation").empty() && CompactValue(line, "rotation") != "0") ||
                !FieldNumber(line, "index", candidate.index) || !FieldNumber(line, "width", candidate.width) ||
                !FieldNumber(line, "height", candidate.height) || !candidate.width || !candidate.height ||
                uint64_t(candidate.width) * candidate.height > kAvifMaxSourcePixels) return {};
            track = std::move(candidate);
        }
    }
    if (!track) return {};
    std::erase_if(packets, [&](const Packet& p) { return p.index != track->index; });
    if (packets.size() != track->count || std::any_of(packets.begin(), packets.end(), [](const Packet& p) {
        return !p.valid;
    })) return {};
    std::sort(packets.begin(), packets.end(), [](const Packet& a, const Packet& b) { return a.pts < b.pts; });
    for (size_t i = 0; i < packets.size(); ++i) {
        const double seconds = i + 1 < packets.size() ? packets[i + 1].pts - packets[i].pts : packets[i].duration;
        if (seconds <= 0 || seconds > 60) return {};
        track->delays.push_back((std::max)(1u, static_cast<uint32_t>(std::lround(seconds * 1000))));
    }
    return track;
}

struct AvifCache {
    std::wstring path, tool_dir;
    AvifIdentity id;
    AvifTrack track;
    std::vector<uint8_t> pixels;
    ULONGLONG retry_after = 0;
    UINT cap = 0, width = 0, height = 0;
};
thread_local AvifCache avif_cache;

bool DecodeAvifTrack(AvifCache& cache, HANDLE client_pipe) {
    ffmpeg::LaunchOptions options;
    options.memory_limit = 384u << 20;
    const std::wstring input = L" -protocol_whitelist file -f mov -i " + ffmpeg::QuoteArgument(FfmpegInput(cache.path));
    const std::wstring probe_args = L"-v error -read_intervals %+#" + std::to_wstring(kAvifPacketLimit) +
        L" -show_packets -show_streams -show_entries "
        L"packet=stream_index,pts_time,duration_time:stream=index,codec_type,codec_name,width,height,nb_frames:"
        L"stream_disposition=attached_pic:stream_side_data=rotation -of compact" + input;
    const auto started = GetTickCount64();
    ToolRun probe = RunTool(cache.tool_dir + L"\\ffprobe.exe", probe_args, 1800, 64u << 10, client_pipe, options);
    if (probe.exit_code != 0 || probe.output_truncated || probe.out.empty() || !PreviewClientConnected(client_pipe)) {
        diagnostics::runtime::Event("avif_sequence_probe_failed", {{"started", probe.started},
            {"exit", probe.exit_code}, {"truncated", probe.output_truncated}, {"bytes", probe.out.size()}});
        return false;
    }
    auto track = ParseAvifTrack(std::string_view(reinterpret_cast<const char*>(probe.out.data()), probe.out.size()));
    if (!track) {
        diagnostics::runtime::Event("avif_sequence_unsupported", {});
        return false;
    }

    const uint32_t extent = (std::max)(track->width, track->height);
    const auto fit = [&](uint32_t dimension) {
        return extent <= cache.cap ? dimension :
            (std::max)(1u, static_cast<uint32_t>((uint64_t(dimension) * cache.cap + extent / 2) / extent));
    };
    const UINT width = fit(track->width), height = fit(track->height);
    const uint64_t expected = uint64_t(width) * height * 4 * track->count;
    if (expected > kAvifMaxRasterBytes) {
        diagnostics::runtime::Event("avif_sequence_raster_limit", {{"frames", track->count}, {"bytes", expected}});
        return false;
    }
    const std::wstring filter = L"scale=" + std::to_wstring(width) + L":" + std::to_wstring(height) + L":flags=bilinear";
    const std::wstring decode_args = L"-v error -nostdin -threads 2 -noautorotate" + input + L" -map 0:" +
        std::to_wstring(track->index) + L" -an -sn -dn -threads 1 -filter_threads 1 -fps_mode passthrough -frames:v " +
        std::to_wstring(track->count) + L" -vf " + ffmpeg::QuoteArgument(filter) +
        L" -c:v rawvideo -pix_fmt bgra -f rawvideo pipe:1";
    ToolRun decode = RunTool(cache.tool_dir + L"\\ffmpeg.exe", decode_args, 3500,
                             static_cast<size_t>(expected), client_pipe, options, true);
    AvifIdentity after;
    if (decode.exit_code != 0 || decode.output_truncated || decode.out.size() != expected ||
        !PreviewClientConnected(client_pipe) || !ReadAvifIdentity(cache.path, after, false) || after != cache.id) {
        diagnostics::runtime::Event("avif_sequence_decode_failed", {{"started", decode.started},
            {"exit", decode.exit_code}, {"truncated", decode.output_truncated},
            {"bytes", decode.out.size()}, {"expected", expected}});
        return false;
    }
    for (size_t at = 0; at < decode.out.size(); at += 4) {
        const auto alpha = decode.out[at + 3];
        if (alpha != 255) for (size_t i = 0; i < 3; ++i)
            decode.out[at + i] = static_cast<uint8_t>((decode.out[at + i] * alpha + 127) / 255);
    }
    cache.width = width;
    cache.height = height;
    cache.track = std::move(*track);
    cache.pixels = std::move(decode.out);
    diagnostics::runtime::Event("avif_sequence_decoded", {{"frames", cache.track.count},
        {"bytes", expected}, {"ms", GetTickCount64() - started}});
    return true;
}

std::wstring FormatDuration(uint64_t seconds) {
    wchar_t text[48]{};
    if (seconds >= 3600) swprintf_s(text, L"%llu:%02u:%02u", seconds / 3600,
        static_cast<unsigned>(seconds / 60 % 60), static_cast<unsigned>(seconds % 60));
    else swprintf_s(text, L"%u:%02u", static_cast<unsigned>(seconds / 60), static_cast<unsigned>(seconds % 60));
    return text;
}

bool HasRow(const std::vector<PreviewPropertyValue>& rows, const wchar_t* label) {
    return std::any_of(rows.begin(), rows.end(), [&](const PreviewPropertyValue& row) { return row.label == label; });
}

std::wstring CodecDisplay(const std::string& name, const std::string& profile) {
    static const struct { const char* id; const wchar_t* text; } kNames[] = {
        {"h264", L"H.264 (AVC)"}, {"hevc", L"H.265 (HEVC)"}, {"av1", L"AV1"}, {"vp9", L"VP9"},
        {"vp8", L"VP8"}, {"mpeg4", L"MPEG-4 Part 2"}, {"mpeg2video", L"MPEG-2"},
        {"mpeg1video", L"MPEG-1"}, {"prores", L"Apple ProRes"}, {"vc1", L"VC-1"},
        {"wmv3", L"Windows Media Video 9"}, {"rv40", L"RealVideo 4"}, {"rv30", L"RealVideo 3"},
        {"flv1", L"Sorenson Spark"}, {"theora", L"Theora"}, {"mjpeg", L"Motion JPEG"},
        {"dnxhd", L"Avid DNxHD"}, {"aac", L"AAC"}, {"mp3", L"MP3"}, {"ac3", L"Dolby Digital"},
        {"eac3", L"Dolby Digital Plus"}, {"truehd", L"Dolby TrueHD"}, {"dts", L"DTS"},
        {"flac", L"FLAC"}, {"opus", L"Opus"}, {"vorbis", L"Vorbis"}, {"alac", L"ALAC"},
        {"ape", L"Monkey's Audio"}, {"wavpack", L"WavPack"}, {"cook", L"RealAudio"},
        {"wmav2", L"Windows Media Audio"}, {"amr_nb", L"AMR"}, {"pcm_s16le", L"PCM"},
        {"pcm_s24le", L"PCM"}, {"dsd_lsbf", L"DSD"}, {"dsd_msbf", L"DSD"},
    };
    std::wstring text;
    for (const auto& entry : kNames)
        if (name == entry.id) { text = entry.text; break; }
    if (text.empty()) text = Utf8ToWide(name);
    if (!profile.empty() && profile != "unknown" && profile != "LC" && name != "aac")
        text += L" " + Utf8ToWide(profile);
    return text;
}

} // namespace

// ---- Public API ------------------------------------------------------------

bool IsMediaPackVideoExtension(std::wstring_view e) { return ffmpeg::IsVideoExtension(e); }

bool IsMediaPackAudioExtension(std::wstring_view e) { return ffmpeg::IsAudioExtension(e); }

bool MediaPackAvailable() {
    return packs::ResolvePack(packs::PackId::Media).source != packs::ToolSource::None;
}

uint32_t ParseFfmpegDurationMs(std::string_view log) { return ffmpeg::ParseDurationMs(log); }

bool ParseFfmpegVideoSize(std::string_view log, UINT& width, UINT& height) {
    return ffmpeg::ParseVideoSize(log, width, height);
}

bool DecodeBmpToBgra(const std::vector<uint8_t>& bmp, std::vector<uint8_t>& out,
                     UINT& width, UINT& height, UINT& stride) {
    if (bmp.size() < 54 || bmp[0] != 'B' || bmp[1] != 'M') return false;
    auto u32 = [&](size_t at) { uint32_t v; memcpy(&v, bmp.data() + at, 4); return v; };
    auto u16 = [&](size_t at) { uint16_t v; memcpy(&v, bmp.data() + at, 2); return v; };
    const uint32_t offset = u32(10);
    const int32_t w = static_cast<int32_t>(u32(18));
    const int32_t h_signed = static_cast<int32_t>(u32(22));
    const uint16_t bpp = u16(28);
    const uint32_t compression = u32(30);
    if (w <= 0 || h_signed == 0 || w > 16384 || std::abs(h_signed) > 16384) return false;
    if ((bpp != 24 && bpp != 32) || (compression != 0 && compression != 3)) return false;
    const bool bottom_up = h_signed > 0;
    const UINT h = static_cast<UINT>(std::abs(h_signed));
    const size_t src_stride = ((static_cast<size_t>(w) * bpp + 31) / 32) * 4;
    if (offset > bmp.size() || bmp.size() - offset < src_stride * h) return false;
    width = static_cast<UINT>(w);
    height = h;
    stride = width * 4;
    out.assign(static_cast<size_t>(stride) * height, 0);
    const size_t step = bpp / 8;
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* src = bmp.data() + offset + src_stride * (bottom_up ? h - 1 - y : y);
        uint8_t* dst = out.data() + static_cast<size_t>(stride) * y;
        for (UINT x = 0; x < width; ++x, src += step, dst += 4) {
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
        }
    }
    return true;
}

uint32_t ThumbnailSeekMs(uint32_t duration_ms) {
    // Skip intros and fade-ins like Explorer, without walking far into long
    // files (seeking a remote or spinning disk costs time).
    if (duration_ms == 0) return 3000;
    if (duration_ms < 2000) return 0;
    return (std::min)(duration_ms / 10, 30000u);
}

std::wstring QuoteArgument(const std::wstring& argument) { return ffmpeg::QuoteArgument(argument); }

bool MediaPackFrame(const std::wstring& path, UINT cap, bool grid, uint32_t known_duration_ms,
                    MediaFrame& frame) {
    const std::wstring ffmpeg = packs::PackToolPath(packs::PackId::Media, L"ffmpeg.exe");
    if (ffmpeg.empty()) return false;
    cap = std::clamp(cap, 16u, 4096u);
    const ULONGLONG started = GetTickCount64();
    // Square-pixel display size, then fit inside cap x cap without enlarging.
    wchar_t filter[256];
    swprintf_s(filter, L"scale=iw*sar:ih,setsar=1,scale=w='min(%u,iw)':h='min(%u,ih)':"
        L"force_original_aspect_ratio=decrease:flags=%s,format=bgr24", cap, cap,
        grid ? L"fast_bilinear" : L"bicubic");
    const DWORD timeout = grid ? 6000 : 12000;
    const uint32_t seeks[2] = {ThumbnailSeekMs(known_duration_ms), 0};
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 1 && seeks[0] == 0) break;
        std::wstring args = L"-hide_banner -nostdin -loglevel info -threads 2 ";
        if (seeks[attempt] > 0) {
            wchar_t ss[48];
            swprintf_s(ss, L"-ss %u.%03u ", seeks[attempt] / 1000, seeks[attempt] % 1000);
            args += ss;
        }
        args += L"-i " + QuoteArgument(FfmpegInput(path));
        args += L" -map 0:v:0 -an -sn -dn -frames:v 1 -vf ";
        args += QuoteArgument(filter);
        args += L" -f image2pipe -c:v bmp pipe:1";
        ToolRun run = RunTool(ffmpeg, args, timeout, 64u * 1024 * 1024);
        if (!run.started) return false;
        if (!frame.duration_ms) frame.duration_ms = ParseFfmpegDurationMs(run.err);
        if (!frame.source_width && ParseFfmpegVideoSize(run.err, frame.source_width, frame.source_height) &&
            ffmpeg::ParseVideoQuarterTurn(run.err)) {
            std::swap(frame.source_width, frame.source_height);
        }
        if (!run.timed_out && DecodeBmpToBgra(run.out, frame.pixels, frame.width, frame.height, frame.stride)) {
            if (frame.width && frame.height && frame.source_width && frame.source_height &&
                frame.width != frame.height && frame.source_width != frame.source_height &&
                ((frame.width < frame.height) != (frame.source_width < frame.source_height))) {
                std::swap(frame.source_width, frame.source_height);
            }
            diagnostics::runtime::Event("media_pack_frame", {{"ms", GetTickCount64() - started},
                {"grid", grid ? 1u : 0u}, {"attempt", static_cast<uint64_t>(attempt)}});
            return true;
        }
        if (run.timed_out) break;   // a second try would only double the wait
        // Otherwise the seek may have passed the end of a short clip: the
        // second attempt starts from the first frame.
    }
    diagnostics::runtime::Event("media_pack_frame_failed", {{"ms", GetTickCount64() - started}});
    frame = MediaFrame{};
    return false;
}

void RetireAvifSequence(const std::wstring& path) {
    if (!avif_cache.path.empty() && avif_cache.path != path) avif_cache = {};
}

bool MediaPackAvifFrame(const std::wstring& path, UINT cap, uint32_t index, HANDLE client_pipe,
                        AvifSequenceFrame& frame) {
    AvifIdentity id;
    auto& cache = avif_cache;
    if (!ReadAvifIdentity(path, id, true)) {
        if (cache.path == path) cache = {};
        return false;
    }
    const auto pack = packs::ResolvePack(packs::PackId::Media);
    if (pack.source == packs::ToolSource::None || !packs::IsRegularFile(pack.directory + L"\\ffprobe.exe")) {
        cache = {};
        return false;
    }
    if (cache.path != path || cache.id != id || cache.cap != cap || cache.tool_dir != pack.directory) {
        cache = {};
        cache.path = path; cache.id = id; cache.cap = cap; cache.tool_dir = pack.directory;
    }
    if (cache.pixels.empty()) {
        if (GetTickCount64() < cache.retry_after) return false;
        if (!DecodeAvifTrack(cache, client_pipe)) {
            cache.retry_after = GetTickCount64() + 3000;
            return false;
        }
    }
    const uint32_t at = (std::min)(index, cache.track.count - 1);
    const size_t frame_bytes = static_cast<size_t>(cache.width) * cache.height * 4;
    const auto* begin = cache.pixels.data() + at * frame_bytes;
    frame.bitmap.pixels.assign(begin, begin + frame_bytes);
    frame.bitmap.width = cache.width; frame.bitmap.height = cache.height; frame.bitmap.stride = cache.width * 4;
    frame.bitmap.source_width = cache.track.width; frame.bitmap.source_height = cache.track.height;
    frame.count = cache.track.count; frame.delay_ms = cache.track.delays[at];
    return true;
}

std::vector<PreviewPropertyValue> ParseProbeRows(std::string_view output) {
    // Sections arrive in order: [STREAM]...[/STREAM] blocks then [FORMAT].
    struct Stream { std::string type, codec, profile, width, height, rate, sample_rate, channels; bool attached = false; };
    std::vector<Stream> streams;
    std::string duration, bit_rate;
    Stream* current = nullptr;
    bool in_format = false;
    size_t pos = 0;
    while (pos < output.size()) {
        size_t end = output.find('\n', pos);
        if (end == std::string_view::npos) end = output.size();
        std::string_view line = output.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line == "[STREAM]") { streams.emplace_back(); current = &streams.back(); in_format = false; continue; }
        if (line == "[/STREAM]") { current = nullptr; continue; }
        if (line == "[FORMAT]") { in_format = true; current = nullptr; continue; }
        if (line == "[/FORMAT]") { in_format = false; continue; }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string key(line.substr(0, eq)), value(line.substr(eq + 1));
        if (current) {
            if (key == "DISPOSITION:attached_pic") current->attached = value == "1";
            else if (key == "codec_type") current->type = value;
            else if (key == "codec_name") current->codec = value;
            else if (key == "profile") current->profile = value;
            else if (key == "width") current->width = value;
            else if (key == "height") current->height = value;
            else if (key == "avg_frame_rate") current->rate = value;
            else if (key == "sample_rate") current->sample_rate = value;
            else if (key == "channels") current->channels = value;
        } else if (in_format) {
            if (key == "duration") duration = value;
            else if (key == "bit_rate") bit_rate = value;
        }
    }
    std::vector<PreviewPropertyValue> rows;
    double seconds = 0;
    const auto parsed = std::from_chars(duration.data(), duration.data() + duration.size(), seconds);
    if (parsed.ec == std::errc{} && parsed.ptr == duration.data() + duration.size() &&
        seconds > 0 && seconds < static_cast<double>(INT64_MAX)) {
        rows.push_back({L"时长", FormatDuration(static_cast<uint64_t>(seconds + 0.5))});
    }
    const Stream* video = nullptr;
    const Stream* audio = nullptr;
    for (const Stream& s : streams) {
        if (s.type == "video" && !video && !s.attached) video = &s;
        if (s.type == "audio" && !audio) audio = &s;
    }
    if (video) {
        if (!video->width.empty() && !video->height.empty() && video->width != "0")
            rows.push_back({L"分辨率", Utf8ToWide(video->width) + L" x " + Utf8ToWide(video->height)});
        const size_t slash = video->rate.find('/');
        if (slash != std::string::npos) {
            const double num = atof(video->rate.substr(0, slash).c_str());
            const double den = atof(video->rate.substr(slash + 1).c_str());
            if (num > 0 && den > 0) {
                // 25 fps, 29.97 fps, 23.976 fps - as players show them.
                const double value = num / den;
                wchar_t fps[32];
                if (std::abs(value - std::round(value)) < 0.005) swprintf_s(fps, L"%.0f fps", value);
                else {
                    swprintf_s(fps, L"%.3f", value);
                    std::wstring trimmed = fps;
                    while (trimmed.back() == L'0') trimmed.pop_back();
                    swprintf_s(fps, L"%s fps", trimmed.c_str());
                }
                rows.push_back({L"帧率", fps});
            }
        }
        rows.push_back({L"编码格式", CodecDisplay(video->codec, video->profile)});
    }
    if (audio) {
        std::wstring text = CodecDisplay(audio->codec, audio->profile);
        const unsigned rate = static_cast<unsigned>(strtoul(audio->sample_rate.c_str(), nullptr, 10));
        if (rate) {
            wchar_t hz[32];
            if (rate % 1000 == 0) swprintf_s(hz, L" · %u kHz", rate / 1000);
            else swprintf_s(hz, L" · %.1f kHz", rate / 1000.0);
            text += hz;
        }
        const unsigned channels = static_cast<unsigned>(strtoul(audio->channels.c_str(), nullptr, 10));
        if (channels == 1) text += L" · 单声道";
        else if (channels == 2) text += L" · 立体声";
        else if (channels > 2) text += L" · " + std::to_wstring(channels) + L" 声道";
        rows.push_back({L"音频", text});
    }
    const double bps = bit_rate.empty() || bit_rate == "N/A" ? 0.0 : atof(bit_rate.c_str());
    if (bps > 0) {
        wchar_t rate[32];
        if (bps >= 1e6) swprintf_s(rate, L"%.1f Mb/s", bps / 1e6);
        else swprintf_s(rate, L"%.0f kb/s", bps / 1e3);
        rows.push_back({L"比特率", rate});
    }
    return rows;
}

bool MediaPackProperties(const std::wstring& path, std::vector<PreviewPropertyValue>& rows,
                         size_t max_rows) {
    const std::wstring ffprobe = packs::PackToolPath(packs::PackId::Media, L"ffprobe.exe");
    if (ffprobe.empty() || rows.size() >= max_rows) return false;
    const std::wstring args = L"-hide_banner -v error -show_entries "
        L"format=duration,bit_rate:stream=codec_type,codec_name,profile,width,height,avg_frame_rate,"
        L"sample_rate,channels:stream_disposition=attached_pic -of default=noprint_wrappers=0 " + QuoteArgument(FfmpegInput(path));
    ToolRun run = RunTool(ffprobe, args, 5000, 256 * 1024);
    if (!run.started || run.timed_out || run.out.empty()) return false;
    const std::string_view text(reinterpret_cast<const char*>(run.out.data()), run.out.size());
    bool added = false;
    for (PreviewPropertyValue& row : ParseProbeRows(text)) {
        if (rows.size() >= max_rows) break;
        if (HasRow(rows, row.label.c_str())) continue;
        rows.push_back(std::move(row));
        added = true;
    }
    return added;
}

} // namespace pulse::preview
