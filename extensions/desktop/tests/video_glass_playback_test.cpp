#include "host/render/desktop_renderer.h"
#include "host/video/video_wallpaper_player.h"
#include "host/video/video_prober.h"
#include "host/video/video_decode_format_policy.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
#include <thread>
#include <psapi.h>

using namespace pulse::desktop;
using Microsoft::WRL::ComPtr;
namespace {
int failed = 0;
bool user_closed = false;
LRESULT CALLBACK PlaybackWindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_CLOSE || (message == WM_KEYDOWN && wp == VK_ESCAPE)) {
        user_closed = true;
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failed += !ok;
}
double Now() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Memory(const char* phase, ID3D11Device* device) {
    PROCESS_MEMORY_COUNTERS_EX mem{};
    mem.cb = sizeof(mem);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem));
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> base;
    ComPtr<IDXGIAdapter3> adapter;
    DXGI_QUERY_VIDEO_MEMORY_INFO gpu{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
        SUCCEEDED(dxgi->GetAdapter(&base)) && SUCCEEDED(base.As(&adapter)))
        adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &gpu);
    std::printf("MEMORY phase=%s working_set_bytes=%zu private_commit_bytes=%zu gpu_local_bytes=%llu\n",
        phase, mem.WorkingSetSize, mem.PrivateUsage, gpu.CurrentUsage);
}
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    Check(video::IsKnownEightBitAvcProfile(66) && video::IsKnownEightBitAvcProfile(77) &&
          video::IsKnownEightBitAvcProfile(88) && video::IsKnownEightBitAvcProfile(100),
          "known 8-bit AVC profiles prefer native NV12");
    Check(!video::IsKnownEightBitAvcProfile(0) && !video::IsKnownEightBitAvcProfile(110) &&
          !video::IsKnownEightBitAvcProfile(122) && !video::IsKnownEightBitAvcProfile(244),
          "unknown and higher-bit-depth AVC profiles preserve P010-first negotiation");
    if (argc < 3 || !std::filesystem::is_regular_file(argv[1])) return 2;
    bool four_k = false;
    bool native_screen = false;
    bool capture_menu = true;
    bool memory_profile = false;
    bool shared_device = false;
    bool visible = false;
    int import_load = 0; // 0: original continuous poster stress, 1: legacy import, 2: shared-device import.
    for (int i = 3; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--4k") == 0) four_k = true;
        else if (std::wcscmp(argv[i], L"--native-screen") == 0) native_screen = true;
        else if (std::wcscmp(argv[i], L"--no-menu") == 0) capture_menu = false;
        else if (std::wcscmp(argv[i], L"--memory-profile") == 0) memory_profile = true;
        else if (std::wcscmp(argv[i], L"--shared-device") == 0) shared_device = true;
        else if (std::wcscmp(argv[i], L"--visible") == 0) visible = true;
        else if (std::wcscmp(argv[i], L"--import-legacy") == 0 && !import_load) import_load = 1;
        else if (std::wcscmp(argv[i], L"--import-job") == 0 && !import_load) import_load = 2;
        else return 2;
    }
    if ((four_k && native_screen) || (import_load && memory_profile)) return 2;
    if (visible && !SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return 3;
    if (memory_profile) SetEnvironmentVariableW(L"PULSE_VIDEO_MEMORY_PROFILE", L"1");
    uint32_t width = four_k ? 3840u : 1920u, height = four_k ? 2160u : 1080u;
    if (native_screen) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) ||
            !mode.dmPelsWidth || !mode.dmPelsHeight) return 2;
        width = mode.dmPelsWidth; height = mode.dmPelsHeight;
    }
    std::printf("SCENE_CONFIG surface=%ux%u menu_capture=%d native_screen=%d\n",
                width, height, capture_menu ? 1 : 0, native_screen ? 1 : 0);
    std::printf("LOAD_KIND %s\n", import_load == 1 ? "legacy_import_1080p" : import_load == 2 ? "job_import_1080p" : "original_poster_320x180");
    const std::wstring clip = argv[1];
    const std::filesystem::path out = argv[2];
    std::filesystem::create_directories(out);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 3;
    struct ComLifetime { ~ComLifetime() { CoUninitialize(); } } com_lifetime;
    WNDCLASSW wc{};
    wc.lpfnWndProc = PlaybackWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseGlassPlaybackHiddenTest";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | (visible ? 0u : WS_EX_NOACTIVATE),
        wc.lpszClassName, L"isolated video glass validation", WS_POPUP, 0, 0, static_cast<int>(width), static_cast<int>(height),
        nullptr, nullptr, wc.hInstance, nullptr); // Hidden unless explicitly requested.
    struct WindowLifetime { HWND value; ~WindowLifetime() { if (value) DestroyWindow(value); } } window_lifetime{window};
    if (visible) {
        ShowWindow(window, SW_SHOWNOACTIVATE);
        Check(IsWindowVisible(window) != FALSE, "explicit visible production-renderer fixture is shown without activation");
    }
    std::printf("PRESENTATION_MODE visible=%d pid=%lu\n", visible ? 1 : 0, GetCurrentProcessId());
    {
        render::DesktopRenderer renderer;
        if (!window || !renderer.InitializeForHwnd(window, width, height, true, false)) return 3;
        renderer.SetSurfaceGeometry(96, {0, 0, static_cast<int>(width), static_cast<int>(height)},
                                        {0, 0, static_cast<int>(width), static_cast<int>(height)});
        if (memory_profile) Memory("renderer_initialized", renderer.D3DDevice());
        model::DesktopModelState state;
        state.settings.default_material = model::CardMaterial::LiquidGlass;
        state.settings.glass_live_rate = 2;
        state.settings.tiles_enabled = true;
        state.settings.organize_enabled = true;
        for (int i = 0; i < 12; ++i) {
            model::DesktopTile tile;
            tile.id = "glass-" + std::to_string(i);
            tile.kind = model::TileKind::Clock;
            tile.title = L"Glass";
            tile.free_placed = true;
            tile.x = 40 + (i % 4) * 280; tile.y = 40 + (i / 4) * 240;
            tile.w = 220; tile.h = 180; tile.size = {2, 2};
            tile.material = model::CardMaterial::LiquidGlass;
            state.tiles.push_back(tile);
        }
        render::DesktopInteractionState interaction;
        interaction.banner_suppressed = true;
        std::mutex lock;
        ComPtr<IDXGISwapChain1> swap;
        uint32_t sw = 0, sh = 0, gw = 0, gh = 0;
        HANDLE shared = nullptr;
        std::shared_ptr<void> shared_lease;
        uint64_t seq = 0, imported_seq = 0;
        bool swap_pending = false;
        bool frozen_pending = false;
        video::FrozenFrameSnapshot frozen;
        video::FrozenFrameSnapshot retained_frozen;
        std::atomic<uint64_t> ready_count{0};
        std::atomic<unsigned> errors{0};
        video::VideoPlayerCallbacks callbacks;
        callbacks.on_swap_chain_ready = [&](uint64_t, ComPtr<IDXGISwapChain1> sc,
                uint32_t w, uint32_t h, uint32_t, uint32_t) {
            std::lock_guard<std::mutex> guard(lock);
            swap = std::move(sc); sw = w; sh = h; swap_pending = true;
            ++ready_count;
        };
        callbacks.on_glass_frame_ready = [&](uint64_t, uint64_t n, HANDLE handle, uint32_t w, uint32_t h) {
            std::lock_guard<std::mutex> guard(lock);
            if (handle != shared || !shared_lease) {
                HANDLE duplicate = nullptr;
                if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate,
                                     0, FALSE, DUPLICATE_SAME_ACCESS)) {
                    ++errors;
                    return;
                }
                shared_lease = std::shared_ptr<void>(duplicate, [](void* owned) { CloseHandle(owned); });
            }
            seq = n; shared = handle; gw = w; gh = h;
        };
        callbacks.on_playback_error = [&](uint64_t, std::wstring error) {
            ++errors; std::wprintf(L"PLAYER_ERROR %s\n", error.c_str());
        };
        callbacks.on_frozen_frame_ready = [&](uint64_t, video::FrozenFrameSnapshot snapshot) {
            std::lock_guard<std::mutex> guard(lock);
            shared = nullptr;
            shared_lease.reset();
            frozen = std::move(snapshot);
            frozen_pending = true;
        };
        video::VideoWallpaperPlayer player;
        Check(player.Start(renderer.AdapterLuid(), std::move(callbacks),
                           shared_device ? renderer.D3DDevice() : nullptr, shared_device), "start production playback device");
        if (memory_profile) Memory("player_started_before_open", renderer.D3DDevice());
        player.SetGlassConsumerActive(true);
        player.SetGlassLiveRate(2);
        player.OpenVideo(clip, width, height);
        auto tick = [&] {
            ComPtr<IDXGISwapChain1> ready_swap;
            uint32_t ready_sw = 0, ready_sh = 0, ready_gw = 0, ready_gh = 0;
            HANDLE ready_shared = nullptr;
            std::shared_ptr<void> ready_shared_lease;
            uint64_t ready_seq = 0;
            video::FrozenFrameSnapshot ready_frozen;
            bool ready_frozen_pending = false;
            {
                std::lock_guard<std::mutex> guard(lock);
                if (swap_pending) {
                    ready_swap = swap;
                    ready_sw = sw; ready_sh = sh;
                    swap_pending = false;
                }
                if (frozen_pending) {
                    ready_frozen = std::move(frozen);
                    ready_frozen_pending = true;
                    frozen_pending = false;
                    swap.Reset();
                }
                ready_shared_lease = shared_lease;
                ready_shared = ready_shared_lease.get(); ready_seq = seq; ready_gw = gw; ready_gh = gh;
            }
            // Production mailboxes release their lock before GPU work. Holding this
            // fixture's callback mutex here would stall the playback producer itself.
            if (ready_swap) {
                Check(renderer.SetVideoWallpaperSwapChain(ready_swap.Get(), ready_sw, ready_sh, video::VideoFillMode::Fill),
                      "bind real video swap chain to composition tree");
            }
            if (ready_frozen_pending) {
                Check(renderer.SetVideoWallpaperFrozenBgra(ready_frozen.bgra.data(), ready_frozen.width,
                    ready_frozen.height, ready_frozen.stride, video::VideoFillMode::Fill), "bind frozen full-resolution frame");
                retained_frozen = std::move(ready_frozen);
            }
            if (ready_shared && ready_seq != imported_seq &&
                renderer.OnInternalVideoGlassFrame(ready_shared, ready_gw, ready_gh, true)) imported_seq = ready_seq;
            renderer.Tick(state, {}, {}, {}, interaction);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        };
        const double deadline = Now() + 8000;
        while (!player.Stats().open && Now() < deadline && !errors && !user_closed) { tick(); Sleep(2); }
        Check(player.Stats().open && !player.Stats().software_decode, "video opens using GPU-backed decoded samples");
        const auto config = player.Stats();
        if (memory_profile) Memory("video_open", renderer.D3DDevice());
        std::printf("PLAYER_CONFIG input_format=%u swap_format=%u source=%ux%u surface=%ux%u\n",
                    static_cast<unsigned>(config.input_format), static_cast<unsigned>(config.swap_format),
                    config.video_width, config.video_height, config.surface_width, config.surface_height);
        auto phase = [&](bool load) {
            std::atomic<bool> stop{false};
            std::atomic<unsigned> thumbnails{0}, thumbnail_failures{0};
            std::thread worker;
            if (load) worker = std::thread([&] {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                unsigned import_requests = 0;
                while (!stop.load() && (!import_load || import_requests < 6)) {
                    const double request_start = Now();
                    ++import_requests;
                    const auto poster = (out / L"load-poster.jpg").wstring();
                    bool saved = false;
                    if (import_load == 2) {
                        video::VideoProbeJob job;
                        const auto caps = job.ProbeCapabilities();
                        const auto probe = job.ProbeFile(clip);
                        saved = caps.system_ram_bytes > 0 && probe.open_ok &&
                            job.ExtractPoster(clip, poster, 1920, 1080);
                    } else if (import_load == 1) {
                        const auto caps = video::ProbeGpuCapabilities();
                        const auto probe = video::ProbeVideoFile(clip);
                        saved = caps.system_ram_bytes > 0 && probe.open_ok &&
                            video::ExtractPosterJpeg(clip, poster, 1920, 1080);
                    } else {
                        saved = video::ExtractPosterJpeg(clip, poster, 320, 180);
                    }
                    if (saved) ++thumbnails;
                    else ++thumbnail_failures;
                    // Match offered load across import variants instead of letting a faster
                    // implementation do more work and invalidate the playback comparison.
                    if (import_load) {
                        const double remaining = request_start + 1000.0 - Now();
                        if (remaining > 0) Sleep(static_cast<DWORD>(remaining));
                    } else {
                        Sleep(40);
                    }
                }
                CoUninitialize();
            });
            std::vector<double> tick_times, capture_times;
            const auto before = player.Stats();
            const auto renders = renderer.Metrics().total_presents;
            const double start = Now();
            double next_menu = start + 400;
            double next_memory = start;
            while (Now() - start < (memory_profile ? 45000 : 6000) && !errors && !user_closed) {
                const double t = Now(); tick(); tick_times.push_back(Now() - t);
                if (memory_profile && Now() >= next_memory) {
                    Memory("playing_timeline", renderer.D3DDevice());
                    const auto sample = player.Stats();
                    std::printf("MEMORY_PLAYER elapsed_ms=%.0f frames=%llu drops=%llu loops=%llu video_views=%u glass_views=%u retained_textures=%u nominal_texture_bytes=%llu\n",
                        Now() - start, sample.frames_shown, sample.frames_dropped, sample.loop_count,
                        sample.video_input_views, sample.glass_input_views, sample.retained_decode_textures,
                        sample.retained_decode_nominal_bytes);
                    next_memory = Now() + 5000;
                }
                if (capture_menu && Now() >= next_menu) {
                    const double m = Now();
                    renderer.CaptureMenuUnderlay({200, 200}, state, {}, {}, {}, interaction);
                    capture_times.push_back(Now() - m);
                    next_menu = Now() + 400;
                }
                Sleep(2);
            }
            const auto after = player.Stats();
            const double seconds = (Now() - start) / 1000;
            stop = true;
            if (worker.joinable()) worker.join();
            std::sort(tick_times.begin(), tick_times.end());
            std::sort(capture_times.begin(), capture_times.end());
            std::printf("SCENE load=%d seconds=%.3f submitted_fps=%.3f drops=%llu renders=%llu tick_p95_ms=%.3f capture_p95_ms=%.3f thumbnails=%u thumbnail_failures=%u position=%lld loops=%llu\n",
                load ? 1 : 0, seconds, (after.frames_shown - before.frames_shown) / seconds,
                after.frames_dropped - before.frames_dropped, renderer.Metrics().total_presents - renders,
                tick_times.empty() ? -1 : tick_times[tick_times.size() * 95 / 100],
                capture_times.empty() ? -1 : capture_times[capture_times.size() * 95 / 100],
                thumbnails.load(), thumbnail_failures.load(), after.position_100ns, after.loop_count);
            Check(after.frames_shown > before.frames_shown + 60 && !errors, "video continues submitting under glass/menu work");
            Check(renderer.Metrics().total_presents > renders + 30 && renderer.HasLiveGlassConsumers(),
                  "real twelve-card scene keeps rendering imported video frames");
            Check(!load || (thumbnails > 0 && thumbnail_failures == 0), "concurrent thumbnail decoder succeeds");
            Check(!load || !import_load || thumbnails == 6, "six offered import requests complete within the load phase or its in-flight drain");
        };
        phase(false);
        if (!memory_profile && !user_closed) phase(true); // Separate long memory diagnostic; default stress assertions remain unchanged.
        if (user_closed) {
            player.Shutdown();
            renderer.ClearVideoWallpaper();
            std::printf("VIDEO_GLASS_PLAYBACK CANCELLED_BY_USER exit=130\n");
            return 130;
        }
#ifndef PULSE_MENU_UNDERLAY_BASELINE
        renderer.SetActivitySuspended(true);
        const auto suspended_presents = renderer.Metrics().total_presents;
        const auto suspended_imports = renderer.LiveGlassStats().frames_imported;
        for (int i = 0; i < 60; ++i) { renderer.InvalidateAll(); tick(); Sleep(2); }
        Check(renderer.Metrics().total_presents == suspended_presents &&
              renderer.LiveGlassStats().frames_imported == suspended_imports && !renderer.HasDirtyWork(),
              "covered desktop suppresses all paints/imports despite dirty scene and live producer");
        renderer.SetActivitySuspended(false);
        tick();
        Check(renderer.Metrics().total_presents > suspended_presents,
              "uncovered desktop repaints retained scene after suspension");
#endif
        Memory("playing", renderer.D3DDevice());
        player.SetPaused(true);
        const double pause_deadline = Now() + 3000;
        while (!player.Stats().paused && Now() < pause_deadline) { tick(); Sleep(2); }
        const auto paused = player.Stats();
        Sleep(250); tick();
        const auto held = player.Stats();
        Check(paused.paused && held.frames_shown == paused.frames_shown && held.position_100ns == paused.position_100ns,
              "pause holds submitted frame and media clock");
        Memory("hot_paused", renderer.D3DDevice());
        player.SetPaused(false);
        const double resume_deadline = Now() + 3000;
        while (player.Stats().frames_shown < held.frames_shown + 4 && Now() < resume_deadline) { tick(); Sleep(2); }
        Check(player.Stats().frames_shown >= held.frames_shown + 4, "resume continues real frame submission");
        Check(player.Stats().loop_count > 0, "real clip loops without playback error");
        player.SetPaused(true);
        const double freeze_start = Now();
        while (!player.Stats().paused && Now() - freeze_start < 3000) { tick(); Sleep(2); }
        uint64_t freeze_seq = 0;
        { std::lock_guard<std::mutex> guard(lock); freeze_seq = seq; }
        player.FreezeAndRelease();
        while (!player.Stats().frozen_released && Now() - freeze_start < 6000) { tick(); Sleep(2); }
        tick();
        Check(player.Stats().frozen_released && !retained_frozen.bgra.empty(), "freeze releases playback pipeline with retained full-resolution pixels");
        renderer.TrimDxgiMemory();
        Sleep(200);
        Memory("frozen_released", renderer.D3DDevice());
        player.ResumeFromFrozen();
        const double thaw_start = Now();
        while ((!player.Stats().playing || imported_seq <= freeze_seq) && Now() - thaw_start < 6000) { tick(); Sleep(2); }
        Check(player.Stats().playing && imported_seq > freeze_seq, "thaw publishes newer glass sequence and restores playback");
        player.CloseVideo();
        const double close_start = Now();
        while (player.Stats().open && Now() - close_start < 3000) Sleep(2);
        { std::lock_guard<std::mutex> guard(lock); shared = nullptr; shared_lease.reset(); swap.Reset(); swap_pending = false; }
        player.OpenVideo(clip, width, height, true, 20000000);
        const double seek_start = Now();
        while (!player.Stats().open && Now() - seek_start < 6000) { tick(); Sleep(2); }
        tick();
        const auto seek = player.Stats();
        std::printf("SEEK first_ready_ms=%.3f position_100ns=%lld\n", Now() - seek_start, seek.position_100ns);
        Check(seek.open && seek.paused && std::llabs(seek.position_100ns - 20000000) < 1000000,
              "paused reopen seeks to requested two-second timestamp within 100 ms");
        if (memory_profile) {
            retained_frozen = {};
            player.FreezeAndRelease();
            const double pixel_deadline = Now() + 6000;
            while ((!player.Stats().frozen_released || retained_frozen.bgra.empty()) && Now() < pixel_deadline) { tick(); Sleep(2); }
            Check(!retained_frozen.bgra.empty() && retained_frozen.width == width && retained_frozen.height == height,
                  "exact sought frame captured at unchanged output resolution for pixel comparison");
            std::ofstream pixels(out / L"seek-2s.bgra", std::ios::binary);
            pixels.write(reinterpret_cast<const char*>(retained_frozen.bgra.data()), static_cast<std::streamsize>(retained_frozen.bgra.size()));
            Check(pixels.good(), "write deterministic sought frame pixels");
            std::printf("SEEK_PIXELS width=%u height=%u stride=%u bytes=%zu position_100ns=%lld\n",
                        retained_frozen.width, retained_frozen.height, retained_frozen.stride, retained_frozen.bgra.size(), seek.position_100ns);
        }
        player.OpenVideo(clip, width, height, true, 40000000);
        Sleep(20);
        const auto ready_before_cancel = ready_count.load();
        const double cancel_start = Now();
        player.CloseVideo();
        player.Shutdown();
        const double cancel_ms = Now() - cancel_start;
        std::printf("CANCEL shutdown_ms=%.3f ready_before=%llu ready_after=%llu\n", cancel_ms,
                    ready_before_cancel, ready_count.load());
        Check(cancel_ms < 3000 && ready_count.load() == ready_before_cancel,
              "cancelled seek does not publish a stale first frame and shutdown finishes within three seconds");
        renderer.ClearVideoWallpaper();
        Check(errors == 0, "no playback errors");
    }
    std::printf("VIDEO_GLASS_PLAYBACK failed=%d (visible=%d; DWM acceptance requires external trace)\n", failed, visible ? 1 : 0);
    return failed ? 1 : 0;
}
