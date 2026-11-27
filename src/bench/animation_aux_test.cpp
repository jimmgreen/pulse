#include "../ui/quick_preview_window.h"
#include "../ui/ffmpeg_playback.h"
#include "../common/localization.h"
#include "../common/preview_packs.h"
#include "../ui/ai_panel_view.h"
#include "../ui/extension_tree.h"
#include "../ui/ui_renderer.h"
#include "../ui/ui_motion.h"
#include <commctrl.h>
#include <filesystem>
#include <cstdio>
#include <functional>
#include <fstream>
#include <set>
#include <atomic>
#include <thread>
#include <algorithm>

namespace pulse::ui {
struct AnimationAuxProbe {
    static inline unsigned paints = 0, failures = 0;
    static inline bool legacy_pump = false;
    static inline double max_pump_ms = 0, max_dispatch_ms = 0;
    static inline UINT longest_message = 0;
    static inline QuickPreviewWindow* traced_preview = nullptr;
    static inline std::filesystem::path trace_directory;
    struct PaintTrace { double at, elapsed, build; uint64_t uploaded; uint32_t present; };
    static inline std::vector<PaintTrace> paint_trace;
    static double Ms() {
        static const double frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart) * 1000.0 / frequency;
    }
    static void Check(bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        std::fflush(stdout);
        if (!ok) ++failures;
    }
    static LRESULT CALLBACK Count(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                   UINT_PTR, DWORD_PTR) {
        if (msg == WM_PAINT) ++paints;
        const double start = Ms();
        const auto result = DefSubclassProc(hwnd, msg, wp, lp);
        if (msg == WM_PAINT && traced_preview && hwnd == traced_preview->hwnd_) {
            paint_trace.push_back({start, Ms() - start, traced_preview->last_render_ms_,
                traced_preview->ffmpeg_serial_, traced_preview->diagnostic_frame_.present});
        }
        return result;
    }
    static void Pump(unsigned ms) {
        const double started = Ms();
        const auto end = GetTickCount64() + ms;
        do {
            MSG msg{};
            // A continuously replenished animation queue is not an idle wait.
            // Respect the caller's observation deadline inside the queue drain.
            while ((legacy_pump || GetTickCount64() < end) && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                const double dispatch_start = Ms();
                TranslateMessage(&msg); DispatchMessageW(&msg);
                const double elapsed = Ms() - dispatch_start;
                if (elapsed > max_dispatch_ms) { max_dispatch_ms = elapsed; longest_message = msg.message; }
                if (legacy_pump && Ms() - started > 10000.0) {
                    std::puts("TRACE legacy pump emergency limit=10000ms");
                    break;
                }
            }
            if (GetTickCount64() < end) Sleep(2);
        } while (GetTickCount64() < end);
        max_pump_ms = (std::max)(max_pump_ms, Ms() - started);
    }
    static bool Until(const std::function<bool()>& condition, unsigned timeout = 10000) {
        const auto end = GetTickCount64() + timeout;
        do { Pump(20); if (condition()) return true; } while (GetTickCount64() < end);
        return false;
    }
    static uint64_t Cpu() {
        FILETIME created{}, exited{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        return (uint64_t{kernel.dwHighDateTime} << 32 | kernel.dwLowDateTime) +
               (uint64_t{user.dwHighDateTime} << 32 | user.dwLowDateTime);
    }
    static QuickPreviewItem Item(const std::wstring& path) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data);
        QuickPreviewItem item;
        item.path = path; item.name = std::filesystem::path(path).filename().wstring();
        item.attrs = data.dwFileAttributes;
        item.size = uint64_t{data.nFileSizeHigh} << 32 | data.nFileSizeLow;
        item.modified = uint64_t{data.ftLastWriteTime.dwHighDateTime} << 32 | data.ftLastWriteTime.dwLowDateTime;
        return item;
    }
    static void Sample(QuickPreviewWindow& q, const char* phase, bool quiet) {
        const auto start = GetTickCount64(), cpu = Cpu();
        const unsigned before = paints;
        const int64_t position = q.video_.Snapshot().position;
        Pump(2000);
        const auto state = q.video_.Snapshot();
        std::printf("METRIC phase=%s wall_ms=%llu cpu_ms=%.3f paints=%u position_delta_ms=%.3f backend=%s\n",
            phase, GetTickCount64() - start, static_cast<double>(Cpu() - cpu) / 10000.0,
            paints - before, static_cast<double>(state.position - position) / 10000.0,
            state.ffmpeg ? "ffmpeg" : "MF");
        if (quiet) Check(paints - before <= 2, "stable paused/ended preview has no periodic paint");
    }
    static void Frames(QuickPreviewWindow& q, const char* phase) {
        std::set<uint64_t> decoded, uploaded, pixels;
        struct DecodeTrace { double at, query_ms; uint64_t serial; };
        std::vector<DecodeTrace> independent;
        std::atomic<bool> stop = false;
        paint_trace.clear(); traced_preview = &q;
        max_pump_ms = max_dispatch_ms = 0; longest_message = 0;
        uint64_t initial_serial = 0;
        q.video_.Frame(initial_serial);
        const double precise_start = Ms();
        std::thread observer([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const double at = Ms();
                uint64_t serial = 0;
                q.video_.Frame(serial);
                independent.push_back({at, Ms() - at, serial});
                Sleep(5);
            }
        });
        const auto start = GetTickCount64();
        const auto position = q.video_.Snapshot().position;
        uint64_t last_serial = 0, last_change = start, longest = 0;
        while (GetTickCount64() - start < 1600) {
            Pump(10);
            uint64_t serial = 0;
            const auto frame = q.video_.Frame(serial);
            if (frame && !frame->pixels.empty()) {
                decoded.insert(serial); uploaded.insert(q.ffmpeg_serial_);
                uint64_t hash = 1469598103934665603ull;
                for (size_t i = 0; i < frame->pixels.size(); i += 251) hash = (hash ^ frame->pixels[i]) * 1099511628211ull;
                pixels.insert(hash);
                if (serial != last_serial) {
                    const auto now = GetTickCount64();
                    if (last_serial) longest = (std::max)(longest, now - last_change);
                    last_change = now; last_serial = serial;
                }
            }
        }
        stop.store(true, std::memory_order_relaxed); observer.join();
        traced_preview = nullptr;
        const double precise_end = Ms();
        uint64_t final_serial = 0; q.video_.Frame(final_serial);
        double decode_gap = 0, decode_query = 0, last_decode_at = precise_start;
        uint64_t last_independent = initial_serial;
        std::set<uint64_t> independent_serials, painted_serials;
        for (const auto& sample : independent) {
            decode_query = (std::max)(decode_query, sample.query_ms);
            independent_serials.insert(sample.serial);
            if (sample.serial != last_independent) {
                decode_gap = (std::max)(decode_gap, sample.at - last_decode_at);
                last_independent = sample.serial; last_decode_at = sample.at;
            }
        }
        decode_gap = (std::max)(decode_gap, precise_end - last_decode_at);
        double paint_gap = 0, paint_ms = 0, build_ms = 0, flush_present_ms = 0, last_paint = precise_start;
        uint64_t last_upload = 0;
        for (const auto& paint : paint_trace) {
            paint_ms = (std::max)(paint_ms, paint.elapsed);
            build_ms = (std::max)(build_ms, paint.build);
            flush_present_ms = (std::max)(flush_present_ms, paint.elapsed - paint.build);
            painted_serials.insert(paint.uploaded);
            if (paint.uploaded != last_upload) {
                paint_gap = (std::max)(paint_gap, paint.at - last_paint);
                last_upload = paint.uploaded; last_paint = paint.at;
            }
        }
        paint_gap = (std::max)(paint_gap, precise_end - last_paint);
        std::printf("TRACE phase=%s pump=%s sample_wall_ms=%.3f max_pump_ms=%.3f max_dispatch_ms=%.3f longest_message=%u serial_delta=%llu independent_serials=%zu decode_gap_ms=%.3f frame_query_ms=%.3f painted_serials=%zu paint_gap_ms=%.3f paint_ms=%.3f build_ms=%.3f flush_present_plus_message_ms=%.3f\n",
            phase, legacy_pump ? "legacy" : "bounded", precise_end - precise_start, max_pump_ms, max_dispatch_ms,
            longest_message, final_serial - initial_serial, independent_serials.size(), decode_gap, decode_query,
            painted_serials.size(), paint_gap, paint_ms, build_ms, flush_present_ms);
        if (!trace_directory.empty()) {
            std::ofstream timeline(trace_directory / (std::string(phase) + "-decode.csv"));
            timeline << "at_ms,frame_query_ms,serial\n";
            for (const auto& row : independent) timeline << row.at - precise_start << ',' << row.query_ms << ',' << row.serial << '\n';
            std::ofstream rendered(trace_directory / (std::string(phase) + "-paint.csv"));
            rendered << "at_ms,wm_paint_ms,build_ms,uploaded_serial,present_hresult\n";
            for (const auto& row : paint_trace) rendered << row.at - precise_start << ',' << row.elapsed << ',' << row.build << ',' << row.uploaded << ',' << row.present << '\n';
        }
        std::printf("FRAMES phase=%s decoded=%zu uploaded=%zu pixel_hashes=%zu max_gap_ms=%llu clock_delta_ms=%.3f\n",
            phase, decoded.size(), uploaded.size(), pixels.size(), longest,
            static_cast<double>(q.video_.Snapshot().position - position) / 10000.0);
        Check(decoded.size() >= 12 && uploaded.size() >= 12 && pixels.size() >= 12,
            "real decoded frames change and reach production compositor");
        Check(q.video_.Snapshot().position > position + 5000000, "real playback clock advances");
    }
    static int Ui(const std::filesystem::path& output, bool modified, bool visual_only = false) {
        HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Auxiliary renderer probe",
            WS_POPUP, 0, 0, 760, 760, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        Check(IsWindowVisible(window) && !IsIconic(window), "isolated auxiliary window is visible and not minimized");
        {
            const auto first = output / L"navigation-first.txt", second = output / L"navigation-second.txt";
            std::ofstream(first) << "FIRST DOCUMENT\nReadable text during preview navigation.\n";
            std::ofstream(second) << "SECOND DOCUMENT\nContent should not slide across the reading surface.\n";
            QuickPreviewWindow q;
            const bool initialized = q.Initialize(window, WM_APP + 1, WM_APP + 2);
            Check(initialized, "production navigation preview initializes");
            if (initialized) {
                q.Show(Item(first.wstring()), false, WindowEffect::None, false);
                ShowWindow(q.hwnd_, SW_SHOWNOACTIVATE);
                Check(q.visible() && !IsIconic(q.hwnd_), "navigation window is visible on isolated desktop");
                Check(Until([&] { return q.text_layout_.get() != nullptr; }), "first real text preview becomes readable");
                q.Update(Item(second.wstring()));
                Check(Until([&] { return q.text_layout_.get() != nullptr; }), "second real text preview becomes readable");
                q.Update(Item(first.wstring())); Pump(500);
                SetWindowSubclass(q.hwnd_, Count, 3, 0);
                q.pending_nav_dir_ = 1; q.pending_nav_at_ = motion::NowMs();
                const auto start = GetTickCount64(); const unsigned before = paints;
                q.Update(Item(second.wstring())); q.Render();
                Check(Until([&] { return q.nav_dir_ == 0; }), "navigation transition reaches stable content");
                const auto elapsed = GetTickCount64() - start;
                std::printf("METRIC phase=navigation wall_ms=%llu paints=%u system_motion=%d\n",
                    elapsed, paints - before, motion::SystemAnimationsEnabled());
                if (modified) Check(elapsed < 250, "single short navigation transition settles within 250ms");
                // Capture a separate navigation after sampling: flip-discard's
                // next back buffer is not necessarily the submitted frame.
                q.Update(Item(first.wstring())); Pump(500);
                q.compositor_.SetFrameCopyEnabled(true);
                q.pending_nav_dir_ = 1; q.pending_nav_at_ = motion::NowMs();
                q.Update(Item(second.wstring())); q.Render();
                Check(q.compositor_.SaveSnapshot((output / L"navigation-start.png").c_str()), "save submitted navigation first frame");
                Until([&] { return q.nav_dir_ == 0; }); q.Render();
                Check(q.compositor_.SaveSnapshot((output / L"navigation-stable.png").c_str()), "save actual stable navigation content");
                q.compositor_.SetFrameCopyEnabled(false);
                RemoveWindowSubclass(q.hwnd_, Count, 3);
                q.Close(); Pump(200);
            }
        }
        if (!visual_only) {
            extensions::UiTree tree;
            tree.root.id = "root";
            extensions::UiNode text;
            text.id = "label"; text.type = extensions::UiNodeType::Text; text.text = "Loading extension data";
            tree.root.children.push_back(text);
            extensions::UiNode progress;
            progress.id = "progress"; progress.type = extensions::UiNodeType::Progress;
            progress.progress_state = "indeterminate";
            tree.root.children.push_back(progress);
            for (int i = 0; i < 30; ++i) {
                text.id = "row" + std::to_string(i); text.text = "Extension row " + std::to_string(i);
                tree.root.children.push_back(text);
            }
            ExtensionTreeView view;
            Check(view.Create(window, tree, {}, false, D2D1::ColorF(0x527d70), 1.0f), "production extension tree created");
            view.SetBounds({0, 0, 760, 760});
            SetWindowSubclass(view.Hwnd(), Count, 2, 0);
            Pump(300);
            unsigned before = paints;
            auto cpu = Cpu(); auto start = GetTickCount64(); Pump(2000);
            std::printf("METRIC phase=extension-loading wall_ms=%llu cpu_ms=%.3f paints=%u\n",
                GetTickCount64() - start, static_cast<double>(Cpu() - cpu) / 10000.0, paints - before);
            if (modified) Check(paints - before <= 42, "extension activity paint frequency bounded at 20Hz");
            tree.root.children[1].progress_state = "normal";
            tree.root.children[1].value = 100;
            Check(view.UpdateTree(tree), "extension progress completes through production tree update");
            Pump(200); before = paints; Pump(500);
            Check(paints == before, "completed extension progress stops requesting paint");
            tree.root.children[1].progress_state = "indeterminate";
            view.UpdateTree(tree); Pump(100);
            ShowWindow(window, SW_HIDE); Pump(200); before = paints; Pump(400);
            Check(paints == before, "hidden extension tree does not paint");
            RemoveWindowSubclass(view.Hwnd(), Count, 2);
            view.Close(); ShowWindow(window, SW_SHOWNOACTIVATE);
        }
        {
            Compositor comp;
            Check(comp.Init(window), "production auxiliary compositor initializes");
            comp.Resize(760, 760); comp.RecreateTextFormats(1.0f);
            auto* dc = comp.Dc();
            const auto theme = MakeTheme(false, D2D1::ColorF(0x527d70));
            AiPanelView panel;
            panel.SetState(true, AiPanelMode::Docked, false);
            AiPanelViewModel vm;
            vm.title = L"Pulse assistant"; vm.greeting = L"What would you like to organize?";
            vm.intro = L"Choose a folder or ask a question."; vm.placeholder = L"Ask Pulse";
            const auto area = D2D1::RectF(0, 0, 460, 760);
            const auto draw = [&] {
                const auto now = motion::NowMs(); panel.Tick(now);
                dc->BeginDraw(); dc->Clear(theme.bg);
                panel.Draw(&comp, vm, theme, area, 460, area, 1.0f, now);
                return SUCCEEDED(dc->EndDraw());
            };
            Check(draw(), "AI empty state draws through production painter");
            const auto cpu = Cpu(), start = GetTickCount64();
            unsigned frames = 1;
            while (GetTickCount64() - start < 2000) {
                Pump(33);
                if (panel.Animating() || panel.WantsDecorFrame()) { draw(); ++frames; }
            }
            std::printf("METRIC phase=ai-empty wall_ms=%llu cpu_ms=%.3f drawn_frames=%u decor=%d system_motion=%d\n",
                GetTickCount64() - start, static_cast<double>(Cpu() - cpu) / 10000.0, frames,
                panel.WantsDecorFrame(), motion::SystemAnimationsEnabled());
            if (modified) Check(!panel.WantsDecorFrame(), "AI empty state settles without decorative loop");
            Check(comp.SaveSnapshot((output / L"ai-empty.png").c_str()), "save actual AI empty painter snapshot");
            vm.empty = false;
            AiBlockView working; working.id = 1; working.kind = AiBlockKind::Working;
            working.text = L"Reading folder contents"; working.running = true;
            vm.blocks.push_back(working);
            Check(draw(), "AI working state draws");
            Check(!motion::SystemAnimationsEnabled() || panel.WantsDecorFrame(), "AI running activity indicator is retained");
            Check(comp.SaveSnapshot((output / L"ai-working.png").c_str()), "save actual AI working painter snapshot");
            vm.blocks.clear(); vm.voice.available = true; vm.voice.listening = true;
            vm.voice.time = L"0:03"; vm.voice.service = L"Test microphone"; vm.voice.hint = L"Listening";
            for (size_t i = 0; i < vm.voice.levels.size(); ++i) vm.voice.levels[i] = static_cast<float>(i + 1) / 14;
            panel.SetState(true, AiPanelMode::Docked, true);
            Check(draw() && panel.WantsDecorFrame(), "live voice levels and recording time remain active");
            Check(comp.SaveSnapshot((output / L"ai-listening.png").c_str()), "save actual voice painter snapshot");
            vm.voice.listening = false; vm.voice.recognizing = true; vm.voice.recognizing_text = L"Recognizing speech";
            Check(draw(), "speech recognition status remains readable");
            {
                MainRenderer renderer; renderer.SetCompositor(&comp); renderer.SetScale(1.0f);
                WindowViewModel base_view;
                dc->BeginDraw();
                renderer.Render(base_view, D2D1::RectF(0, 0, 760, 760), theme);
                dc->EndDraw();
                HoverPreviewView hover; hover.visible = true; hover.folder = true; hover.owner = window;
                hover.path = output.wstring(); hover.name = L"Loading folder"; hover.type_text = L"Folder";
                hover.anchor = D2D1::RectF(10, 10, 90, 35); hover.pointer = D2D1::Point2F(20, 20);
                dc->BeginDraw(); dc->Clear(theme.bg);
                renderer.DrawHoverPreview(hover, D2D1::RectF(0, 0, 760, 760), theme, false);
                Check(SUCCEEDED(dc->EndDraw()), "production hover loading card draws");
                Pump(200);
                dc->BeginDraw(); dc->Clear(theme.bg);
                renderer.DrawHoverPreview(hover, D2D1::RectF(0, 0, 760, 760), theme, false);
                dc->EndDraw();
                std::printf("METRIC phase=hover-folder-loading next_frame_ms=%u\n", renderer.HoverPreviewFrameMs());
                Check(comp.SaveSnapshot((output / L"hover-loading.png").c_str()), "save actual hover loading card snapshot");
                hover.visible = false;
                dc->BeginDraw(); renderer.DrawHoverPreview(hover, area, theme, false); dc->EndDraw();
                Check(renderer.HoverPreviewFrameMs() == 0, "hidden hover stops requesting frames");
                renderer.SetCompositor(nullptr);
            }
            comp.Shutdown();
        }
        DestroyWindow(window);
        std::printf("failures=%u\n", failures);
        return failures ? 1 : 0;
    }
    static void BackgroundFrames(QuickPreviewWindow& q, const std::wstring& fixture,
                                 const std::filesystem::path& output) {
        const bool ffmpeg = q.video_.Snapshot().ffmpeg;
        const auto thumbnail_directory = output / (ffmpeg ? L"ffmpeg-thumbnails" : L"native-thumbnails");
        std::filesystem::create_directories(thumbnail_directory);
        HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Background thumbnail surface",
            WS_POPUP, 0, 0, 512, 512, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Compositor comp;
        Check(comp.Init(window), "background thumbnail compositor initializes");
        comp.Resize(512, 512);
        {
            ThumbnailCache thumbnails;
            thumbnails.SetDeviceContext(comp.Dc());
            std::vector<QuickPreviewItem> items;
            for (int i = 0; i < 8; ++i) {
                const auto path = thumbnail_directory / (L"thumbnail-" + std::to_wstring(i) + L".mp4");
                std::filesystem::copy_file(fixture, path, std::filesystem::copy_options::overwrite_existing);
                items.push_back(Item(path.wstring()));
            }
            unsigned ready = 0, failed = 0;
            const auto request = [&] {
                ready = failed = 0;
                thumbnails.BeginFrame();
                comp.Dc()->BeginDraw();
                for (const auto& item : items) {
                    const auto result = thumbnails.Draw(comp.Dc(), D2D1::RectF(0, 0, 128, 128),
                        item.path, item.attrs, 256, 1, item.modified, item.size);
                    ready += result == PreviewDrawResult::Bitmap;
                    failed += result == PreviewDrawResult::Failed;
                }
                comp.Dc()->EndDraw();
            };
            request();
            if (ffmpeg) {
                Frames(q, "background-thumbnail-load");
            } else {
                const auto position = q.video_.Snapshot().position;
                const double start = Ms();
                Pump(1600);
                const auto state = q.video_.Snapshot();
                std::printf("NATIVE_BACKGROUND wall_ms=%.3f clock_delta_ms=%.3f playing=%d control_hr=%08lx\n",
                    Ms() - start, static_cast<double>(state.position - position) / 10000.0,
                    state.playing, state.control_error);
                Check(!state.ffmpeg && state.playing && SUCCEEDED(state.control_error) &&
                    state.position > position + 5000000, "native playback clock advances during real thumbnail requests");
            }
            Until([&] { request(); return ready == items.size() || failed != 0; }, 15000);
            std::printf("THUMBNAILS requested=%zu ready=%u failed=%u\n", items.size(), ready, failed);
            Check(ready == items.size() && failed == 0, "real background video thumbnail requests complete during playback");
        }
        comp.Shutdown(); DestroyWindow(window);
    }
    static int Run(const std::wstring& fixture, const std::filesystem::path& output, bool expect_quiet) {
        trace_directory = output;
        HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Auxiliary animation probe",
            WS_OVERLAPPEDWINDOW, 40, 40, 700, 500, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        QuickPreviewWindow q;
        Check(q.Initialize(owner, WM_APP + 1, WM_APP + 2), "production quick preview initializes");
        q.options_.muted = true;
        q.options_.loop = 2;
        q.Show(Item(fixture), false, WindowEffect::None, false);
        ShowWindow(q.hwnd_, SW_SHOWNOACTIVATE);
        Check(q.visible() && !IsIconic(q.hwnd_), "media window is visible on isolated desktop");
        SetWindowSubclass(q.hwnd_, Count, 1, 0);
        Check(Until([&] { return q.video_.Snapshot().ready || FAILED(q.video_.Snapshot().error); }), "media opening completes");
        auto state = q.video_.Snapshot();
        std::printf("OPEN hr=%08lx ready=%d ffmpeg=%d duration_ms=%lld size=%ux%u audio_tracks=%zu\n",
            state.error, state.ready, state.ffmpeg, state.duration / 10000, state.width, state.height, state.audio_tracks.size());
        Check(state.ready && SUCCEEDED(state.error), "real video opens without lowering quality or disabling audio stream");
        Check(!state.ffmpeg, "first playback path is native Media Foundation");
        Check(state.audio_tracks.size() == 1, "fixture audio stream remains present");
        if (state.ready) {
            Check(Until([&] { return q.video_.Snapshot().position > 2000000; }), "native playback clock advances");
            BackgroundFrames(q, fixture, output);
            q.TogglePlayback();
            Check(Until([&] { return !q.video_.Snapshot().playing; }), "production playback toggle pauses");
            Pump(600);
            Sample(q, "native-paused", expect_quiet);
            const auto duration = q.video_.Snapshot().duration;
            q.video_.Seek(0.3);
            Check(Until([&] { return std::llabs(q.video_.Snapshot().position - duration * 3 / 10) < 3000000; }), "paused native seek reaches requested position");
            Check(!q.video_.Snapshot().playing, "paused native seek stays paused");
            const auto steps = q.video_.Snapshot().stepped_frames;
            q.video_.Step();
            Check(Until([&] { return q.video_.Snapshot().stepped_frames > steps; }), "native player acknowledges an actual frame step");
            q.options_.loop = 1; q.video_.Seek(0.94); q.video_.Play(true);
            Check(Until([&] { return q.video_.Snapshot().position > duration * 8 / 10; }), "native seek reaches end region");
            Check(Until([&] { const auto s = q.video_.Snapshot(); return s.playing && s.position < duration / 5; }), "native production loop restarts");
            q.options_.loop = 2; q.video_.Seek(0.97);
            Check(Until([&] { return q.video_.Snapshot().ended; }), "native non-looping playback reaches end");
            q.video_.Seek(0.3); q.video_.Play(false);
            Check(Until([&] { const auto s = q.video_.Snapshot(); return !s.playing && !s.ended; }), "native seek after end returns to paused state");
            q.video_.EnableTrackPlayback();
            Check(Until([&] { return q.video_.Snapshot().ready && q.video_.Snapshot().ffmpeg; }), "real FFmpeg pack backend starts");
            q.video_.Seek(0.05); q.video_.Play(true); Pump(700);
            Frames(q, "playing");
            BackgroundFrames(q, fixture, output);
            q.TogglePlayback();
            Check(Until([&] { return !q.video_.Snapshot().playing; }), "FFmpeg playback pauses");
            Pump(600); Sample(q, "ffmpeg-paused", expect_quiet);
            const auto paused = q.video_.Snapshot().position;
            Pump(300);
            Check(std::llabs(q.video_.Snapshot().position - paused) < 500000, "paused playback clock remains stable");
            q.video_.Seek(0.5);
            Check(Until([&] { return std::llabs(q.video_.Snapshot().position - duration / 2) < 3000000; }), "paused FFmpeg seek reaches requested position");
            q.video_.Play(true); Pump(400); Frames(q, "after-seek");
            q.options_.loop = 1;
            q.video_.Seek(0.94);
            Check(Until([&] { return q.video_.Snapshot().position > duration * 8 / 10; }), "seek reaches end region");
            Check(Until([&] { const auto s = q.video_.Snapshot(); return s.playing && s.position < duration / 5; }), "production loop restarts after real end");
            q.options_.loop = 2; q.video_.Seek(0.97);
            Check(Until([&] { return q.video_.Snapshot().ended; }), "non-looping playback reaches end");
            Pump(600); Sample(q, "ended", expect_quiet);
        }
        RemoveWindowSubclass(q.hwnd_, Count, 1);
        q.Close(); DestroyWindow(owner); Pump(500);
        std::printf("failures=%u\n", failures);
        return failures ? 1 : 0;
    }
    static int IdleRollback(const std::wstring& fixture) {
        HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Playback rollback probe",
            WS_OVERLAPPEDWINDOW, 40, 40, 700, 500, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        QuickPreviewWindow q;
        const bool initialized = q.Initialize(owner, WM_APP + 1, WM_APP + 2);
        Check(initialized, "rollback production preview initializes");
        if (!initialized) { DestroyWindow(owner); return 1; }
        q.options_.muted = true;
        q.options_.loop = 2;
        q.Show(Item(fixture), false, WindowEffect::None, false);
        ShowWindow(q.hwnd_, SW_SHOWNOACTIVATE);
        Check(q.visible() && !IsIconic(q.hwnd_), "rollback preview visible on isolated desktop");
        SetWindowSubclass(q.hwnd_, Count, 4, 0);
        Check(Until([&] { return q.video_.Snapshot().ready; }), "rollback real video opens");
        q.video_.Play(false);
        Check(Until([&] { return !q.video_.Snapshot().playing; }), "rollback playback pauses");
        Pump(600);
        const auto before = paints;
        const auto position = q.video_.Snapshot().position;
        Sample(q, "rollback-native-paused", false);
        Check(paints - before >= 20 && paints - before <= 60, "original periodic paused redraw is restored (20-60 paints / 2s)");
        Check(q.video_.Snapshot().position == position, "rollback paused media clock remains stable");
        RemoveWindowSubclass(q.hwnd_, Count, 4);
        q.Close(); DestroyWindow(owner); Pump(500);
        std::printf("failures=%u\n", failures);
        return failures ? 1 : 0;
    }
};
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 5 && argc != 6) {
        std::puts("usage: pulse_animation_aux_test fixture output ffmpeg mode [--expect-idle-gating|--legacy-pump]"); return 2;
    }
    const bool expect_idle_gating = argc == 6 && wcscmp(argv[5], L"--expect-idle-gating") == 0;
    const bool legacy_pump = argc == 6 && wcscmp(argv[5], L"--legacy-pump") == 0;
    if (argc == 6 && !expect_idle_gating && !legacy_pump) return 2;
    pulse::ui::AnimationAuxProbe::legacy_pump = legacy_pump;
    const std::filesystem::path output = std::filesystem::absolute(argv[2]);
    std::filesystem::create_directories(output / L"profile");
    SetEnvironmentVariableW(L"LOCALAPPDATA", (output / L"profile").c_str());
    const auto desktop_name = L"PulseAnimationAux-" + std::to_wstring(GetCurrentProcessId());
    const HDESK desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) { std::printf("isolated desktop error=%lu\n", GetLastError()); return 3; }
    std::puts("desktop=isolated never switched input desktop");
    pulse::packs::PackSettings packs;
    packs.use_custom_ffmpeg = true; packs.custom_ffmpeg = argv[3];
    if (!pulse::packs::SavePackSettings(packs)) return 4;
    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const std::wstring mode = argv[4];
    const bool modified = mode.find(L"modified") != std::wstring::npos;
    const int result = mode == L"--idle-rollback" ? pulse::ui::AnimationAuxProbe::IdleRollback(std::filesystem::absolute(argv[1]).wstring())
        : mode.starts_with(L"visual-") ? pulse::ui::AnimationAuxProbe::Ui(output, modified, true)
        : mode.starts_with(L"ui-") ? pulse::ui::AnimationAuxProbe::Ui(output, modified)
        : pulse::ui::AnimationAuxProbe::Run(std::filesystem::absolute(argv[1]).wstring(), output, expect_idle_gating);
    OleUninitialize();
    return result;
}
