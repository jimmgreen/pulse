#include "../ui/playback_timeline.h"
#include "../ui/video_preview.h"
#include "../ui/ffmpeg_playback.h"
#include <windows.h>
#include <objbase.h>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>

using pulse::ui::VideoPreview;
static int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (!ok) ++failures;
}
void Pump(unsigned ms) {
    const auto end = GetTickCount64() + ms;
    do {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        Sleep(5);
    } while (GetTickCount64() < end);
}
bool Until(const std::function<bool()>& condition, unsigned ms = 10000) {
    const auto end = GetTickCount64() + ms;
    do { Pump(20); if (condition()) return true; } while (GetTickCount64() < end);
    return false;
}
int wmain(int argc, wchar_t** argv) {
    const bool unsupported_audio_only = argc == 3 && wcscmp(argv[2], L"--unsupported-audio-only") == 0;
    const bool open_only = argc == 3 && wcscmp(argv[2], L"--open-only") == 0;
    const bool native_fallback_only = argc == 3 && wcscmp(argv[2], L"--native-fallback-only") == 0;
    const bool end_seek_only = argc == 3 && wcscmp(argv[2], L"--end-seek-only") == 0;
    const bool relative_seek_only = argc == 3 && wcscmp(argv[2], L"--relative-seek-only") == 0;
    using namespace pulse::ui::playback;
    if (!open_only && !native_fallback_only && !end_seek_only && !unsupported_audio_only) {
        Check(std::abs(SeekOffset(.5, 1'200'000'000, 5) - 65.0 / 120) < 1e-9,
            "relative video seek converts seconds to 100 ns timeline");
        Check(SeekOffset(.02, 1'200'000'000, -5) == 0 &&
              SeekOffset(.98, 1'200'000'000, 5) == 1, "relative video seek clamps both endpoints");
        Check(std::abs(SeekOffset(SeekOffset(.25, 1'200'000'000, 30), 1'200'000'000, -5)
              - 55.0 / 120) < 1e-9, "relative seek accumulates forward and reverse requests");
        Check(SeekOffset(.5, 0, 5) == 0 && SeekOffset(.5, 10, std::numeric_limits<double>::infinity()) == 0,
            "relative seek rejects invalid timelines and offsets");
        Check(FrameAt(0, 20) == 0 && FrameAt(1, 20) == 19, "GIF seek endpoints are valid frame indices");
        Check(FrameAt(.5, 11) == 5, "GIF seek maps progress to frame");
        Check(FrameAt(-1, 10) == 0 && FrameAt(2, 10) == 9, "GIF seek clamps beyond track");
        Check(FrameAt(.5, 0) == 0 && FrameAt(1, 1) == 0, "empty/static timelines do not underflow");
        Check(FrameAt(std::numeric_limits<double>::quiet_NaN(), 10) == 0, "invalid seek is bounded");
        Check(StepFrame(0, -1, 8) == 0 && StepFrame(7, 1, 8) == 7, "manual steps stop at endpoints");
        Check(StepFrame(3, -1, 8) == 2 && StepFrame(3, 1, 8) == 4, "manual previous/next are one frame");
        Check(Fraction(-1, 0, 100) == 0 && Fraction(120, 0, 100) == 1, "drag clamps outside window");
        Check(Fraction(50, 0, 100) == .5 && Fraction(1, 1, 1) == 0, "DPI-independent fraction and zero width");
        Check(VideoPreview::Supports(L"test.MP4") && !VideoPreview::Supports(L"test.gif"), "video selection preserves GIF path");
    }
    if (argc == 2 && wcscmp(argv[1], L"--timeline-only") == 0) return failures ? 1 : 0;
    if (argc != 2 && !open_only && !native_fallback_only && !end_seek_only && !unsupported_audio_only && !relative_seek_only) { std::puts("Video runtime tests require an isolated fixture path"); return 2; }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
        L"Pulse playback regression fixture", WS_OVERLAPPEDWINDOW,
        80, 80, 480, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    VideoPreview video;
#ifdef PULSE_VIDEO_PREVIEW_TESTING
    if (native_fallback_only) VideoPreview::FailNativeAfterReadyForTest(E_NOTIMPL, true);
#endif
    video.Open(window, argv[1]);
    video.Layout(RECT{0, 0, 460, 260}, true);
    if (native_fallback_only) {
        Check(Until([&] {
            const auto s = video.Snapshot();
            return s.ffmpeg && s.ready && SUCCEEDED(s.error);
        }), "fatal native error after MEDIAITEM_SET recovers through FFmpeg");
        const auto recovered = video.Snapshot();
        Check(recovered.native_error == E_NOTIMPL &&
            wcscmp(recovered.native_error_stage, L"MF-error-event") == 0,
            "recovery preserves E_NOTIMPL and its native error event stage");
        Check(recovered.ffmpeg_available && recovered.ffmpeg_attempted && !recovered.ffmpeg_probe_failed,
            "recovery reports a successful configured FFmpeg probe");
        uint64_t serial = 0;
        Check(Until([&] {
            const auto frame = video.Frame(serial);
            return frame && frame->width && frame->height &&
                frame->pixels.size() == static_cast<size_t>(frame->width) * frame->height * 4;
        }), "native error recovery produces a complete decoded video frame");
        Check(Until([&] {
            const auto s = video.Snapshot();
            return s.playing && s.position > 2'000'000;
        }, 1500), "native Play failure followed by fallback preserves autoplay and advances time");
        Check(Until([&] { return video.Snapshot().ended; }, 5000),
            "recovered short fixture reaches its natural endpoint");
        Pump(200);
        Check(video.Snapshot().ended && !video.Snapshot().playing,
            "natural endpoint remains stopped after fallback playback");
        video.Reset();
        Pump(800);
#ifdef PULSE_VIDEO_PREVIEW_TESTING
        VideoPreview::FailNativeAfterReadyForTest(E_NOTIMPL, true);
#endif
        video.Open(window, argv[1]);
        video.Play(false);
        video.Layout(RECT{0, 0, 460, 260}, true);
        Check(Until([&] {
            const auto s = video.Snapshot();
            return s.ffmpeg && s.ready && SUCCEEDED(s.error);
        }), "paused native preview recovers through FFmpeg");
        const auto paused = video.Snapshot();
        Pump(400);
        const auto still_paused = video.Snapshot();
        Check(!paused.playing && !still_paused.playing &&
            std::llabs(still_paused.position - paused.position) < 50000,
            "explicit pause survives native failure and fallback without advancing time");
        video.Reset();
        Pump(800);
        DestroyWindow(window);
        CoUninitialize();
        return failures ? 1 : 0;
    }
    const bool opened = Until([&] { auto s = video.Snapshot(); return s.ready || FAILED(s.error); });
    auto state = video.Snapshot();
    std::printf("open HRESULT=%08lx ready=%d size=%ux%u duration=%lld\n",
        static_cast<unsigned long>(state.error), state.ready, state.width, state.height, state.duration);
    if (unsupported_audio_only) {
        Check(opened && FAILED(state.error) && !state.ready && !state.playing && state.unsupported_audio,
            "unsupported audio reports the system format limitation without offering playback");
        video.Open(window, L"Z:\\__pulse_missing_fixture__\\missing.ogg");
        Check(Until([&] { return FAILED(video.Snapshot().error); }), "missing audio returns an error");
        Check(!video.Snapshot().unsupported_audio, "missing audio is not mislabeled as a system format limitation");
        video.Reset();
        Pump(800);
        DestroyWindow(window);
        CoUninitialize();
        return failures ? 1 : 0;
    }
    Check(opened && state.ready && SUCCEEDED(state.error), "real media opens through production media worker");
    if (relative_seek_only) {
        if (state.ready) {
            const auto at = [&](double seconds) {
                const auto s = video.Snapshot();
                return !s.busy && SUCCEEDED(s.error) && SUCCEEDED(s.control_error) &&
                    std::abs(static_cast<double>(s.position) / 10'000'000 - seconds) < .6;
            };
            Check(state.duration >= 1'000'000'000, "relative seek fixture is at least 100 seconds");
            video.Play(false); Pump(400);
            video.Seek(30.0 * 10'000'000 / static_cast<double>(state.duration));
            video.SeekRelative(5); video.SeekRelative(5); video.SeekRelative(-5);
            Check(Until([&] { return at(35); }), "rapid relative keys accumulate on pending absolute seek");
            Check(!video.Snapshot().playing, "relative seek preserves pause");
            video.SeekRelative(30);
            Check(Until([&] { return at(65); }), "large forward seek advances thirty seconds");
            video.SeekRelative(-60);
            Check(Until([&] { return at(5); }), "large backward seek retreats sixty seconds");
            video.SeekRelative(-60);
            Check(Until([&] { return at(0); }), "backward seek stops at beginning");
            video.SeekRelative(10000);
            Check(Until([&] { return at(static_cast<double>(state.duration) / 10'000'000); }),
                "forward seek stops at decodable end");
            video.SeekRelative(-5);
            Check(Until([&] { return at(static_cast<double>(state.duration) / 10'000'000 - 5); }),
                "reverse seek recovers from endpoint");
            video.Seek(.25);
            Check(Until([&] { return at(static_cast<double>(state.duration) / 40'000'000); }),
                "absolute seek still works after relative input");
            video.Play(true);
            Check(Until([&] { return video.Snapshot().playing; }), "resume before playing seek");
            video.SeekRelative(5);
            Check(Until([&] { const auto s = video.Snapshot(); return s.playing && !s.busy &&
                s.position >= state.duration / 4 + 45'000'000; }), "relative seek preserves playback");
        }
        video.Reset(); Pump(800); DestroyWindow(window); CoUninitialize();
        return failures ? 1 : 0;
    }
    if (end_seek_only) {
        if (state.ready) {
            video.Play(false);
            Pump(500);
            video.Seek(1.0);
            const bool at_end = Until([&] {
                const auto s = video.Snapshot();
                return FAILED(s.error) || (!s.busy && s.position > s.duration * 9 / 10);
            }, 12000);
            state = video.Snapshot();
            std::printf("end error=%08lx control=%08lx busy=%d position=%lld duration=%lld\n",
                static_cast<unsigned long>(state.error), static_cast<unsigned long>(state.control_error),
                state.busy, state.position, state.duration);
            Check(at_end && SUCCEEDED(state.error), "seek to track endpoint remains usable");
            video.Seek(.25);
            Check(Until([&] {
                const auto s = video.Snapshot();
                return SUCCEEDED(s.error) && !s.busy &&
                    std::llabs(s.position - s.duration / 4) < 1500000;
            }, 4000), "seek backward from endpoint reaches quarter position");
        }
        video.Reset();
        Pump(800);
        DestroyWindow(window);
        CoUninitialize();
        return failures ? 1 : 0;
    }
    if (open_only) {
        std::printf("backend ffmpeg=%d missing_decoder=%d can_seek=%d codec=%ls\n",
            state.ffmpeg, state.missing_decoder, state.can_seek, state.codec.c_str());
        std::printf("stage=%ls native_error=%08lx native_stage=%ls ffmpeg_available=%d attempted=%d probe_failed=%d\n",
            state.error_stage, static_cast<unsigned long>(state.native_error), state.native_error_stage,
            state.ffmpeg_available, state.ffmpeg_attempted, state.ffmpeg_probe_failed);
        if (state.ready) Check(Until([&] { return video.Snapshot().position > 2000000; }),
            "video clock advances while playing");
        if (state.ready && state.ffmpeg) {
            uint64_t serial = 0;
            Check(Until([&] {
                const auto frame = video.Frame(serial);
                return frame && frame->width && frame->height &&
                    frame->pixels.size() == static_cast<size_t>(frame->width) * frame->height * 4;
            }), "FFmpeg backend produces a complete video frame");
            std::printf("decoded frame serial=%llu\n", static_cast<unsigned long long>(serial));
        }
        video.Reset();
        Pump(800);
        DestroyWindow(window);
        CoUninitialize();
        return failures ? 1 : 0;
    }
    if (state.ready) {
        Check(Until([&] { return video.Snapshot().position > 2000000; }), "video clock advances while playing");
        video.Play(false);
        Pump(400);
        auto before = video.Snapshot();
        Pump(400);
        auto after = video.Snapshot();
        Check(!after.playing && std::llabs(after.position - before.position) < 50000,
            "pause freezes real media clock");
        Check(after.can_seek && after.duration > 0, "seek capability and duration reported");
        video.Seek(.5);
        Check(Until([&] { auto s = video.Snapshot(); return !s.busy &&
            std::llabs(s.position - s.duration / 2) < 1500000; }), "paused seek reaches midpoint without auto-resume");
        before = video.Snapshot();
        video.Step();
        const bool stepped = Until([&] { auto s = video.Snapshot(); return s.stepped_frames == 1 || FAILED(s.control_error); });
        after = video.Snapshot();
        std::printf("step HRESULT=%08lx delta=%lld count=%u\n", static_cast<unsigned long>(after.control_error),
            after.position - before.position, after.stepped_frames);
        Check(stepped && after.stepped_frames == 1 && !after.playing && SUCCEEDED(after.control_error),
            "MFPlay confirms exact forward frame step while paused");
        // 10 fps fixture: verify timestamp advances by one frame, not arbitrary milliseconds.
        Check(std::llabs((after.position - before.position) - 1000000) < 200000,
            "single step advances fixture by one 100 ms frame");
        video.Step(); video.Step(); video.Step();
        Check(Until([&] { return video.Snapshot().stepped_frames == 4; }), "rapid step clicks are serialized");
        video.Seek(.1); video.Seek(.9); video.Seek(.25);
        Check(Until([&] { auto s = video.Snapshot(); return !s.busy &&
            std::llabs(s.position - s.duration / 4) < 1500000; }), "rapid seeks settle on newest request");
        video.Play(true);
        Check(Until([&] { return video.Snapshot().position > video.Snapshot().duration / 2; }), "resume after seek advances");
        Check(Until([&] { return video.Snapshot().ended; }), "end of media stops playback");
        video.Play(true);
        Check(Until([&] { auto s = video.Snapshot(); return s.playing && !s.ended && s.position < s.duration / 2; }),
            "play at EOF restarts from beginning");
    }
    video.Reset();
    Check(!video.active(), "close clears media state immediately");
    for (int i = 0; i < 4; ++i) { video.Open(window, argv[1]); Pump(30); video.Reset(); }
    video.Open(window, L"Z:\\__pulse_missing_fixture__\\missing.mp4");
    Check(Until([&] { return FAILED(video.Snapshot().error); }), "missing media reports failure instead of spinning");
    Check(!video.Snapshot().ready, "old video cannot overwrite newer failed open");
    video.Reset();
    Pump(800);
    int children = 0;
    EnumChildWindows(window, [](HWND, LPARAM data) -> BOOL {
        ++*reinterpret_cast<int*>(data); return TRUE;
    }, reinterpret_cast<LPARAM>(&children));
    Check(children == 0, "retired video render windows are released after rapid close");
    DestroyWindow(window);
    Pump(200);
    CoUninitialize();
    std::printf("Failures: %d\n", failures);
    return failures ? 1 : 0;
}
