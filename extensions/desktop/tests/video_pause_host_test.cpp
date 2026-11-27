#define PULSE_DESKTOP_DRAG_TEST 1
#define main DesktopPluginEntryForVideoPauseTest
#include "host/host_main.cpp"
#undef main
#include <psapi.h>

namespace pulse::desktop::video {
struct VideoWallpaperControllerTestPeer {
    static VideoPlayerStats Stats(const VideoWallpaperController& controller) { return controller.player_.Stats(); }
    static uint64_t PausedSince(const VideoWallpaperController& controller) { return controller.pause_sm_.PausedSinceMs(); }
    static PauseLifecycleState State(const VideoWallpaperController& controller) { return controller.pause_sm_.State(); }
};
}

namespace pulse::desktop::render {
struct DesktopRendererTestAccess {
    static bool HasVideoSwap(const DesktopRenderer& renderer) { return renderer.bound_video_swap_chain_ != nullptr; }
    static bool HasFrozenSurface(const DesktopRenderer& renderer) { return renderer.video_static_surface_ != nullptr; }
};
}

namespace {
int failures = 0;
void Verify(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}

uint64_t ProcessCpu100ns() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return k.QuadPart + u.QuadPart;
}

struct MemorySample {
    SIZE_T working_set = 0;
    SIZE_T private_commit = 0;
    uint64_t gpu_local = 0;
};

MemorySample ReadMemory(ID3D11Device* device, const char* phase) {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> base;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    DXGI_QUERY_VIDEO_MEMORY_INFO gpu{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&base)) &&
        SUCCEEDED(base.As(&adapter))) adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &gpu);
    std::printf("MEMORY phase=%s working_set_bytes=%zu private_commit_bytes=%zu gpu_local_bytes=%llu\n",
                phase, counters.WorkingSetSize, counters.PrivateUsage, gpu.CurrentUsage);
    return {counters.WorkingSetSize, counters.PrivateUsage, gpu.CurrentUsage};
}

struct DesktopVideoPauseHostTestPeer {
    using ControllerPeer = video::VideoWallpaperControllerTestPeer;
    using RendererPeer = render::DesktopRendererTestAccess;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        auto* host = reinterpret_cast<DesktopHostSession*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
            host = static_cast<DesktopHostSession*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
        }
        if (host) {
            bool handled = false;
            const LRESULT result = host->OnSurfaceMessage(hwnd, message, wp, lp, handled);
            if (handled || message == WM_POWERBROADCAST || message == WM_WTSSESSION_CHANGE) return result;
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }

    static void Pump(DesktopHostSession& host, DWORD duration_ms) {
        const ULONGLONG deadline = GetTickCount64() + duration_ms;
        do {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            // The real host loop calls Tick even without a pending window
            // message; exercise that same renderer entry point while suspended.
            host.renderer_.Tick(host.state_, host.group_views_, host.tile_board_rect_, host.live_data_, host.interaction_);
            Sleep(10);
        } while (GetTickCount64() < deadline);
    }

    template<class Predicate>
    static bool WaitFor(DesktopHostSession& host, Predicate predicate, DWORD timeout_ms) {
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        while (!predicate() && GetTickCount64() < deadline) Pump(host, 20);
        return predicate();
    }

    static void DisplayState(HWND hwnd, DWORD state) {
        struct Setting { GUID guid; DWORD length; DWORD data; } setting{GUID_CONSOLE_DISPLAY_STATE, sizeof(DWORD), state};
        SendMessageW(hwnd, WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, reinterpret_cast<LPARAM>(&setting));
    }

    static bool CheckAutoCadForeground(DWORD expected_pid, DWORD* actual_pid = nullptr, bool print = false) {
        const HWND foreground = GetForegroundWindow();
        DWORD pid = 0;
        GetWindowThreadProcessId(foreground, &pid);
        wchar_t executable[32768]{};
        DWORD executable_size = ARRAYSIZE(executable);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        const bool have_name = process && QueryFullProcessImageNameW(process, 0, executable, &executable_size);
        if (process) CloseHandle(process);
        const auto filename = have_name ? std::filesystem::path(executable).filename().wstring() : std::wstring{};
        const bool is_acad = _wcsicmp(filename.c_str(), L"acad.exe") == 0;
        const bool covers_desktop = video::VideoWallpaperController::IsDesktopOccludedByForeground(nullptr, nullptr);
        const bool valid = foreground && is_acad && (expected_pid == 0 || expected_pid == pid) && covers_desktop;
        if (actual_pid) *actual_pid = pid;
        if (print || !valid) {
            RECT frame{};
            if (FAILED(DwmGetWindowAttribute(foreground, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
                GetWindowRect(foreground, &frame);
            }
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            GetMonitorInfoW(MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY), &monitor);
            std::printf("REAL_FOREGROUND hwnd=%p pid=%lu expected_pid=%lu acad_exe=%d zoomed=%d geometry_occluded=%d "
                "frame=%ld,%ld,%ld,%ld work=%ld,%ld,%ld,%ld valid=%d\n", foreground, pid, expected_pid,
                is_acad ? 1 : 0, IsZoomed(foreground) ? 1 : 0, covers_desktop ? 1 : 0,
                frame.left, frame.top, frame.right, frame.bottom, monitor.rcWork.left, monitor.rcWork.top,
                monitor.rcWork.right, monitor.rcWork.bottom, valid ? 1 : 0);
        }
        return valid;
    }

    static int RunForegroundAutoCad(const std::wstring& clip, const std::wstring& output, DWORD expected_pid) {
        DWORD observed_pid = 0;
        if (!CheckAutoCadForeground(expected_pid, &observed_pid, true)) {
            Verify(false, "actual foreground must be acad.exe with requested PID and production geometry coverage");
            return 4;
        }
        expected_pid = observed_pid;
        constexpr uint32_t width = 3840, height = 2160;
        DesktopHostSession host({}, 0);
        host.state_.settings.pause_on_fullscreen_or_battery = true;
        host.state_.settings.glass_live_rate = 2;
        host.state_.settings.wallpaper_preset = model::WallpaperPreset::Bloom;
        host.state_.settings.tiles_enabled = true;
        host.state_.settings.organize_enabled = true;
        host.state_.settings.dock_enabled = false;
        host.interaction_.banner_suppressed = true;
        host.last_activity_tick_ = GetTickCount64();
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = WindowProc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = L"PulseVideoPauseRealForegroundHiddenTest";
        RegisterClassW(&window_class);
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
            window_class.lpszClassName, L"hidden actual-foreground host validation", WS_POPUP,
            0, 0, width, height, nullptr, nullptr, window_class.hInstance, &host);
        if (!hwnd || !host.renderer_.InitializeForHwnd(hwnd, width, height, true, false)) return 3;
        host.renderer_.SetSurfaceGeometry(96, {0, 0, width, height}, {0, 0, width, height});
        unsigned live_notices = 0, frozen_notices = 0, foreground_samples = 0;
        video::VideoWallpaperControllerCallbacks callbacks;
        callbacks.on_live_glass_updated = [&](bool frozen) { if (frozen) ++frozen_notices; else ++live_notices; };
        host.video_ctrl_.Initialize(hwnd, kMsgVideoWallpaper, &host.renderer_, std::move(callbacks));
        video::VideoWallpaperPersistedState persisted;
        persisted.active = persisted.optimized = true;
        persisted.source_path = persisted.cached_video_path = clip;
        persisted.source_width = persisted.target_width = width;
        persisted.source_height = persisted.target_height = height;
        host.state_.video_wallpaper = persisted;
        host.video_ctrl_.RestorePersisted(persisted, true, width, height, 2);
        host.video_ctrl_.SetGlassConsumers(true, true);
        auto cleanup = [&] {
            KillTimer(hwnd, kTimerHeartbeat);
            host.video_ctrl_.Shutdown();
            host.renderer_.Shutdown();
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            DestroyWindow(hwnd);
            UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
        };
        auto foreground_pump = [&](DWORD milliseconds) {
            ++foreground_samples;
            if (!CheckAutoCadForeground(expected_pid)) {
                Verify(false, "AutoCAD PID and actual geometry remain foreground throughout real pause measurement");
                return false;
            }
            Pump(host, milliseconds);
            return true;
        };
        Verify(SetTimer(hwnd, kTimerHeartbeat, 1000, nullptr) != 0, "foreground mode starts real 1 Hz host heartbeat");
        const uint64_t ready_deadline = GetTickCount64() + 15000;
        while (GetTickCount64() < ready_deadline) {
            if (!foreground_pump(50)) { cleanup(); return 1; }
            const auto current = ControllerPeer::Stats(host.video_ctrl_);
            if (current.open && current.paused && current.frames_shown > 0 && host.renderer_.ActivitySuspended()) break;
        }
        const auto held = ControllerPeer::Stats(host.video_ctrl_);
        const uint64_t paused_since = ControllerPeer::PausedSince(host.video_ctrl_);
        const bool paused = held.open && held.paused && held.frames_shown > 0 && paused_since != 0 &&
                            host.renderer_.ActivitySuspended();
        Verify(paused && !host.session_locked_ && !host.display_off_,
               "real AutoCAD foreground drives UpdateDesktopActivity and controller/player pause without injected signals");
        Verify(!IsWindowVisible(hwnd) && host.mount_.Hwnd() == nullptr,
               "real foreground test keeps hidden host unmounted and never changes desktop icons or focus");
        if (!paused) { cleanup(); return 1; }
        const auto hot_memory = ReadMemory(host.renderer_.D3DDevice(), "actual_foreground_hot_paused");
        const auto presents = host.renderer_.Metrics().total_presents;
        const auto imports = host.renderer_.LiveGlassStats().frames_imported;
        const auto notices = live_notices;
        const auto cpu_start = ProcessCpu100ns();
        const auto wall_start = GetTickCount64();
        while (GetTickCount64() < paused_since + 29000) {
            if (!foreground_pump(50)) { cleanup(); return 1; }
        }
        const auto at_29_seconds = ControllerPeer::Stats(host.video_ctrl_);
        Verify(!at_29_seconds.frozen_released && at_29_seconds.frames_shown == held.frames_shown &&
               at_29_seconds.position_100ns == held.position_100ns,
               "real foreground pause holds player frames/media clock for 29 seconds without early freeze");
        Verify(host.renderer_.Metrics().total_presents == presents &&
               host.renderer_.LiveGlassStats().frames_imported == imports && live_notices == notices,
               "real foreground host remains paint/live-import idle throughout hot pause");
        while (GetTickCount64() < paused_since + 35000 &&
               (!ControllerPeer::Stats(host.video_ctrl_).frozen_released || frozen_notices == 0)) {
            if (!foreground_pump(50)) { cleanup(); return 1; }
        }
        const uint64_t frozen_at = GetTickCount64();
        const auto frozen = ControllerPeer::Stats(host.video_ctrl_);
        Verify(frozen.frozen_released && frozen_notices == 1 && frozen_at >= paused_since + 30000 &&
               frozen_at < paused_since + 35000 && ControllerPeer::PausedSince(host.video_ctrl_) == paused_since,
               "real AutoCAD foreground persists through automatic production 30-second heartbeat freeze");
        Verify(frozen.frames_shown == held.frames_shown && frozen.position_100ns == held.position_100ns &&
               frozen.glass_width == 0 && frozen.glass_height == 0 &&
               !RendererPeer::HasVideoSwap(host.renderer_) && RendererPeer::HasFrozenSurface(host.renderer_) &&
               host.suspended_resources_trimmed_,
               "real-foreground freeze releases decoder shared textures/swap while preserving the stopped frame");
        const auto frozen_memory = ReadMemory(host.renderer_.D3DDevice(), "actual_foreground_frozen");
        const double wall_ms = static_cast<double>(GetTickCount64() - wall_start);
        const DWORD processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        const double cpu = static_cast<double>(ProcessCpu100ns() - cpu_start) / 10000.0 / wall_ms * 100.0 /
                           std::max<DWORD>(processors, 1);
        std::printf("REAL_FOREGROUND_RESULT pid=%lu samples=%u elapsed_to_freeze_ms=%llu heartbeats=%u "
            "frames_before=%llu frames_after=%llu clock_before=%lld clock_after=%lld presents_before=%llu "
            "presents_after=%llu live_notices_before=%u live_notices_after=%u frozen_notices=%u "
            "paused_cpu_percent_machine=%.4f duration_ms=%.0f processors=%lu\n",
            expected_pid, foreground_samples, frozen_at - paused_since, host.heartbeat_tick_,
            held.frames_shown, frozen.frames_shown, held.position_100ns, frozen.position_100ns,
            presents, host.renderer_.Metrics().total_presents, notices, live_notices, frozen_notices, cpu, wall_ms, processors);
        std::printf("REAL_FOREGROUND_RELEASE gpu_local_delta=%lld private_commit_delta=%lld working_set_delta=%lld\n",
            static_cast<long long>(frozen_memory.gpu_local) - static_cast<long long>(hot_memory.gpu_local),
            static_cast<long long>(frozen_memory.private_commit) - static_cast<long long>(hot_memory.private_commit),
            static_cast<long long>(frozen_memory.working_set) - static_cast<long long>(hot_memory.working_set));
        Verify(CheckAutoCadForeground(expected_pid, nullptr, true), "AutoCAD remains actual foreground at measurement end");
        std::printf("REAL_FOREGROUND_FIXTURE clip=%ls output=%ls auto_pause=1 injected_WTS_power=0 foreground_changes=0\n",
                    clip.c_str(), output.c_str());
        cleanup();
        return failures ? 1 : 0;
    }

    static int Run(const std::wstring& clip, const std::wstring& output) {
        constexpr uint32_t width = 3840, height = 2160;
        DesktopHostSession host({}, 0);
        // Do not run DesktopHostSession::Run: mounting Explorer, desktop icon
        // takeover, shell hooks and persisted preferences are not part of this fixture.
        host.state_.settings.pause_on_fullscreen_or_battery = false;
        host.state_.settings.glass_live_rate = 2;
        host.state_.settings.wallpaper_preset = model::WallpaperPreset::Bloom;
        host.state_.settings.tiles_enabled = true;
        host.state_.settings.organize_enabled = true;
        host.state_.settings.dock_enabled = false;
        host.interaction_.banner_suppressed = true;
        host.last_activity_tick_ = GetTickCount64();
        model::DesktopTile tile;
        tile.id = "pause-host-glass";
        tile.kind = model::TileKind::Clock;
        tile.title = L"Host pause fixture";
        tile.free_placed = true;
        tile.x = 40; tile.y = 40; tile.w = 400; tile.h = 240;
        tile.size = {3, 2};
        tile.material = model::CardMaterial::LiquidGlass;
        host.state_.tiles.push_back(tile);

        WNDCLASSW window_class{};
        window_class.lpfnWndProc = WindowProc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = L"PulseVideoPauseHostHiddenTest";
        RegisterClassW(&window_class);
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
            window_class.lpszClassName, L"isolated host pause integration", WS_POPUP, 0, 0, width, height,
            nullptr, nullptr, window_class.hInstance, &host);
        if (!hwnd || !host.renderer_.InitializeForHwnd(hwnd, width, height, true, false)) return 3;
        host.renderer_.SetSurfaceGeometry(96, {0, 0, width, height}, {0, 0, width, height});
        unsigned live_notices = 0, frozen_notices = 0;
        video::VideoWallpaperControllerCallbacks callbacks;
        callbacks.on_live_glass_updated = [&](bool frozen) { if (frozen) ++frozen_notices; else ++live_notices; };
        // Keep fixture persistence in memory. The controller/player callbacks
        // and Win32 notification/drain path remain the production implementation.
        host.video_ctrl_.Initialize(hwnd, kMsgVideoWallpaper, &host.renderer_, std::move(callbacks));
        video::VideoWallpaperPersistedState persisted;
        persisted.active = true;
        persisted.optimized = true;
        persisted.source_path = clip;
        persisted.cached_video_path = clip;
        persisted.source_width = persisted.target_width = width;
        persisted.source_height = persisted.target_height = height;
        host.state_.video_wallpaper = persisted;
        host.video_ctrl_.RestorePersisted(persisted, true, width, height, 2);
        host.video_ctrl_.SetGlassConsumers(true, true);
        Verify(SetTimer(hwnd, kTimerHeartbeat, 1000, nullptr) != 0, "real 1000 ms host heartbeat timer starts");
        const bool playing = WaitFor(host, [&] {
            return ControllerPeer::Stats(host.video_ctrl_).frames_shown >= 10 && live_notices > 0 &&
                   host.renderer_.Metrics().total_presents > 0;
        }, 10000);
        Verify(playing && RendererPeer::HasVideoSwap(host.renderer_),
               "RestorePersisted -> controller -> player -> window notification -> renderer produces frames");
        Verify(!IsWindowVisible(hwnd) && host.mount_.Hwnd() == nullptr,
               "fixture remains hidden and never mounts or takes over desktop icons");
        auto stats = ControllerPeer::Stats(host.video_ctrl_);
        std::printf("PLAYING frames=%llu position_100ns=%lld glass_imports=%llu presents=%llu notice=%u\n",
            stats.frames_shown, stats.position_100ns, host.renderer_.LiveGlassStats().frames_imported,
            host.renderer_.Metrics().total_presents, live_notices);
        const auto playing_memory = ReadMemory(host.renderer_.D3DDevice(), "playing");
        if (!playing) {
            std::wprintf(L"PLAYER_ERROR %s\n", stats.last_error.c_str());
            KillTimer(hwnd, kTimerHeartbeat);
            host.video_ctrl_.Shutdown(); host.renderer_.Shutdown();
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); DestroyWindow(hwnd);
            return 1;
        }

        SendMessageW(hwnd, WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 0);
        Verify(host.renderer_.ActivitySuspended(), "WM_WTSSESSION_CHANGE immediately suspends host renderer activity");
        Verify(WaitFor(host, [&] { return ControllerPeer::Stats(host.video_ctrl_).paused; }, 3000),
               "real heartbeat routes session pause through controller to player");
        const uint64_t paused_since = ControllerPeer::PausedSince(host.video_ctrl_);
        const auto held_stats = ControllerPeer::Stats(host.video_ctrl_);
        const auto held_presents = host.renderer_.Metrics().total_presents;
        const auto held_imports = host.renderer_.LiveGlassStats().frames_imported;
        const auto held_notices = live_notices;
        const auto paused_cpu = ProcessCpu100ns();
        const auto paused_wall = GetTickCount64();
        DisplayState(hwnd, 0);
        SendMessageW(hwnd, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
        Pump(host, 1200);
        Verify(host.display_off_ && !host.session_locked_ && host.renderer_.ActivitySuspended() &&
               ControllerPeer::Stats(host.video_ctrl_).paused && ControllerPeer::PausedSince(host.video_ctrl_) == paused_since,
               "unlock while display remains off does not resume or reset 30-second freeze deadline");
        // Exercise the production animation message entry too; it must not
        // bypass the host's activity gate while timer-driven pause is active.
        SendMessageW(hwnd, kMsgAnimFrame, 0, 0);
        while (GetTickCount64() < paused_since + 29000) Pump(host, 50);
        Verify(!ControllerPeer::Stats(host.video_ctrl_).frozen_released,
               "real wall-clock 29 seconds remains hot-paused, not prematurely frozen");
        Verify(host.renderer_.Metrics().total_presents == held_presents &&
               host.renderer_.LiveGlassStats().frames_imported == held_imports && live_notices == held_notices,
               "renderer Tick, animation message and heartbeat produce zero paints/live imports while paused");
        const auto still = ControllerPeer::Stats(host.video_ctrl_);
        Verify(still.frames_shown == held_stats.frames_shown && still.position_100ns == held_stats.position_100ns,
               "host-triggered hot pause holds actual player frame count and media clock");
        Verify(WaitFor(host, [&] {
            return ControllerPeer::Stats(host.video_ctrl_).frozen_released && frozen_notices > 0;
        }, 5000), "actual 30-second host heartbeat freezes/releases player and binds controller frozen callback");
        const uint64_t frozen_at = GetTickCount64();
        Verify(frozen_at >= paused_since + 30000 && frozen_at < paused_since + 34000,
               "freeze transition uses unchanged production GetTickCount64 30-second deadline");
        Pump(host, 500);
        const auto frozen = ControllerPeer::Stats(host.video_ctrl_);
        Verify(frozen.glass_width == 0 && frozen.glass_height == 0 && !RendererPeer::HasVideoSwap(host.renderer_) &&
               RendererPeer::HasFrozenSurface(host.renderer_) && host.suspended_resources_trimmed_,
               "decoder shared textures and bound swap are released; frozen surface and host trim remain");
        const auto frozen_memory = ReadMemory(host.renderer_.D3DDevice(), "frozen");
        std::printf("RELEASE gpu_local_delta=%lld private_commit_delta=%lld working_set_delta=%lld\n",
            static_cast<long long>(frozen_memory.gpu_local) - static_cast<long long>(playing_memory.gpu_local),
            static_cast<long long>(frozen_memory.private_commit) - static_cast<long long>(playing_memory.private_commit),
            static_cast<long long>(frozen_memory.working_set) - static_cast<long long>(playing_memory.working_set));
        const double wall_ms = static_cast<double>(GetTickCount64() - paused_wall);
        const DWORD processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        const double cpu_percent = static_cast<double>(ProcessCpu100ns() - paused_cpu) / 10000.0 / wall_ms * 100.0 /
                                   std::max<DWORD>(processors, 1);
        std::printf("PAUSED_CPU duration_ms=%.0f cpu_percent_machine=%.4f logical_processors=%lu\n", wall_ms, cpu_percent, processors);
        std::printf("FREEZE elapsed_ms=%llu heartbeats=%u frozen_notices=%u player_frames=%llu position_100ns=%lld\n",
            frozen_at - paused_since, host.heartbeat_tick_, frozen_notices, frozen.frames_shown, frozen.position_100ns);
        const auto frozen_presents = host.renderer_.Metrics().total_presents;
        const auto frozen_imports = host.renderer_.LiveGlassStats().frames_imported;
        Pump(host, 1200);
        Verify(host.renderer_.Metrics().total_presents == frozen_presents &&
               host.renderer_.LiveGlassStats().frames_imported == frozen_imports,
               "after one frozen snapshot, suspended host remains paint/import idle");

        DisplayState(hwnd, 1);
        Verify(!host.renderer_.ActivitySuspended(), "WM_POWERBROADCAST display-on resumes host renderer gate");
        Verify(WaitFor(host, [&] {
            const auto resumed = ControllerPeer::Stats(host.video_ctrl_);
            return resumed.playing && !resumed.frozen_released && resumed.frames_shown > frozen.frames_shown &&
                   live_notices > held_notices && RendererPeer::HasVideoSwap(host.renderer_);
        }, 10000), "heartbeat automatically reopens frozen player and controller imports fresh frames after display-on");
        Pump(host, 400);
        Verify(host.renderer_.Metrics().total_presents > frozen_presents,
               "resumed host paints again after prior suspended no-paint interval");
        ReadMemory(host.renderer_.D3DDevice(), "resumed");
        const auto resumed = ControllerPeer::Stats(host.video_ctrl_);
        std::printf("RESUMED frames=%llu position_100ns=%lld imports=%llu presents=%llu\n",
            resumed.frames_shown, resumed.position_100ns, host.renderer_.LiveGlassStats().frames_imported,
            host.renderer_.Metrics().total_presents);
        std::printf("FIXTURE cache=%ls output=%ls auto_fullscreen_setting=0 signal=synthetic-session-display no_AutoCAD_claim=1\n",
                    host.video_ctrl_.BuildUiState().active_source_path.c_str(), output.c_str());
        KillTimer(hwnd, kTimerHeartbeat);
        host.video_ctrl_.Shutdown();
        host.renderer_.Shutdown();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
        UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
        return failures ? 1 : 0;
    }
};
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const bool actual_foreground = (argc == 4 || argc == 5) && std::wcscmp(argv[3], L"--foreground-autocad") == 0;
    if ((argc != 3 && !actual_foreground) || !std::filesystem::is_regular_file(argv[1])) return 2;
    DWORD expected_pid = 0;
    if (argc == 5) {
        wchar_t* end = nullptr;
        expected_pid = std::wcstoul(argv[4], &end, 10);
        if (expected_pid == 0 || !end || *end != L'\0') return 2;
    }
    const auto output = std::filesystem::absolute(argv[2]);
    std::filesystem::create_directories(output / L"cache");
    SetEnvironmentVariableW(L"PULSE_WALLPAPER_CACHE_DIR", (output / L"cache").c_str());
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int result = actual_foreground
        ? DesktopVideoPauseHostTestPeer::RunForegroundAutoCad(std::filesystem::absolute(argv[1]).wstring(), output.wstring(), expected_pid)
        : DesktopVideoPauseHostTestPeer::Run(std::filesystem::absolute(argv[1]).wstring(), output.wstring());
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
