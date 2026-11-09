#include "../ui/playback_timeline.h"
#include "../ui/video_preview.h"
#include "../common/runtime_log.h"
#include <windows.h>
#include <objbase.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>

using pulse::ui::VideoPreview;
static int failures = 0;
static int layout_messages = 0;
static WNDPROC original_video_proc = nullptr;
LRESULT CALLBACK CountLayout(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_WINDOWPOSCHANGING) ++layout_messages;
    return CallWindowProcW(original_video_proc, hwnd, message, wparam, lparam);
}
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
    struct TestLog {
        bool enabled = false;
        TestLog() {
            wchar_t root[32768]{};
            const DWORD length = GetEnvironmentVariableW(L"PULSE_TEST_PLAYBACK_LOG_ROOT", root, static_cast<DWORD>(std::size(root)));
            if (length > 0 && length < std::size(root)) {
                enabled = pulse::diagnostics::runtime::Initialize(root, "playback-test");
                Check(enabled, "isolated playback diagnostic log initializes");
            }
        }
        ~TestLog() {
            if (enabled) {
                pulse::diagnostics::runtime::Flush();
                pulse::diagnostics::runtime::Shutdown();
            }
        }
    } test_log;

    const bool tracks_only = argc == 3 && wcscmp(argv[2], L"--tracks-only") == 0;
    const bool unsupported_audio_only = argc == 3 && wcscmp(argv[2], L"--unsupported-audio-only") == 0;
    const bool cadence_only = argc == 3 && wcscmp(argv[2], L"--cadence-only") == 0;
    const bool open_only = argc == 3 && wcscmp(argv[2], L"--open-only") == 0;
    const bool end_seek_only = argc == 3 && wcscmp(argv[2], L"--end-seek-only") == 0;
    using namespace pulse::ui::playback;
    if (!cadence_only && !open_only && !end_seek_only && !unsupported_audio_only && !tracks_only) {
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
    if (argc != 2 && !cadence_only && !open_only && !end_seek_only && !unsupported_audio_only && !tracks_only) { std::puts("Video runtime tests require an isolated fixture path"); return 2; }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
        L"Pulse playback regression fixture", WS_OVERLAPPEDWINDOW,
        80, 80, 480, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    VideoPreview video;
    video.Open(window, argv[1]);
    video.Layout(RECT{0, 0, 460, 260}, true);
    HWND child = GetWindow(window, GW_CHILD);
    if (child) {
        original_video_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(child, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(CountLayout)));
        for (int i = 0; i < 120; ++i) video.Layout(RECT{0, 0, 460, 260}, true);
        Check(layout_messages == 0, "identical playback layouts do not reposition the video window");
        video.Layout(RECT{0, 0, 400, 240}, true);
        Check(layout_messages > 0, "changed playback bounds reposition the video window");
        layout_messages = 0;
        video.Layout(RECT{0, 0, 400, 240}, true);
        Check(layout_messages == 0, "resized playback layout also remains stable");
        video.Layout(RECT{0, 0, 400, 240}, false);
        Check(!IsWindowVisible(child), "layout visibility change hides video");
        video.Layout(RECT{0, 0, 460, 260}, true);
        Check(IsWindowVisible(child), "layout visibility change restores video");
        SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original_video_proc));
    }
    Check(child != nullptr, "video child exists for layout regression");
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
    if (tracks_only) {
        Check(state.ready && !state.ffmpeg, "normal MP4 retains native backend by default");
        Check(state.audio_tracks.size() == 2 && state.can_select_audio, "native audio tracks enumerated");
        video.Play(false);
        video.SetVolume(0.37f);
        video.SetMuted(true);
        Pump(300);
        video.Seek(0.25);
        Check(Until([&] { const auto s = video.Snapshot(); return s.ready && !s.busy && s.position > s.duration / 5; }),
            "native paused seek reaches saved position");
        if (state.audio_tracks.size() == 2) {
            const int wanted = state.audio_tracks[1].id;
            video.SelectAudioTrack(wanted);
            Check(Until([&] { const auto s = video.Snapshot(); return s.ready && !s.busy && s.audio_track == wanted && s.position > s.duration / 5; }),
                "native track switch reapplies item and restores position");
        }
        state = video.Snapshot();
        Check(!state.playing && state.muted && std::fabs(state.volume - 0.37f) < .001f,
            "native track switch preserves paused volume and mute");
        Check(state.ffmpeg_available, "installed pack available for explicit subtitle activation");
        if (state.ffmpeg_available) {
            const int64_t position = state.position;
            video.EnableTrackPlayback();
            Check(Until([&] { auto s = video.Snapshot(); return s.ffmpeg && s.ready; }), "explicit action switches to FFmpeg");
            state = video.Snapshot();
            Check(!state.playing && state.muted && std::fabs(state.volume - .37f) < .001f &&
                std::llabs(state.position - position) < 5'000'000, "backend switch preserves position pause and audio settings");
            Check(state.audio_track == 1, "backend switch preserves selected second audio track");
            Check(state.audio_tracks.size() == 2 && state.subtitle_tracks.size() == 2,
                "enhanced backend exposes audio and subtitle tracks");
            Check(state.can_select_subtitle == state.ffmpeg_subtitles_available, "subtitle selection follows actual pack filters");
            if (state.ffmpeg_subtitles_available) {
                video.SelectSubtitleTrack(0);
                Check(Until([&] { auto s = video.Snapshot(); return s.ready && s.subtitle_track == 0; }), "subtitle request applies on worker");
                video.SelectSubtitleTrack(-1);
                Check(Until([&] { auto s = video.Snapshot(); return s.ready && s.subtitle_track == -1; }), "subtitle off applies on worker");
            } else {
                Check(!state.subtitle_tracks[0].supported && !state.subtitle_tracks[1].supported,
                    "limited pack clearly marks both subtitle tracks unavailable");
                video.SelectSubtitleTrack(0);
                Pump(200);
                Check(video.Snapshot().ready && video.Snapshot().subtitle_track == -1,
                    "unsupported subtitle request leaves playback healthy");
            }
            video.Open(window, argv[1]);
            Check(Until([&] { return video.Snapshot().ready; }), "reopen native fixture");
            video.EnableTrackPlayback();
            video.Reset();
            Check(!video.active(), "reset cancels pending backend switch");
        }
        video.Reset();
        Pump(1000);
        DestroyWindow(window);
        CoUninitialize();
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
    if (cadence_only) {
        Pump(1000);
        HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, 320, 180);
        HGDIOBJ old = SelectObject(memory, bitmap);
        unsigned char pixels[320 * 180 * 4]{};
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 320; info.bmiHeader.biHeight = -180;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        POINT origin{60, 40}; ClientToScreen(child, &origin);
        unsigned long long previous = 0;
        ULONGLONG start = GetTickCount64(), changed = start;
        unsigned changes = 0, stalls = 0; ULONGLONG longest = 0;
        while (GetTickCount64() - start < 8000) {
            Pump(8);
            BitBlt(memory, 0, 0, 320, 180, screen, origin.x, origin.y, SRCCOPY);
            SelectObject(memory, old);
            GetDIBits(memory, bitmap, 0, 180, pixels, &info, DIB_RGB_COLORS);
            SelectObject(memory, bitmap);
            unsigned long long hash = 1469598103934665603ULL;
            for (unsigned i = 0; i < sizeof(pixels); i += 4) { hash ^= pixels[i]; hash *= 1099511628211ULL; }
            const auto now = GetTickCount64();
            if (hash != previous) {
                const auto gap = now - changed;
                if (changes && gap > 100) ++stalls;
                longest = (std::max)(longest, gap);
                previous = hash; changed = now; ++changes;
            }
        }
        std::printf("presentation changes=%u gaps_over_100ms=%u longest_ms=%llu final_hold_ms=%llu backend=%s\n",
            changes, stalls, longest, GetTickCount64() - changed, video.Snapshot().ffmpeg ? "ffmpeg" : "MF");
        SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr, screen);
        video.Reset(); Pump(800); DestroyWindow(window); CoUninitialize();
        return failures ? 1 : 0;
    }
    if (open_only) {
        if (state.ready) Check(Until([&] { return video.Snapshot().position > 2000000; }),
            "video clock advances while playing");
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
