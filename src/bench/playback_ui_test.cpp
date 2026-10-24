#include "../ui/quick_preview_window.h"
#include "../common/localization.h"
#include <windowsx.h>
#include <commctrl.h>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {
struct QuickPreviewPlaybackProbe {
    static inline int failures = 0;
    static inline HWND scrub_test_window = nullptr;
    static void Check(bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); std::fflush(stdout);
        if (!ok) ++failures;
    }
    static void Pump(unsigned ms) {
        auto end = GetTickCount64() + ms;
        do {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.hwnd == scrub_test_window && msg.message == WM_MOUSEMOVE) {
                    // This test drives pointer positions via SendMessage. Do not
                    // mix in desktop cursor messages while pumping decode results;
                    // moving the real user's cursor to run a test is unnecessary.
                    std::printf("[TRACE] isolated queued desktop mouse move: %d,%d\n",
                        GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam));
                    continue;
                }
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            Sleep(5);
        } while (GetTickCount64() < end);
    }
    static bool Until(const std::function<bool()>& condition) {
        const auto end = GetTickCount64() + 10000;
        do { Pump(20); if (condition()) return true; } while (GetTickCount64() < end);
        return false;
    }
    static QuickPreviewItem Item(const std::wstring& path) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data);
        QuickPreviewItem item;
        item.path = path; item.name = path.substr(path.find_last_of(L"/\\") + 1);
        item.attrs = data.dwFileAttributes;
        item.size = (uint64_t{data.nFileSizeHigh} << 32) | data.nFileSizeLow;
        item.modified = (uint64_t{data.ftLastWriteTime.dwHighDateTime} << 32) | data.ftLastWriteTime.dwLowDateTime;
        return item;
    }
    static void Capture(QuickPreviewWindow& q, const wchar_t* name) {
        // Render the production controls/HUD into an isolated GPU target.
        // The live flip-discard swap-chain and GDI return blank after Present;
        // this captures the actual component painters, not the user's desktop.
        auto* dc = q.compositor_.Dc();
        ID2D1Image* previous = nullptr; dc->GetTarget(&previous);
        const D2D1_SIZE_U size{static_cast<UINT32>(q.compositor_.Width()),
            static_cast<UINT32>(q.compositor_.Height())};
        D2D1_BITMAP_PROPERTIES1 properties{};
        properties.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
        properties.dpiX = properties.dpiY = 96;
        properties.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        ID2D1Bitmap1* target = nullptr;
        HRESULT hr = dc->CreateBitmap(size, nullptr, 0, &properties, &target);
        if (FAILED(hr)) { if (previous) previous->Release(); Check(false, "create component GPU target"); return; }
        dc->SetTarget(target); dc->BeginDraw();
        dc->Clear(q.dark_ ? D2D1::ColorF(0x151515) : D2D1::ColorF(0xF7F7F7));
        const auto video = q.video_.Snapshot();
        if (video.ready && video.ffmpeg)
            q.DrawFfmpegFrame(dc, q.VideoFrameRect(video), 0);
        ID2D1SolidColorBrush* brush = nullptr;
        dc->CreateSolidColorBrush(q.dark_ ? D2D1::ColorF(0xF4F4F4) : D2D1::ColorF(0x202020), &brush);
        q.DrawHud(dc, q.ContentRect(), brush);
        q.DrawPlayback(dc, brush);
        if (brush) brush->Release();
        const HRESULT drawn = dc->EndDraw();
        Check(SUCCEEDED(drawn), "production playback/HUD painters render successfully");
        dc->SetTarget(previous);
        if (previous) previous->Release();
        properties.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        ID2D1Bitmap1* readback = nullptr;
        hr = dc->CreateBitmap(size, nullptr, 0, &properties, &readback);
        if (SUCCEEDED(hr)) hr = readback->CopyFromBitmap(nullptr, target, nullptr);
        D2D1_MAPPED_RECT mapped{};
        if (SUCCEEDED(hr)) hr = readback->Map(D2D1_MAP_OPTIONS_READ, &mapped);
        Check(SUCCEEDED(hr), "read back production playback/HUD component rendering");
        if (SUCCEEDED(hr)) {
            bool varied = false;
            const auto first = *reinterpret_cast<const uint32_t*>(mapped.bits);
            for (UINT y = 0; y < size.height && !varied; ++y) {
                const auto* row = reinterpret_cast<const uint32_t*>(mapped.bits + y * mapped.pitch);
                for (UINT x = 0; x < size.width; ++x) if (row[x] != first) { varied = true; break; }
            }
            Check(varied, "GPU capture contains rendered content, not a blank surface");
            BITMAPINFOHEADER info{}; info.biSize = sizeof(info);
            info.biWidth = static_cast<LONG>(size.width); info.biHeight = -static_cast<LONG>(size.height);
            info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
            BITMAPFILEHEADER file{}; file.bfType = 0x4d42;
            file.bfOffBits = sizeof(file) + sizeof(info); file.bfSize = file.bfOffBits + size.width * size.height * 4;
            HANDLE output = CreateFileW(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            DWORD written = 0;
            WriteFile(output, &file, sizeof(file), &written, nullptr);
            WriteFile(output, &info, sizeof(info), &written, nullptr);
            for (UINT y = 0; y < size.height; ++y)
                WriteFile(output, mapped.bits + y * mapped.pitch, size.width * 4, &written, nullptr);
            CloseHandle(output); readback->Unmap();
        }
        if (readback) readback->Release();
        target->Release();
    }
    static void CheckSlowScrub(QuickPreviewWindow& q) {
        q.frame_count_ = 806;
        q.frame_index_ = 37;
        q.requested_frame_ = 38;
        q.frame_delay_ms_ = 70;
        q.animation_active_ = true;
        q.waiting_for_frame_ = false;
        const auto committed = q.PlaybackInfo({});
        q.waiting_for_frame_ = true;
        Check(q.PlaybackInfo({}) == committed,
            "70 ms GIF decoder wait does not insert or shift footer status");
        const auto track = q.PlaybackTrackRect();
        const float span = track.right - track.left;
        const POINT down{static_cast<LONG>(track.left + span * .2f),
            static_cast<LONG>((track.top + track.bottom) * .5f)};
        q.PlaybackMouseDown(down);
        Check(q.playback_drag_ && !q.animation_active_, "drag pauses without waiting for decoder");
        bool follows = true;
        for (unsigned i = 0; i <= 1000; ++i) {
            const float x = track.left + span * (static_cast<float>(i) / 1000.0f);
            q.SeekPlayback(x);
            const double expected = (x - track.left) / span;
            follows = follows && std::abs(q.PlaybackFraction({}) - expected) < .00001;
        }
        Check(follows && q.frame_index_ == 37,
            "1001 scrub positions follow pointer continuously while displayed frame is stalled");
        Check(q.requested_frame_ == 38 && q.waiting_for_frame_ && q.playback_seek_dirty_,
            "1001 moves retain one in-flight GIF request and only the latest desired target");
        q.EndPlaybackDrag(true);
        Check(q.PlaybackFraction({}) == 1 && q.playback_scrub_pending_ && q.animation_active_,
            "release keeps thumb at final target instead of snapping to stale frame");
        q.TogglePlayback();
        Check(!q.animation_active_ && q.playback_scrub_pending_ && q.playback_seek_dirty_,
            "pause during pending scrub preserves final user target");
        q.TogglePlayback();
        Check(q.animation_active_ && q.playback_scrub_pending_ && q.PlaybackFraction({}) == 1,
            "resume during pending scrub does not discard target or snap thumb");
        SendMessageW(q.hwnd_, WM_TIMER, 7, 0);
        Check(q.requested_frame_ == 38, "autoplay cannot overtake a pending scrub target");
        // Emulate a slow decoder completing its old request, then the final one.
        q.frame_index_ = 38; q.waiting_for_frame_ = false;
        q.TickPlaybackSeek();
        Check(q.requested_frame_ == 805 && q.waiting_for_frame_ && !q.playback_seek_dirty_,
            "completion submits only the latest scrub target, not intermediate frames");
        Check(q.PlaybackFraction({}) == 1, "old decode completion cannot move thumb backwards");
        q.frame_index_ = 805; q.waiting_for_frame_ = false;
        q.TickPlaybackSeek();
        Check(!q.playback_scrub_pending_ && q.animation_active_, "autoplay resumes after final frame commits");
        q.ResetAnimation();
        q.frame_count_ = 806; q.frame_delay_ms_ = 70;
        q.playback_drag_ = true; q.playback_last_seek_ms_ = GetTickCount64();
        q.SeekPlayback(track.left + span * .5f);
        Check(q.playback_seek_dirty_ && !q.waiting_for_frame_, "scrub decode rate is bounded even on cache hits");
        q.playback_last_seek_ms_ = GetTickCount64() - 100;
        q.TickPlaybackSeek();
        Check(!q.playback_seek_dirty_ && q.waiting_for_frame_, "timer submits deferred newest target after throttle interval");
        q.SeekPlayback(track.right);
        q.StepPlayback(-1);
        Check(q.requested_frame_ == 804 && !q.playback_scrub_pending_,
            "frame step after pending scrub uses final intended frame, not stale in-flight frame");
        q.ResetAnimation();
        Check(!q.playback_drag_ && !q.playback_scrub_pending_ && !q.playback_seek_dirty_,
            "reset clears drag and deferred seek state");
    }
    static void CheckLongGif(QuickPreviewWindow& q, const std::wstring& path) {
        q.Update(Item(path));
        Check(Until([&] { return q.frame_count_ == 806 && q.frame_index_ > 0; }),
            "806-frame GIF opens through real preview decoder");
        if (q.frame_count_ != 806) return;
        q.TogglePlayback();
        const auto track = q.PlaybackTrackRect();
        const int y = static_cast<int>((track.top + track.bottom) * .5f);
        int x = static_cast<int>(track.left);
        scrub_test_window = q.hwnd_;
        SendMessageW(q.hwnd_, WM_LBUTTONDOWN, 0, MAKELPARAM(x, y));
        bool follows = true;
        for (int i = 1; i <= 60; ++i) {
            x = static_cast<int>(track.left + (track.right - track.left) * i / 60.0f);
            SendMessageW(q.hwnd_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y));
            Pump(16);
            const double expected = std::clamp((x - track.left) / (track.right - track.left), 0.0f, 1.0f);
            const double actual = q.PlaybackFraction(q.video_.Snapshot());
            if (std::abs(actual - expected) >= .00001)
                std::printf("[TRACE] scrub mismatch i=%d x=%d expected=%.6f actual=%.6f drag=%d pending=%d frame=%u waiting=%d\n",
                    i, x, expected, actual, q.playback_drag_, q.playback_scrub_pending_, q.frame_index_, q.waiting_for_frame_);
            follows = follows && std::abs(actual - expected) < .00001;
        }
        Check(follows, "real 806-frame GIF thumb stays on pointer through async commits");
        // Mouse-up can carry a newer point than the last move; clamp it to end.
        x = static_cast<int>(track.right) + 30;
        SendMessageW(q.hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
        scrub_test_window = nullptr;
        Check(q.PlaybackFraction({}) == 1, "mouse-up takes final coordinate and clamps outside track");
        Check(Until([&] { return !q.playback_scrub_pending_ && !q.waiting_for_frame_ && q.frame_index_ == 805; }),
            "real 806-frame seek commits final target without a stale-request backlog");
        Check(!q.animation_active_, "paused long GIF remains paused after scrubbing");
    }
    static int RunEndSeek(const std::wstring& path) {
        HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"End seek test owner",
            WS_OVERLAPPEDWINDOW, 40, 40, 680, 500, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        QuickPreviewWindow q;
        Check(q.Initialize(owner, WM_APP + 1, WM_APP + 2), "initialize production preview window");
        q.Show(Item(path), false, WindowEffect::None, false);
        Check(Until([&] { auto s = q.video_.Snapshot(); return s.ready || FAILED(s.error); }),
            "target video open completes");
        Check(q.video_.Snapshot().ready, "target video ready for endpoint seek");
        auto report = [&](const char* phase) {
            const auto s = q.video_.Snapshot();
            std::printf("[STATE] %s ready=%d playing=%d ended=%d busy=%d position=%lld duration=%lld error=%08lx control=%08lx\n",
                phase, s.ready, s.playing, s.ended, s.busy, s.position, s.duration,
                static_cast<unsigned long>(s.error), static_cast<unsigned long>(s.control_error));
            std::fflush(stdout);
        };
        auto drag = [&](double fraction) {
            const auto track = q.PlaybackTrackRect();
            const int y = static_cast<int>((track.top + track.bottom) * .5f);
            const int start = static_cast<int>((track.left + track.right) * .5f);
            const int end = static_cast<int>(track.left + (track.right - track.left) * fraction);
            scrub_test_window = q.hwnd_;
            SendMessageW(q.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(start, y));
            SendMessageW(q.hwnd_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(end, y));
            SendMessageW(q.hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(end, y));
        };
        auto at_quarter = [&] {
            const auto s = q.video_.Snapshot();
            return s.ready && SUCCEEDED(s.error) && !s.busy &&
                std::llabs(s.position - s.duration / 4) < 2000000;
        };
        if (q.video_.Snapshot().ready) {
            q.video_.Play(false); Pump(400);
            drag(1.0); Pump(700); report("after endpoint");
            drag(.25);
            Check(Until(at_quarter), "drag from endpoint back to 25 percent settles");
            report("after reverse drag");
            q.video_.Play(true);
            Check(Until([&] { auto s = q.video_.Snapshot(); return s.ready && s.playing &&
                s.position > s.duration / 4 + 3000000; }), "video plays after reverse seek");
            // Reopen so the timeout case cannot inherit a failure from the first drag.
            q.Update(Item(path));
            Check(Until([&] { return q.video_.Snapshot().ready; }), "reopen before delayed endpoint seek");
            q.video_.Play(false); Pump(400);
            drag(1.0); Pump(11000); report("endpoint after command timeout budget");
            drag(.25);
            Check(Until(at_quarter), "reverse drag still works after endpoint exceeds 10 second command budget");
            report("after delayed reverse drag");
            drag(.95);
            Check(Until([&] { const auto s = q.video_.Snapshot(); return s.ready && !s.busy &&
                std::llabs(s.position - s.duration * 95 / 100) < 2000000; }),
                "seek near end before natural playback completion");
            q.video_.Play(true);
            Check(Until([&] { return q.video_.Snapshot().ended; }), "video naturally reaches EOF");
            report("natural EOF");
            drag(.25);
            Check(Until(at_quarter), "drag from natural EOF back to 25 percent settles");
            q.video_.Play(true);
            Check(Until([&] { const auto s = q.video_.Snapshot(); return s.ready && s.playing &&
                !s.ended && SUCCEEDED(s.error) && s.position > s.duration / 4 + 3000000; }),
                "video resumes after seeking back from natural EOF");
            report("playing after natural EOF reverse seek");
            q.video_.Play(false); Pump(400); drag(.95);
            Check(Until([&] { const auto s = q.video_.Snapshot(); return !s.busy &&
                std::llabs(s.position - s.duration * 95 / 100) < 2000000; }),
                "seek near end before restart-at-EOF check");
            q.video_.Play(true);
            Check(Until([&] { return q.video_.Snapshot().ended; }), "reach natural EOF before pressing play");
            const auto play_button = q.PlaybackButtonRect(0);
            const int play_x = static_cast<int>((play_button.left + play_button.right) * .5f);
            const int play_y = static_cast<int>((play_button.top + play_button.bottom) * .5f);
            SendMessageW(q.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(play_x, play_y));
            SendMessageW(q.hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(play_x, play_y));
            Check(Until([&] { const auto s = q.video_.Snapshot(); return s.ready && s.playing &&
                !s.ended && SUCCEEDED(s.error) && s.position > 2000000 && s.position < s.duration / 2; }),
                "clicking play at natural EOF restarts from beginning");
            report("playing after direct EOF restart");
        }
        scrub_test_window = nullptr;
        q.Close(); DestroyWindow(owner); Pump(800);
        return failures ? 1 : 0;
    }
    static int RunVideo(const std::wstring& path) {
        HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Video preview test owner",
            WS_OVERLAPPEDWINDOW, 40, 40, 680, 500, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        QuickPreviewWindow q;
        Check(q.Initialize(owner, WM_APP + 1, WM_APP + 2), "initialize production preview window");
        q.Show(Item(path), false, WindowEffect::None, false);
        Check(Until([&] { auto s = q.video_.Snapshot(); return s.ready || FAILED(s.error); }),
            "production preview finishes opening video");
        const auto state = q.video_.Snapshot();
        std::printf("window video HRESULT=%08lx ready=%d size=%ux%u duration=%lld\n",
            static_cast<unsigned long>(state.error), state.ready, state.width, state.height, state.duration);
        std::printf("window decoder missing=%d codec=%ls\n", state.missing_decoder, state.codec.c_str());
        Check(state.ready && SUCCEEDED(state.error), "production space preview accepts target video");
        if (state.ready) {
            Check(Until([&] { return q.video_.Snapshot().position > 2000000; }),
                "production space preview advances video playback");
            q.Render(); Pump(100);
            bool visible_child = false;
            EnumChildWindows(q.hwnd_, [](HWND child, LPARAM context) -> BOOL {
                wchar_t name[64]{}; GetClassNameW(child, name, ARRAYSIZE(name));
                RECT rect{}; GetWindowRect(child, &rect);
                if (wcscmp(name, L"Pulse.VideoPreview") == 0 && IsWindowVisible(child) &&
                    rect.right > rect.left && rect.bottom > rect.top)
                    *reinterpret_cast<bool*>(context) = true;
                return TRUE;
            }, reinterpret_cast<LPARAM>(&visible_child));
            const bool rendered = state.ffmpeg
                ? q.ffmpeg_bitmap_.get() && q.ffmpeg_serial_ > 0 : visible_child;
            std::printf("window backend ffmpeg=%d uploaded_frame_serial=%llu\n", state.ffmpeg,
                static_cast<unsigned long long>(q.ffmpeg_serial_));
            Check(q.visible() && rendered, "video backend has visible rendered content");
            SendMessageW(q.hwnd_, WM_KEYDOWN, VK_SPACE, 0); Pump(300);
            Check(q.visible() && !q.video_.Snapshot().playing, "space pauses video without closing preview");
            Capture(q, L"build\\playback-check\\target-video-controls.bmp");
        }
        q.Close(); DestroyWindow(owner); Pump(800);
        return failures ? 1 : 0;
    }
    static int Run(const std::wstring& dir) {
        HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Playback test owner",
            WS_OVERLAPPEDWINDOW, 40, 40, 680, 500, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        QuickPreviewWindow q;
        Check(q.Initialize(owner, WM_APP + 1, WM_APP + 2), "initialize production preview window");
        CheckSlowScrub(q);
        const auto gif = Item(dir + L"\\fixture.gif");
        q.Show(gif, false, WindowEffect::None, false);
        Check(Until([&] { return q.frame_count_ > 1 && q.frame_index_ > 0; }), "real GIF decodes and automatically advances");
        SendMessageW(q.hwnd_, WM_KEYDOWN, VK_SPACE, 0);
        const auto frozen = q.frame_index_;
        Pump(500);
        Check(q.visible() && !q.animation_active_ && q.frame_index_ == frozen, "space pauses GIF instead of closing preview");
        SendMessageW(q.hwnd_, WM_KEYDOWN, VK_OEM_PERIOD, 0);
        Check(Until([&] { return !q.waiting_for_frame_ && q.frame_index_ == frozen + 1; }), "next key commits exactly one GIF frame");
        SendMessageW(q.hwnd_, WM_KEYDOWN, VK_OEM_COMMA, 0);
        Check(Until([&] { return !q.waiting_for_frame_ && q.frame_index_ == frozen; }), "previous key restores preceding GIF frame");
        SendMessageW(q.hwnd_, WM_KEYDOWN, VK_HOME, 0);
        Check(Until([&] { return !q.waiting_for_frame_ && q.frame_index_ == 0; }), "Home seeks to first GIF frame");
        for (int i = 0; i < 3; ++i) SendMessageW(q.hwnd_, WM_KEYDOWN, VK_OEM_PERIOD, 0);
        Check(Until([&] { return !q.waiting_for_frame_ && q.frame_index_ == 3; }), "rapid step input tracks requested frame while decoding");
        SendMessageW(q.hwnd_, WM_KEYDOWN, VK_END, 0);
        Check(Until([&] { return !q.waiting_for_frame_ && q.frame_index_ == q.frame_count_ - 1; }), "End seeks to last GIF frame");
        q.StepPlayback(1);
        Check(!q.waiting_for_frame_, "next at last frame does not wrap unexpectedly");
        auto track = q.PlaybackTrackRect();
        POINT target{static_cast<LONG>(track.left + (track.right - track.left) * .5f), static_cast<LONG>((track.top + track.bottom) * .5f)};
        SendMessageW(q.hwnd_, WM_LBUTTONDOWN, 0, MAKELPARAM(target.x, target.y));
        SendMessageW(q.hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(target.x, target.y));
        Check(Until([&] { return !q.waiting_for_frame_ && q.frame_index_ >= 4 && q.frame_index_ <= 5; }), "mouse timeline seeks GIF midpoint");
        Check(!q.animation_active_ && !q.playback_drag_, "paused seek remains paused and releases capture");
        q.TogglePlayback();
        SendMessageW(q.hwnd_, WM_LBUTTONDOWN, 0, MAKELPARAM(target.x, target.y));
        SendMessageW(q.hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(target.x, target.y));
        Check(q.animation_active_, "seek while playing resumes on release");
        SendMessageW(q.hwnd_, WM_LBUTTONDOWN, 0, MAKELPARAM(target.x, target.y));
        SendMessageW(q.hwnd_, WM_CAPTURECHANGED, 0, 0);
        Check(!q.animation_active_ && !q.playback_drag_, "capture loss cancels scrub without accidental resume");
        ReleaseCapture();
        q.loop_count_ = 1; q.completed_loops_ = 1; q.frame_index_ = q.frame_count_ - 1;
        q.waiting_for_frame_ = false; q.animation_active_ = true;
        SendMessageW(q.hwnd_, WM_TIMER, 7, 0);
        Check(!q.animation_active_, "finite GIF loop limit updates stopped state");
        q.dark_ = false; SetWindowPos(q.hwnd_, nullptr, 80, 80, 480, 320, SWP_NOZORDER);
        InvalidateRect(q.hwnd_, nullptr, FALSE); Pump(300);
        Capture(q, L"build\\playback-check\\gif-light-480.bmp");
        l10n::SetLanguage(L"en-US");
        InvalidateRect(q.hwnd_, nullptr, FALSE); Pump(100);
        Capture(q, L"build\\playback-check\\gif-english.bmp");
        l10n::SetLanguage(L"zh-CN");
        q.dark_ = true; q.scale_ = 2; q.RecreateFormats(); q.compositor_.RecreateTextFormats(2);
        SetWindowPos(q.hwnd_, nullptr, 80, 80, 960, 640, SWP_NOZORDER);
        InvalidateRect(q.hwnd_, nullptr, FALSE); Pump(300);
        Capture(q, L"build\\playback-check\\gif-dark-200.bmp");
        Check(q.ContentRect().bottom == q.PlaybackRect().top, "image/HUD and controls reserve non-overlapping layout");
        q.scale_ = 1; q.RecreateFormats(); q.compositor_.RecreateTextFormats(1);
        CheckLongGif(q, dir + L"\\scrub-806.gif");
        q.Update(Item(dir + L"\\fixture.mp4"));
        Check(Until([&] { return q.video_.Snapshot().ready; }), "production window uses controllable video player");
        SendMessageW(q.hwnd_, WM_KEYDOWN, VK_SPACE, 0); Pump(300);
        Check(q.visible() && !q.video_.Snapshot().playing, "space pauses video and keeps preview open");
        SendMessageW(q.hwnd_, WM_KEYDOWN, VK_OEM_PERIOD, 0);
        Check(Until([&] { return q.video_.Snapshot().stepped_frames > 0; }), "video next-frame key reaches media backend");
        SetWindowPos(q.hwnd_, nullptr, 80, 80, 640, 400, SWP_NOZORDER); Pump(300);
        Capture(q, L"build\\playback-check\\video-dark.bmp");
        q.Update(Item(dir + L"\\fixture.png"));
        Check(Until([&] { return q.native_kind_ == QuickPreviewWindow::NativeKind::Bitmap; }), "switch back to static image completes");
        Check(!q.HasPlayback() && q.frame_count_ == 1 && !q.video_.active(), "static image has no stale playback controls");
        SetCapture(q.hwnd_);
        q.Render();
        Check(GetCapture() == q.hwnd_, "static image repaint preserves non-scrub mouse capture");
        ReleaseCapture();
        SendMessageW(q.hwnd_, WM_KEYDOWN, VK_SPACE, 0);
        Check(!q.visible(), "static preview preserves existing space-to-close behavior");
        DestroyWindow(owner); Pump(300);
        std::printf("Failures: %d\n", failures);
        return failures ? 1 : 0;
    }
};
}
int wmain(int argc, wchar_t** argv) {
    const bool video_only = argc == 3 && wcscmp(argv[1], L"--video-only") == 0;
    const bool end_seek_only = argc == 3 && wcscmp(argv[1], L"--end-seek-only") == 0;
    if (argc != 2 && !video_only && !end_seek_only) return 2;
    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const int result = end_seek_only ? pulse::ui::QuickPreviewPlaybackProbe::RunEndSeek(argv[2])
        : video_only ? pulse::ui::QuickPreviewPlaybackProbe::RunVideo(argv[2])
        : pulse::ui::QuickPreviewPlaybackProbe::Run(argv[1]);
    OleUninitialize(); return result;
}
