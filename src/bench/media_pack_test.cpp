// media_pack_test - FFmpeg preview pack: parsing units, pack resolution and an
// end-to-end run through Pulse.Preview.exe.
//
// Integration needs a real FFmpeg to stand in for the pack and to generate
// fixtures: PULSE_TEST_FFMPEG=<path to ffmpeg.exe>, else C:\ffmpeg\bin. Without
// one the integration part is skipped (reported, not failed). Everything runs
// under a private LOCALAPPDATA, so the user's packs and settings are untouched.
#include "../preview_host/media_pack.h"
#include "../common/preview_packs.h"
#include "preview_host_client.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pulse;

static int g_failures = 0;
static int g_checks = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failures; printf("FAIL %s\n", what); }
    else printf("ok   %s\n", what);
}

static std::wstring Env(const wchar_t* name) {
    wchar_t buffer[32768];
    const DWORD n = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
    return n && n < ARRAYSIZE(buffer) ? std::wstring(buffer, n) : std::wstring{};
}

static void WriteText(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

static bool RunWait(const std::wstring& command, DWORD timeout_ms = 60000) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = command;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    const bool done = WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0;
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (!done) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return done && code == 0;
}

static bool PixelsVary(const std::vector<uint8_t>& px) {
    if (px.size() < 8) return false;
    for (size_t i = 4; i < px.size(); i += 4)
        if (px[i] != px[0] || px[i + 1] != px[1] || px[i + 2] != px[2]) return true;
    return false;
}

static bool HasLabel(const std::vector<preview::PreviewPropertyValue>& rows, const wchar_t* label,
                     const wchar_t* contains = nullptr) {
    for (const auto& row : rows)
        if (row.label == label && (!contains || row.value.find(contains) != std::wstring::npos)) return true;
    return false;
}

static void UnitTests() {
    using namespace preview;
    Check(ParseFfmpegDurationMs("  Duration: 00:42:18.12, start: 0.000000, bitrate: 10100 kb/s") == 2538120,
          "duration parse h:m:s.cc");
    Check(ParseFfmpegDurationMs("  Duration: N/A, start: 0") == 0, "duration N/A");
    Check(ParseFfmpegDurationMs("nothing") == 0, "duration absent");
    Check(ParseFfmpegDurationMs("  Duration: 00:00:01.5,") == 1500, "duration single fraction digit");
    UINT w = 0, h = 0;
    Check(ParseFfmpegVideoSize("  Stream #0:0: Video: hevc (Main 10), yuv420p10le(tv), 3840x2160 [SAR 1:1 DAR 16:9], 29.97 fps",
                               w, h) && w == 3840 && h == 2160, "video size parse");
    Check(!ParseFfmpegVideoSize("  Stream #0:1: Audio: aac (LC), 48000 Hz, stereo", w, h), "no video size in audio line");
    w = h = 0;
    Check(ParseFfmpegVideoSize("  Stream #0:0: Video: h264 (avc1 / 0x31637661), yuv420p, 1920x1080, 5000 kb/s", w, h) && w == 1920,
          "codec tag hex is not a size");

    Check(ThumbnailSeekMs(0) == 3000 && ThumbnailSeekMs(1500) == 0 && ThumbnailSeekMs(20000) == 2000 &&
          ThumbnailSeekMs(3600000) == 30000, "seek policy");
    Check(QuoteArgument(L"plain") == L"plain", "quote plain");
    Check(QuoteArgument(L"file:C:\\a b\\c.mkv") == L"\"file:C:\\a b\\c.mkv\"", "quote spaces");
    Check(QuoteArgument(L"C:\\dir with space\\") == L"\"C:\\dir with space\\\\\"", "quote trailing backslash");
    Check(QuoteArgument(L"a\"b") == L"\"a\\\"b\"", "quote embedded quote");

    // 2x2 24bpp bottom-up BMP: rows padded to 8 bytes.
    std::vector<uint8_t> bmp(54 + 16, 0);
    bmp[0] = 'B'; bmp[1] = 'M';
    auto put32 = [&](size_t at, uint32_t v) { memcpy(bmp.data() + at, &v, 4); };
    put32(10, 54); put32(14, 40); put32(18, 2); put32(22, 2);
    bmp[26] = 1; bmp[28] = 24;
    // bottom row (y=1): blue, green; top row (y=0): red, white
    const uint8_t bottom[] = {255,0,0, 0,255,0, 0,0}, top[] = {0,0,255, 255,255,255, 0,0};
    memcpy(bmp.data() + 54, bottom, 8); memcpy(bmp.data() + 62, top, 8);
    std::vector<uint8_t> out; UINT stride = 0;
    const bool decoded = DecodeBmpToBgra(bmp, out, w, h, stride);
    Check(decoded && w == 2 && h == 2 && stride == 8, "bmp header");
    Check(decoded && out[2] == 255 && out[0] == 0 && out[3] == 255, "bmp top-left red, opaque");
    Check(decoded && out[8] == 255 && out[9] == 0, "bmp bottom-left blue (bottom-up order)");
    bmp.resize(60);
    Check(!DecodeBmpToBgra(bmp, out, w, h, stride), "truncated bmp rejected");

    const char* probe =
        "[STREAM]\ncodec_name=hevc\nprofile=Main 10\ncodec_type=video\nwidth=3840\nheight=2160\navg_frame_rate=30000/1001\n[/STREAM]\n"
        "[STREAM]\ncodec_name=aac\nprofile=LC\ncodec_type=audio\nsample_rate=48000\nchannels=2\navg_frame_rate=0/0\n[/STREAM]\n"
        "[FORMAT]\nduration=2538.120000\nbit_rate=10100000\n[/FORMAT]\n";
    const auto rows = ParseProbeRows(probe);
    Check(HasLabel(rows, L"时长", L"42:18"), "probe duration row");
    Check(HasLabel(rows, L"分辨率", L"3840 x 2160"), "probe resolution row");
    Check(HasLabel(rows, L"帧率", L"29.97"), "probe frame rate row");
    Check(HasLabel(rows, L"编码格式", L"H.265 (HEVC) Main 10"), "probe codec row");
    Check(HasLabel(rows, L"音频", L"AAC · 48 kHz · 立体声"), "probe audio row");
    Check(HasLabel(rows, L"比特率", L"10.1 Mb/s"), "probe bitrate row");
}

static void ResolutionTests(const fs::path& profile) {
    using namespace packs;
    SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
    Check(ResolvePackUncached(PackId::Media).source == ToolSource::None, "no pack, no settings -> unavailable");
    const fs::path base = fs::path(PacksRoot()) / L"ffmpeg";
    WriteText(base / L"installed.json", "{\"version\":\"7.1.1\",\"dir\":\"..\\\\..\\\\evil\"}");
    Check(!ReadInstalledPack(PackId::Media).present, "installed.json dir with separators rejected");
    WriteText(base / L"installed.json", "{\"version\":\"7.1.1\",\"dir\":\"7.1.1\"}");
    Check(!ReadInstalledPack(PackId::Media).present, "installed.json without files is not present");
    WriteText(base / L"7.1.1" / L"ffmpeg.exe", "MZ");
    const InstalledPack installed = ReadInstalledPack(PackId::Media);
    Check(installed.present && installed.version == L"7.1.1", "installed pack found");
    Check(ResolvePackUncached(PackId::Media).source == ToolSource::Pack, "installed pack resolves");
    PackSettings settings;
    settings.enabled[0] = false;
    Check(SavePackSettings(settings), "save settings");
    Check(ResolvePackUncached(PackId::Media).source == ToolSource::None, "disabled pack does not resolve");
    const PackSettings round = LoadPackSettings();
    Check(!round.enabled[0] && round.enabled[1] && round.remove_on_uninstall, "settings round trip");
    settings.enabled[0] = true;
    settings.use_custom_ffmpeg = true;
    settings.custom_ffmpeg = (profile / L"missing\\ffmpeg.exe").wstring();
    SavePackSettings(settings);
    Check(ResolvePackUncached(PackId::Media).source == ToolSource::None && PackToolPath(PackId::Media, L"ffmpeg.exe").empty(),
          "missing custom path never silently falls back to installed pack");
    WriteText(settings.custom_ffmpeg, "MZ");
    Check(ResolvePackUncached(PackId::Media).source == ToolSource::Custom, "valid custom source resolves");
    fs::remove(settings.custom_ffmpeg);
    Check(ResolvePackUncached(PackId::Media).source == ToolSource::None, "moved custom source stays unavailable");
    settings.enabled[0] = false;
    SavePackSettings(settings);
    Check(ResolvePackUncached(PackId::Media).source == ToolSource::None, "disabled custom source remains unavailable");
    settings.enabled[0] = true;
    settings.use_custom_ffmpeg = false;
    SavePackSettings(settings);
    Check(ResolvePackUncached(PackId::Media).source == ToolSource::Pack, "explicit source switch restores installed pack");
    fs::remove_all(profile / L"Pulse");
}

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    const fs::path root = fs::temp_directory_path() / (L"pulse_media_pack_" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root);
    const std::wstring saved_profile = Env(L"LOCALAPPDATA");

    UnitTests();
    ResolutionTests(root / L"profile_units");

    std::wstring ffmpeg = Env(L"PULSE_TEST_FFMPEG");
    if (ffmpeg.empty() && packs::IsRegularFile(L"C:\\ffmpeg\\bin\\ffmpeg.exe")) ffmpeg = L"C:\\ffmpeg\\bin\\ffmpeg.exe";
    if (ffmpeg.empty() || !packs::IsRegularFile(packs::DirectoryOf(ffmpeg) + L"\\ffprobe.exe")) {
        printf("SKIP integration: no FFmpeg (set PULSE_TEST_FFMPEG)\n");
    } else {
        // Fixtures, generated by the stand-in FFmpeg itself.
        const fs::path media = root / L"media dir";   // a space in the path on purpose
        fs::create_directories(media);
        auto gen = [&](const std::wstring& args, const fs::path& out) {
            return RunWait(preview::QuoteArgument(ffmpeg) + L" -hide_banner -loglevel error -y " + args + L" " +
                           preview::QuoteArgument(out.wstring()));
        };
        const fs::path hevc = media / L"clip hevc.mkv", flv = media / L"clip.flv", shortclip = media / L"short.mp4",
            tall = media / L"tall.mkv", wide = media / L"anamorphic.mkv", junk = media / L"junk.mkv",
            audio = media / L"tone.mka", ts = media / L"stream.ts",
            m2ts = media / L"concert.m2ts";
        bool have_hevc = gen(L"-f lavfi -i testsrc2=size=640x360:rate=25:duration=10 -c:v libx265 -preset ultrafast -x265-params log-level=error", hevc);
        if (!have_hevc) have_hevc = gen(L"-f lavfi -i testsrc2=size=640x360:rate=25:duration=10 -c:v mpeg4", hevc);
        Check(have_hevc, "fixture: 10 s 640x360 mkv");
        Check(gen(L"-f lavfi -i testsrc2=size=320x240:rate=25:duration=6 -f lavfi -i sine=duration=6 -c:v flv1 -c:a mp3 -shortest", flv) ||
              gen(L"-f lavfi -i testsrc2=size=320x240:rate=25:duration=6 -c:v flv1", flv), "fixture: 6 s flv");
        Check(gen(L"-f lavfi -i testsrc2=size=320x240:rate=25:duration=1 -c:v mpeg4", shortclip), "fixture: 1 s mp4");
        Check(gen(L"-f lavfi -i testsrc2=size=360x640:rate=25:duration=4 -c:v mpeg4", tall), "fixture: portrait mkv");
        Check(gen(L"-f lavfi -i testsrc2=size=720x576:rate=25:duration=4 -vf setsar=16/15 -c:v mpeg4", wide), "fixture: anamorphic mkv");
        Check(gen(L"-f lavfi -i sine=frequency=440:duration=5 -c:a flac", audio), "fixture: flac mka");
        Check(gen(L"-f lavfi -i testsrc2=size=320x240:rate=25:duration=5 -c:v mpeg2video -f mpegts", ts), "fixture: mpeg-ts");
        Check(gen(L"-f lavfi -i smptehdbars=s=1280x720:d=22 -c:v mpeg2video -f mpegts", m2ts), "fixture: 720p mpeg-ts");
        WriteText(junk, std::string(200000, 'x'));

        // Pack = the stand-in FFmpeg, through the "use installed FFmpeg" setting.
        const fs::path profile = root / L"profile";
        SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
        packs::PackSettings settings;
        settings.use_custom_ffmpeg = true;
        settings.custom_ffmpeg = ffmpeg;
        Check(packs::SavePackSettings(settings), "custom FFmpeg setting saved");
        Check(packs::ResolvePackUncached(packs::PackId::Media).source == packs::ToolSource::Custom, "custom FFmpeg resolves");
        Check(preview::MediaPackAvailable(), "MediaPackAvailable");

        preview::MediaFrame frame;
        ULONGLONG t0 = GetTickCount64();
        bool ok = preview::MediaPackFrame(hevc.wstring(), 256, true, 0, frame);
        const ULONGLONG grid_ms = GetTickCount64() - t0;
        printf("     grid frame %ux%u in %llu ms, duration %u ms, source %ux%u\n", frame.width, frame.height,
               grid_ms, frame.duration_ms, frame.source_width, frame.source_height);
        Check(ok && frame.width == 256 && frame.height == 144, "hevc mkv: 256x144 grid frame");
        Check(ok && frame.duration_ms >= 9900 && frame.duration_ms <= 10100, "hevc mkv: duration from the same run");
        Check(ok && frame.source_width == 640 && frame.source_height == 360, "hevc mkv: source size");
        Check(ok && PixelsVary(frame.pixels) && frame.pixels[3] == 255, "hevc mkv: real, opaque picture");
        Check(grid_ms < 3000, "hevc mkv: grid frame under 3 s");

        frame = {};
        ok = preview::MediaPackFrame(hevc.wstring(), 2000, false, 10000, frame);
        Check(ok && frame.width == 640 && frame.height == 360, "large request never enlarges");

        frame = {};
        ok = preview::MediaPackFrame(shortclip.wstring(), 256, true, 0, frame);
        Check(ok && frame.width == 256 && frame.height == 192, "1 s clip: retry from start after seeking past the end");

        frame = {};
        ok = preview::MediaPackFrame(tall.wstring(), 256, true, 0, frame);
        Check(ok && frame.width == 144 && frame.height == 256, "portrait: fits height");

        frame = {};
        ok = preview::MediaPackFrame(wide.wstring(), 256, true, 0, frame);
        Check(ok && frame.width == 256 && frame.height == 192, "anamorphic: square-pixel aspect (768x576)");

        frame = {};
        ok = preview::MediaPackFrame(ts.wstring(), 256, true, 0, frame);
        Check(ok && frame.width == 256 && frame.height == 192, "mpeg-ts frame");

        frame = {};
        t0 = GetTickCount64();
        ok = preview::MediaPackFrame(junk.wstring(), 256, true, 0, frame);
        printf("     junk: ok=%d %ux%u in %llu ms\n", ok ? 1 : 0, frame.width, frame.height, GetTickCount64() - t0);
        Check(!ok && frame.pixels.empty() && GetTickCount64() - t0 < 5000, "junk file fails fast and clean");

        std::vector<preview::PreviewPropertyValue> rows;
        Check(preview::MediaPackProperties(flv.wstring(), rows, 6), "flv properties");
        Check(HasLabel(rows, L"时长", L"0:06") && HasLabel(rows, L"分辨率", L"320 x 240") &&
              HasLabel(rows, L"编码格式", L"Sorenson"), "flv rows: duration, size, codec");
        rows.clear();
        rows.push_back({L"时长", L"from shell"});
        preview::MediaPackProperties(audio.wstring(), rows, 6);
        Check(rows[0].value == L"from shell" && HasLabel(rows, L"音频", L"FLAC"), "audio rows keep shell values");

        // End to end through the preview host and its decoder table.
        {
            pulse_test::Host host;
            Check(host.Start(), "preview host starts with the pack");
            pulse_test::Result r;
            const uint32_t grid = ipc::kPreviewRequestFlagGrid;
            Check(host.Request(flv.wstring(), r, MAXDWORD, 256, ipc::PreviewRequestKind::Content, grid) &&
                  r.response.kind == ipc::PreviewContentKind::Bitmap && r.response.width == 256, "host: flv grid thumbnail");
            Check(r.response.duration_ms >= 5900 && r.response.duration_ms <= 6100, "host: flv duration chip");
            r = {};
            Check(host.Request(ts.wstring(), r, MAXDWORD, 256, ipc::PreviewRequestKind::Content, grid) &&
                  r.response.kind == ipc::PreviewContentKind::Bitmap, "host: mpeg-ts grid thumbnail");
            r = {};
            // Windows' own provider stalls > 8 s on this file; the pack must not wait for it.
            const ULONGLONG m2ts_started = GetTickCount64();
            Check(host.Request(m2ts.wstring(), r, 4000, 256, ipc::PreviewRequestKind::Content, grid) &&
                  r.response.kind == ipc::PreviewContentKind::Bitmap && r.response.duration_ms >= 21000,
                  "host: 720p m2ts grid thumbnail without the shell stall");
            Check(GetTickCount64() - m2ts_started < 4000, "host: 720p m2ts answers within 4 s");
            r = {};
            Check(host.Request(hevc.wstring(), r, MAXDWORD, 256, ipc::PreviewRequestKind::Content, grid) &&
                  r.response.kind == ipc::PreviewContentKind::Bitmap && r.response.duration_ms > 0,
                  "host: mkv grid thumbnail with duration (shell or pack)");
            r = {};
            Check(host.Request(flv.wstring(), r, MAXDWORD, 256, ipc::PreviewRequestKind::Properties) &&
                  std::any_of(r.properties.begin(), r.properties.end(), [](const auto& p) { return p.first == L"编码格式"; }),
                  "host: flv details from ffprobe");
            r = {};
            Check(host.Request(junk.wstring(), r, MAXDWORD, 256, ipc::PreviewRequestKind::Content, grid) &&
                  r.response.kind != ipc::PreviewContentKind::Bitmap, "host: junk mkv is not a picture");
        }
        // Disabled pack: the host behaves exactly as without it.
        settings.enabled[0] = false;
        packs::SavePackSettings(settings);
        {
            pulse_test::Host host;
            Check(host.Start(), "preview host starts with the pack disabled");
            pulse_test::Result r;
            host.Request(flv.wstring(), r, MAXDWORD, 256, ipc::PreviewRequestKind::Content, ipc::kPreviewRequestFlagGrid);
            Check(r.response.kind != ipc::PreviewContentKind::Bitmap,
                  "host: disabled pack is not used");
        }
    }

    SetEnvironmentVariableW(L"LOCALAPPDATA", saved_profile.empty() ? nullptr : saved_profile.c_str());
    std::error_code ec;
    fs::remove_all(root, ec);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
