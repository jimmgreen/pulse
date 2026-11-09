// ffmpeg_playback_test - Quick Look playback through the FFmpeg preview pack.
// Pure helpers always run; the integration part needs a real FFmpeg with
// libx265 (PULSE_TEST_FFMPEG=<ffmpeg.exe>, else C:\ffmpeg\bin) and is skipped
// without one.
#include "../ui/ffmpeg_playback.h"
#include "../ui/audio_waveform.h"
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace pulse;
using namespace pulse::ui;

namespace {
int g_failed = 0, g_passed = 0;
void Check(bool ok, const char* what) {
    if (ok) { ++g_passed; return; }
    ++g_failed;
    std::printf("FAIL: %s\n", what);
}
bool Near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }
double Sec(int64_t t) { return t / 10'000'000.0; }

std::wstring Env(const wchar_t* name) {
    wchar_t value[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(name, value, MAX_PATH);
    return n && n < MAX_PATH ? value : L"";
}

bool RunWait(const std::wstring& command) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring line = command;
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

int ChildProcesses() {
    int count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry))
        if (entry.th32ParentProcessID == GetCurrentProcessId()) ++count;
    CloseHandle(snap);
    return count;
}

// Pumps like VideoPreview's loop for `ms`; returns the frames shown.
int PumpFor(FfmpegPlayback& player, DWORD ms, std::shared_ptr<const FfmpegFrame>* last = nullptr) {
    int frames = 0;
    const ULONGLONG until = GetTickCount64() + ms;
    while (GetTickCount64() < until) {
        if (auto frame = player.Pump()) {
            ++frames;
            if (last) *last = frame;
        }
        Sleep(4);
    }
    return frames;
}

// The first frame after Start (paused or not).
std::shared_ptr<const FfmpegFrame> FirstFrame(FfmpegPlayback& player, DWORD timeout_ms = 5000) {
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < until) {
        if (auto frame = player.Pump()) return frame;
        Sleep(4);
    }
    return nullptr;
}

void PureHelpers() {
    SIZE s = FfmpegFrameSize(1920, 1080, {960, 600});
    Check(s.cx == 960 && s.cy == 540, "frame size fits the box");
    s = FfmpegFrameSize(640, 360, {1920, 1080});
    Check(s.cx == 640 && s.cy == 360, "frame size never enlarges");
    s = FfmpegFrameSize(1001, 563, {1001, 563});
    Check(s.cx % 2 == 0 && s.cy % 2 == 0, "frame size is even");
    s = FfmpegFrameSize(4000, 10, {100, 100});
    Check(s.cx >= 16 && s.cy >= 16, "frame size has a floor");

    Check(FfmpegOutputRate(0) == 25.0, "unknown rate plays at 25 fps");
    Check(Near(FfmpegOutputRate(23.976), 23.976, 1e-9), "film rate kept");
    Check(FfmpegOutputRate(59.94) == 30.0, "high rates capped at 30 fps");

    Check(FfmpegTempoFilter(1.0f).empty(), "no tempo filter at 1x");
    Check(FfmpegTempoFilter(2.0f) == L"atempo=2", "2x is one stage");
    Check(FfmpegTempoFilter(4.0f) == L"atempo=2,atempo=2", "4x chains two stages");
    Check(FfmpegTempoFilter(0.25f) == L"atempo=0.5,atempo=0.5", "0.25x chains two stages");
    Check(FfmpegTempoFilter(1.5f) == L"atempo=1.5", "1.5x is one stage");

    const char* dvd =
        "Input #0, mpeg, from 'file:x.vob':\n"
        "  Duration: 00:01:30.50, start: 0.280633, bitrate: 5000 kb/s\n"
        "  Stream #0:0[0x1e0]: Video: mpeg2video (Main), yuv420p(tv, top first), 720x576 [SAR 64:45 DAR 16:9], 25 fps, 25 tbr, 90k tbn\n"
        "  Stream #0:1[0x80]: Audio: ac3, 48000 Hz, stereo, fltp, 192 kb/s\n";
    FfmpegMediaInfo info;
    Check(ParseFfmpegMediaInfo(dvd, info), "DVD log parses");
    Check(info.duration == 905'000'000, "duration in 100 ns");
    Check(info.video && info.audio, "video and audio streams found");
    Check(info.width == 1024 && info.height == 576, "anamorphic picture gets its display width");
    Check(info.fps == 25.0, "frame rate read");

    const std::string tracks = std::string(dvd) +
        "  Stream #0:2(eng): Audio: aac, 48000 Hz, stereo\n"
        "  Stream #0:3(zho): Subtitle: subrip (default)\n"
        "  Stream #0:4: Subtitle: hdmv_pgs_subtitle\n"
        "    comment: Stream #0:5: Audio: mp3\n";
    Check(ParseFfmpegMediaInfo(tracks, info) && info.audio_tracks.size() == 2 &&
        info.subtitle_tracks.size() == 2, "track enumeration ignores metadata impostors");
    Check(info.audio_tracks.size() == 2 && info.audio_tracks[1].id == 1 &&
        info.audio_tracks[1].label.find(L"eng") != std::wstring::npos, "audio map uses type-relative index and language");
    Check(info.subtitle_tracks.size() == 2 && info.subtitle_tracks[0].supported &&
        !info.subtitle_tracks[1].supported, "text subtitles supported, bitmap subtitles explicit unsupported");
    Check(ParseFfmpegSubtitleFilters(" ... subtitles V->V Render\n ... setpts V->V Timestamp\n"), "required subtitle filters detected");
    Check(ParseFfmpegSubtitleFilters(" .. subtitles V->V Render\n TS setpts V->V Timestamp\n"), "two flag FFmpeg format supported");
    Check(ParseFfmpegSubtitleFilters(" ... subtitles V->V Render\n TSC setpts V->V Timestamp\n"), "three flag FFmpeg format supported");
    Check(!ParseFfmpegSubtitleFilters(" xx subtitles V->V Render\n .. setpts V->V Timestamp\n"), "invalid filter flags rejected");
    Check(!ParseFfmpegSubtitleFilters(" note: subtitles V->V Render\n .. setpts V->V Timestamp\n"), "misleading descriptive text is not a subtitle filter");
    Check(!ParseFfmpegSubtitleFilters(" .. subtitles A->A Render\n .. setpts V->V Timestamp\n"), "filter stream type must be video");
    Check(!ParseFfmpegSubtitleFilters(" ... subtitles V->V Render\n"), "setpts is required for subtitle seeking");
    Check(!ParseFfmpegSubtitleFilters(" ... scale V->V Scale\n"), "limited preview pack has no subtitles");
    info.subtitle_filters = true;
    FfmpegPlayback selection(L"", L"", info);
    Check(selection.SelectTracks(1, 0), "second audio and first subtitle accepted");
    Check(!selection.SelectTracks(2, 0) && !selection.SelectTracks(0, 1) && !selection.SelectTracks(0, -2),
        "invalid track and bitmap subtitle rejected");
    Check(selection.SelectTracks(0, -1), "subtitles can be disabled");

    const char* mp3 =
        "Input #0, mp3, from 'file:x.mp3':\n"
        "  Duration: 00:03:12.04, start: 0.025057, bitrate: 320 kb/s\n"
        "  Stream #0:0: Audio: mp3 (mp3float), 44100 Hz, stereo, fltp, 320 kb/s\n"
        "  Stream #0:1: Video: mjpeg (Baseline), yuvj420p(pc), 600x600 [SAR 1:1 DAR 1:1], 90k tbr, 90k tbn (attached pic)\n";
    Check(ParseFfmpegMediaInfo(mp3, info) && info.audio && !info.video, "cover art is not a video stream");
    const char* tbr_only = "  Stream #0:0: Video: h264, yuv420p, 1280x720, 23.98 tbr, 1k tbn\n";
    Check(Near(ffmpeg::ParseFrameRate(tbr_only), 23.98, 1e-9), "tbr used when fps is absent");
    Check(!ParseFfmpegMediaInfo("Invalid data found when processing input\n", info), "garbage is no media");
    const std::string misleading =
        "Input #0, matroska, from 'file:test.mkv':\n"
        "  Metadata:\n"
        "    title           : Duration: 99:59:59.99 Video: h264, 9999x9999, SAR 99:1, 120 fps\n"
        "    comment         : Stream #0:0: Video: h264, 9999x9999, 120 fps\n";
    Check(ParseFfmpegMediaInfo(misleading + dvd, info) && info.duration == 905'000'000 &&
          info.width == 1024 && info.height == 576 && info.fps == 25,
          "metadata cannot override actual playback size SAR fps or seek duration");
    Check(!ParseFfmpegMediaInfo(misleading, info), "metadata-only Video and Stream text do not create a stream");
    const char* multiple =
        "  Duration: N/A, start: 0\n"
        "  Stream #0:0: Video: mjpeg, 600x600, 90k tbr (attached pic)\n"
        "  Stream #0:1: Video: h264, yuv420p, 320x240 [SAR 2:1 DAR 8:3]\n"
        "  Stream #0:2: Video: hevc, yuv420p, 1920x1080 [SAR 4:1 DAR 64:9], 60 fps\n";
    Check(ParseFfmpegMediaInfo(multiple, info) && info.duration == 0 && info.width == 640 &&
          info.height == 240 && info.fps == 0, "video properties all belong to first real stream, not cover or later stream");
}

void Integration(const std::wstring& fixture_exe, const std::wstring& ffmpeg_exe) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"pulse_ffmpeg_playback_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring video = dir + L"\\clip hevc.mkv";
    const std::wstring audio = dir + L"\\tone.wv";
    const std::wstring q = ffmpeg::QuoteArgument(fixture_exe);
    const bool made_video = RunWait(q + L" -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=320x240:rate=30:duration=4"
        L" -f lavfi -i sine=frequency=440:duration=4 -c:v libx265 -preset ultrafast -x265-params log-level=none"
        L" -c:a aac -shortest -metadata " + ffmpeg::QuoteArgument(
            L"title=Duration: 99:59:59.99 Video: h264, 9999x9999 [SAR 99:1], 120 fps") +
        L" -metadata " + ffmpeg::QuoteArgument(L"comment=Stream #0:0: Video: h264, 9999x9999, 120 fps") +
        L" " + ffmpeg::QuoteArgument(video));
    const bool made_audio = RunWait(q + L" -hide_banner -loglevel error -y -f lavfi -i sine=frequency=330:duration=1.5"
        L" -c:a wavpack " + ffmpeg::QuoteArgument(audio));
    Check(made_video && made_audio, "fixtures generated");
    if (!made_video || !made_audio) return;
    const int children_before = ChildProcesses();

    FfmpegMediaInfo info;
    ULONGLONG t0 = GetTickCount64();
    Check(ProbeFfmpegMedia(ffmpeg_exe, video, info), "probe opens the HEVC clip");
    const ULONGLONG probe_ms = GetTickCount64() - t0;
    Check(Near(Sec(info.duration), 4.0, 0.1), "probe duration");
    Check(info.video && info.audio && info.width == 320 && info.height == 240 && Near(info.fps, 30, 0.01),
          "probe streams, size and rate");
    {
        FfmpegPlayback broken(ffmpeg_exe, dir + L"\\missing.mkv", info);
        Check(broken.Start(0, 1.0f, {320, 240}, true), "decoder process starts for invalid input");
        const ULONGLONG until = GetTickCount64() + 5000;
        while (!broken.Failed() && GetTickCount64() < until) Sleep(10);
        Check(broken.Failed() && !broken.Ended(), "decoder failure is not normal EOF");
        for (int i = 0; i < 100 && broken.Diagnostic().empty(); ++i) Sleep(10);
        Check(!broken.Diagnostic().empty(), "decoder error text retained");
    }
    {
        std::atomic<unsigned> wakes{0};
        FfmpegPlayback player(ffmpeg_exe, video, info, [&] { ++wakes; });
        t0 = GetTickCount64();
        Check(player.Start(0, 1.0f, {1920, 1080}, false), "paused start");
        auto first = FirstFrame(player);
        const ULONGLONG first_ms = GetTickCount64() - t0;
        Check(first && first->width == 320 && first->height == 240 && first->time == 0, "first frame, not enlarged");
        Check(first && first->pixels.size() == 320u * 240 * 4, "BGRA frame bytes");
        Check(PumpFor(player, 300) == 0 && player.Position() == 0, "paused: no frames, position held");
        Check(wakes.load() > 0, "decoded frames wake the waiting player");
        Sleep(200);
        const unsigned idle_wakes = wakes.load();
        Sleep(200);
        Check(wakes.load() == idle_wakes, "paused queue stops producing wakeups");
        Check(player.Step(), "step while paused");
        auto stepped = player.Pump();
        Check(stepped && Near(Sec(stepped->time), 1.0 / 30, 0.002), "step shows the next frame");

        player.SetPlaying(true);   // after a step: restarts at the stepped frame
        const int frames = PumpFor(player, 1500);
        const double played = Sec(player.Position());
        std::printf("  probe %llu ms, first frame %llu ms, 1.5 s play: %d frames, clock %.2f s, audio %s\n",
                    probe_ms, first_ms, frames, played, player.audio_open() ? "waveOut" : "wall clock");
        Check(frames >= 30, "about 30 fps while playing");
        Check(played > 0.9 && played < 2.0, "clock advances with playback");

        player.SetPlaying(false);
        const int64_t paused = player.Position();
        PumpFor(player, 300);
        Check(player.Position() == paused, "pause holds the clock");

        player.Start(30'000'000, 1.0f, {1920, 1080}, false);
        auto seeked = FirstFrame(player);
        Check(seeked && Near(Sec(seeked->time), 3.0, 0.001), "seek shows the frame at the target");

        player.Start(35'000'000, 1.0f, {1920, 1080}, true);
        bool ended = false;
        for (ULONGLONG until = GetTickCount64() + 4000; GetTickCount64() < until && !ended; Sleep(10)) {
            player.Pump();
            ended = player.Ended();
        }
        Check(ended, "playback ends at the end of the file");

        player.Start(0, 2.0f, {640, 480}, true);
        FirstFrame(player);
        PumpFor(player, 1000);
        const double fast = Sec(player.Position());
        Check(fast > 1.4 && fast < 2.8, "2x rate doubles the clock");
        Check(player.frame_size().cx == 320, "box larger than the video keeps its size");
        player.Start(0, 1.0f, {160, 160}, false);
        auto small = FirstFrame(player);
        Check(small && small->width == 160 && small->height == 120, "small box scales the frames down");
    }
    {
        FfmpegMediaInfo tone;
        Check(ProbeFfmpegMedia(ffmpeg_exe, audio, tone) && tone.audio && !tone.video, "audio-only probe");
        FfmpegPlayback player(ffmpeg_exe, audio, tone);
        Check(player.Start(0, 1.0f, {640, 480}, true), "audio-only start");
        PumpFor(player, 700);
        const double at = Sec(player.Position());
        Check(at > 0.3 && at < 1.2, "audio-only clock advances");
        bool ended = false;
        for (ULONGLONG until = GetTickCount64() + 3000; GetTickCount64() < until && !ended; Sleep(20)) ended = player.Ended();
        Check(ended, "audio-only playback ends");
    }
    {
        // Waveform for audio Media Foundation cannot read: 1 s silence, 1 s tone.
        const std::wstring wave = dir + L"\\half.wv";
        const bool made = RunWait(q + L" -hide_banner -loglevel error -y -f lavfi -i "
            L"\"aevalsrc=if(lt(t\\,1)\\,0\\,0.9*sin(2*PI*440*t)):s=44100:d=2\" -c:a wavpack " + ffmpeg::QuoteArgument(wave));
        Check(made, "waveform fixture generated");
        std::vector<float> peaks;
        float progress = 0;
        int publishes = 0;
        std::atomic<bool> stop{false};
        const ULONGLONG w0 = GetTickCount64();
        const bool ok = made && FfmpegWaveform(ffmpeg_exe, wave, stop,
            [&](const std::vector<float>& levels, float at) { peaks = levels; progress = at; ++publishes; });
        std::printf("  waveform: %llu ms, %d publishes\n", GetTickCount64() - w0, publishes);
        Check(ok && progress == 1.0f && peaks.size() == AudioWaveform::kBuckets, "ffmpeg waveform completes");
        float quiet = 0, tone = 1;
        for (size_t i = 0; i + 20 < peaks.size() / 2; ++i) quiet = (std::max)(quiet, peaks[i]);
        for (size_t i = peaks.size() / 2 + 20; i < peaks.size(); ++i) tone = (std::min)(tone, peaks[i]);
        Check(quiet < 0.02f && tone > 0.9f, "waveform follows silence then tone");
        Check(!FfmpegWaveform(ffmpeg_exe, dir + L"\\missing.wv", stop, [](const std::vector<float>&, float) {}),
              "missing file has no waveform");
        DeleteFileW(wave.c_str());
    }
    FfmpegMediaInfo none;
    Check(!ProbeFfmpegMedia(ffmpeg_exe, dir + L"\\missing.mkv", none), "missing file does not probe");
    Sleep(200);
    Check(ChildProcesses() == children_before, "no ffmpeg left running");
    DeleteFileW(video.c_str());
    DeleteFileW(audio.c_str());
    RemoveDirectoryW(dir.c_str());
}
} // namespace

void LevelsMath() {
    WaveformLevels levels(4);
    for (int i = 0; i < 100; ++i) {
        levels.Add(0, (i & 1) ? 0.5f : -0.5f);                       // loudest: RMS 0.5
        levels.Add(1, (i & 1) ? 0.5f * 0.17783f : -0.5f * 0.17783f);  // -15 dB
        levels.Add(2, (i & 1) ? 0.005f : -0.005f);                   // -40 dB
    }
    const std::vector<float> v = levels.Levels();
    Check(v.size() == 4 && std::fabs(v[0] - 1.0f) < 1e-4f, "loudest bucket is full height");
    Check(std::fabs(v[1] - 0.5f) < 0.01f, "-15 dB is half height (30 dB window)");
    Check(v[2] == 0.0f && v[3] == 0.0f, "below the window and unreached buckets are empty");
    WaveformLevels silent(3);
    for (int i = 0; i < 10; ++i) silent.Add(1, 0.0f);
    const std::vector<float> s = silent.Levels();
    Check(s[0] == 0.0f && s[1] == 0.0f && s[2] == 0.0f, "silence stays flat");
}

void ProbeCancellation() {
    const int children_before = ChildProcesses();
    wchar_t self[32768];
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    std::atomic<bool> stop{false};
    std::thread cancel([&] { Sleep(100); stop = true; });
    FfmpegMediaInfo info;
    const ULONGLONG start = GetTickCount64();
    Check(!ProbeFfmpegMedia(self, L"cancel-test", info, &stop), "cancelled probe produces no result");
    cancel.join();
    Check(GetTickCount64() - start < 1500, "probe cancels without eight-second timeout");
    Check(ChildProcesses() == children_before, "cancelled probe child has exited before return");
    stop = false;
    const ULONGLONG filters_start = GetTickCount64();
    std::thread cancel_filters([&] { Sleep(100); stop = true; });
    Check(!FfmpegSubtitleFilters(self, &stop), "cancelled filter capability probe reports unavailable");
    cancel_filters.join();
    Check(GetTickCount64() - filters_start < 1500 && ChildProcesses() == children_before,
        "filter probe cancellation exits promptly and reaps child");
}

void TrackIntegration(const std::wstring& media, bool limited = false) {
    const std::wstring ffmpeg_exe = Env(L"PULSE_TEST_FFMPEG");
    Check(!ffmpeg_exe.empty(), "track tests require PULSE_TEST_FFMPEG");
    if (ffmpeg_exe.empty()) return;
    FfmpegMediaInfo info;
    Check(ProbeFfmpegMedia(ffmpeg_exe, media, info), "probe multitrack fixture");
    Check(info.audio_tracks.size() == 2 && info.subtitle_tracks.size() == 2, "real fixture has two audio and two subtitle tracks");
    if (info.audio_tracks.size() != 2 || info.subtitle_tracks.size() != 2) return;
    Check(info.subtitle_filters != limited, limited ? "installed limited pack rejects subtitle filters" : "full pack has subtitle filters");
    const int before = ChildProcesses();
    {
        FfmpegPlayback player(ffmpeg_exe, media, info);
        if (limited) {
            Check(!info.subtitle_tracks[0].supported && !info.subtitle_tracks[1].supported, "unsupported pack marks subtitle tracks unavailable");
            Check(!player.SelectTracks(1, 0), "missing filters reject subtitle selection before starting decoder");
            Check(player.SelectTracks(1, -1), "limited pack still selects second audio");
            Check(player.Start(10'000'000, 1.0f, {640, 360}, true), "limited pack starts enhanced audio playback");
            Check(FirstFrame(player) && PumpFor(player, 500) > 0 && !player.Failed(), "limited pack plays video and second audio without subtitle filters");
        } else {
        Check(player.Start(10'000'000, 1.0f, {640, 360}, false), "paused seek with subtitles off");
        auto off = FirstFrame(player);
        Check(off && !player.Failed(), "off frame decodes");
        Check(player.SelectTracks(1, 0), "select second audio and first subtitle");
        Check(player.Start(10'000'000, 1.0f, {640, 360}, false), "restart at seek with subtitle");
        auto on = FirstFrame(player);
        Check(on && !player.Failed(), "subtitle filter decodes after seek");
        Check(off && on && off->pixels != on->pixels, "embedded text subtitle changes rendered pixels");
        Check(player.SelectTracks(1, 1), "select second subtitle");
        player.Start(10'000'000, 1.0f, {640, 360}, false);
        auto other = FirstFrame(player);
        Check(on && other && on->pixels != other->pixels, "second subtitle changes rendered pixels");
        Check(player.SelectTracks(1, -1), "disable subtitles");
        player.Start(10'000'000, 1.0f, {640, 360}, false);
        auto restored = FirstFrame(player);
        Check(off && restored && off->pixels == restored->pixels, "subtitle off restores original frame");
        player.SetPlaying(true);
        Check(PumpFor(player, 600) > 0 && !player.Failed(), "second audio track plays without decoder failure");
        }
    }
    Check(ChildProcesses() == before, "track switching leaves no decoder process");
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && (wcscmp(argv[1], L"--tracks-only") == 0 || wcscmp(argv[1], L"--limited-pack-only") == 0)) {
        PureHelpers();
        TrackIntegration(argv[2], wcscmp(argv[1], L"--limited-pack-only") == 0);
        std::printf("ffmpeg tracks: %d passed, %d failed\n", g_passed, g_failed);
        return g_failed ? 1 : 0;
    }
    if (argc == 2 && wcscmp(argv[1], L"--cancel") == 0) {
        ProbeCancellation();
        std::printf("ffmpeg cancellation: %d passed, %d failed\n", g_passed, g_failed);
        return g_failed ? 1 : 0;
    }
    // Deterministic stalled decoder used by ProbeCancellation.
    if (argc > 1) { Sleep(30000); return 1; }
    ProbeCancellation();
    PureHelpers();
    LevelsMath();
    std::wstring ffmpeg_exe = Env(L"PULSE_TEST_FFMPEG");
    if (ffmpeg_exe.empty() && GetFileAttributesW(L"C:\\ffmpeg\\bin\\ffmpeg.exe") != INVALID_FILE_ATTRIBUTES)
        ffmpeg_exe = L"C:\\ffmpeg\\bin\\ffmpeg.exe";
    if (ffmpeg_exe.empty()) std::printf("  (no FFmpeg: integration part skipped)\n");
    else {
        const std::wstring pack_exe = Env(L"PULSE_TEST_PACK_FFMPEG");
        Integration(ffmpeg_exe, pack_exe.empty() ? ffmpeg_exe : pack_exe);
    }
    std::printf("ffmpeg_playback_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
