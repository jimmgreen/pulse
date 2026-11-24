// host_main.cpp - Out-of-process Desktop Enhancement Host (pulse_desktop_host.exe)
// Modes:
//   --probe-hierarchy               Read-only desktop HWND & Shell COM snapshot JSON probe
//   --benchmark-4k                  Hardware 3840x2160 D3D11/D2D/WIC memory (<25 MB) & 0-Present idle benchmark
//   --render-snapshots <out_dir>    Headless 1920x1080 PNG exporter for all 8 1:1 mockup states
//   --pipe <name> --parent-pid <n>  Interactive desktop takeover session supervised by pulse_desktop.dll
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <atomic>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <imm.h>
#include <objbase.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shldisp.h>
#include <exdisp.h>
#include <tlhelp32.h>
#include <wtsapi32.h>

#pragma comment(lib, "wtsapi32.lib")

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include "desktop_takeover_record.h"
#include "host/ai/desktop_ai_organizer.h"
#include "host/desktop_mount.h"
#include "host/live_wallpaper_watcher.h"
#include "host/live/widget_data.h"
#include "host/model/desktop_model.h"
#include "host/group_interaction.h"
#include "host/preview_request.h"
#include "host/hover_preview_session.h"
#include "../../../src/common/localization.h"
#include "../../../src/common/config_json.h"
#include "host/model/drag_damage.h"
#include "host/model/tile_drag_session.h"
#include "host/overlays/desktop_overlays.h"
#include "host/overlays/ole_drag_drop.h"
#include "host/render/desktop_renderer.h"
#include "host/render/desktop_appearance.h"
#include "ipc/assistant_availability.h"
#include "ipc/desktop_ipc.h"
#include "tests/hosted_edit_checks.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <malloc.h>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// A host started from a console (development) unmounts on Ctrl+C or console close instead of dying with
// Explorer's icons still hidden; Pulse-launched hosts have no console and shut down over the pipe.
std::atomic<HWND> g_console_close_target{nullptr};

BOOL WINAPI OnConsoleCtrl(DWORD type) noexcept {
    const HWND target = g_console_close_target.load();
    if (!target || !IsWindow(target)) return FALSE;
    PostMessageW(target, WM_CLOSE, 0, 0);
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) Sleep(3000);
    return TRUE;
}

namespace l10n = pulse::l10n;
namespace ai = pulse::desktop::ai;
namespace host = pulse::desktop::host;
namespace ipc = pulse::desktop::ipc;
namespace model = pulse::desktop::model;
namespace mount = pulse::desktop::mount;
namespace render = pulse::desktop::render;
namespace live = pulse::desktop::live;
namespace overlays = pulse::desktop::overlays;

constexpr UINT kMsgDrainIpc = WM_APP + 41;
constexpr UINT kMsgFolderChanged = WM_APP + 42;
constexpr UINT kMsgAiPlanReady = WM_APP + 43;
constexpr UINT kMsgIconsReady = WM_APP + 44;
constexpr UINT kMsgWeatherReady = WM_APP + 46;   // lParam: live::WeatherResult* from the fetch thread
constexpr UINT kMsgLiveWallpaperProbe = WM_APP + 47;
constexpr UINT kMsgLiveGlassFrame = WM_APP + 48;
constexpr UINT_PTR kTimerGroupInteraction = 109;
constexpr UINT_PTR kTimerHoverGlass = 110;
constexpr UINT_PTR kTimerHeartbeat = 101;
constexpr UINT_PTR kTimerLiveTiles = 102;
constexpr UINT_PTR kTimerPhotos = 106;     // the 照片 slideshow, at the tile's own interval
constexpr UINT_PTR kTimerTileReorder = 108; // only while considering a stack drop
constexpr UINT_PTR kTimerTilePress = 107;  // a pressed card sinking in or springing back
constexpr UINT_PTR kTimerLongPress = 103;    // a held tile enters 编辑磁贴区
constexpr UINT_PTR kTimerEditJiggle = 104;   // the jiggle while editing (stops with edit mode)
constexpr UINT_PTR kTimerWeatherCity = 105;  // the city is still being typed in settings: fetch when it settles

int64_t NowUnixMs() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

struct ProcessMemorySnapshot {
    uint64_t private_working_set_bytes = 0;
    uint64_t working_set_bytes = 0;
    uint64_t private_commit_bytes = 0;
};

ProcessMemorySnapshot QueryCurrentProcessMemory() noexcept {
    ProcessMemorySnapshot out{};
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        out.working_set_bytes = pmc.WorkingSetSize;
        out.private_commit_bytes = pmc.PrivateUsage;
    }

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uint64_t page_size = si.dwPageSize > 0 ? si.dwPageSize : 4096u;

    std::vector<ULONG_PTR> ws_buf(16384);
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (QueryWorkingSet(GetCurrentProcess(), ws_buf.data(),
                            static_cast<DWORD>(ws_buf.size() * sizeof(ULONG_PTR)))) {
            const ULONG_PTR count = ws_buf[0];
            uint64_t priv_pages = 0;
            for (ULONG_PTR i = 1; i <= count && i < ws_buf.size(); ++i) {
                const ULONG_PTR flags = ws_buf[i] & 0xFFFu;
                const bool shared = (flags & 0x100u) != 0; // PSAPI_WORKING_SET_BLOCK::Shared bit 8
                if (!shared) ++priv_pages;
            }
            out.private_working_set_bytes = priv_pages * page_size;
            break;
        }
        if (GetLastError() == ERROR_BAD_LENGTH && ws_buf[0] > 0) {
            ws_buf.resize(static_cast<size_t>(ws_buf[0]) + 1024);
        } else {
            break;
        }
    }
    if (out.private_working_set_bytes == 0) {
        out.private_working_set_bytes = out.working_set_bytes;
    }
    return out;
}

bool SaveTextFileAtomic(const std::wstring& file_path, std::string_view utf8) {
    if (file_path.empty()) return false;
    std::error_code ec;
    const auto parent = std::filesystem::path(file_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    const std::wstring tmp = file_path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = utf8.empty() || WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || written != utf8.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), file_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::string LoadTextFile(const std::wstring& file_path) {
    std::ifstream ifs(std::filesystem::path(file_path), std::ios::binary);
    if (!ifs) return {};
    return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

bool CopyWideTextToClipboard(HWND owner, const std::wstring& text) noexcept {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) {
        CloseClipboard();
        return false;
    }
    void* ptr = GlobalLock(mem);
    if (ptr) {
        std::memcpy(ptr, text.c_str(), bytes);
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    } else {
        GlobalFree(mem);
    }
    CloseClipboard();
    return true;
}

std::vector<overlays::ColorTagOption> DefaultTagPalette() {
    return {
        {L"red",    L"红", 0xFFE5484Du},
        {L"orange", L"橙", 0xFFF5A524u},
        {L"green",  L"绿", 0xFF30A46Cu},
        {L"yellow", L"黄", 0xFFF2C94Cu},
        {L"blue",   L"蓝", 0xFF3B82F6u},
        {L"purple", L"紫", 0xFF8B5CF6u},
        {L"gray",   L"灰", 0xFF8E96A4u},
    };
}

// ---------------------------------------------------------------------------
// Mode 1: --probe-hierarchy (Strictly Read-Only!)
// ---------------------------------------------------------------------------
int RunProbeHierarchyMode() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const mount::DesktopHierarchyInfo h = mount::ProbeDesktopHierarchy();
    mount::NativeIconSnapshot snap{};
    HRESULT snap_hr = E_FAIL;
    const bool snap_ok = mount::CaptureNativeIconSnapshot(snap, &snap_hr);

    std::printf(
        "{\n"
        "  \"mode\": \"%s\",\n"
        "  \"progman\": \"0x%llX\",\n"
        "  \"shelldll_defview\": \"0x%llX\",\n"
        "  \"sys_listview32\": \"0x%llX\",\n"
        "  \"explorer_pid\": %lu,\n"
        "  \"listview_visible\": %s,\n"
        "  \"virtual_bounds\": {\"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d},\n"
        "  \"snapshot_ok\": %s,\n"
        "  \"snapshot_hr\": \"0x%08lX\",\n"
        "  \"monitor_count\": %zu,\n"
        "  \"native_icon_count\": %zu\n"
        "}\n",
        mount::HierarchyModeName(h.mode),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h.progman)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h.shelldll_defview)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h.sys_listview32)),
        static_cast<unsigned long>(h.explorer_pid),
        h.listview_visible ? "true" : "false",
        h.virtual_bounds.x, h.virtual_bounds.y, h.virtual_bounds.w, h.virtual_bounds.h,
        snap_ok ? "true" : "false",
        static_cast<unsigned long>(snap_hr),
        snap.monitors.size(),
        snap.items.size());

    CoUninitialize();
    return (h.mode != mount::DesktopHierarchyMode::NotFound && snap_ok) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Mode 2: --benchmark-4k (3840x2160 Hardware D3D11/D2D <25 MB + 0-Present Idle)
// ---------------------------------------------------------------------------
int RunBenchmark4KMode() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    bool pass = false;
    {
        render::DesktopRenderer renderer;
        if (!renderer.InitializeOffscreen(3840, 2160)) {
            std::fprintf(stderr, "{\"error\":\"InitializeOffscreen(3840,2160) failed\"}\n");
            CoUninitialize();
            return 2;
        }
        renderer.Backdrop().SetUseVectorIconsOnly(true);

        model::DesktopModelState state{};
        const model::RectI work_area{0, 0, 3840, 2112};
        state.monitors.push_back({L"\\\\.\\DISPLAY1", {0, 0, 3840, 2160}, work_area, 144, true});
        state.groups = model::MakeDefaultGroups(work_area);
        state.tiles = model::MakeDefaultTiles();

        const int64_t now_ms = NowUnixMs();
        for (int i = 0; i < 24; ++i) {
            model::DesktopItem it{};
            it.path = L"C:\\Users\\Public\\Desktop\\BenchItem_" + std::to_wstring(i) + L".docx";
            it.name = L"基准文档_" + std::to_wstring(i) + L".docx";
            it.extension = L".docx";
            it.size_bytes = 4096u * static_cast<uint64_t>(i + 1);
            it.modified_unix_ms = now_ms - i * 3600000ll;
            if (i < 12 && !state.groups.empty()) {
                it.pinned_group_id = state.groups[static_cast<size_t>(i) % state.groups.size()].id;
            } else {
                it.force_loose = true;
            }
            state.items.push_back(std::move(it));
        }

        const model::RectI board_rect = model::ComputeTileBoardRect(work_area, true);
        const auto group_views = model::ComputeGroupViews(state, now_ms);
        model::SettleLooseIcons(state.items, group_views, work_area, board_rect,
                                state.settings.icon_size,
                                state.settings.organize_enabled,
                                state.settings.align_to_grid,
                                state.settings.loose_sort_by,
                                state.settings.loose_sort_ascending);

        render::LiveTileRuntimeData live{};
        live.drives.push_back(mount::QueryDriveSpaceStats(L"C:\\"));
        live.recycle_bin = {437, 128ull * 1024ull * 1024ull};
        render::DesktopInteractionState inter{};
        inter.cleanable_installer_bytes = 753ull * 1024ull * 1024ull;

        // 5 warm full frames
        double warm_ms_sum = 0.0;
        for (int i = 0; i < 5; ++i) {
            renderer.InvalidateAll();
            renderer.Tick(state, group_views, board_rect, live, inter);
            warm_ms_sum += renderer.Metrics().last_draw_ms;
        }

        // 1 dirty-rect single-card update
        if (!group_views.empty()) {
            renderer.InvalidateRect(group_views[0].frame);
            renderer.Tick(state, group_views, board_rect, live, inter);
        }
        const double dirty_draw_ms = renderer.Metrics().last_draw_ms;
        const uint64_t presents_before_idle = renderer.Metrics().total_presents;

        // 10 idle ticks (MUST perform 0 BeginDraw and 0 Present1!)
        for (int i = 0; i < 10; ++i) {
            renderer.Tick(state, group_views, board_rect, live, inter);
        }
        const uint64_t idle_presents = renderer.Metrics().total_presents - presents_before_idle;

        renderer.TrimDxgiMemory();
        const ProcessMemorySnapshot mem = QueryCurrentProcessMemory();
        const double priv_ws_mb = static_cast<double>(mem.private_working_set_bytes) / (1024.0 * 1024.0);
        const double total_ws_mb = static_cast<double>(mem.working_set_bytes) / (1024.0 * 1024.0);
        const double commit_mb = static_cast<double>(mem.private_commit_bytes) / (1024.0 * 1024.0);

        // LiquidGlass 4K pass: verify sharp source + frost + pass pool stay bounded & 0-Present idle
        state.settings.default_material = model::CardMaterial::LiquidGlass;
        model::ApplyGlassPreset(static_cast<uint8_t>(model::GlassPreset::ClearDroplet), state.settings);
        double lg_warm_ms_sum = 0.0;
        for (int i = 0; i < 5; ++i) {
            renderer.InvalidateAll();
            renderer.Tick(state, group_views, board_rect, live, inter);
            lg_warm_ms_sum += renderer.Metrics().last_draw_ms;
        }
        const uint64_t lg_presents_before_idle = renderer.Metrics().total_presents;
        for (int i = 0; i < 10; ++i) {
            renderer.Tick(state, group_views, board_rect, live, inter);
        }
        const uint64_t lg_idle_presents = renderer.Metrics().total_presents - lg_presents_before_idle;
        renderer.TrimDxgiMemory();
        const ProcessMemorySnapshot lg_mem = QueryCurrentProcessMemory();
        const double lg_priv_ws_mb = static_cast<double>(lg_mem.private_working_set_bytes) / (1024.0 * 1024.0);

        std::printf(
            "{\n"
            "  \"resolution\": \"3840x2160\",\n"
            "  \"quarter_backdrop\": \"%ux%u\",\n"
            "  \"vibrant_accent_argb\": \"0x%08X\",\n"
            "  \"private_working_set_mb\": %.2f,\n"
            "  \"working_set_mb\": %.2f,\n"
            "  \"private_commit_mb\": %.2f,\n"
            "  \"lru_cache_bytes\": %zu,\n"
            "  \"avg_warm_frame_ms\": %.3f,\n"
            "  \"dirty_card_frame_ms\": %.3f,\n"
            "  \"dirty_presents\": %llu,\n"
            "  \"idle_presents\": %llu,\n"
            "  \"idle_ticks_skipped\": %llu,\n"
            "  \"liquid_glass_private_working_set_mb\": %.2f,\n"
            "  \"liquid_glass_warm_frame_ms\": %.3f,\n"
            "  \"liquid_glass_idle_presents\": %llu\n"
            "}\n",
            renderer.Backdrop().QuarterWidth(),
            renderer.Backdrop().QuarterHeight(),
            renderer.Backdrop().VibrantAccentArgb(),
            priv_ws_mb,
            total_ws_mb,
            commit_mb,
            renderer.Backdrop().CachedBytes(),
            warm_ms_sum / 5.0,
            dirty_draw_ms,
            static_cast<unsigned long long>(renderer.Metrics().dirty_presents),
            static_cast<unsigned long long>(idle_presents),
            static_cast<unsigned long long>(renderer.Metrics().idle_ticks_skipped),
            lg_priv_ws_mb,
            lg_warm_ms_sum / 5.0,
            static_cast<unsigned long long>(lg_idle_presents));
        std::fflush(stdout);

        pass = (mem.private_working_set_bytes < 25ull * 1024ull * 1024ull) &&
               (lg_mem.private_working_set_bytes < 25ull * 1024ull * 1024ull) &&
               (idle_presents == 0) && (lg_idle_presents == 0);
        renderer.Shutdown();
    }

    CoUninitialize();
    return pass ? 0 : 1;
}

uint32_t CountCurrentProcessThreads() noexcept {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    const DWORD pid = GetCurrentProcessId();
    uint32_t count = 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) ++count;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return count;
}

int RunBenchmarkLiveMemoryMode() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PulseDesktopLiveMemProbe";
        RegisterClassW(&wc);

        const uint32_t sw = static_cast<uint32_t>(std::max(64, GetSystemMetrics(SM_CXSCREEN)));
        const uint32_t sh = static_cast<uint32_t>(std::max(64, GetSystemMetrics(SM_CYSCREEN)));
        const UINT dpi = GetDpiForSystem();
        const float dip_scale = static_cast<float>(dpi) / 96.0f;
        const int work_w = static_cast<int>(std::lround(static_cast<float>(sw) / dip_scale));
        const int work_h = std::max(200, static_cast<int>(std::lround(static_cast<float>(sh) / dip_scale)) - 48);

        HWND hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
            wc.lpszClassName, L"", WS_POPUP,
            -32000, -32000, static_cast<int>(sw), static_cast<int>(sh),
            nullptr, nullptr, wc.hInstance, nullptr);

        mount::NativeIconSnapshot snap{};
        mount::CaptureNativeIconSnapshot(snap, nullptr);

        model::DesktopModelState state{};
        const model::RectI work_area{0, 0, work_w, work_h};
        state.monitors.push_back({L"\\\\.\\DISPLAY1", {0, 0, work_w, work_h + 48}, work_area, static_cast<uint32_t>(dpi), true});
        state.settings.tiles_enabled = false;
        state.settings.dark_mode = false;
        state.groups = model::MakeDefaultGroups(work_area);
        state.items = mount::EnumerateDesktopItems(&snap);

        const int64_t now_ms = NowUnixMs();
        const auto group_views = model::ComputeGroupViews(state, now_ms);
        model::SettleLooseIcons(state.items, group_views, work_area, {},
                                state.settings.icon_size, true, true,
                                state.settings.loose_sort_by, state.settings.loose_sort_ascending);

        render::DesktopRenderer renderer;
        renderer.SetSurfaceGeometry(dpi, {0, 0, static_cast<int>(sw), static_cast<int>(sh)},
                                    {0, 0, static_cast<int>(sw), static_cast<int>(sh)});
        if (!renderer.InitializeForHwnd(hwnd, sw, sh, true)) {
            if (hwnd) DestroyWindow(hwnd);
            CoUninitialize();
            return 2;
        }

        overlays::DesktopOverlayController overlays;
        overlays.SetDipScale(dip_scale);
        overlays.Initialize({});
        overlays.SetBackdropSource(&renderer.Backdrop(), renderer.DeviceContext());

        render::LiveTileRuntimeData live{};
        render::DesktopInteractionState inter{};
        renderer.InvalidateAll();
        renderer.Tick(state, group_views, {}, live, inter);

        // Wait up to 2.2s for real async Shell icons & image thumbnails to decode, then draw them in.
        const ULONGLONG t_icons = GetTickCount64();
        while (GetTickCount64() - t_icons < 2200) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            renderer.InvalidateAll();
            renderer.Tick(state, group_views, {}, live, inter);
            Sleep(120);
        }

        // Exercise context menu + Beautify drawer + AI drawer + Toast (all sharing one thread D3D11/D2D device).
        overlays::ContextMenuSpec mspec{};
        mspec.target_kind = overlays::ContextMenuTargetKind::BlankDesktop;
        mspec.screen_anchor = POINT{200, 200};
        overlays.OpenContextMenu(mspec, state.settings);
        overlays.Dismiss();

        overlays.OpenBeautifyDrawer(RECT{0, 0, work_w, work_h}, state.settings, {});
        overlays.Dismiss();

        overlays::AiDrawerViewState ai_st{};
        ai_st.phase = overlays::AiDrawerPhase::UnconfiguredGuide;
        overlays.OpenAiOrganizeDrawer(RECT{0, 0, work_w, work_h}, ai_st);
        overlays.Dismiss();

        // Wait > 4s for async icon worker auto-retirement, then run idle trim just like heartbeat.
        Sleep(4300);
        overlays.TrimIdleResources();
        renderer.TrimDxgiMemory();
        _heapmin();

        const ProcessMemorySnapshot mem = QueryCurrentProcessMemory();
        const uint32_t threads = CountCurrentProcessThreads();
        DWORD handles = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handles);

        std::printf(
            "{\n"
            "  \"screen_px\": \"%ux%u\",\n"
            "  \"dpi\": %u,\n"
            "  \"desktop_items\": %zu,\n"
            "  \"cached_icons\": %zu,\n"
            "  \"cached_icon_kb\": %.1f,\n"
            "  \"private_working_set_mb\": %.2f,\n"
            "  \"working_set_mb\": %.2f,\n"
            "  \"private_commit_mb\": %.2f,\n"
            "  \"threads\": %u,\n"
            "  \"handles\": %lu\n"
            "}\n",
            sw, sh, static_cast<unsigned>(dpi),
            state.items.size(),
            renderer.Backdrop().CachedEntryCount(),
            static_cast<double>(renderer.Backdrop().CachedBytes()) / 1024.0,
            static_cast<double>(mem.private_working_set_bytes) / (1024.0 * 1024.0),
            static_cast<double>(mem.working_set_bytes) / (1024.0 * 1024.0),
            static_cast<double>(mem.private_commit_bytes) / (1024.0 * 1024.0),
            threads,
            static_cast<unsigned long>(handles));
        std::fflush(stdout);

        overlays.Shutdown();
        renderer.Shutdown();
        if (hwnd) DestroyWindow(hwnd);
    }
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------
// Mode 3: --render-snapshots <output_dir> (1:1 Showcase 1920x1080 PNG Exporter)
// ---------------------------------------------------------------------------
int RunRenderSnapshotsMode(const std::wstring& out_dir_str) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::error_code ec;
    const std::filesystem::path out_dir(out_dir_str);
    std::filesystem::create_directories(out_dir, ec);

    render::DesktopRenderer renderer;
    if (!renderer.InitializeOffscreen(1920, 1080)) {
        std::fprintf(stderr, "Failed to initialize offscreen 1920x1080 renderer\n");
        CoUninitialize();
        return 2;
    }
    renderer.Backdrop().SetUseVectorIconsOnly(true);

    overlays::DesktopOverlayController ov;
    ov.Initialize({});
    ov.SetBackdropSource(&renderer.Backdrop(), renderer.DeviceContext());

    const model::RectI work_area{0, 0, 1920, 1032};
    const RECT work_area_rc{0, 0, 1920, 1032};
    const int64_t now_ms = NowUnixMs();
    constexpr int64_t kHourMs = 3600ll * 1000ll;
    constexpr int64_t kDayMs = 24ll * kHourMs;

    model::DesktopModelState base_state{};
    base_state.monitors.push_back({L"\\\\.\\DISPLAY1", {0, 0, 1920, 1080}, work_area, 96, true});
    base_state.settings.dark_mode = true;
    base_state.settings.default_material = model::CardMaterial::Acrylic;
    base_state.settings.card_opacity_percent = 60;
    base_state.settings.auto_wallpaper_accent = true;
    base_state.settings.accent_argb = 0xFF4CC2FFu;
    base_state.settings.wallpaper_preset = model::WallpaperPreset::Bloom;
    base_state.settings.label_style = model::IconLabelStyle::Shadow;
    base_state.settings.icon_size = model::DesktopIconSize::Medium;
    base_state.settings.active_scene = model::DesktopScene::Work;
    base_state.settings.organize_enabled = true;
    base_state.settings.tiles_enabled = true;
    base_state.settings.live_tiles_enabled = true;

    base_state.groups = model::MakeDefaultGroups(work_area);
    // Ensure canonical 4-group layout & metadata matching desktop_pack_mockup.html
    if (base_state.groups.size() >= 4) {
        base_state.groups[0].title = L"进行中";
        base_state.groups[0].icon_glyph = L'\xE8A5';
        base_state.groups[0].rule_summary = L"最近 7 天改过的文档";
        base_state.groups[0].focus_visible = true;
        base_state.groups[0].cols = 4;
        base_state.groups[0].visible_rows = 2;
        base_state.groups[0].x = 24;
        base_state.groups[0].y = 24;

        base_state.groups[1].title = L"安装包";
        base_state.groups[1].icon_glyph = L'\xE7B8';
        base_state.groups[1].rule_summary = L".exe · .msi";
        base_state.groups[1].cols = 4;
        base_state.groups[1].visible_rows = 1;
        base_state.groups[1].x = 424;
        base_state.groups[1].y = 24;

        base_state.groups[2].title = L"截图与图片";
        base_state.groups[2].icon_glyph = L'\xEB9F';
        base_state.groups[2].rule_summary = L".png · .jpg · .webp";
        base_state.groups[2].cols = 4;
        base_state.groups[2].visible_rows = 2;
        base_state.groups[2].x = 24;
        base_state.groups[2].y = 296;

        base_state.groups[3].title = L"未归类";
        base_state.groups[3].icon_glyph = L'\xE8B7';
        base_state.groups[3].rule_summary = L"其余桌面文件";
        base_state.groups[3].cols = 4;
        base_state.groups[3].visible_rows = 2;
        base_state.groups[3].x = 424;
        base_state.groups[3].y = 204;
    }

    const std::string gid_work = base_state.groups.size() > 0 ? base_state.groups[0].id : "work";
    const std::string gid_inst = base_state.groups.size() > 1 ? base_state.groups[1].id : "installers";
    const std::string gid_shot = base_state.groups.size() > 2 ? base_state.groups[2].id : "screenshots";
    const std::string gid_misc = base_state.groups.size() > 3 ? base_state.groups[3].id : "misc";

    auto add_item = [&](const wchar_t* name, const wchar_t* ext, bool is_folder, uint64_t bytes,
                        int64_t age_ms, const std::string& gid, const wchar_t* tag_id = L"",
                        uint32_t tag_argb = 0, bool force_loose = false) {
        model::DesktopItem it{};
        it.name = name;
        it.path = std::wstring(L"C:\\Users\\SS\\Desktop\\") + name;
        it.extension = is_folder ? std::wstring{} : std::wstring(ext);
        it.is_folder = is_folder;
        it.size_bytes = bytes;
        it.modified_unix_ms = now_ms - age_ms;
        it.pinned_group_id = gid;
        it.tag_id = tag_id;
        it.tag_argb = tag_argb;
        it.force_loose = force_loose;
        base_state.items.push_back(std::move(it));
    };

    // Group 0: 进行中 (6 items)
    add_item(L"季度汇报.pptx",    L".pptx", false, 14800000ull,  2 * kHourMs, gid_work, L"red",    0xFFE5484Du);
    add_item(L"预算表-2026.xlsx", L".xlsx", false,  1920000ull,  5 * kHourMs, gid_work, L"green",  0xFF30A46Cu);
    add_item(L"合同评审-v3.pdf",  L".pdf",  false,  4380000ull, 20 * kHourMs, gid_work, L"orange", 0xFFF5A524u);
    add_item(L"产品路线图.docx",  L".docx", false,   860000ull,  2 * kDayMs,  gid_work, L"blue",   0xFF3B82F6u);
    add_item(L"架构备忘.md",      L".md",   false,    42000ull,  3 * kDayMs,  gid_work);
    add_item(L"周会纪要.docx",    L".docx", false,   320000ull,  4 * kDayMs,  gid_work);

    // Group 1: 安装包 (4 items -> 753 MB cleanable)
    add_item(L"PulseSetup-1.0.58.exe", L".exe", false,  28ull * 1024ull * 1024ull, 12 * kDayMs, gid_inst);
    add_item(L"7z2600-x64.exe",        L".exe", false,   2ull * 1024ull * 1024ull, 40 * kDayMs, gid_inst);
    add_item(L"node-v22.11.0-x64.msi", L".msi", false,  31ull * 1024ull * 1024ull, 35 * kDayMs, gid_inst);
    add_item(L"显卡驱动-566.14.exe",    L".exe", false, 692ull * 1024ull * 1024ull, 28 * kDayMs, gid_inst);

    // Group 2: 截图与图片 (6 items)
    add_item(L"屏幕截图 2026-11-12.png", L".png",  false,  940000ull,  1 * kDayMs,  gid_shot);
    add_item(L"界面走查-暗色.png",        L".png",  false, 1240000ull,  3 * kDayMs,  gid_shot);
    add_item(L"设计稿参考.webp",          L".webp", false,  620000ull,  6 * kDayMs,  gid_shot);
    add_item(L"白板拍照.jpg",             L".jpg",  false, 3100000ull, 18 * kDayMs,  gid_shot);
    add_item(L"报错截图-已修.png",        L".png",  false,  510000ull, 22 * kDayMs,  gid_shot);
    add_item(L"图标导出预览.png",         L".png",  false,  380000ull,  5 * kDayMs,  gid_shot);

    // Group 3: 未归类 (4 items)
    add_item(L"临时归档",       L"",     true,         0ull, 10 * kDayMs, gid_misc);
    add_item(L"字体备份",       L"",     true,         0ull, 25 * kDayMs, gid_misc);
    add_item(L"ffmpeg-7.1.zip", L".zip", false, 92000000ull, 19 * kDayMs, gid_misc);
    add_item(L"todo.txt",       L".txt", false,     4096ull,  1 * kDayMs, gid_misc);

    // 2 loose items settled cleanly below groups
    add_item(L"年度述职提纲.docx", L".docx", false, 640000ull, 6 * kHourMs, "", L"purple", 0xFF8B5CF6u, true);
    add_item(L"素材包整理",        L"",      true,       0ull, 2 * kDayMs,  "", L"",       0u,          true);

    // Canonical 10-tile 5x7 board matching HTML mockup
    base_state.tiles.clear();
    auto push_tile = [&](const char* id, model::TileKind kind, const wchar_t* title,
                         int col, int row, int w, int h, const wchar_t* target = L"",
                         const wchar_t* note = L"") {
        model::DesktopTile t{};
        t.id = id;
        t.kind = kind;
        t.title = title;
        t.col = col;
        t.row = row;
        t.size = {w, h};
        t.target_path = target;
        t.note_body = note;
        t.analog_clock = true;
        t.live_content = true;
        base_state.tiles.push_back(std::move(t));
    };
    push_tile("t_photos",  model::TileKind::Photos,        L"照片",     0, 0, 4, 2);
    push_tile("t_recycle", model::TileKind::RecycleBin,    L"回收站",   4, 0, 1, 1);
    push_tile("t_thispc",  model::TileKind::ThisPC,        L"此电脑",   4, 1, 1, 1);
    push_tile("t_dl",      model::TileKind::Downloads,     L"下载",     0, 2, 2, 2);
    push_tile("t_recent",  model::TileKind::Recent,        L"最近使用", 2, 2, 2, 2);
    push_tile("t_disk",    model::TileKind::DiskRing,      L"D: 剩 140 GB", 4, 2, 1, 1, L"D:\\");
    push_tile("t_pulse",   model::TileKind::PulseLauncher, L"Pulse",    4, 3, 1, 1);
    push_tile("t_apps",    model::TileKind::Apps,          L"应用",     0, 4, 4, 2);
    push_tile("t_clock",   model::TileKind::Clock,         L"时钟",     4, 4, 1, 1);
    push_tile("t_tray",    model::TileKind::StagingTray,   L"暂存区",   0, 6, 4, 1);
    push_tile("t_note",    model::TileKind::StickyNote,    L"便笺",     4, 5, 1, 2, L"",
              L"周五前发版检查单\n· 4K 显存压测\n· 8 张快照核对");
    // The default smart stack where 最近使用 sits.
    base_state.tiles[4].stack = {{model::TileKind::Recent, L"最近使用", L"", "t_recent"},
                                 {model::TileKind::Calendar, L"日历", L"", "t_cal"},
                                 {model::TileKind::Weather, L"天气", L"", "t_weather"}};
    // App shortcuts the installers left on the desktop: the 应用 tile collects them.
    for (const wchar_t* app : {L"微信.lnk", L"QQ.lnk", L"钉钉.lnk", L"WPS Office.lnk", L"腾讯会议.lnk",
                               L"网易云音乐.lnk", L"Microsoft Edge.lnk", L"百度网盘.lnk", L"剪映.lnk", L"Steam.lnk"}) {
        model::DesktopItem it{};
        it.name = app;
        it.path = std::wstring(L"C:\\Users\\Public\\Desktop\\") + app;
        it.extension = L".lnk";
        it.is_shortcut = true;
        it.modified_unix_ms = now_ms - 20 * kDayMs;
        base_state.items.push_back(std::move(it));
    }
    base_state.todos = model::MakeDefaultTodos();
    base_state.settings.weather_city = L"北京";

    render::LiveTileRuntimeData live{};
    live.photo_total_count = 128;
    live.recycle_bin = {437, 1820ull * 1024ull * 1024ull};
    live.drives = {{L"D:\\", 140ull * 1024ull * 1024ull * 1024ull, 584ull * 1024ull * 1024ull * 1024ull}};
    live.download_items = {
        {L"C:\\Users\\SS\\Downloads\\设计评审-终版.pptx", L"设计评审-终版.pptx", L".pptx", false, 18400000ull, now_ms - 8 * 60000ll},
        {L"C:\\Users\\SS\\Downloads\\账单明细-11月.xlsx", L"账单明细-11月.xlsx", L".xlsx", false,   920000ull, now_ms - 2 * kHourMs},
        {L"C:\\Users\\SS\\Downloads\\字体授权书.pdf",     L"字体授权书.pdf",     L".pdf",  false,  2100000ull, now_ms - 6 * kHourMs},
        {L"C:\\Users\\SS\\Downloads\\组件图标集.zip",     L"组件图标集.zip",     L".zip",  false, 12400000ull, now_ms - 1 * kDayMs},
    };
    live.recent_items = {
        {L"C:\\Users\\SS\\Desktop\\季度汇报.pptx",    L"季度汇报.pptx",    L".pptx", false, 14800000ull, now_ms - 15 * 60000ll},
        {L"P:\\源代码\\desktop_renderer.cpp",         L"desktop_renderer.cpp", L".cpp", false, 98000ull, now_ms - 42 * 60000ll},
        {L"C:\\Users\\SS\\Desktop\\预算表-2026.xlsx", L"预算表-2026.xlsx", L".xlsx", false,  1920000ull, now_ms - 3 * kHourMs},
        {L"C:\\Users\\SS\\Desktop\\合同评审-v3.pdf",  L"合同评审-v3.pdf",  L".pdf",  false,  4380000ull, now_ms - 9 * kHourMs},
    };
    live.pinned_folder_items[L"P:\\源代码"] = {
        {L"P:\\源代码\\src",                  L"src",                  L"",     true,        0ull, now_ms - 10 * 60000ll},
        {L"P:\\源代码\\desktop_renderer.cpp", L"desktop_renderer.cpp", L".cpp", false,  112000ull, now_ms - 18 * 60000ll},
        {L"P:\\源代码\\desktop_overlays.cpp", L"desktop_overlays.cpp", L".cpp", false,  104000ull, now_ms - 25 * 60000ll},
        {L"P:\\源代码\\CMakeLists.txt",       L"CMakeLists.txt",       L".txt", false,    6400ull, now_ms - 2 * kHourMs},
        {L"P:\\源代码\\README.md",            L"README.md",            L".md",  false,   14200ull, now_ms - 1 * kDayMs},
    };
    live.tray_entries = {
        {1, 14800000ull, false, false, L"C:\\Users\\SS\\Desktop\\季度汇报.pptx",   L"季度汇报.pptx"},
        {2,  4380000ull, false, false, L"C:\\Users\\SS\\Desktop\\合同评审-v3.pdf", L"合同评审-v3.pdf"},
        {3,  1240000ull, false, false, L"C:\\Users\\SS\\Desktop\\界面走查-暗色.png", L"界面走查-暗色.png"},
    };
    live.tray_total_bytes = 20420000ull;
    live.weather.valid = true;
    live.weather.city = L"北京";
    live.weather.temp_c = 12; live.weather.code = 2; live.weather.hi_c = 15; live.weather.lo_c = 7;
    live.weather.hours = {{L"15时", 2, 15}, {L"16时", 0, 15}, {L"17时", 0, 14}, {L"18时", 3, 12}, {L"19时", 3, 11}};
    live.weather.days = {{L"明天", 61, 6, 11}, {L"周日", 2, 5, 13}, {L"周一", 0, 7, 16}};
    live.system = {true, 23, 10ull << 30, 16ull << 30, 76, 113ull << 30, 2500000ull};

    auto compute_views_and_settle = [&](model::DesktopModelState& st, bool drawer_open)
        -> std::pair<model::RectI, std::vector<model::GroupComputedView>> {
        const model::RectI br = model::ComputeTileBoardRect(
            work_area, st.settings.tiles_enabled && st.settings.organize_enabled && !drawer_open);
        model::UpdateAppsTileMembership(st);
        model::ResolveGroupCollisions(st.groups, std::nullopt, work_area, br, st.settings.icon_size);
        auto gvs = model::ComputeGroupViews(st, now_ms);
        model::SettleLooseIcons(st.items, gvs, work_area, br,
                                st.settings.icon_size, st.settings.organize_enabled,
                                st.settings.align_to_grid, st.settings.loose_sort_by,
                                st.settings.loose_sort_ascending);
        return {br, std::move(gvs)};
    };

    bool all_ok = true;
    auto export_png = [&](const wchar_t* filename,
                          const model::DesktopModelState& st,
                          const std::vector<model::GroupComputedView>& gvs,
                          const model::RectI& br,
                          const render::DesktopInteractionState& inter,
                          bool with_overlay) {
        const std::wstring path = (out_dir / filename).wstring();
        const bool ok = renderer.RenderCompositePng(
            path, st, gvs, br, live, inter,
            with_overlay ? std::function<void(ID2D1DeviceContext*)>([&](ID2D1DeviceContext* dc) {
                ov.CompositeOnto(dc, 0, 0);
            }) : std::function<void(ID2D1DeviceContext*)>{});
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", ipc::WideToUtf8(path).c_str());
        if (!ok) all_ok = false;
    };

    // -----------------------------------------------------------------------
    // 01_organized_work.png: Bloom wallpaper + 4 groups + [可清理 753 MB] + 5x7 tiles
    // -----------------------------------------------------------------------
    {
        model::DesktopModelState st = base_state;
        renderer.ReloadWallpaper(model::WallpaperPreset::Bloom, true);
        auto [br, gvs] = compute_views_and_settle(st, false);
        render::DesktopInteractionState inter{};
        inter.force_opaque_wallpaper = true;
        inter.cleanable_installer_bytes = 753ull * 1024ull * 1024ull;
        inter.hovered_group_id = gid_work;
        export_png(L"01_organized_work.png", st, gvs, br, inter, false);
    }

    // -----------------------------------------------------------------------
    // 09_widgets_edit.png: 1:1 Fig.2 — Tile Edit Mode ([+ 添加小组件] [完成], − badges, + cells)
    // + floating "+ 添加小组件" visual picker beside the visible 5x7 board;
    // 10_widgets_light.png: light theme widget showcase;
    // 11_weather_note_hover.png: 1:1 Fig.1 — Weather 2x2 hovered ([✏] + stack dots) + StickyNote 2x2 + snow-grain headers.
    // -----------------------------------------------------------------------
    {
        model::DesktopModelState st = base_state;
        st.settings.dark_mode = true;
        st.tiles.clear();
        auto wt = [&](const char* id, model::TileKind k, const wchar_t* title, int c, int r, int w, int h,
                      const wchar_t* target = L"", const wchar_t* note = L"") {
            model::DesktopTile t{};
            t.id = id; t.kind = k; t.title = title; t.col = c; t.row = r; t.size = {w, h};
            t.target_path = target; t.note_body = note; t.analog_clock = true; t.live_content = true;
            st.tiles.push_back(std::move(t));
        };
        wt("w_photos", model::TileKind::Photos, L"照片", 0, 0, 4, 2);
        wt("w_bin",    model::TileKind::RecycleBin, L"回收站", 4, 0, 1, 1);
        wt("w_pc",     model::TileKind::ThisPC, L"此电脑", 4, 1, 1, 1);
        wt("w_apps",   model::TileKind::Apps, L"应用", 0, 2, 4, 2);
        wt("w_disk",   model::TileKind::DiskRing, L"D: 剩 140 GB", 4, 2, 1, 1, L"D:\\");
        wt("w_pulse",  model::TileKind::PulseLauncher, L"Pulse", 4, 3, 1, 1);
        wt("w_tray",   model::TileKind::StagingTray, L"暂存区", 0, 4, 4, 1);
        wt("w_clock",  model::TileKind::Clock, L"时钟", 4, 4, 1, 1);
        wt("w_wx",     model::TileKind::Weather, L"天气", 1, 5, 2, 2);
        wt("w_note",   model::TileKind::StickyNote, L"便笺", 3, 5, 2, 2, L"",
           L"周五 16:00 前交季度汇报\n· 预算表再核一遍\n· 合同扫描件发法务");
        // Stack Calendar + Todo + Recent inside w_wx (active index 2 = Weather) so green dots & stack dots match Fig.1 & Fig.2!
        st.tiles[8].stack = {
            {model::TileKind::Calendar, L"日历", L"", "s_cal"},
            {model::TileKind::Todo,     L"待办", L"", "s_todo"},
            {model::TileKind::Weather,  L"天气", L"", "s_wx"},
        };
        st.tiles[8].stack_index = 2;

        renderer.ReloadWallpaper(model::WallpaperPreset::Bloom, true);
        auto [br, gvs] = compute_views_and_settle(st, false);

        // Save/restore tray entries so StagingTray shows the inline <kbd>Ctrl+Shift+S</kbd> empty state per Fig.2
        const auto saved_tray = live.tray_entries;
        const uint64_t saved_tray_bytes = live.tray_total_bytes;
        live.tray_entries.clear();
        live.tray_total_bytes = 0;

        render::DesktopInteractionState inter{};
        inter.force_opaque_wallpaper = true;
        inter.cleanable_installer_bytes = 753ull * 1024ull * 1024ull;
        inter.tile_edit_mode = true;
        inter.edit_phase = 0.0f;

        ov.SetAppearance(true, renderer.Backdrop().VibrantAccentArgb());
        ov.OpenWidgetGalleryDrawer(work_area_rc, br.x, br.y, st.tiles, model::TileKind::Calendar, std::nullopt);
        export_png(L"09_widgets_edit.png", st, gvs, br, inter, true);
        ov.Dismiss();

        // 11_weather_note_hover.png: normal desktop mode with Weather 2x2 hovered ([✏] + stack dots) + StickyNote 2x2
        render::DesktopInteractionState hover_inter{};
        hover_inter.force_opaque_wallpaper = true;
        hover_inter.cleanable_installer_bytes = 753ull * 1024ull * 1024ull;
        hover_inter.hovered_tile_id = "w_wx";
        hover_inter.hovered_tile_edit_btn = true;
        export_png(L"11_weather_note_hover.png", st, gvs, br, hover_inter, false);

        live.tray_entries = saved_tray;
        live.tray_total_bytes = saved_tray_bytes;
    }
    {
        model::DesktopModelState st = base_state;
        st.settings.dark_mode = false;
        st.tiles.clear();
        auto wt = [&](const char* id, model::TileKind k, const wchar_t* title, int c, int r, int w, int h) {
            model::DesktopTile t{}; t.id = id; t.kind = k; t.title = title; t.col = c; t.row = r; t.size = {w, h};
            st.tiles.push_back(std::move(t));
        };
        wt("w_weather", model::TileKind::Weather, L"天气", 0, 0, 4, 2);
        wt("w_clock", model::TileKind::Clock, L"时钟", 4, 0, 1, 1);
        wt("w_bin", model::TileKind::RecycleBin, L"回收站", 4, 1, 1, 1);
        wt("w_todo", model::TileKind::Todo, L"待办", 0, 2, 2, 2);
        wt("w_sys", model::TileKind::System, L"系统", 2, 2, 2, 2);
        wt("w_cal", model::TileKind::Calendar, L"日历", 4, 2, 1, 1);
        wt("w_disk", model::TileKind::DiskRing, L"D:", 4, 3, 1, 1);
        wt("w_apps", model::TileKind::Apps, L"应用", 0, 4, 4, 2);
        wt("w_tray", model::TileKind::StagingTray, L"暂存区", 0, 6, 4, 1);
        st.tiles[3].look.background = model::TileBackground::Color;
        st.tiles[3].look.color_argb = 0xFF10893Eu;
        st.tiles[6].look.show_title = false;
        renderer.ReloadWallpaper(model::WallpaperPreset::Bloom, false);
        auto [br, gvs] = compute_views_and_settle(st, false);
        render::DesktopInteractionState inter{};
        inter.force_opaque_wallpaper = true;
        export_png(L"10_widgets_light.png", st, gvs, br, inter, false);
    }
    // 12/13_widget_sizes_*.png: every size opened up for the widgets (glance strips, 4x4 to-dos, 2x2 clock…).
    for (const bool dark : {true, false}) {
        model::DesktopModelState st = base_state;
        st.settings.dark_mode = dark;
        st.tiles.clear();
        auto wt = [&](const char* id, model::TileKind k, const wchar_t* title, int c, int r, int w, int h) {
            model::DesktopTile t{}; t.id = id; t.kind = k; t.title = title; t.col = c; t.row = r; t.size = {w, h};
            t.analog_clock = true;
            st.tiles.push_back(std::move(t));
        };
        if (dark) {
            wt("s_wx4", model::TileKind::Weather, L"天气", 0, 0, 4, 1);
            wt("s_clk", model::TileKind::Clock, L"时钟", 4, 0, 1, 1);
            wt("s_cal4", model::TileKind::Calendar, L"日历", 0, 1, 4, 1);
            wt("s_wx2", model::TileKind::Weather, L"天气", 0, 2, 2, 1);
            wt("s_sys2", model::TileKind::System, L"系统", 2, 2, 2, 1);
            wt("s_todo4", model::TileKind::Todo, L"待办", 0, 3, 4, 1);
            wt("s_cal2", model::TileKind::Calendar, L"日历", 0, 4, 2, 1);
            wt("s_todo2", model::TileKind::Todo, L"待办", 2, 4, 2, 1);
            wt("s_sys4", model::TileKind::System, L"系统", 0, 5, 4, 1);
            wt("s_clk4", model::TileKind::Clock, L"时钟", 0, 6, 4, 1);
        } else {
            wt("s_todo44", model::TileKind::Todo, L"待办", 0, 0, 4, 4);
            wt("s_clk", model::TileKind::Clock, L"时钟", 4, 0, 1, 1);
            wt("s_note42", model::TileKind::StickyNote, L"便笺", 0, 4, 4, 2);
            wt("s_tray42", model::TileKind::StagingTray, L"暂存区", 0, 6, 4, 1);
            st.tiles[2].note_body = L"周五 16:00 前交季度汇报\n· 预算表再核一遍\n· 合同扫描件发法务";
        }
        renderer.ReloadWallpaper(model::WallpaperPreset::Bloom, dark);
        auto [br, gvs] = compute_views_and_settle(st, false);
        render::DesktopInteractionState inter{};
        inter.force_opaque_wallpaper = true;
        export_png(dark ? L"12_widget_sizes_dark.png" : L"13_widget_sizes_light.png", st, gvs, br, inter, false);
    }
    {
        // 15_gallery_*.png: the drawer's preview at one-row sizes, as the strips draw them on the desktop.
        model::DesktopModelState st = base_state;
        st.settings.dark_mode = true;
        renderer.ReloadWallpaper(model::WallpaperPreset::Bloom, true);
        auto [br, gvs] = compute_views_and_settle(st, false);
        render::DesktopInteractionState inter{};
        inter.force_opaque_wallpaper = true;
        ov.SetAppearance(true, renderer.Backdrop().VibrantAccentArgb());
        const std::pair<model::TileKind, model::TileSize> cases[] = {
            {model::TileKind::Calendar, {2, 1}}, {model::TileKind::Weather, {4, 1}},
            {model::TileKind::Todo, {2, 1}}, {model::TileKind::System, {4, 1}}};
        for (const auto& [kind, size] : cases) {
            ov.OpenWidgetGalleryDrawer(work_area_rc, br.x, br.y, st.tiles, kind, std::nullopt);
            ov.SelectGallerySize(size);
            const std::wstring name = L"15_gallery_" + std::to_wstring(static_cast<int>(kind)) + L"_" +
                                      std::to_wstring(size.cols) + L"x" + std::to_wstring(size.rows) + L".png";
            export_png(name.c_str(), st, gvs, br, inter, true);
            ov.Dismiss();
        }
    }
    {
        // 16_detail_*.png: the cards 天气 / 日历 / 系统 open beside their tile; the 最近使用 stack hovered (dots + arrows).
        model::DesktopModelState st = base_state;
        st.settings.dark_mode = true;
        st.todos = {{L"核对预算表", false}, {L"合同扫描件发法务", false}, {L"订周五的会议室", true}, {L"给设计评审写纪要", true}};
        renderer.ReloadWallpaper(model::WallpaperPreset::Bloom, true);
        auto [br, gvs] = compute_views_and_settle(st, false);
        render::DesktopInteractionState inter{};
        inter.force_opaque_wallpaper = true;
        for (const auto& t : st.tiles)
            if (t.stack.size() >= 2) inter.hovered_tile_id = t.id;
        ov.SetAppearance(true, renderer.Backdrop().VibrantAccentArgb());
        overlays::WidgetDetailData d;
        d.weather.valid = true;
        d.weather.city = L"上海";
        d.weather.temp_c = 18; d.weather.code = 2; d.weather.hi_c = 21; d.weather.lo_c = 13;
        d.weather.feels_c = 17; d.weather.humidity_pct = 64; d.weather.wind_kmh = 11; d.weather.updated = L"07:42";
        const int temps[8] = {18, 19, 20, 21, 20, 18, 16, 15};
        const int codes[8] = {2, 1, 0, 0, 2, 3, 61, 61};
        for (int i = 0; i < 8; ++i) d.weather.hours.push_back({std::to_wstring(9 + i) + L"时", codes[i], temps[i]});
        d.weather.days = {{L"明天", 61, 12, 17}, {L"周三", 3, 11, 16}, {L"周四", 2, 10, 18}, {L"周五", 0, 9, 19}, {L"周六", 1, 11, 20}, {L"周日", 71, 4, 9}};
        d.system.valid = true; d.system.cpu_pct = 23; d.system.mem_used = 10ull << 30; d.system.mem_total = 16ull << 30;
        d.system.disk_used_pct = 71; d.system.disk_free = 113ull << 30; d.system.net_down_bps = 2400ull * 1024;
        d.todos = st.todos;
        d.weather_city = L"上海";
        const RECT anchor{br.x + 400, br.y + 40, br.x + 584, br.y + 224};
        d.weather.has_utc_offset = true;
        d.weather.utc_offset_s = 8 * 3600 + 3 * 3600;  // a city three hours ahead of UTC+8, so the card shows it
        d.drives = {{L"C:\\", 113ull << 30, 476ull << 30}, {L"D:\\", 3546ull << 30, 3726ull << 30}, {L"E:\\", 12ull << 30, 238ull << 30}};
        d.analog_clock = true;
        for (const auto kind : {model::TileKind::Weather, model::TileKind::Calendar, model::TileKind::System,
                                model::TileKind::Clock, model::TileKind::DiskRing}) {
            d.kind = kind;
            ov.OpenWidgetDetail(anchor, "snap", d);
            export_png((L"16_detail_" + std::to_wstring(static_cast<int>(kind)) + L".png").c_str(), st, gvs, br, inter, true);
            ov.Dismiss();
        }
    }
    {
        // 17_hover_*.png: a hovered 下载 row (在文件夹中显示), a hovered 回收站 (清空), a blue 便笺.
        model::DesktopModelState st = base_state;
        st.settings.dark_mode = true;
        for (auto& t : st.tiles)
            if (t.kind == model::TileKind::StickyNote) t.note_color = 2;
        renderer.ReloadWallpaper(model::WallpaperPreset::Bloom, true);
        auto [br, gvs] = compute_views_and_settle(st, false);
        // 1x1 stack (dots only, no arrows) and a blue note with its matching accent.
        for (auto& t : st.tiles)
            if (t.kind == model::TileKind::RecycleBin && t.stack.empty())
                t.stack = {{model::TileKind::RecycleBin, L"回收站", L"", t.id}, {model::TileKind::ThisPC, L"此电脑", L"", "pc2"}};
        for (const auto kind : {model::TileKind::Downloads, model::TileKind::RecycleBin}) {
            render::DesktopInteractionState inter{};
            inter.force_opaque_wallpaper = true;
            for (const auto& t : st.tiles)
                if (model::ActiveTileView(t).kind == kind) inter.hovered_tile_id = t.id;
            inter.hovered_tile_row = 1;
            export_png((L"17_hover_" + std::to_wstring(static_cast<int>(kind)) + L".png").c_str(), st, gvs, br, inter, false);
        }
        {
            // An app pressed in the 应用 tile: only its icon reacts, the card stays put.
            render::DesktopInteractionState inter{};
            inter.force_opaque_wallpaper = true;
            inter.pressed_app_index = 1;
            export_png(L"17_app_pressed.png", st, gvs, br, inter, false);
        }
    }
    {
        // 14: a 2x2 clock and a 4x2 tray, the remaining new sizes.
        model::DesktopModelState st = base_state;
        st.settings.dark_mode = true;
        st.tiles.clear();
        auto wt = [&](const char* id, model::TileKind k, const wchar_t* title, int c, int r, int w, int h) {
            model::DesktopTile t{}; t.id = id; t.kind = k; t.title = title; t.col = c; t.row = r; t.size = {w, h};
            t.analog_clock = true;
            st.tiles.push_back(std::move(t));
        };
        wt("s_clk22", model::TileKind::Clock, L"时钟", 0, 0, 2, 2);
        wt("s_tray42", model::TileKind::StagingTray, L"暂存区", 0, 2, 4, 2);
        renderer.ReloadWallpaper(model::WallpaperPreset::Bloom, true);
        auto [br, gvs] = compute_views_and_settle(st, false);
        render::DesktopInteractionState inter{};
        inter.force_opaque_wallpaper = true;
        export_png(L"14_widget_sizes_more.png", st, gvs, br, inter, false);
    }

    // -----------------------------------------------------------------------
    // 02_context_menu_file.png: FileIcon context menu + cascading "移到分组 ▸" submenu
    // -----------------------------------------------------------------------
    {
        model::DesktopModelState st = base_state;
        renderer.ReloadWallpaper(model::WallpaperPreset::Bloom, true);
        auto [br, gvs] = compute_views_and_settle(st, false);
        render::DesktopInteractionState inter{};
        inter.force_opaque_wallpaper = true;
        inter.cleanable_installer_bytes = 753ull * 1024ull * 1024ull;
        inter.selected_paths.insert(st.items[0].path);
        inter.focused_path = st.items[0].path;

        overlays::ContextMenuSpec spec{};
        spec.target_kind = overlays::ContextMenuTargetKind::FileIcon;
        spec.screen_anchor = POINT{118, 128};
        spec.target_path = st.items[0].path;
        spec.target_name = st.items[0].name;
        spec.current_group_id = gid_work;
        spec.current_tag_id = L"red";
        spec.item_in_tray = true;
        spec.tag_palette = DefaultTagPalette();
        for (const auto& g : st.groups) {
            spec.groups_list.push_back({g.id, g.title, g.icon_glyph});
        }
        spec.initial_open_submenu_id = "sub:move-group";

        ov.SetAppearance(true, renderer.Backdrop().VibrantAccentArgb());
        ov.OpenContextMenu(spec, st.settings);
        ov.SetAsyncShellVerbs(1, {
            {101, true, false, false, 0, L"", L"7zip", L"用 7-Zip 打开压缩包"},
            {102, true, false, false, 0, L"", L"defender", L"使用 Microsoft Defender 扫描"},
        });
        export_png(L"02_context_menu_file.png", st, gvs, br, inter, true);
        ov.Dismiss();
    }

    // -----------------------------------------------------------------------
    // 03_type_to_search.png: #dsearch popup ("汇报") + local & Pulse hits + highlighted icon
    // -----------------------------------------------------------------------
    {
        model::DesktopModelState st = base_state;
        renderer.ReloadWallpaper(model::WallpaperPreset::Bloom, true);
        auto [br, gvs] = compute_views_and_settle(st, false);
        render::DesktopInteractionState inter{};
        inter.force_opaque_wallpaper = true;
        inter.cleanable_installer_bytes = 753ull * 1024ull * 1024ull;
        inter.search_highlight_query = L"汇报";
        inter.banner_suppressed = true;

        std::vector<overlays::SearchHitEntry> local_hits = {
            {st.items[0].path, L"季度汇报.pptx", 14800000ull, false, true, L"进行中"},
        };
        std::vector<overlays::SearchHitEntry> remote_hits = {
            {L"D:\\归档\\2026Q3\\Q3业务汇报终稿.pptx", L"Q3业务汇报终稿.pptx", 22400000ull, false, false, L"D:\\归档\\2026Q3"},
            {L"D:\\文档\\汇报模板-深色宽屏.potx",      L"汇报模板-深色宽屏.potx", 6100000ull, false, false, L"D:\\文档"},
            {L"P:\\项目\\周度进展汇报汇总.xlsx",       L"周度进展汇报汇总.xlsx", 1450000ull, false, false, L"P:\\项目"},
        };
        ov.SetAppearance(true, renderer.Backdrop().VibrantAccentArgb());
        ov.OpenSearchPopup(work_area_rc, L"汇报", std::move(local_hits));
        ov.UpdateRemoteSearchHits(std::move(remote_hits));
        export_png(L"03_type_to_search.png", st, gvs, br, inter, true);
        ov.Dismiss();
    }

    // -----------------------------------------------------------------------
    // 04_health_drawer.png: Right-hand Health Check drawer (tiles hidden per HTML)
    // -----------------------------------------------------------------------
    {
        model::DesktopModelState st = base_state;
        renderer.ReloadWallpaper(model::WallpaperPreset::Bloom, true);
        auto [br, gvs] = compute_views_and_settle(st, true);
        render::DesktopInteractionState inter{};
        inter.force_opaque_wallpaper = true;
        inter.side_drawer_open = true;
        inter.cleanable_installer_bytes = 753ull * 1024ull * 1024ull;

        model::HealthReport rep{};
        rep.score = 72;
        rep.issues = {
            {model::HealthIssueKind::InstalledInstaller, L"C:\\Users\\SS\\Desktop\\显卡驱动-566.14.exe",
             L"显卡驱动-566.14.exe", 692ull * 1024ull * 1024ull, L"已安装同版本 · 692 MB", true},
            {model::HealthIssueKind::InstalledInstaller, L"C:\\Users\\SS\\Desktop\\node-v22.11.0-x64.msi",
             L"node-v22.11.0-x64.msi", 31ull * 1024ull * 1024ull, L"已安装 Node.js 22.11 · 31 MB", true},
            {model::HealthIssueKind::InstalledInstaller, L"C:\\Users\\SS\\Desktop\\7z2600-x64.exe",
             L"7z2600-x64.exe", 2ull * 1024ull * 1024ull, L"已安装 7-Zip 26.00 · 2.0 MB", true},
            {model::HealthIssueKind::StaleScreenshot, L"C:\\Users\\SS\\Desktop\\报错截图-已修.png",
             L"报错截图-已修.png", 510000ull, L"22 天未打开 · 498 KB", true},
            {model::HealthIssueKind::StaleScreenshot, L"C:\\Users\\SS\\Desktop\\白板拍照.jpg",
             L"白板拍照.jpg", 3100000ull, L"18 天未打开 · 3.0 MB", true},
            {model::HealthIssueKind::BrokenShortcut, L"C:\\Users\\SS\\Desktop\\旧版原型预览.lnk",
             L"旧版原型预览.lnk", 2048ull, L"目标路径已不存在", true},
            {model::HealthIssueKind::EmptyFolder, L"C:\\Users\\SS\\Desktop\\临时归档",
             L"临时归档", 0ull, L"空文件夹", true},
            {model::HealthIssueKind::OneTimeUse, L"C:\\Users\\SS\\Desktop\\ffmpeg-7.1.zip",
             L"ffmpeg-7.1.zip", 92000000ull, L"已解压过 · 87.7 MB", false},
        };
        for (const auto& iss : rep.issues) rep.total_reclaimable_bytes += iss.reclaimable_bytes;

        ov.SetAppearance(true, renderer.Backdrop().VibrantAccentArgb());
        ov.OpenHealthDrawer(work_area_rc, rep, st.items.size());
        export_png(L"04_health_drawer.png", st, gvs, br, inter, true);
        ov.Dismiss();
    }

    // -----------------------------------------------------------------------
    // 05_beautify_drawer.png: Dusk preset wallpaper + Beautify drawer open
    // -----------------------------------------------------------------------
    {
        model::DesktopModelState st = base_state;
        st.settings.wallpaper_preset = model::WallpaperPreset::Dusk;
        renderer.ReloadWallpaper(model::WallpaperPreset::Dusk, true);
        auto [br, gvs] = compute_views_and_settle(st, true);
        render::DesktopInteractionState inter{};
        inter.force_opaque_wallpaper = true;
        inter.side_drawer_open = true;
        inter.cleanable_installer_bytes = 753ull * 1024ull * 1024ull;

        std::unordered_set<uint8_t> active_widgets{
            static_cast<uint8_t>(model::TileKind::Clock),
            static_cast<uint8_t>(model::TileKind::StickyNote),
            static_cast<uint8_t>(model::TileKind::StagingTray),
        };
        ov.SetAppearance(true, renderer.Backdrop().VibrantAccentArgb());
        ov.OpenBeautifyDrawer(work_area_rc, st.settings, active_widgets);
        export_png(L"05_beautify_drawer.png", st, gvs, br, inter, true);
        ov.Dismiss();
    }

    // -----------------------------------------------------------------------
    // 06_focus_scene.png: Focus scene (only 进行中 expanded; others collapsed & dimmed + #banner)
    // -----------------------------------------------------------------------
    {
        model::DesktopModelState st = base_state;
        st.settings.active_scene = model::DesktopScene::Focus;
        renderer.ReloadWallpaper(model::WallpaperPreset::Bloom, true);
        auto [br, gvs] = compute_views_and_settle(st, false);
        render::DesktopInteractionState inter{};
        inter.force_opaque_wallpaper = true;
        inter.cleanable_installer_bytes = 753ull * 1024ull * 1024ull;
        export_png(L"06_focus_scene.png", st, gvs, br, inter, false);
    }

    // -----------------------------------------------------------------------
    // 07_present_scene.png: Present scene (groups/loose hidden, personal tiles veiled + #banner)
    // -----------------------------------------------------------------------
    {
        model::DesktopModelState st = base_state;
        st.settings.active_scene = model::DesktopScene::Present;
        st.settings.wallpaper_preset = model::WallpaperPreset::Photo;
        renderer.ReloadWallpaper(model::WallpaperPreset::Photo, true);
        auto [br, gvs] = compute_views_and_settle(st, false);
        render::DesktopInteractionState inter{};
        inter.force_opaque_wallpaper = true;
        export_png(L"07_present_scene.png", st, gvs, br, inter, false);
    }

    // -----------------------------------------------------------------------
    // 08_ai_organize_drawer.png: Right-hand "✦ AI 智能收纳" acrylic drawer (22 -> 12 / 4 / 13)
    // -----------------------------------------------------------------------
    {
        model::DesktopModelState st = base_state;
        renderer.ReloadWallpaper(model::WallpaperPreset::Bloom, true);
        auto [br, gvs] = compute_views_and_settle(st, true);
        render::DesktopInteractionState inter{};
        inter.force_opaque_wallpaper = true;
        inter.side_drawer_open = true;
        inter.cleanable_installer_bytes = 753ull * 1024ull * 1024ull;

        overlays::AiDrawerViewState ais{};
        ais.phase = overlays::AiDrawerPhase::PlanReady;
        ais.connection.configured = true;
        ais.connection.assistant_enabled = true;
        ais.connection.service_id = "deepseek";
        ais.connection.provider_name = L"DeepSeek";
        ais.connection.model_id = "deepseek-flash";
        ais.strategy = ai::AiOrganizeStrategy::ArchiveClutter;
        ais.pin_created_folders = true;

        ais.plan.from_ai_model = true;
        ais.plan.model_badge = L"DeepSeek · deepseek-flash";
        ais.plan.summary = L"保留「进行中」活跃文档，将 13 项散落与历史文件归入 4 个目录";
        ais.plan.desktop_before_count = st.items.size(); // 22 items -> 12 after archiving 13 into 3 new + 1 existing

        {
            ai::AiArchiveGroupPlan g1{};
            g1.folder_name = L"项目汇报与文档归档";
            g1.target_folder_path = L"C:\\Users\\SS\\Desktop\\项目汇报与文档归档";
            g1.reason = L"汇集述职提纲、历史周会纪要与备忘文档，便于按阶段查阅";
            g1.folder_already_exists = false;
            g1.items = {
                {L"年度述职提纲.docx", L"C:\\Users\\SS\\Desktop\\年度述职提纲.docx", 640000ull, 0, false, true},
                {L"周会纪要.docx",     L"C:\\Users\\SS\\Desktop\\周会纪要.docx",     320000ull, 4, false, true},
                {L"架构备忘.md",       L"C:\\Users\\SS\\Desktop\\架构备忘.md",        42000ull, 3, false, true},
                {L"todo.txt",          L"C:\\Users\\SS\\Desktop\\todo.txt",            4096ull, 1, false, true},
            };
            ais.plan.groups.push_back(std::move(g1));
        }
        {
            ai::AiArchiveGroupPlan g2{};
            g2.folder_name = L"安装包与驱动归档";
            g2.target_folder_path = L"C:\\Users\\SS\\Desktop\\安装包与驱动归档";
            g2.reason = L"已安装或一次性使用的安装程序统一收纳，释放 753 MB 空间";
            g2.folder_already_exists = false;
            g2.items = {
                {L"显卡驱动-566.14.exe",    L"C:\\Users\\SS\\Desktop\\显卡驱动-566.14.exe",    692ull * 1024ull * 1024ull, 28, false, true},
                {L"node-v22.11.0-x64.msi",  L"C:\\Users\\SS\\Desktop\\node-v22.11.0-x64.msi",   31ull * 1024ull * 1024ull, 35, false, true},
                {L"PulseSetup-1.0.58.exe",  L"C:\\Users\\SS\\Desktop\\PulseSetup-1.0.58.exe",   28ull * 1024ull * 1024ull, 12, false, true},
                {L"7z2600-x64.exe",         L"C:\\Users\\SS\\Desktop\\7z2600-x64.exe",           2ull * 1024ull * 1024ull, 40, false, true},
            };
            ais.plan.groups.push_back(std::move(g2));
        }
        {
            ai::AiArchiveGroupPlan g3{};
            g3.folder_name = L"素材包整理";
            g3.target_folder_path = L"C:\\Users\\SS\\Desktop\\素材包整理";
            g3.reason = L"复用桌面上已有的「素材包整理」目录收纳历史截图与白板照片";
            g3.folder_already_exists = true;
            g3.items = {
                {L"白板拍照.jpg",      L"C:\\Users\\SS\\Desktop\\白板拍照.jpg",      3100000ull, 18, false, true},
                {L"报错截图-已修.png", L"C:\\Users\\SS\\Desktop\\报错截图-已修.png",  510000ull, 22, false, true},
                {L"设计稿参考.webp",   L"C:\\Users\\SS\\Desktop\\设计稿参考.webp",    620000ull,  6, false, true},
            };
            ais.plan.groups.push_back(std::move(g3));
        }
        {
            ai::AiArchiveGroupPlan g4{};
            g4.folder_name = L"工具包与历史备份";
            g4.target_folder_path = L"C:\\Users\\SS\\Desktop\\工具包与历史备份";
            g4.reason = L"压缩包与字体备份集中存放，保持工作桌面专注清爽";
            g4.folder_already_exists = false;
            g4.items = {
                {L"ffmpeg-7.1.zip", L"C:\\Users\\SS\\Desktop\\ffmpeg-7.1.zip", 92000000ull, 19, false, true},
                {L"字体备份",       L"C:\\Users\\SS\\Desktop\\字体备份",              0ull, 25, true,  true},
            };
            ais.plan.groups.push_back(std::move(g4));
        }

        ov.SetAppearance(true, renderer.Backdrop().VibrantAccentArgb());
        ov.OpenAiOrganizeDrawer(work_area_rc, ais);
        export_png(L"08_ai_organize_drawer.png", st, gvs, br, inter, true);
        ov.Dismiss();
    }

    // -----------------------------------------------------------------------
    // 09_sticky_and_beautify_light.png: Light-mode Sticky Note + Beautify verification
    // -----------------------------------------------------------------------
    {
        model::DesktopModelState st = base_state;
        st.settings.dark_mode = false;
        st.settings.wallpaper_preset = model::WallpaperPreset::Bloom;
        renderer.ReloadWallpaper(model::WallpaperPreset::Bloom, false);
        auto [br, gvs] = compute_views_and_settle(st, false);
        render::DesktopInteractionState inter{};
        inter.host_dark_mode = false;
        inter.force_opaque_wallpaper = true;
        inter.cleanable_installer_bytes = 753ull * 1024ull * 1024ull;

        ov.SetAppearance(false, 0xFF0078D4u);
        ov.OpenStickyNotePopover(RECT{960, 260, 1280, 480}, "tile_note", L"下载\n整理桌面安装包");
        export_png(L"09_sticky_note_light.png", st, gvs, br, inter, true);
        ov.Dismiss();

        inter.side_drawer_open = true;
        auto [br2, gvs2] = compute_views_and_settle(st, true);
        std::unordered_set<uint8_t> active_widgets{
            static_cast<uint8_t>(model::TileKind::Clock),
            static_cast<uint8_t>(model::TileKind::StagingTray),
        };
        ov.SetAppearance(false, 0xFF0078D4u);
        ov.OpenBeautifyDrawer(work_area_rc, st.settings, active_widgets);
        export_png(L"10_beautify_light.png", st, gvs2, br2, inter, true);
        ov.Dismiss();
    }

    // 11-12. Liquid Glass dark & light full desktop states
    for (int lg_dark : {1, 0}) {
        model::DesktopModelState st = base_state;
        st.settings.dark_mode = (lg_dark != 0);
        st.settings.default_material = model::CardMaterial::LiquidGlass;
        model::ApplyGlassPreset(static_cast<uint8_t>(model::GlassPreset::ClearDroplet), st.settings);
        st.settings.wallpaper_preset = lg_dark ? model::WallpaperPreset::Dusk : model::WallpaperPreset::Bloom;
        renderer.ReloadWallpaper(st.settings.wallpaper_preset, st.settings.dark_mode);
        auto [br, gvs] = compute_views_and_settle(st, false);
        render::DesktopInteractionState inter{};
        inter.host_dark_mode = st.settings.dark_mode;
        inter.force_opaque_wallpaper = true;
        inter.cleanable_installer_bytes = 753ull * 1024ull * 1024ull;
        export_png(lg_dark ? L"11_liquid_glass_dark.png" : L"12_liquid_glass_light.png",
                   st, gvs, br, inter, false);
    }

    ov.Shutdown();
    renderer.Shutdown();
    CoUninitialize();
    return all_ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Mode 4: Interactive Out-of-Process Desktop Session
// ---------------------------------------------------------------------------
class DesktopHostSession {
    friend struct DesktopDragHostTestPeer;
    friend struct DesktopGroupHostTestPeer;
    friend struct DesktopPersistenceHostTestPeer;
    friend struct DesktopPreviewHostTestPeer;
public:
    DesktopHostSession(std::wstring pipe_name, DWORD parent_pid)
        : pipe_name_(std::move(pipe_name)), parent_pid_(parent_pid) {}

    int Run() {
        if (parent_pid_ != 0) {
            parent_proc_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent_pid_);
        }

        tag_palette_ = DefaultTagPalette();
        SeedTagsFromLocalPulseFiles();
        overlays_.SetTagPalette(tag_palette_);
        LoadOrInitModel();
        ApplySystemAppearance();

        mount::MountHostCallbacks mcb{};
        mcb.on_message = [this](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
            return OnSurfaceMessage(hwnd, msg, wp, lp, handled);
        };
        mcb.on_geometry_changed = [this](const model::RectI& bounds, uint32_t dpi) {
            state_.monitors = MonitorsInDip();
            RecomputeLayout();
            renderer_.Resize(static_cast<uint32_t>(bounds.w), static_cast<uint32_t>(bounds.h));
            renderer_.SetSurfaceGeometry(dpi, PrimaryMonitorPx(), bounds);
            overlays_.SetDipScale(dpi / 96.0f);
            RefreshLiveWallpaperState();
            renderer_.ReloadWallpaper(state_.settings.wallpaper_preset, state_.settings.dark_mode);
            renderer_.Tick(state_, group_views_, tile_board_rect_, live_data_, interaction_);
            SyncHoverGlassTimer();
        };
        mcb.on_wallpaper_changed = [this]() {
            RecordExternalSystemWallpaperIfChanged();
            RefreshLiveWallpaperState();
            if (state_.settings.wallpaper_preset == model::WallpaperPreset::System) {
                renderer_.ReloadWallpaper(model::WallpaperPreset::System, state_.settings.dark_mode);
                renderer_.Tick(state_, group_views_, tile_board_rect_, live_data_, interaction_);
                SyncHoverGlassTimer();
            }
        };
        mcb.on_explorer_restarted = [this]() {
            RefreshLiveWallpaperState();
            renderer_.InvalidateAll();
            renderer_.Tick(state_, group_views_, tile_board_rect_, live_data_, interaction_);
            SyncHoverGlassTimer();
        };

        if (!mount_.Mount(true, mount::DefaultSnapshotFilePath(), std::move(mcb))) {
            return 2;
        }

        RefreshDesktopItems(&mount_.InitialSnapshot());
        RefreshLiveTileData();
        RefreshHealthStats();
        RecomputeLayout();

        const model::RectI sb = mount_.SurfaceBounds();
        state_.monitors = MonitorsInDip();
        renderer_.SetSurfaceGeometry(mount_.CurrentDpi(), PrimaryMonitorPx(), sb);
        if (!renderer_.InitializeForHwnd(mount_.Hwnd(),
                                         static_cast<uint32_t>(sb.w),
                                         static_cast<uint32_t>(sb.h),
                                         true,
                                         false)) {
            mount_.Unmount(true);
            return 3;
        }
        renderer_.Backdrop().SetAsyncReadyCallback([this, hwnd = mount_.Hwnd()]() {
            if (!icons_ready_posted_.exchange(true) && hwnd && IsWindow(hwnd)) PostMessageW(hwnd, kMsgIconsReady, 0, 0);
        });
        renderer_.ConfigureLiveWallpaperNotification(mount_.Hwnd(), kMsgLiveGlassFrame);
        live_watcher_.Start(mount_.Hwnd(), kMsgLiveWallpaperProbe);
        RefreshLiveWallpaperState();
        WTSRegisterSessionNotification(mount_.Hwnd(), NOTIFY_FOR_THIS_SESSION);
        renderer_.ReloadWallpaper(state_.settings.wallpaper_preset, state_.settings.dark_mode);
        if (state_.settings.wallpaper_preset != model::WallpaperPreset::System) {
            SyncSystemDesktopWallpaper(state_.settings.wallpaper_preset, state_.settings.dark_mode, false);
        }
        last_activity_tick_ = GetTickCount64();
        working_set_trimmed_ = false;
        SyncOverlayInteractionFlags();

        overlays_.SetDipScale(mount_.DipScale());
        SetupOverlays();
        SetupDropTarget();

        SyncWatcherDirectories(true);

        if (!pipe_name_.empty()) {
            pipe_.StartClient(
                pipe_name_,
                [this](ipc::Frame&& f) {
                    {
                        std::lock_guard<std::mutex> lk(ipc_mutex_);
                        ipc_inbox_.push_back(std::move(f));
                    }
                    if (HWND h = mount_.Hwnd()) PostMessageW(h, kMsgDrainIpc, 0, 0);
                },
                [hwnd = mount_.Hwnd()]() {
                    if (hwnd && IsWindow(hwnd)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
                });
            pipe_.Send(ipc::MessageType::Hello, ++next_req_id_, BuildWatchedPathsPayload());
            items_paths_dirty_ = false;
        }

        g_console_close_target.store(mount_.Hwnd());
        SetConsoleCtrlHandler(&OnConsoleCtrl, TRUE);
        SetTimer(mount_.Hwnd(), kTimerHeartbeat, 1000, nullptr);
        SetTimer(mount_.Hwnd(), kTimerLiveTiles, 3000, nullptr);

        renderer_.InvalidateAll();
        renderer_.Tick(state_, group_views_, tile_board_rect_, live_data_, interaction_);
        mount_.HideNativeIconsNow();
        renderer_.TrimDxgiMemory();
        _heapmin();

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            renderer_.Tick(state_, group_views_, tile_board_rect_, live_data_, interaction_);
            SyncHoverGlassTimer();
        }

        if (active_ai_session_) {
            active_ai_session_->Cancel();
            active_ai_session_.reset();
        }
        if (hover_glass_timer_active_ && mount_.Hwnd()) {
            KillTimer(mount_.Hwnd(), kTimerHoverGlass);
            hover_glass_timer_active_ = false;
        }
        if (mount_.Hwnd()) {
            WTSUnRegisterSessionNotification(mount_.Hwnd());
        }
        live_watcher_.Stop();
        g_console_close_target.store(nullptr);
        SaveModel();
        watcher_.Stop();
        CancelHoverPreview();
        pipe_.Stop();
        drop_mgr_.Detach();
        overlays_.Shutdown();
        renderer_.Shutdown();
        if (restore_on_exit_) {
            SyncSystemDesktopWallpaper(model::WallpaperPreset::System, state_.settings.dark_mode, true);
        }
        mount_.Unmount(restore_on_exit_);
        if (parent_proc_) CloseHandle(parent_proc_);
        return 0;
    }

private:
    model::RectI PrimaryWorkAreaClient() const noexcept {
        const model::RectI surf = mount_.SurfaceBoundsDip();
        for (const auto& m : state_.monitors) {
            if (m.primary) {
                return model::RectI{
                    m.work_area.x - surf.x,
                    m.work_area.y - surf.y,
                    m.work_area.w,
                    m.work_area.h,
                };
            }
        }
        return model::RectI{0, 0, std::max(1280, surf.w), std::max(720, surf.h - 48)};
    }

    model::RectI TileDragWorkAreaClient(model::PointI point) const noexcept {
        const auto surface = mount_.SurfaceBoundsDip();
        for (const auto& monitor : state_.monitors) {
            const model::RectI bounds{monitor.bounds.x - surface.x, monitor.bounds.y - surface.y,
                                      monitor.bounds.w, monitor.bounds.h};
            if (bounds.contains(point.x, point.y))
                return {monitor.work_area.x - surface.x, monitor.work_area.y - surface.y,
                        monitor.work_area.w, monitor.work_area.h};
        }
        return PrimaryWorkAreaClient();
    }

    RECT PrimaryWorkAreaScreen() const noexcept {
        for (const auto& m : state_.monitors) {
            if (m.primary) {
                return RECT{m.work_area.x, m.work_area.y, m.work_area.right(), m.work_area.bottom()};
            }
        }
        RECT rc{0, 0, 1920, 1032};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
        return rc;
    }

    uint32_t EffectiveAccentArgb() const noexcept {
        if (state_.settings.auto_wallpaper_accent) {
            const uint32_t vib = renderer_.Backdrop().VibrantAccentArgb();
            if ((vib & 0x00FFFFFFu) != 0) return vib;
        }
        return (state_.settings.accent_argb & 0x00FFFFFFu) != 0
                   ? state_.settings.accent_argb
                   : interaction_.host_accent_argb;
    }

    std::unordered_set<uint8_t> ActiveWidgetKinds() const {
        std::unordered_set<uint8_t> out;
        for (const auto& t : state_.tiles) {
            out.insert(static_cast<uint8_t>(t.kind));
            for (const auto& m : t.stack) {
                out.insert(static_cast<uint8_t>(m.kind));
            }
        }
        return out;
    }

    void SyncOverlayInteractionFlags() noexcept {
        if (overlays_.IsOpen()) CancelHoverPreview();
        interaction_.side_drawer_open = overlays_.IsSideDrawerOpen();
        interaction_.banner_suppressed = (overlays_.ActiveKind() == overlays::OverlayKind::SearchPopup);
        // Live desktop uses a sparse 256x256 virtual surface; presets apply directly to Windows' desktop wallpaper
        // so unoccupied tiles remain 100% transparent without creating an opaque color block on the desktop.
        interaction_.force_opaque_wallpaper = false;
    }

    void RefreshLiveWallpaperState() noexcept {
        const model::RectI sb = mount_.SurfaceBounds();
        live_watcher_.Refresh(mount_.Hierarchy(), sb.w, sb.h);
        renderer_.SetLiveWallpaperDecision(live_watcher_.Current());
    }

    void SyncHoverGlassTimer() noexcept {
        const HWND hwnd = mount_.Hwnd();
        if (!hwnd) return;
        const bool want = renderer_.HasDirtyWork();
        if (want && !hover_glass_timer_active_) {
            SetTimer(hwnd, kTimerHoverGlass, 16, nullptr);
            hover_glass_timer_active_ = true;
        } else if (!want && hover_glass_timer_active_) {
            KillTimer(hwnd, kTimerHoverGlass);
            hover_glass_timer_active_ = false;
        }
    }

    static std::wstring PresetWallpaperPngPath() {
        const std::filesystem::path base(mount::DefaultLayoutStateFilePath());
        return (base.parent_path() / L"pulse_preset_wallpaper.png").wstring();
    }

    static std::wstring OriginalWallpaperRecordPath() {
        const std::filesystem::path base(mount::DefaultLayoutStateFilePath());
        return (base.parent_path() / L"desktop_original_wallpaper.txt").wstring();
    }

    void RecordExternalSystemWallpaperIfChanged() {
        wchar_t cur_wp[MAX_PATH * 2]{};
        if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(cur_wp), cur_wp, 0) || cur_wp[0] == L'\0') return;
        const std::wstring preset_png = PresetWallpaperPngPath();
        if (_wcsicmp(cur_wp, preset_png.c_str()) != 0) {
            SaveTextFileAtomic(OriginalWallpaperRecordPath(), ipc::WideToUtf8(cur_wp));
        }
    }

    void SyncSystemDesktopWallpaper(model::WallpaperPreset preset, bool dark_mode, bool synchronous_restore) {
        const std::wstring preset_png = PresetWallpaperPngPath();
        const std::wstring record_path = OriginalWallpaperRecordPath();
        wchar_t cur_wp[MAX_PATH * 2]{};
        SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(cur_wp), cur_wp, 0);

        if (preset == model::WallpaperPreset::System) {
            ++(*wallpaper_apply_seq_);
            if (_wcsicmp(cur_wp, preset_png.c_str()) == 0) {
                const std::wstring orig = ipc::Utf8ToWide(LoadTextFile(record_path));
                std::error_code ec;
                if (!orig.empty() && std::filesystem::exists(orig, ec)) {
                    if (synchronous_restore) {
                        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, const_cast<wchar_t*>(orig.c_str()),
                                              SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
                    } else {
                        std::thread([orig]() {
                            SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, const_cast<wchar_t*>(orig.c_str()),
                                                  SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
                        }).detach();
                    }
                }
            }
            return;
        }

        if (cur_wp[0] != L'\0' && _wcsicmp(cur_wp, preset_png.c_str()) != 0) {
            SaveTextFileAtomic(record_path, ipc::WideToUtf8(cur_wp));
        }

        const model::RectI pm = PrimaryMonitorPx();
        const uint32_t mon_w = static_cast<uint32_t>(std::clamp(pm.w, 1280, 7680));
        const uint32_t mon_h = static_cast<uint32_t>(std::clamp(pm.h, 720, 4320));
        const uint64_t seq = ++(*wallpaper_apply_seq_);
        auto seq_holder = wallpaper_apply_seq_;
        std::thread([seq, seq_holder, preset_png, mon_w, mon_h, preset, dark_mode]() {
            if (seq != seq_holder->load()) return;
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool ok = render::WallpaperBackdrop::RenderPresetWallpaperFile(
                preset_png, mon_w, mon_h, preset, dark_mode);
            if (ok && seq == seq_holder->load()) {
                SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, const_cast<wchar_t*>(preset_png.c_str()),
                                      SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
            }
            CoUninitialize();
        }).detach();
    }

    void SeedTagsFromLocalPulseFiles() {
        wchar_t local_app[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local_app))) return;
        const std::filesystem::path tags_file = std::filesystem::path(local_app) / L"Pulse" / L"tags.json";
        const std::string raw = LoadTextFile(tags_file.wstring());
        if (raw.empty()) return;
        std::vector<overlays::ColorTagOption> parsed;
        size_t pos = raw.find("\"tags\"");
        if (pos == std::string::npos) pos = 0;
        pos = raw.find('[', pos);
        while (pos != std::string::npos) {
            const size_t ob = raw.find('{', pos);
            if (ob == std::string::npos) break;
            const size_t cb = raw.find('}', ob);
            if (cb == std::string::npos) break;
            const std::string_view obj(raw.data() + ob, cb - ob + 1);
            overlays::ColorTagOption t{};
            uint64_t argb = 0;
            ipc::JsonFindWide(obj, "id", t.id);
            ipc::JsonFindWide(obj, "name", t.name);
            if (t.name.empty()) ipc::JsonFindWide(obj, "label", t.name);
            if (ipc::JsonFindUint64(obj, "argb", argb) || ipc::JsonFindUint64(obj, "color", argb)) {
                t.argb = static_cast<uint32_t>(argb | 0xFF000000ull);
            }
            if (!t.id.empty() && !t.name.empty() && t.argb != 0) {
                parsed.push_back(std::move(t));
            }
            pos = cb + 1;
        }
        if (!parsed.empty()) {
            tag_palette_ = std::move(parsed);
        }
    }

    std::string BuildWatchedPathsPayload() const {
        std::string out = "{\"client\":\"pulse_desktop_host\",\"paths\":[";
        bool first = true;
        for (const auto& it : state_.items) {
            if (it.path.empty()) continue;
            if (!first) out.push_back(',');
            first = false;
            out += ipc::JsonQuoteWide(it.path);
        }
        out += "]}";
        return out;
    }

    // Monitors as the host sees them: screen DIPs at the primary monitor's scale (the layer's scale).
    static std::vector<model::MonitorWorkArea> MonitorsInDip() {
        std::vector<model::MonitorWorkArea> mons = mount::QueryMonitorWorkAreas();
        uint32_t dpi = 96;
        for (const auto& m : mons) {
            if (m.primary && m.dpi) dpi = m.dpi;
        }
        const float s = dpi / 96.0f;
        auto scale = [s](model::RectI r) {
            return model::RectI{static_cast<int>(std::lround(r.x / s)), static_cast<int>(std::lround(r.y / s)),
                                static_cast<int>(std::lround(r.w / s)), static_cast<int>(std::lround(r.h / s))};
        };
        for (auto& m : mons) {
            m.bounds = scale(m.bounds);
            m.work_area = scale(m.work_area);
        }
        return mons;
    }

    model::RectI PrimaryMonitorPx() const {
        for (const auto& m : mount::QueryMonitorWorkAreas()) {
            if (m.primary) return m.bounds;
        }
        return mount_.SurfaceBounds();
    }

    void LoadOrInitModel() {
        state_.monitors = MonitorsInDip();
#ifdef PULSE_DESKTOP_PERSISTENCE_TEST
        const std::string& saved = test_model_input_;
#else
        const std::string saved = LoadTextFile(mount::DefaultLayoutStateFilePath());
#endif
        if (!saved.empty() && model::DeserializeModelStateJson(saved, state_)) {
            state_.monitors = MonitorsInDip();
            if (state_.groups.empty()) {
                state_.groups = model::MakeDefaultGroups(PrimaryWorkAreaClient());
            }
            // Empty is a persisted deletion, not first-run initialization.
            // Only older models without a top-level tiles member need defaults.
            if (state_.tiles.empty() && pulse::json::ConfigSyntax(ipc::Utf8ToWide(saved), false)
                    .MemberPosition(L"tiles") == std::wstring_view::npos) {
                state_.tiles = model::MakeDefaultTiles();
            }
            model::MigrateDefaultTiles(state_.tiles);
            model::ApplyWidgetDefaults(state_);
            if (state_.todos.empty() && saved.find("\"todos\"") == std::string::npos) state_.todos = model::MakeDefaultTodos();
            if (state_.settings.weather_city.empty()) state_.settings.weather_city = L"北京";
            return;
        }
        const model::RectI wa = state_.monitors.empty()
                                    ? model::RectI{0, 0, 1920, 1032}
                                    : state_.monitors.front().work_area;
        state_.groups = model::MakeDefaultGroups(wa);
        state_.tiles = model::MakeDefaultTiles();
        state_.todos = model::MakeDefaultTodos();
        state_.settings.weather_city = L"北京";
        state_.widget_defaults_applied = true;
    }

    std::string CommittedModelJson() const {
        if (!tile_drag_session_.Active() && !group_drag_session_.Pending()) return model::SerializeModelStateJson(state_);
        auto committed = state_;
        if (tile_drag_session_.Active()) committed.tiles = tile_drag_session_.Original();
        if (group_drag_session_.Pending()) committed.groups = group_drag_session_.Original();
        return model::SerializeModelStateJson(committed);
    }

    void SaveModel() {
#ifdef PULSE_DESKTOP_DRAG_TEST
        test_saved_model_ = CommittedModelJson();
#else
        SaveTextFileAtomic(mount::DefaultLayoutStateFilePath(), CommittedModelJson());
#endif
    }

    static std::wstring NormalizeFolderKey(std::wstring p) {
        while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) {
            p.pop_back();
        }
        for (auto& ch : p) {
            ch = static_cast<wchar_t>(std::towlower(ch));
        }
        return p;
    }

    static std::wstring MappedFolderLeafName(std::wstring p) {
        while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) {
            p.pop_back();
        }
        const std::wstring leaf = std::filesystem::path(p).filename().wstring();
        return leaf.empty() ? p : leaf;
    }

    static bool IsDirectChildOfDir(const std::wstring& item_path, const std::wstring& dir_path) {
        std::filesystem::path parent = std::filesystem::path(item_path).parent_path();
        return NormalizeFolderKey(parent.wstring()) == NormalizeFolderKey(dir_path);
    }

    static std::wstring MoveOrCopyToDirectoryCollisionSafe(const std::wstring& src,
                                                           const std::wstring& dst_dir,
                                                           bool copy_mode) {
        if (src.empty() || dst_dir.empty()) return {};
        const std::wstring src_key = NormalizeFolderKey(src);
        const std::wstring dst_key = NormalizeFolderKey(dst_dir);
        if (src_key == dst_key) return src;
        if (IsDirectChildOfDir(src, dst_dir) && !copy_mode) return src;

        std::filesystem::path sp(src);
        std::filesystem::path dp_base(dst_dir);
        std::filesystem::path cand = dp_base / sp.filename();
        const bool is_dir = (GetFileAttributesW(src.c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const std::wstring stem = is_dir ? sp.filename().wstring() : sp.stem().wstring();
        const std::wstring ext = is_dir ? std::wstring{} : sp.extension().wstring();
        for (int n = 2; GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES && n < 500; ++n) {
            cand = dp_base / (stem + L" (" + std::to_wstring(n) + L")" + ext);
        }
        if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) return {};

        const std::wstring dst = cand.wstring();
        bool ok = false;
        if (copy_mode) {
            if (!is_dir) {
                ok = CopyFileW(src.c_str(), dst.c_str(), TRUE) != FALSE;
            }
        } else {
            ok = MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED) != FALSE;
        }
        if (!ok) {
            std::wstring from_zz = src;
            from_zz.push_back(L'\0');
            from_zz.push_back(L'\0');
            std::wstring to_zz = dst;
            to_zz.push_back(L'\0');
            to_zz.push_back(L'\0');
            SHFILEOPSTRUCTW op{};
            op.wFunc = copy_mode ? FO_COPY : FO_MOVE;
            op.pFrom = from_zz.c_str();
            op.pTo = to_zz.c_str();
            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
            ok = (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted);
        }
        return ok ? dst : std::wstring{};
    }

    std::unordered_set<std::wstring> EvacuateMappedItemsToDesktop(const std::vector<std::wstring>& paths) {
        const std::wstring desk = mount::ResolveKnownFolder(FOLDERID_Desktop);
        std::unordered_set<std::wstring> dest_keys;
        bool physical_moved = false;
        for (const auto& tp : paths) {
            const std::wstring tp_key = NormalizeFolderKey(tp);
            bool is_mapped_child = false;
            for (const auto& it : state_.items) {
                if (NormalizeFolderKey(it.path) == tp_key && !it.mapped_owner_group_id.empty()) {
                    is_mapped_child = true;
                    break;
                }
            }
            if (is_mapped_child && !desk.empty()) {
                const std::wstring moved = MoveOrCopyToDirectoryCollisionSafe(tp, desk, false);
                if (!moved.empty()) {
                    dest_keys.insert(NormalizeFolderKey(moved));
                    physical_moved = true;
                    continue;
                }
            }
            dest_keys.insert(tp_key);
        }
        if (physical_moved) {
            RefreshDesktopItems();
            RefreshHealthStats();
            interaction_.selected_paths.clear();
        }
        return dest_keys;
    }

    void SyncWatcherDirectories(bool force = false) {
        std::vector<std::wstring> desired;
        std::unordered_set<std::wstring> seen;
        for (const auto& g : state_.groups) {
            if (g.mapped_folder_path.empty()) continue;
            const std::wstring key = NormalizeFolderKey(g.mapped_folder_path);
            if (!key.empty() && seen.insert(key).second) {
                desired.push_back(g.mapped_folder_path);
            }
        }
        if (!force && desired.size() == watched_extra_dirs_.size()) {
            bool same = true;
            for (size_t i = 0; i < desired.size(); ++i) {
                if (NormalizeFolderKey(desired[i]) != NormalizeFolderKey(watched_extra_dirs_[i])) {
                    same = false;
                    break;
                }
            }
            if (same) return;
        }
        watched_extra_dirs_ = desired;
        watcher_.Start(
            [hwnd = mount_.Hwnd()]() {
                if (hwnd && IsWindow(hwnd)) PostMessageW(hwnd, kMsgFolderChanged, 0, 0);
            },
            watched_extra_dirs_);
    }

    void BindGroupToFolder(size_t group_idx, const std::wstring& raw_dir, bool push_undo = true) {
        if (group_idx >= state_.groups.size() || raw_dir.empty()) return;
        std::wstring clean_dir = raw_dir;
        while (clean_dir.size() > 3 && (clean_dir.back() == L'\\' || clean_dir.back() == L'/')) {
            clean_dir.pop_back();
        }
        const DWORD attrs = GetFileAttributesW(clean_dir.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            ShowUndoToast(L"所选路径不是有效文件夹");
            return;
        }

        const std::wstring leaf = MappedFolderLeafName(clean_dir);
        const std::wstring target_key = NormalizeFolderKey(clean_dir);
        if (push_undo) {
            PushUndo(L"关联文件夹");
        }
        if (state_.groups[group_idx].rule.fallback_catch_all) {
            model::DesktopGroup ng{};
            ng.id = "grp_" + std::to_string(NowUnixMs());
            ng.title = leaf;
            ng.icon_glyph = L'\xE8B7';
            ng.x = state_.groups[group_idx].x + 28;
            ng.y = state_.groups[group_idx].y + 28;
            state_.groups.push_back(std::move(ng));
            group_idx = state_.groups.size() - 1;
        }
        auto& g = state_.groups[group_idx];
        g.mapped_folder_path = clean_dir;
        g.collapsed = false;
        if (g.title.empty() || g.title == L"新建分组") {
            g.title = leaf;
        }
        if (g.icon_glyph == 0) {
            g.icon_glyph = L'\xE8B7';
        }
        interaction_.group_pages[g.id] = 0;

        for (auto& it : state_.items) {
            if (it.is_folder && NormalizeFolderKey(it.path) == target_key) {
                it.pinned_group_id.clear();
            }
        }

        SyncWatcherDirectories();
        RefreshDesktopItems();
        RefreshHealthStats();
        RecomputeLayout();
        SaveModel();
        renderer_.InvalidateAll();
        ShowUndoToast(L"已将「" + leaf + L"」内容显示到此分组");
    }

    void FillGroupContextMenuSpec(size_t group_idx, overlays::ContextMenuSpec& spec) {
        if (group_idx >= state_.groups.size()) return;
        const auto& g = state_.groups[group_idx];
        spec.target_kind = overlays::ContextMenuTargetKind::GroupHeader;
        spec.ai_organize_enabled = AiOrganizerEnabled();
        spec.target_group_id = g.id;
        spec.group_collapsed = g.collapsed;
        spec.group_view_mode = g.view_mode;
        spec.group_tabbed = model::TabMembers(state_.groups, g.id).size() > 1;
        spec.group_focus_visible = g.focus_visible;
        spec.group_cols = g.cols;
        spec.group_material = g.material;
        spec.group_sort = g.sort_by;
        spec.group_mapped_folder_path = g.mapped_folder_path;
        if (group_idx < group_views_.size()) {
            spec.group_single_subfolder_path = group_views_[group_idx].single_subfolder_path;
            spec.group_single_subfolder_name = group_views_[group_idx].single_subfolder_name;
        }
    }

    void RefreshDesktopItems(const mount::NativeIconSnapshot* snap = nullptr) {
        std::unordered_set<std::wstring> mapped_folder_keys;
        for (const auto& g : state_.groups) {
            if (!g.mapped_folder_path.empty()) {
                mapped_folder_keys.insert(NormalizeFolderKey(g.mapped_folder_path));
            }
        }

        std::unordered_map<std::wstring, const model::DesktopItem*> old_by_key;
        old_by_key.reserve(state_.items.size());
        for (const auto& old : state_.items) {
            old_by_key.emplace(NormalizeFolderKey(old.path), &old);
        }

        auto fresh = mount::EnumerateDesktopItems(snap);
        if (snap) {
            // Explorer's icon positions are physical pixels; the layout runs in DIPs.
            const float s = mount_.DipScale() > 0.0f ? mount_.DipScale() : 1.0f;
            for (auto& fi : fresh) {
                fi.loose_pos.x = static_cast<int>(std::lround(fi.loose_pos.x / s));
                fi.loose_pos.y = static_cast<int>(std::lround(fi.loose_pos.y / s));
            }
        }
        for (auto& fi : fresh) {
            const std::wstring key = NormalizeFolderKey(fi.path);
            if (fi.is_folder && mapped_folder_keys.contains(key)) {
                fi.hidden_by_mapped_group = true;
            }
            if (const auto oit = old_by_key.find(key); oit != old_by_key.end()) {
                const auto& old = *oit->second;
                fi.pinned_group_id = old.pinned_group_id;
                fi.force_loose = old.force_loose;
                fi.loose_pos = old.loose_pos;
                fi.tag_id = old.tag_id;
                fi.tag_argb = old.tag_argb;
            }
        }

        for (const auto& g : state_.groups) {
            if (g.mapped_folder_path.empty()) continue;
            auto mapped_items = mount::EnumerateDirectoryItems(g.mapped_folder_path, g.id, 500);
            for (auto& mi : mapped_items) {
                if (const auto oit = old_by_key.find(NormalizeFolderKey(mi.path)); oit != old_by_key.end()) {
                    mi.tag_id = oit->second->tag_id;
                    mi.tag_argb = oit->second->tag_argb;
                }
                fresh.push_back(std::move(mi));
            }
        }
        state_.items = std::move(fresh);
        items_paths_dirty_ = true;
    }

    void RefreshHealthStats() {
        if (!state_.settings.health_check_enabled) {
            cached_health_report_ = {};
            interaction_.cleanable_installer_bytes = 0;
            return;
        }
        if (installed_apps_cache_.empty()) {
            installed_apps_cache_ = model::QueryInstalledAppsFromRegistry();
        }
        const int64_t now_ms = NowUnixMs();
        cached_health_report_ = model::AnalyzeDesktopHealth(state_.items, installed_apps_cache_, now_ms);

        // Also surface installer & archive packages inside the 安装包 group so the mockup's
        // [🗑 可清理 XXX MB] pill reflects total reclaimable installer/archive space and opens Health Check cleanly.
        std::optional<size_t> installer_gi;
        for (size_t gi = 0; gi < state_.groups.size(); ++gi) {
            if (state_.groups[gi].id == "grp_installers" || state_.groups[gi].title == L"安装包") {
                installer_gi = gi;
                break;
            }
        }
        if (installer_gi) {
            for (const auto& it : state_.items) {
                if (it.is_folder || it.size_bytes == 0) continue;
                if (model::ClassifyItemToGroup(it, state_.groups, now_ms) != *installer_gi) continue;
                bool already_listed = false;
                for (auto& iss : cached_health_report_.issues) {
                    if (_wcsicmp(iss.path.c_str(), it.path.c_str()) == 0) {
                        already_listed = true;
                        break;
                    }
                }
                if (!already_listed) {
                    const bool is_setup_bin =
                        (it.extension == L".exe" || it.extension == L".msi" ||
                         it.extension == L".msix" || it.extension == L".appx");
                    model::HealthIssue extra{};
                    extra.kind = model::HealthIssueKind::InstalledInstaller;
                    extra.path = it.path;
                    extra.name = it.name;
                    extra.reclaimable_bytes = it.size_bytes;
                    extra.reason = is_setup_bin ? L"桌面安装程序 · 安装后可归档或清理"
                                                : L"桌面压缩包 · 解压后可归档或清理";
                    extra.selected_by_default = is_setup_bin;
                    cached_health_report_.issues.push_back(std::move(extra));
                    cached_health_report_.total_reclaimable_bytes += it.size_bytes;
                }
            }
        }

        uint64_t inst_bytes = 0;
        std::unordered_set<std::wstring> counted;
        for (const auto& iss : cached_health_report_.issues) {
            if (iss.kind == model::HealthIssueKind::InstalledInstaller ||
                (installer_gi && iss.kind == model::HealthIssueKind::OneTimeUse)) {
                std::wstring key = iss.path;
                for (auto& ch : key) ch = static_cast<wchar_t>(std::towlower(ch));
                if (counted.insert(std::move(key)).second) {
                    inst_bytes += iss.reclaimable_bytes;
                }
            }
        }
        interaction_.cleanable_installer_bytes = inst_bytes;
    }

    void RefreshLiveTileData() {
        if (!state_.settings.tiles_enabled) return;
        const std::wstring dl = mount::ResolveKnownFolder(FOLDERID_Downloads);
        if (!dl.empty()) live_data_.download_items = mount::QueryFolderTopItems(dl, 24);

        const std::wstring rec = mount::ResolveKnownFolder(FOLDERID_Recent);
        if (!rec.empty()) live_data_.recent_items = mount::QueryFolderTopItems(rec, 24);

        std::wstring pics = mount::ResolveKnownFolder(FOLDERID_Pictures);
        for (const auto& t : state_.tiles) {
            if (model::ActiveTileView(t).kind == model::TileKind::Photos && !t.target_path.empty()) {
                pics = t.target_path;
                break;
            }
        }
        if (!pics.empty()) {
            const auto pic_items = mount::QueryFolderTopItems(pics, 64);
            live_data_.photo_paths.clear();
            for (const auto& m : pic_items) {
                if (m.extension == L".jpg" || m.extension == L".jpeg" ||
                    m.extension == L".png" || m.extension == L".webp") {
                    live_data_.photo_paths.push_back(m.path);
                }
            }
            if (!live_data_.photo_paths.empty()) {
                live_data_.photo_total_count = live_data_.photo_paths.size();
                live_data_.active_photo_index %= live_data_.photo_paths.size();
            }
        }

        live_data_.pinned_folder_items.clear();
        for (const auto& t : state_.tiles) {
            if (t.kind == model::TileKind::PinnedFolder && !t.target_path.empty() &&
                !live_data_.pinned_folder_items.contains(t.target_path)) {
                live_data_.pinned_folder_items[t.target_path] = mount::QueryFolderTopItems(t.target_path, 24);
            }
            for (const auto& m : t.stack) {
                if (m.kind == model::TileKind::PinnedFolder && !m.target_path.empty() &&
                    !live_data_.pinned_folder_items.contains(m.target_path)) {
                    live_data_.pinned_folder_items[m.target_path] = mount::QueryFolderTopItems(m.target_path, 24);
                }
            }
        }
        live_data_.recycle_bin = mount::QueryRecycleBinStats();

        live_data_.drives.clear();
        const DWORD mask = GetLogicalDrives();
        for (int d = 0; d < 26; ++d) {
            if ((mask & (1u << d)) == 0) continue;
            wchar_t root[4] = {static_cast<wchar_t>(L'A' + d), L':', L'\\', 0};
            if (GetDriveTypeW(root) == DRIVE_FIXED) {
                auto st = mount::QueryDriveSpaceStats(root);
                if (st.total_bytes > 0) live_data_.drives.push_back(std::move(st));
            }
        }
        if (live_data_.drives.empty()) {
            live_data_.drives = {mount::QueryDriveSpaceStats(L"C:\\")};
        }
    }

    void ApplySystemAppearance() {
        const auto appearance = render::ReadSystemAppearance();
        state_.settings.dark_mode = appearance.dark;
        interaction_.host_dark_mode = appearance.dark;
        interaction_.host_accent_argb = appearance.accent_argb;
        interaction_.transparency_enabled = appearance.transparency;
        overlays_.SetAppearance(appearance.dark, EffectiveAccentArgb());
        overlays_.SetHostMaterial(interaction_.host_material);
    }

    std::vector<model::RectI> CurrentGroupFrames() const {
        std::vector<model::RectI> frames;
        if (!state_.settings.organize_enabled || state_.settings.icons_hidden ||
            state_.settings.active_scene == model::DesktopScene::Present) {
            return frames;
        }
        const bool focus_scene = (state_.settings.active_scene == model::DesktopScene::Focus);
        frames.reserve(group_views_.size());
        for (size_t i = 0; i < group_views_.size() && i < state_.groups.size(); ++i) {
            model::RectI f = group_views_[i].frame;
            if (f.empty()) continue;
            if (state_.groups[i].collapsed || (focus_scene && !state_.groups[i].focus_visible)) {
                f.h = model::kGroupHeaderHeight;
            }
            frames.push_back(f);
        }
        return frames;
    }

    std::vector<model::RectI> CurrentTileObstacles() const {
        const bool show_board = state_.settings.tiles_enabled &&
                                state_.settings.organize_enabled &&
                                !interaction_.side_drawer_open;
        return model::CollectTileObstacleRects(state_.tiles, tile_board_rect_, show_board);
    }

    model::DragDamageSnapshot CaptureDragDamage() const {
        model::DragDamageSnapshot snapshot;
        snapshot.frames = CurrentGroupFrames();
        const auto tiles = CurrentTileObstacles();
        snapshot.frames.insert(snapshot.frames.end(), tiles.begin(), tiles.end());
        snapshot.preview = interaction_.dragging_tile ? interaction_.drag_tile_preview_rect : model::RectI{};
        snapshot.stack_target = interaction_.stack_drop_target_tile_id;
        return snapshot;
    }

    void InvalidateDragDamage(const model::DragDamageSnapshot& before) {
        const auto damage = model::ComputeDragDamage(before, CaptureDragDamage());
        if (damage.empty()) return;
        // Native acrylic records the complete card visual list each frame.
        if (renderer_.NativeAcrylicActive()) renderer_.InvalidateAll();
        else renderer_.InvalidateRect(damage);
    }

    void RefreshGroupViews() {
        if (!group_hover_.Expanded().empty() && std::none_of(state_.groups.begin(), state_.groups.end(),
                [&](const auto& g) { return g.id == group_hover_.Expanded(); })) group_hover_.Reset();
        group_views_ = model::ComputeGroupViews(state_, NowUnixMs(), &interaction_.group_pages, group_hover_.Expanded());
    }

    std::vector<size_t> GroupHitOrder() const {
        std::vector<size_t> order;
        for (size_t i = 0; i < group_views_.size() && i < state_.groups.size(); ++i)
            if (!group_views_[i].frame.empty() && group_views_[i].group_id == group_hover_.Expanded()) order.push_back(i);
        for (size_t i = group_views_.size(); i > 0; --i) {
            const size_t at = i - 1;
            if (at < state_.groups.size() && !group_views_[at].frame.empty() &&
                group_views_[at].group_id != group_hover_.Expanded()) order.push_back(at);
        }
        return order;
    }

    void ArmGroupInteractionTimer() {
        if (mount_.Hwnd()) SetTimer(mount_.Hwnd(), kTimerGroupInteraction, 50, nullptr);
    }

    void UpdateGroupHoverAt(int x, int y, bool on_desktop, uint64_t now) {
        std::string id;
        bool collapsed = false;
        if (on_desktop && state_.settings.organize_enabled && !state_.settings.icons_hidden &&
            state_.settings.active_scene != model::DesktopScene::Present && !group_drag_session_.Pending()) {
            for (const size_t i : GroupHitOrder()) {
                if (!group_views_[i].frame.contains(x, y)) continue;
                id = state_.groups[i].id;
                collapsed = state_.groups[i].collapsed ||
                    (state_.settings.active_scene == model::DesktopScene::Focus && !state_.groups[i].focus_visible);
                break;
            }
        }
        const bool keep = overlays_.IsOpen() || overlays_.IsInlineRenaming() ||
            ((interaction_.dragging_items || interaction_.drop_hint_visible) && id == group_hover_.Expanded());
        if (group_hover_.Update(id, collapsed, keep, now)) {
            RefreshGroupViews();
            renderer_.InvalidateAll();
        }
        if (group_hover_.Pending() || group_drag_session_.Pending()) ArmGroupInteractionTimer();
        else if (mount_.Hwnd()) KillTimer(mount_.Hwnd(), kTimerGroupInteraction);
    }

    void BeginGroupDrag(size_t index, int x, int y, bool resize) {
        if (index >= state_.groups.size()) return;
        CancelTileDrag(false);
        if (group_hover_.Pending()) { group_hover_.Reset(); RefreshGroupViews(); }
        drag_group_undo_snapshot_ = state_;
        group_drag_session_.Begin(state_.groups, group_views_, index, {x, y}, resize,
                                  PrimaryWorkAreaClient(), state_.settings.icon_size, CurrentTileObstacles());
        pending_group_idx_ = index;
        pending_group_tab_id_.clear();
        if (resize) resizing_group_idx_ = index;
        ArmGroupInteractionTimer();
    }

    void UpdateGroupDrag(int x, int y, uint64_t now) {
        if (!group_drag_session_.Pending()) return;
        const auto before = CaptureDragDamage();
        if (group_drag_session_.Update(state_.groups, {x, y}, now)) {
            if (!group_drag_session_.Resizing()) dragging_group_idx_ = pending_group_idx_;
            if (group_drag_session_.Resizing() || !model::UpdateMovedGroupViewOrigins(state_.groups, group_views_))
                RefreshGroupViews();
            const std::string target = group_drag_session_.MergeReady(now) ? group_drag_session_.MergeTarget() : std::string();
            if (interaction_.drop_target_group_id != target) {
                interaction_.drop_target_group_id = target;
                renderer_.InvalidateAll();
            }
            InvalidateDragDamage(before);
        }
    }

    void CancelGroupDrag(bool redraw = true) {
        if (!group_drag_session_.Pending()) return;
        group_drag_session_.Cancel(state_.groups);
        interaction_.drop_target_group_id.clear();
        pending_group_idx_.reset(); dragging_group_idx_.reset(); resizing_group_idx_.reset();
        pending_group_tab_id_.clear();
        drag_group_undo_snapshot_ = {};
        RefreshGroupViews();
        if (redraw) renderer_.InvalidateAll();
    }

    void FinishGroupDrag(int x, int y) {
        if (!group_drag_session_.Pending()) return;
        UpdateGroupDrag(x, y, GetTickCount64());
        const bool resize = group_drag_session_.Resizing();
        if (group_drag_session_.MergeReady(GetTickCount64()) && pending_group_idx_ &&
            model::TabMembers(group_drag_session_.Original(), group_drag_session_.Original()[*pending_group_idx_].id).size() +
            model::TabMembers(group_drag_session_.Original(), group_drag_session_.MergeTarget()).size() > model::kMaxGroupTabs) {
            CancelGroupDrag();
            ShowUndoToast(l10n::Get(l10n::StringId::DesktopGroupTabsFull));
            return;
        }
        interaction_.drop_target_group_id.clear();
        bool merged = false;
        const bool changed = group_drag_session_.Commit(state_.groups, GetTickCount64(), merged);
        bool selected = false;
        if (!pending_group_tab_id_.empty()) {
            const auto tab = std::find_if(state_.groups.begin(), state_.groups.end(),
                [&](const auto& g) { return g.id == pending_group_tab_id_; });
            if (tab != state_.groups.end() && !tab->tab_active)
                selected = model::SelectGroupTab(state_.groups, pending_group_tab_id_);
        }
        pending_group_tab_id_.clear();
        pending_group_idx_.reset(); dragging_group_idx_.reset(); resizing_group_idx_.reset();
        if (changed || selected) {
            auto undo_state = state_;
            undo_state.groups = drag_group_undo_snapshot_.groups;
            undo_.Push(undo_state, merged ? l10n::Get(l10n::StringId::DesktopGroupMergeTabs) :
                (!changed ? l10n::Get(l10n::StringId::DesktopGroupSelectTab) :
                 (resize ? l10n::Get(l10n::StringId::DesktopGroupResize) : l10n::Get(l10n::StringId::DesktopGroupMove))));
            last_undo_is_ai_archive_ = false;
            RecomputeLayout();
            SaveModel();
        } else RefreshGroupViews();
        drag_group_undo_snapshot_ = {};
        renderer_.InvalidateAll();
    }

    void RecomputeLayout() {
        // A changed work area, files or groups invalidates the drag's fixed obstacle snapshot.
        if (tile_drag_session_.Active()) CancelTileDrag(false);
        if (group_drag_session_.Pending()) CancelGroupDrag(false);
        model::NormalizeGroupTabs(state_.groups);
        SyncOverlayInteractionFlags();
        const model::RectI wa = PrimaryWorkAreaClient();
        const bool show_board = state_.settings.tiles_enabled &&
                                state_.settings.organize_enabled &&
                                !interaction_.side_drawer_open;
        tile_board_rect_ = model::ComputeTileBoardRect(wa, show_board);
        const auto tile_obs = model::CollectTileObstacleRects(state_.tiles, tile_board_rect_, show_board);
        model::UpdateAppsTileMembership(state_);
        model::UpdateGroupContentCounts(state_, NowUnixMs());
        model::ResolveGroupCollisions(state_.groups, std::nullopt, wa, model::RectI{}, state_.settings.icon_size, tile_obs);
        group_views_ = model::ComputeGroupViews(state_, NowUnixMs(), &interaction_.group_pages);
        for (const auto& gv : group_views_) {
            if (gv.page_count <= 1) {
                interaction_.group_pages.erase(gv.group_id);
            } else {
                interaction_.group_pages[gv.group_id] = gv.page_index;
            }
        }
        model::SettleLooseIcons(state_.items, group_views_, wa, model::RectI{},
                                state_.settings.icon_size,
                                state_.settings.organize_enabled,
                                state_.settings.align_to_grid,
                                state_.settings.loose_sort_by,
                                state_.settings.loose_sort_ascending,
                                tile_obs);
        if (!group_hover_.Expanded().empty()) RefreshGroupViews();
    }

    void PushUndo(std::wstring label) {
        undo_.Push(state_, std::move(label));
        last_undo_is_ai_archive_ = false;
    }

    void PerformUndo() {
        if (last_undo_is_ai_archive_ && !last_ai_archive_undo_.empty()) {
            size_t restored_files = 0;
            model::DesktopModelState restored_model = state_;
            if (ai::UndoAiArchiveTransaction(last_ai_archive_undo_, restored_model, &restored_files)) {
                last_ai_archive_undo_ = {};
                last_undo_is_ai_archive_ = false;
                pending_ai_archive_toast_count_ = 0;
                state_ = std::move(restored_model);
                RefreshDesktopItems();
                RefreshLiveTileData();
                RefreshHealthStats();
                renderer_.ReloadWallpaper(state_.settings.wallpaper_preset, state_.settings.dark_mode);
                RecomputeLayout();
                SaveModel();
                SyncSettingsToPlugin();
                renderer_.InvalidateAll();
                overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
                overlays_.ShowToast(PrimaryWorkAreaScreen(),
                                    L"已无损恢复 " + std::to_wstring(restored_files) + L" 个归档文件至桌面原位");
                SyncOverlayInteractionFlags();
            }
            return;
        }
        if (undo_.Undo(state_)) {
            SyncWatcherDirectories();
            RefreshDesktopItems();
            RefreshLiveTileData();
            RefreshHealthStats();
            renderer_.ReloadWallpaper(state_.settings.wallpaper_preset, state_.settings.dark_mode);
            RecomputeLayout();
            SaveModel();
            SyncSettingsToPlugin();
            renderer_.InvalidateAll();
        }
    }

    void ShowUndoToast(const std::wstring& msg) {
        overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
        overlays_.ShowToast(PrimaryWorkAreaScreen(), msg, L"撤销", [this]() { PerformUndo(); });
        SyncOverlayInteractionFlags();
    }

    std::vector<overlays::SearchHitEntry> BuildLocalSearchHits(std::wstring_view q) const {
        std::vector<overlays::SearchHitEntry> local;
        if (q.empty()) return local;
        for (size_t idx = 0; idx < state_.items.size(); ++idx) {
            const auto& it = state_.items[idx];
            if (!model::MatchesDesktopSearchQuery(it, q)) continue;
            std::wstring loc = L"桌面";
            for (size_t gi = 0; gi < group_views_.size() && gi < state_.groups.size(); ++gi) {
                const auto& gv = group_views_[gi];
                if (std::find(gv.member_item_indices.begin(), gv.member_item_indices.end(), idx) !=
                    gv.member_item_indices.end() ||
                    it.pinned_group_id == state_.groups[gi].id) {
                    loc = state_.groups[gi].title;
                    break;
                }
            }
            local.push_back({it.path, it.name, it.size_bytes, it.is_folder, true, std::move(loc)});
        }
        return local;
    }

    void LaunchPulseUriFallback(std::wstring_view uri) const {
        if (uri.empty() || parent_pid_ == 0) return;
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent_pid_);
        if (!proc) return;
        wchar_t pulse_exe[MAX_PATH * 4]{};
        DWORD len = ARRAYSIZE(pulse_exe);
        const BOOL ok = QueryFullProcessImageNameW(proc, 0, pulse_exe, &len);
        CloseHandle(proc);
        if (!ok || len == 0) return;

        AllowSetForegroundWindow(ASFW_ANY);
        std::wstring cmdline = L"\"" + std::wstring(pulse_exe, len) + L"\" \"" + std::wstring(uri) + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(pulse_exe, cmdline.data(), nullptr, nullptr, FALSE,
                           0, nullptr, nullptr, &si, &pi)) {
            if (pi.dwProcessId != 0) AllowSetForegroundWindow(pi.dwProcessId);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    void OpenPathInPulseOrShell(const std::wstring& path, const model::RectI* client_origin_rc = nullptr) {
        if (path.empty()) return;
        if (pipe_.IsConnected()) {
            AllowSetForegroundWindow(ASFW_ANY);
            RECT scr{0, 0, 0, 0};
            if (client_origin_rc && mount_.Hwnd()) {
                const POINT tl = mount_.ClientDipToScreenPx(POINT{client_origin_rc->x, client_origin_rc->y});
                const POINT br = mount_.ClientDipToScreenPx(POINT{client_origin_rc->right(), client_origin_rc->bottom()});
                scr = RECT{tl.x, tl.y, br.x, br.y};
            }
            char rect_json[160]{};
            std::snprintf(rect_json, sizeof(rect_json),
                          ",\"rect_x\":%ld,\"rect_y\":%ld,\"rect_w\":%ld,\"rect_h\":%ld",
                          scr.left, scr.top, scr.right - scr.left, scr.bottom - scr.top);
            pipe_.Send(ipc::MessageType::OpenInPulseReq, ++next_req_id_,
                       "{\"mode\":\"open\",\"path\":" + ipc::JsonQuoteWide(path) + rect_json + "}");
        } else {
            LaunchAsync(path);
        }
    }

    // -----------------------------------------------------------------------
    // Overlays & OLE Drag-Drop Wiring
    // -----------------------------------------------------------------------
    void SetupOverlays() {
        overlays::OverlayCallbacks cb{};
        cb.on_menu_command = [this](const std::string& cmd, const overlays::ContextMenuSpec& spec) {
            ExecuteContextMenuCommand(cmd, spec);
        };
        cb.on_invoke_shell_verb = [this](uint64_t sid, uint32_t iid, const std::wstring& verb) {
            const std::string json = "{\"session_id\":" + std::to_string(sid) +
                                     ",\"item_id\":" + std::to_string(iid) +
                                     ",\"verb\":" + ipc::JsonQuoteWide(verb) + "}";
            pipe_.Send(ipc::MessageType::ShellMenuInvokeReq, ++next_req_id_, json);
        };
        cb.on_close_shell_session = [this](uint64_t sid) {
            const std::string json = "{\"session_id\":" + std::to_string(sid) + ",\"close\":true}";
            pipe_.Send(ipc::MessageType::ShellMenuInvokeReq, ++next_req_id_, json);
        };
        cb.on_prepare_menu_underlay = [this](POINT anchor_screen_px) {
            renderer_.CaptureMenuUnderlay(
                anchor_screen_px, state_, group_views_, tile_board_rect_, live_data_, interaction_);
        };
        cb.on_search_query_changed = [this](const std::wstring& q) {
            interaction_.search_highlight_query = q;
            if (!q.empty()) {
                bool page_changed = false;
                for (const auto& gv : group_views_) {
                    if (gv.page_count <= 1 || gv.page_capacity == 0) continue;
                    auto matches = [&](size_t idx) {
                        return idx < state_.items.size() &&
                               model::MatchesDesktopSearchQuery(state_.items[idx], q);
                    };
                    bool visible_has_match = false;
                    for (size_t idx : gv.visible_item_indices) {
                        if (matches(idx)) {
                            visible_has_match = true;
                            break;
                        }
                    }
                    if (!visible_has_match) {
                        for (size_t pos = 0; pos < gv.member_item_indices.size(); ++pos) {
                            if (matches(gv.member_item_indices[pos])) {
                                interaction_.group_pages[gv.group_id] = static_cast<int>(pos / gv.page_capacity);
                                page_changed = true;
                                break;
                            }
                        }
                    }
                }
                if (page_changed) {
                    RefreshGroupViews();
                }
            }
            overlays_.UpdateLocalSearchHits(BuildLocalSearchHits(q));
            renderer_.InvalidateAll();
            if (!q.empty()) {
                pipe_.Send(ipc::MessageType::SearchQueryReq, ++next_req_id_,
                           "{\"query\":" + ipc::JsonQuoteWide(q) + "}");
            }
        };
        cb.on_open_path = [this](const std::wstring& p) {
            interaction_.search_highlight_query.clear();
            SyncOverlayInteractionFlags();
            renderer_.InvalidateAll();
            if (!p.empty()) LaunchAsync(p);
        };
        cb.on_open_search_in_pulse = [this](const std::wstring& q) {
            interaction_.search_highlight_query.clear();
            SyncOverlayInteractionFlags();
            renderer_.InvalidateAll();
            pipe_.Send(ipc::MessageType::OpenInPulseReq, ++next_req_id_,
                       "{\"mode\":\"search\",\"query\":" + ipc::JsonQuoteWide(q) + "}");
        };
        cb.on_cycle_quick_look = [this](int delta) { CycleQuickLook(delta); };
        cb.on_execute_health_action = [this](bool archive, const std::vector<std::wstring>& paths) {
            ExecuteHealthRemediation(archive, paths);
        };
        cb.on_undo_requested = [this]() { PerformUndo(); };
        auto apply_beautify = [this](const model::BeautifySettings& s, bool dragging) {
            const bool wp_changed = (s.wallpaper_preset != state_.settings.wallpaper_preset) ||
                                    (s.dark_mode != state_.settings.dark_mode);
            const bool layout_changed =
                (s.icon_size != state_.settings.icon_size) ||
                (s.tiles_enabled != state_.settings.tiles_enabled) ||
                (s.organize_enabled != state_.settings.organize_enabled) ||
                (s.active_scene != state_.settings.active_scene) ||
                (s.icons_hidden != state_.settings.icons_hidden);
            state_.settings = s;
            if (wp_changed) {
                renderer_.ReloadWallpaper(state_.settings.wallpaper_preset, state_.settings.dark_mode);
                SyncSystemDesktopWallpaper(state_.settings.wallpaper_preset, state_.settings.dark_mode, false);
            }
            if (!dragging) {
                overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
            }
            if (layout_changed) {
                RecomputeLayout();
            }
            if (!dragging) {
                SaveModel();
                SyncSettingsToPlugin();
            }
            renderer_.InvalidateAll();
        };
        cb.on_beautify_changed = [apply_beautify](const model::BeautifySettings& s) {
            apply_beautify(s, false);
        };
        cb.on_beautify_live_changed = apply_beautify;
        cb.on_toggle_widget_tile = [this](model::TileKind wk) {
            PushUndo(L"切换桌面小工具");
            auto it = std::find_if(state_.tiles.begin(), state_.tiles.end(),
                                   [wk](const model::DesktopTile& t) {
                                       if (t.kind == wk) return true;
                                       for (const auto& m : t.stack) if (m.kind == wk) return true;
                                       return false;
                                   });
            if (it != state_.tiles.end()) {
                if (it->stack.size() > 1) {
                    std::erase_if(it->stack, [wk](const model::TileStackEntry& m) { return m.kind == wk; });
                    it->stack_index = std::clamp(it->stack_index, 0, static_cast<int>(it->stack.size()) - 1);
                    if (it->stack.size() == 1) {
                        it->kind = it->stack[0].kind;
                        it->title = it->stack[0].title;
                        it->target_path = it->stack[0].target_path;
                        it->analog_clock = it->stack[0].analog_clock;
                        it->stack.clear();
                        it->stack_index = 0;
                    }
                } else {
                    state_.tiles.erase(it);
                }
            } else {
                InsertOrStackWidgetKind(wk);
            }
            RecomputeLayout();
            SaveModel();
            if (overlays_.ActiveKind() == overlays::OverlayKind::BeautifyDrawer) {
                overlays_.OpenBeautifyDrawer(PrimaryWorkAreaScreen(), state_.settings, ActiveWidgetKinds());
            } else if (overlays_.ActiveKind() == overlays::OverlayKind::WidgetGalleryDrawer) {
                overlays_.UpdateWidgetGalleryTiles(state_.tiles);
            }
            renderer_.InvalidateAll();
        };
        cb.on_request_open_widget_gallery = [this](std::optional<model::TileKind> wk) {
            OpenWidgetGallery(wk, std::nullopt);
        };
        cb.on_add_widget_tile = [this](model::TileKind wk, model::TileSize sz, std::optional<model::PointI> target_cell) {
            if (model::BoardHasKind(state_.tiles, wk)) {
                overlays_.UpdateWidgetGalleryTiles(state_.tiles);
                return;
            }
            PushUndo(L"添加小组件");
            state_.settings.tiles_enabled = true;
            state_.settings.organize_enabled = true;
            if (wk == model::TileKind::StagingTray) state_.settings.sync_staging_tray = true;
            model::DesktopTile nt = MakeWidgetTileForKind(wk, sz);

            const bool any_free = std::any_of(state_.tiles.begin(), state_.tiles.end(),
                                              [](const model::DesktopTile& t) { return t.free_placed; });
            auto fits_at = [&](int c, int r, model::TileSize s) -> bool {
                if (any_free) return false;
                if (c < 0 || r < 0 || c + s.cols > model::kTileCols || r + s.rows > model::kTileRows) return false;
                for (const auto& o : state_.tiles) {
                    const bool ox = !(c + s.cols <= o.col || o.col + o.size.cols <= c);
                    const bool oy = !(r + s.rows <= o.row || o.row + o.size.rows <= r);
                    if (ox && oy) return false;
                }
                return true;
            };

            bool placed = false;
            if (target_cell && fits_at(target_cell->x, target_cell->y, sz)) {
                nt.col = target_cell->x;
                nt.row = target_cell->y;
                state_.tiles.push_back(nt);
                placed = true;
            } else if (!any_free && model::FindFreeTileCell(state_.tiles, sz)) {
                const auto free_spot = *model::FindFreeTileCell(state_.tiles, sz);
                nt.col = free_spot.x;
                nt.row = free_spot.y;
                state_.tiles.push_back(nt);
                placed = true;
            } else {
                auto ins = !any_free ? model::InsertNewTile(state_.tiles, nt) : model::TilePlacementResult{};
                if (ins.valid) {
                    state_.tiles = std::move(ins.updated_tiles);
                    placed = true;
                } else {
                    const model::RectI wa = PrimaryWorkAreaClient();
                    const model::RectI br = !tile_board_rect_.empty() ? tile_board_rect_ : model::ComputeTileBoardRect(wa, true);
                    nt.w = model::DefaultTileSpanPx(sz.cols);
                    nt.h = model::DefaultTileSpanPx(sz.rows);
                    const model::PointI spot = model::FindFreeDesktopSpotForTile(
                        state_.tiles, CurrentGroupFrames(), wa, br, nt.w, nt.h);
                    nt.free_placed = true;
                    nt.x = spot.x;
                    nt.y = spot.y;
                    state_.tiles.push_back(nt);
                    placed = true;
                }
            }

            if (!placed) {
                model::DesktopModelState discarded = state_;
                undo_.Undo(discarded);
                return;
            }

            if (wk == model::TileKind::Weather) RequestWeather(true);
            if (wk == model::TileKind::System) SampleSystemMetrics();
            RefreshLiveTileData();
            RecomputeLayout();
            SaveModel();
            SyncSettingsToPlugin();
            if (overlays_.ActiveKind() == overlays::OverlayKind::WidgetGalleryDrawer) {
                overlays_.UpdateWidgetGalleryTiles(state_.tiles);
            }
            renderer_.InvalidateAll();
        };
        cb.on_tray_remove_item = [this](uint64_t id) {
            pipe_.Send(ipc::MessageType::TrayMutateReq, ++next_req_id_,
                       "{\"op\":\"remove\",\"item_id\":" + std::to_string(id) + "}");
        };
        cb.on_tray_clear = [this]() {
            pipe_.Send(ipc::MessageType::TrayMutateReq, ++next_req_id_, "{\"op\":\"clear\"}");
        };
        cb.on_tray_release_to_desktop = [this]() {
            const std::wstring desk = mount::ResolveKnownFolder(FOLDERID_Desktop);
            if (!desk.empty()) {
                pipe_.Send(ipc::MessageType::TrayMutateReq, ++next_req_id_,
                           "{\"op\":\"release\",\"dest\":" + ipc::JsonQuoteWide(desk) + "}");
            }
        };
        cb.on_tray_open_in_pulse = [this]() {
            const std::wstring desk = mount::ResolveKnownFolder(FOLDERID_Desktop);
            pipe_.Send(ipc::MessageType::OpenInPulseReq, ++next_req_id_,
                       "{\"mode\":\"tray\",\"path\":" + ipc::JsonQuoteWide(desk) + "}");
        };
        cb.on_group_rule_changed = [this](const std::string& gid, const model::GroupRule& r) {
            PushUndo(L"修改整理规则");
            for (auto& g : state_.groups) {
                if (g.id == gid) {
                    g.rule = r;
                    g.rule_summary = model::FormatRuleSummary(r);
                    break;
                }
            }
            RecomputeLayout();
            SaveModel();
            renderer_.InvalidateAll();
        };
        cb.on_sticky_note_changed = [this](const std::string& tid, const std::wstring& body) {
            if (tid == "@weather_city") {
                std::wstring line = body.substr(0, body.find(L'\n'));
                while (!line.empty() && (line.back() == L' ' || line.back() == L'\r')) line.pop_back();
                if (line != state_.settings.weather_city) {
                    state_.settings.weather_city = line;
                    RequestWeather(true);
                    SaveModel();
                    SyncSettingsToPlugin();
                    renderer_.InvalidateAll();
                }
                return;
            }
            if (tid.starts_with("@title:")) {
                for (auto& t : state_.tiles) {
                    if (t.id != tid.substr(7)) continue;
                    std::wstring line = body.substr(0, body.find(L'\n'));
                    t.look.title_override = line == model::ActiveTileView(t).title ? std::wstring{} : line;
                }
                SaveModel();
                renderer_.InvalidateAll();
                return;
            }
            if (tid == "@todos") {
                // One to-do per line; a line kept from before keeps its tick.
                std::vector<model::TodoItem> next;
                size_t start = 0;
                while (start <= body.size()) {
                    size_t nl = body.find(L'\n', start);
                    std::wstring line = body.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
                    while (!line.empty() && (line.back() == L' ' || line.back() == L'\r')) line.pop_back();
                    if (!line.empty()) {
                        bool done = false;
                        for (const auto& old : state_.todos) if (old.text == line) done = old.done;
                        next.push_back({line, done});
                    }
                    if (nl == std::wstring::npos) break;
                    start = nl + 1;
                }
                state_.todos = std::move(next);
                SaveModel();
                renderer_.InvalidateAll();
                return;
            }
            for (auto& t : state_.tiles) {
                if (t.id == tid) {
                    t.note_body = body;
                    break;
                }
            }
            SaveModel();
            renderer_.InvalidateAll();
        };
        cb.on_ai_open_assistant_settings = [this]() {
            if (!RequireAiOrganizerEnabled()) return;
            overlays_.Dismiss();
            if (pipe_.IsConnected()) {
                pipe_.Send(ipc::MessageType::OpenInPulseReq, ++next_req_id_,
                           "{\"mode\":\"open\",\"path\":\"pulse:settings:assistant\"}");
            }
        };
        cb.on_ai_request_plan = [this](ai::AiOrganizeStrategy strat,
                                       const std::wstring& custom_prompt,
                                       bool force_local) {
            StartAiPlanRequest(strat, custom_prompt, force_local);
        };
        cb.on_ai_cancel = [this]() {
            if (!RequireAiOrganizerEnabled()) return;
            if (active_ai_session_) {
                active_ai_session_->Cancel();
                active_ai_session_.reset();
            }
            ++ai_req_seq_;
            if (!ai_view_state_.plan.groups.empty()) {
                ai_view_state_.phase = overlays::AiDrawerPhase::PlanReady;
            } else if (!ai_view_state_.connection.configured) {
                ai_view_state_.phase = overlays::AiDrawerPhase::UnconfiguredGuide;
            } else {
                std::vector<ai::DesktopAiCandidate> cands;
                std::vector<ai::ExistingDesktopFolder> existing_dirs;
                const std::wstring desk = mount::ResolveKnownFolder(FOLDERID_Desktop);
                ai::CollectDesktopAiContext(desk, state_.items, state_.groups, NowUnixMs(), cands, existing_dirs);
                ai_view_state_.plan = ai::BuildSmartLocalOrganizePlan(
                    desk, ai_view_state_.strategy, ai_view_state_.custom_instruction, cands, existing_dirs);
                ai_view_state_.phase = overlays::AiDrawerPhase::PlanReady;
            }
            if (overlays_.ActiveKind() == overlays::OverlayKind::AiOrganizeDrawer) {
                overlays_.UpdateAiOrganizeDrawer(ai_view_state_);
            }
        };
        cb.on_ai_execute_archive = [this](const ai::AiOrganizePlan& plan, bool pin_created_folders) {
            ExecuteAiArchiveAndRecordUndo(plan, pin_created_folders);
        };
        cb.on_ai_apply_virtual_groups = [this](const ai::AiOrganizePlan& plan) {
            if (!RequireAiOrganizerEnabled()) return;
            PushUndo(L"AI 虚拟分组整理");
            state_.settings.organize_enabled = true;
            const model::RectI wa = PrimaryWorkAreaClient();
            const model::RectI br = model::ComputeTileBoardRect(wa, state_.settings.tiles_enabled);
            ai::ApplyAiPlanAsVirtualGroups(plan, state_, wa, br);
            overlays_.Dismiss();
            RecomputeLayout();
            SaveModel();
            renderer_.InvalidateAll();
            ShowUndoToast(L"已按 AI 方案更新桌面虚拟分组");
        };
        cb.on_ai_open_desktop_in_pulse = [this]() {
            if (!RequireAiOrganizerEnabled()) return;
            OpenPathInPulseOrShell(mount::ResolveKnownFolder(FOLDERID_Desktop));
        };
        cb.on_overlay_dismissed = [this]() {
            last_activity_tick_ = GetTickCount64();
            working_set_trimmed_ = false;
            if (active_ai_session_) {
                active_ai_session_->Cancel();
                active_ai_session_.reset();
            }
            interaction_.search_highlight_query.clear();
            RecomputeLayout();
            renderer_.InvalidateAll();
            const size_t moved = std::exchange(pending_ai_archive_toast_count_, 0);
            if (moved > 0 && last_undo_is_ai_archive_ && !last_ai_archive_undo_.empty()) {
                ShowUndoToast(L"已归档 " + std::to_wstring(moved) + L" 项到桌面子目录");
            }
        };

        overlays_.Initialize(std::move(cb));
        overlays_.SetBackdropSource(&renderer_.Backdrop(), renderer_.DeviceContext());
    }

    void SetupDropTarget() {
        overlays::DropTargetCallbacks dcb{};
        dcb.hit_test_zone = [this](model::PointI pt) {
            overlays::DropZoneQueryResult z{};
            z.kind = overlays::DropZoneKind::BareDesktop;
            for (const size_t i : GroupHitOrder()) {
                if (group_views_[i].frame.contains(pt.x, pt.y)) {
                    z.kind = overlays::DropZoneKind::GroupCard;
                    z.target_id = state_.groups[i].id;
                    z.target_title = state_.groups[i].title;
                    z.target_path = state_.groups[i].mapped_folder_path;
                    return z;
                }
            }
            if (state_.settings.tiles_enabled && !interaction_.side_drawer_open) {
                for (const auto& t : state_.tiles) {
                    const model::RectI tr = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t);
                    if (!tr.contains(pt.x, pt.y)) continue;
                    if (t.kind == model::TileKind::StagingTray) {
                        z.kind = overlays::DropZoneKind::StagingTrayTile;
                        z.target_id = t.id;
                        return z;
                    }
                    if (t.kind == model::TileKind::RecycleBin) {
                        z.kind = overlays::DropZoneKind::RecycleBinTile;
                        z.target_id = t.id;
                        return z;
                    }
                    if (t.kind == model::TileKind::PinnedFolder && !t.target_path.empty()) {
                        z.kind = overlays::DropZoneKind::PinnedFolderTile;
                        z.target_id = t.id;
                        z.target_title = t.title;
                        z.target_path = t.target_path;
                        return z;
                    }
                }
            }
            return z;
        };
        dcb.on_drag_hint = [this](bool vis, model::PointI pt, const std::wstring& lbl) {
            if (vis) CancelHoverPreview();
            interaction_.drop_hint_visible = vis;
            UpdateGroupHoverAt(pt.x, pt.y, vis, GetTickCount64());
            interaction_.drop_hint_pos = pt;
            interaction_.drop_hint_label = lbl;
            renderer_.InvalidateAll();
            renderer_.Tick(state_, group_views_, tile_board_rect_, live_data_, interaction_);
        };
        dcb.on_drop_files = [this](const overlays::DropZoneQueryResult& zone, model::PointI pt,
                                   const std::vector<std::wstring>& paths, DWORD key_state, DWORD& /*eff*/) {
            HandleDroppedFiles(zone, pt, paths, (key_state & MK_CONTROL) != 0);
        };
        dcb.to_client = [this](POINTL p) {
            const POINT c = mount_.ScreenPxToClientDip(POINT{p.x, p.y});
            return model::PointI{c.x, c.y};
        };
        drop_mgr_.Attach(mount_.Hwnd(), std::move(dcb));
    }

    void HandleDroppedFiles(const overlays::DropZoneQueryResult& zone, model::PointI pt,
                            const std::vector<std::wstring>& paths, bool copy_mode) {
        if (paths.empty()) return;

        if (zone.kind == overlays::DropZoneKind::StagingTrayTile) {
            StagePathsInTray(paths);
            return;
        }

        if (zone.kind == overlays::DropZoneKind::RecycleBinTile) {
            RecyclePathsWithUndoToast(paths);
            return;
        }

        std::wstring dest_dir;
        bool target_is_mapped_group = false;
        if (zone.kind == overlays::DropZoneKind::PinnedFolderTile && !zone.target_path.empty()) {
            dest_dir = zone.target_path;
        } else if (zone.kind == overlays::DropZoneKind::GroupCard) {
            for (const auto& g : state_.groups) {
                if (g.id == zone.target_id && !g.mapped_folder_path.empty()) {
                    dest_dir = g.mapped_folder_path;
                    target_is_mapped_group = true;
                    break;
                }
            }
        }
        if (dest_dir.empty()) {
            dest_dir = mount::ResolveKnownFolder(FOLDERID_Desktop);
        }
        if (dest_dir.empty()) return;

        PushUndo(L"拖放文件");
        std::vector<std::wstring> final_paths;
        final_paths.reserve(paths.size());
        for (const auto& src : paths) {
            const std::wstring final_path = MoveOrCopyToDirectoryCollisionSafe(src, dest_dir, copy_mode);
            if (!final_path.empty()) {
                final_paths.push_back(final_path);
            }
        }
        SyncWatcherDirectories();
        RefreshDesktopItems();
        if (!target_is_mapped_group) {
            if (zone.kind == overlays::DropZoneKind::GroupCard) {
                for (const auto& fp : final_paths) {
                    const std::wstring fp_key = NormalizeFolderKey(fp);
                    for (auto& it : state_.items) {
                        if (NormalizeFolderKey(it.path) == fp_key) {
                            it.pinned_group_id = zone.target_id;
                            it.force_loose = false;
                        }
                    }
                }
            } else if (zone.kind == overlays::DropZoneKind::BareDesktop) {
                int idx = 0;
                for (const auto& fp : final_paths) {
                    const std::wstring fp_key = NormalizeFolderKey(fp);
                    for (auto& it : state_.items) {
                        if (NormalizeFolderKey(it.path) == fp_key) {
                            it.pinned_group_id.clear();
                            it.force_loose = true;
                            it.loose_pos = {pt.x + (idx % 3) * model::kLooseCellW,
                                            pt.y + (idx / 3) * model::kLooseCellH};
                            ++idx;
                        }
                    }
                }
            }
        }
        RefreshHealthStats();
        RecomputeLayout();
        SaveModel();
        renderer_.InvalidateAll();
    }

    void StagePathsInTray(const std::vector<std::wstring>& paths) {
        if (paths.empty()) return;
        std::string json = "{\"op\":\"collect\",\"move\":false,\"paths\":[";
        for (size_t i = 0; i < paths.size(); ++i) {
            if (i > 0) json.push_back(',');
            json += ipc::JsonQuoteWide(paths[i]);
        }
        json += "]}";
        pipe_.Send(ipc::MessageType::TrayMutateReq, ++next_req_id_, json);
    }

    void RecyclePathsWithUndoToast(const std::vector<std::wstring>& paths) {
        if (paths.empty()) return;
        std::wstring dbl;
        for (const auto& p : paths) {
            dbl += p;
            dbl.push_back(L'\0');
        }
        dbl.push_back(L'\0');
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = dbl.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
        SHFileOperationW(&op);
        RefreshDesktopItems();
        RefreshLiveTileData();
        RefreshHealthStats();
        RecomputeLayout();
        renderer_.InvalidateAll();
        ShowUndoToast(L"已将 " + std::to_wstring(paths.size()) + L" 项移入回收站");
    }

    bool IsPathInTray(const std::wstring& path) const noexcept {
        for (const auto& te : live_data_.tray_entries) {
            if (_wcsicmp(te.path.c_str(), path.c_str()) == 0) return true;
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // Hit Testing & Item Geometry
    // -----------------------------------------------------------------------
    std::optional<size_t> HitTestItem(int x, int y, model::RectI* out_cell_rc = nullptr,
                                      std::string* out_group_id = nullptr) const {
        if (state_.settings.icons_hidden || state_.settings.active_scene == model::DesktopScene::Present)
            return std::nullopt;
        const auto cell = model::IconCellMetrics(state_.settings.icon_size);
        std::unordered_set<size_t> claimed;
        for (size_t i = 0; i < state_.items.size(); ++i)
            if (state_.items[i].in_apps_tile) claimed.insert(i);
        if (state_.settings.organize_enabled) {
            for (const auto& view : group_views_)
                claimed.insert(view.member_item_indices.begin(), view.member_item_indices.end());
            for (const size_t gi : GroupHitOrder()) {
                const auto& view = group_views_[gi];
                if (!view.frame.contains(x, y)) continue;
                if (view.expanded) {
                    for (size_t ci = 0; ci < view.visible_item_indices.size(); ++ci) {
                        const auto rect = model::GroupItemRect(view, ci);
                        if (!rect.contains(x, y)) continue;
                        if (out_cell_rc) *out_cell_rc = rect;
                        if (out_group_id) *out_group_id = view.group_id;
                        return view.visible_item_indices[ci];
                    }
                }
                return std::nullopt; // Opaque top card blocks items on cards underneath.
            }
        }
        const int width = std::max(model::kLooseCellW, cell.x + 6);
        const int height = std::max(model::kLooseCellH, cell.y + 6);
        for (size_t i = 0; i < state_.items.size(); ++i) {
            if (claimed.contains(i)) continue;
            const auto& item = state_.items[i];
            if (item.hidden_by_mapped_group || !item.mapped_owner_group_id.empty()) continue;
            if (state_.settings.organize_enabled && !item.force_loose && !item.pinned_group_id.empty()) continue;
            const model::RectI rect{item.loose_pos.x, item.loose_pos.y, width, height};
            if (!rect.contains(x, y)) continue;
            if (out_cell_rc) *out_cell_rc = rect;
            if (out_group_id) out_group_id->clear();
            return i;
        }
        return std::nullopt;
    }

    std::optional<model::RectI> FindItemCellRect(const std::wstring& path) const {
        std::unordered_set<size_t> claimed;
        if (state_.settings.organize_enabled) {
            for (const auto& view : group_views_)
                claimed.insert(view.member_item_indices.begin(), view.member_item_indices.end());
            for (const size_t gi : GroupHitOrder()) {
                const auto& view = group_views_[gi];
                if (!view.expanded) continue;
                for (size_t ci = 0; ci < view.visible_item_indices.size(); ++ci) {
                    const size_t index = view.visible_item_indices[ci];
                    if (index < state_.items.size() && _wcsicmp(state_.items[index].path.c_str(), path.c_str()) == 0)
                        return model::GroupItemRect(view, ci);
                }
            }
        }
        const auto cell = model::IconCellMetrics(state_.settings.icon_size);
        for (size_t i = 0; i < state_.items.size(); ++i) {
            const auto& item = state_.items[i];
            if (claimed.contains(i) || item.in_apps_tile || item.hidden_by_mapped_group || !item.mapped_owner_group_id.empty()) continue;
            if (_wcsicmp(item.path.c_str(), path.c_str()) == 0)
                return model::RectI{item.loose_pos.x, item.loose_pos.y,
                    std::max(model::kLooseCellW, cell.x + 6), std::max(model::kLooseCellH, cell.y + 6)};
        }
        return std::nullopt;
    }

    // -----------------------------------------------------------------------
    // 4-State Context Menu & Action Execution
    // -----------------------------------------------------------------------
    void CreateNewDesktopEntry(bool folder, const wchar_t* default_name) {
        const std::wstring desk = mount::ResolveKnownFolder(FOLDERID_Desktop);
        if (desk.empty()) return;
        std::filesystem::path base = std::filesystem::path(desk) / default_name;
        std::filesystem::path target = base;
        for (int n = 2; std::filesystem::exists(target) && n < 100; ++n) {
            if (folder) {
                target = std::filesystem::path(desk) / (std::wstring(default_name) + L" (" + std::to_wstring(n) + L")");
            } else {
                target = std::filesystem::path(desk) /
                         (base.stem().wstring() + L" (" + std::to_wstring(n) + L")" + base.extension().wstring());
            }
        }
        std::error_code ec;
        if (folder) {
            std::filesystem::create_directory(target, ec);
        } else {
            HANDLE h = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
        RefreshDesktopItems();
        RecomputeLayout();
        interaction_.selected_paths.clear();
        interaction_.selected_paths.insert(target.wstring());
        interaction_.focused_path = target.wstring();
        renderer_.InvalidateAll();
        StartInlineRenameForPath(target.wstring());
    }

    static model::DesktopTile MakeWidgetTileForKind(model::TileKind wk,
                                                    std::optional<model::TileSize> custom_size = std::nullopt) {
        model::DesktopTile nt{};
        nt.id = "tile_" + std::to_string(NowUnixMs());
        nt.kind = wk;
        switch (nt.kind) {
        case model::TileKind::Weather:       nt.title = L"天气";     nt.size = {2, 2}; break;
        case model::TileKind::Calendar:      nt.title = L"日历";     nt.size = {1, 1}; break;
        case model::TileKind::Todo:          nt.title = L"待办";     nt.size = {2, 2}; break;
        case model::TileKind::System:        nt.title = L"系统";     nt.size = {2, 2}; break;
        case model::TileKind::Apps:          nt.title = L"应用";     nt.size = {4, 2}; break;
        case model::TileKind::Photos:        nt.title = L"照片";     nt.size = {4, 2}; break;
        case model::TileKind::Downloads:     nt.title = L"下载";     nt.size = {2, 2}; break;
        case model::TileKind::Recent:        nt.title = L"最近使用"; nt.size = {2, 2}; break;
        case model::TileKind::PinnedFolder:  nt.title = L"文件夹";   nt.size = {2, 2}; break;
        case model::TileKind::DiskRing:      nt.title = L"本地磁盘"; nt.size = {1, 1}; break;
        case model::TileKind::Clock:         nt.title = L"时钟";     nt.size = {1, 1}; break;
        case model::TileKind::StickyNote:    nt.title = L"便笺";     nt.size = {2, 2}; nt.note_body.clear(); break;
        case model::TileKind::StagingTray:   nt.title = L"暂存区";   nt.size = {4, 1}; break;
        case model::TileKind::RecycleBin:    nt.title = L"回收站";   nt.size = {1, 1}; break;
        case model::TileKind::ThisPC:        nt.title = L"此电脑";   nt.size = {1, 1}; break;
        case model::TileKind::PulseLauncher: nt.title = L"Pulse";    nt.size = {1, 1}; break;
        default:                             nt.title = L"磁贴";     nt.size = {1, 1}; break;
        }
        if (custom_size) nt.size = *custom_size;
        return nt;
    }

    void OpenWidgetGallery(std::optional<model::TileKind> initial_kind = std::nullopt,
                           std::optional<model::PointI> target_cell = std::nullopt) {
        if (!state_.settings.tiles_enabled || !state_.settings.organize_enabled) {
            state_.settings.tiles_enabled = true;
            state_.settings.organize_enabled = true;
        }
        if (overlays_.IsSideDrawerOpen()) {
            overlays_.Dismiss();
        }
        RecomputeLayout();
        POINT board_scr{tile_board_rect_.x, tile_board_rect_.y};
        mount_.ClientToScreenDip(&board_scr);
        overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
        overlays_.OpenWidgetGalleryDrawer(PrimaryWorkAreaScreen(), board_scr.x, board_scr.y,
                                          state_.tiles, initial_kind, target_cell);
        SyncOverlayInteractionFlags();
        renderer_.InvalidateAll();
    }

    void InsertOrStackWidgetKind(model::TileKind wk) {
        model::DesktopTile nt = MakeWidgetTileForKind(wk);
        const bool any_free = std::any_of(state_.tiles.begin(), state_.tiles.end(),
                                          [](const model::DesktopTile& t) { return t.free_placed; });
        model::TilePlacementResult res{};
        if (!any_free) {
            res = model::InsertNewTile(state_.tiles, nt);
            if (!res.valid) {
                for (const auto s : model::AllowedTileSizesFor(nt.kind)) {
                    nt.size = s;
                    res = model::InsertNewTile(state_.tiles, nt);
                    if (res.valid) break;
                }
            }
        }
        if (res.valid) {
            state_.tiles = std::move(res.updated_tiles);
        } else {
            const model::RectI wa = PrimaryWorkAreaClient();
            const model::RectI br = !tile_board_rect_.empty() ? tile_board_rect_ : model::ComputeTileBoardRect(wa, true);
            nt.w = model::DefaultTileSpanPx(nt.size.cols);
            nt.h = model::DefaultTileSpanPx(nt.size.rows);
            const model::PointI spot = model::FindFreeDesktopSpotForTile(
                state_.tiles, CurrentGroupFrames(), wa, br, nt.w, nt.h);
            nt.free_placed = true;
            nt.x = spot.x;
            nt.y = spot.y;
            state_.tiles.push_back(nt);
        }
        if (nt.kind == model::TileKind::Weather) RequestWeather(true);
        if (nt.kind == model::TileKind::System) SampleSystemMetrics();
        RefreshLiveTileData();
    }

    static HRESULT LaunchViaDesktopExplorer(const std::wstring& executable,
                                            const std::wstring& arguments = {},
                                            const std::wstring& directory = {},
                                            const wchar_t* verb = L"open",
                                            int show = SW_SHOWNORMAL) {
        using Microsoft::WRL::ComPtr;
        ComPtr<IShellWindows> windows;
        HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows));
        if (FAILED(hr) || !windows) return FAILED(hr) ? hr : E_FAIL;

        VARIANT location{}, empty{};
        location.vt = VT_I4;
        location.lVal = CSIDL_DESKTOP;
        long hwnd = 0;
        ComPtr<IDispatch> desktop;
        hr = windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop);
        if (FAILED(hr) || !desktop) return FAILED(hr) ? hr : E_NOINTERFACE;

        DWORD pid = 0, session = 0, own_session = 0;
        GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), &pid);
        if (!pid || !ProcessIdToSessionId(pid, &session) ||
            !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || session != own_session) {
            return E_ACCESSDENIED;
        }

        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return HRESULT_FROM_WIN32(GetLastError());
        HANDLE token = nullptr;
        const bool opened = OpenProcessToken(process, TOKEN_QUERY, &token) != FALSE;
        CloseHandle(process);
        if (!opened) return HRESULT_FROM_WIN32(GetLastError());

        TOKEN_ELEVATION elevation{};
        alignas(TOKEN_MANDATORY_LABEL) BYTE label[256]{};
        alignas(TOKEN_USER) BYTE desk_user_buf[256]{};
        DWORD bytes = 0;
        const bool queried =
            GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes) &&
            GetTokenInformation(token, TokenIntegrityLevel, label, sizeof(label), &bytes) &&
            GetTokenInformation(token, TokenUser, desk_user_buf, sizeof(desk_user_buf), &bytes);
        CloseHandle(token);
        if (!queried) return E_ACCESSDENIED;

        const PSID il_sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label)->Label.Sid;
        const UCHAR il_count = il_sid ? *GetSidSubAuthorityCount(il_sid) : 0;
        if (!il_count || *GetSidSubAuthority(il_sid, il_count - 1) != SECURITY_MANDATORY_MEDIUM_RID ||
            elevation.TokenIsElevated) {
            return E_ACCESSDENIED;
        }

        HANDLE self_tok = nullptr;
        alignas(TOKEN_USER) BYTE self_user_buf[256]{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &self_tok)) return E_ACCESSDENIED;
        const bool self_ok =
            GetTokenInformation(self_tok, TokenUser, self_user_buf, sizeof(self_user_buf), &bytes) != FALSE;
        CloseHandle(self_tok);
        if (!self_ok || !EqualSid(reinterpret_cast<TOKEN_USER*>(desk_user_buf)->User.Sid,
                                  reinterpret_cast<TOKEN_USER*>(self_user_buf)->User.Sid)) {
            return E_ACCESSDENIED;
        }

        ComPtr<IServiceProvider> services;
        if (FAILED(hr = desktop.As(&services))) return hr;
        ComPtr<IShellBrowser> browser;
        if (FAILED(hr = services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))) return hr;
        ComPtr<IShellView> view;
        if (FAILED(hr = browser->QueryActiveShellView(&view))) return hr;
        ComPtr<IDispatch> background;
        if (FAILED(hr = view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background)))) return hr;
        ComPtr<IShellFolderViewDual> folder;
        if (FAILED(hr = background.As(&folder))) return hr;
        ComPtr<IDispatch> application;
        if (FAILED(hr = folder->get_Application(&application))) return hr;
        ComPtr<IShellDispatch2> shell;
        if (FAILED(hr = application.As(&shell))) return hr;

        BSTR file = SysAllocString(executable.c_str());
        VARIANT v_args{}, v_dir{}, v_verb{}, v_show{};
        v_args.vt = v_dir.vt = v_verb.vt = VT_BSTR;
        v_args.bstrVal = SysAllocString(arguments.c_str());
        v_dir.bstrVal = SysAllocString(directory.c_str());
        v_verb.bstrVal = SysAllocString(verb ? verb : L"open");
        v_show.vt = VT_I4;
        v_show.lVal = show;
        hr = (file && v_args.bstrVal && v_dir.bstrVal && v_verb.bstrVal)
            ? shell->ShellExecute(file, v_args, v_dir, v_verb, v_show)
            : E_OUTOFMEMORY;
        SysFreeString(file);
        VariantClear(&v_args);
        VariantClear(&v_dir);
        VariantClear(&v_verb);
        return hr;
    }

    static void LaunchDetached(const std::wstring& target,
                               const std::wstring& arguments = {},
                               const std::wstring& directory = {}) {
        if (target.empty()) return;
        AllowSetForegroundWindow(ASFW_ANY);
        std::thread([path = target, args = arguments, dir = directory] {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            const HRESULT delegated = SUCCEEDED(com) ? LaunchViaDesktopExplorer(path, args, dir) : E_FAIL;
            if (FAILED(delegated)) {
                SHELLEXECUTEINFOW sei{};
                sei.cbSize = sizeof(sei);
                sei.fMask = SEE_MASK_NOASYNC;
                sei.lpFile = path.c_str();
                if (!args.empty()) sei.lpParameters = args.c_str();
                if (!dir.empty()) sei.lpDirectory = dir.c_str();
                sei.nShow = SW_SHOWNORMAL;
                ShellExecuteExW(&sei);
            }
            if (SUCCEEDED(com)) CoUninitialize();
        }).detach();
    }

    static std::wstring ResolveShortcutTargetPath(const std::wstring& lnk_path) {
        if (lnk_path.size() < 4 || _wcsicmp(lnk_path.c_str() + lnk_path.size() - 4, L".lnk") != 0) {
            return lnk_path;
        }
        Microsoft::WRL::ComPtr<IShellLinkW> sl;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))) || !sl) {
            return lnk_path;
        }
        Microsoft::WRL::ComPtr<IPersistFile> pf;
        if (FAILED(sl.As(&pf)) || !pf || FAILED(pf->Load(lnk_path.c_str(), STGM_READ))) {
            return lnk_path;
        }
        wchar_t target[MAX_PATH * 2]{};
        if (SUCCEEDED(sl->GetPath(target, ARRAYSIZE(target), nullptr, SLGP_RAWPATH)) && target[0] != L'\0') {
            std::error_code ec;
            if (std::filesystem::exists(target, ec)) return target;
        }
        return lnk_path;
    }

    std::wstring PickFolderDialog(const wchar_t* title) {
        std::wstring out;
        Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) {
            return out;
        }
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts))) {
            dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        }
        if (title) dlg->SetTitle(title);
        if (FAILED(dlg->Show(mount_.Hwnd()))) return out;
        Microsoft::WRL::ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
            out = path;
            CoTaskMemFree(path);
        }
        return out;
    }

    void ExecuteContextMenuCommand(const std::string& cmd, const overlays::ContextMenuSpec& spec) {
        std::vector<std::wstring> target_paths;
        if (!interaction_.selected_paths.empty() &&
            (!spec.target_path.empty() && interaction_.selected_paths.contains(spec.target_path))) {
            target_paths.assign(interaction_.selected_paths.begin(), interaction_.selected_paths.end());
        } else if (!spec.target_path.empty()) {
            target_paths.push_back(spec.target_path);
        }

        if (cmd == "new-group") {
            PushUndo(L"新建分组");
            model::DesktopGroup g{};
            g.id = "grp_" + std::to_string(NowUnixMs());
            g.title = L"新建分组";
            POINT cpt = spec.screen_anchor;
            mount_.ScreenDipToClient(&cpt);
            g.x = cpt.x;
            g.y = cpt.y;
            state_.groups.push_back(std::move(g));
            RecomputeLayout();
            SaveModel();
            renderer_.InvalidateAll();
        } else if (cmd == "new-mapped-group") {
            const std::wstring dir = PickFolderDialog(L"选择要作为分组显示的文件夹");
            if (!dir.empty()) {
                PushUndo(L"从文件夹创建分组");
                model::DesktopGroup g{};
                g.id = "grp_" + std::to_string(NowUnixMs());
                g.title = MappedFolderLeafName(dir);
                g.icon_glyph = L'\xE8B7';
                POINT cpt = spec.screen_anchor;
                mount_.ScreenDipToClient(&cpt);
                g.x = cpt.x;
                g.y = cpt.y;
                state_.groups.push_back(std::move(g));
                BindGroupToFolder(state_.groups.size() - 1, dir, /*push_undo=*/false);
            }
        } else if (cmd == "map-folder-to-current-group" && !spec.target_path.empty() && !spec.current_group_id.empty()) {
            for (size_t gi = 0; gi < state_.groups.size(); ++gi) {
                if (state_.groups[gi].id == spec.current_group_id && state_.groups[gi].mapped_folder_path.empty()) {
                    BindGroupToFolder(gi, spec.target_path, /*push_undo=*/true);
                    break;
                }
            }
        } else if (cmd == "create-mapped-group-from-folder" && !spec.target_path.empty()) {
            PushUndo(L"将文件夹转为分组");
            model::DesktopGroup g{};
            g.id = "grp_" + std::to_string(NowUnixMs());
            g.title = MappedFolderLeafName(spec.target_path);
            g.icon_glyph = L'\xE8B7';
            POINT cpt = spec.screen_anchor;
            mount_.ScreenDipToClient(&cpt);
            g.x = cpt.x;
            g.y = cpt.y;
            state_.groups.push_back(std::move(g));
            BindGroupToFolder(state_.groups.size() - 1, spec.target_path, /*push_undo=*/false);
        } else if (cmd == "new-group-with-sel" && !target_paths.empty()) {
            PushUndo(L"新建分组并移入");
            model::DesktopGroup g{};
            g.id = "grp_" + std::to_string(NowUnixMs());
            g.title = L"新建分组";
            POINT cpt = spec.screen_anchor;
            mount_.ScreenDipToClient(&cpt);
            g.x = cpt.x;
            g.y = cpt.y;
            const std::string gid = g.id;
            state_.groups.push_back(std::move(g));
            const auto dest_keys = EvacuateMappedItemsToDesktop(target_paths);
            for (auto& it : state_.items) {
                if (dest_keys.contains(NormalizeFolderKey(it.path))) {
                    it.pinned_group_id = gid;
                    it.force_loose = false;
                }
            }
            RecomputeLayout();
            SaveModel();
            renderer_.InvalidateAll();
        } else if (cmd.starts_with("move-to-group:") && !target_paths.empty()) {
            const std::string gid = cmd.substr(14);
            std::wstring mapped_dst_dir;
            for (const auto& g : state_.groups) {
                if (g.id == gid) {
                    mapped_dst_dir = g.mapped_folder_path;
                    break;
                }
            }
            PushUndo(L"移到分组");
            if (!mapped_dst_dir.empty()) {
                for (const auto& tp : target_paths) {
                    MoveOrCopyToDirectoryCollisionSafe(tp, mapped_dst_dir, false);
                }
                interaction_.selected_paths.clear();
                RefreshDesktopItems();
                RefreshHealthStats();
            } else {
                const auto dest_keys = EvacuateMappedItemsToDesktop(target_paths);
                for (auto& it : state_.items) {
                    if (dest_keys.contains(NormalizeFolderKey(it.path))) {
                        it.pinned_group_id = gid;
                        it.force_loose = false;
                    }
                }
            }
            RecomputeLayout();
            SaveModel();
            renderer_.InvalidateAll();
        } else if (cmd == "move-to-loose" && !target_paths.empty()) {
            PushUndo(L"放到桌面");
            const auto dest_keys = EvacuateMappedItemsToDesktop(target_paths);
            for (auto& it : state_.items) {
                if (dest_keys.contains(NormalizeFolderKey(it.path))) {
                    it.pinned_group_id.clear();
                    it.force_loose = true;
                }
            }
            RecomputeLayout();
            SaveModel();
            renderer_.InvalidateAll();
        } else if (cmd == "pin-as-tile" && !spec.target_path.empty()) {
            PushUndo(L"固定为磁贴");
            model::DesktopTile nt{};
            nt.id = "pin_" + std::to_string(NowUnixMs());
            nt.kind = spec.target_is_folder ? model::TileKind::PinnedFolder : model::TileKind::PinnedItem;
            nt.title = spec.target_name.empty()
                           ? std::filesystem::path(spec.target_path).filename().wstring()
                           : spec.target_name;
            nt.target_path = spec.target_path;
            nt.size = spec.target_is_folder ? model::TileSize{2, 2} : model::TileSize{1, 1};
            auto res = model::InsertNewTile(state_.tiles, nt);
            if (res.valid) {
                state_.tiles = std::move(res.updated_tiles);
                RefreshLiveTileData();
                SaveModel();
                renderer_.InvalidateAll();
            }
        } else if (cmd == "tiles-edit" || cmd == "tiles-edit-done") {
            SetTileEditMode(cmd == "tiles-edit");
        } else if (cmd == "app-out" && !spec.target_path.empty()) {
            PushUndo(L"放回桌面");
            for (auto& it : state_.items) if (it.path == spec.target_path) { it.pinned_group_id.clear(); it.force_loose = true; }
            RecomputeLayout();
            SaveModel();
            renderer_.InvalidateAll();
        } else if (cmd.starts_with("add-tile:")) {
            const int kind_val = std::stoi(cmd.substr(9));
            PushUndo(L"添加小组件");
            const auto wk = static_cast<model::TileKind>(std::clamp(kind_val, 0, static_cast<int>(model::kLastTileKind)));
            InsertOrStackWidgetKind(wk);
            RecomputeLayout();
            SaveModel();
            renderer_.InvalidateAll();
        } else if (cmd == "widget-gallery") {
            OpenWidgetGallery();
        } else if (cmd == "ai-organize") {
            OpenAiOrganizeDrawer();
        } else if (cmd == "auto-organize") {
            PushUndo(L"按规则重新整理");
            state_.settings.organize_enabled = true;
            for (auto& it : state_.items) {
                it.force_loose = false;
                it.pinned_group_id.clear();
            }
            RecomputeLayout();
            SaveModel();
            renderer_.InvalidateAll();
            ShowUndoToast(L"已按规则重新整理桌面图标");
        } else if (cmd == "refresh") {
            RefreshDesktopItems();
            RefreshLiveTileData();
            RefreshHealthStats();
            RecomputeLayout();
            renderer_.InvalidateAll();
        } else if (cmd == "undo") {
            PerformUndo();
        } else if (cmd == "open-search") {
            OpenSearchOverlay(L"");
        } else if (cmd == "health-check") {
            OpenHealthCheckDrawer();
        } else if (cmd == "beautify") {
            OpenBeautifyOverlay();
        } else if (cmd.starts_with("view-size:")) {
            const int sz = std::clamp(cmd[10] - '0', 0, 2);
            state_.settings.icon_size = static_cast<model::DesktopIconSize>(sz);
            RecomputeLayout();
            SaveModel();
            SyncSettingsToPlugin();
            renderer_.InvalidateAll();
        } else if (cmd == "toggle-organize") {
            state_.settings.organize_enabled = !state_.settings.organize_enabled;
            RecomputeLayout();
            SaveModel();
            renderer_.InvalidateAll();
        } else if (cmd == "toggle-tiles") {
            state_.settings.tiles_enabled = !state_.settings.tiles_enabled;
            if (state_.settings.tiles_enabled) RefreshLiveTileData();
            RecomputeLayout();
            SaveModel();
            renderer_.InvalidateAll();
        } else if (cmd == "toggle-grid") {
            state_.settings.align_to_grid = !state_.settings.align_to_grid;
            RecomputeLayout();
            SaveModel();
            renderer_.InvalidateAll();
        } else if (cmd.starts_with("loose-sort:")) {
            const int sb = std::clamp(cmd[11] - '0', 0, 4);
            state_.settings.loose_sort_by = static_cast<model::GroupSortBy>(sb);
            RecomputeLayout();
            SaveModel();
            renderer_.InvalidateAll();
        } else if (cmd == "scene-work" || cmd == "scene-focus" || cmd == "scene-present") {
            state_.settings.active_scene = (cmd == "scene-focus")
                                               ? model::DesktopScene::Focus
                                               : ((cmd == "scene-present") ? model::DesktopScene::Present
                                                                           : model::DesktopScene::Work);
            RecomputeLayout();
            SaveModel();
            SyncSettingsToPlugin();
            renderer_.InvalidateAll();
        } else if (cmd == "toggle-icons") {
            state_.settings.icons_hidden = !state_.settings.icons_hidden;
            if (state_.settings.icons_hidden) CancelHoverPreview();
            SaveModel();
            SyncSettingsToPlugin();
            renderer_.InvalidateAll();
        } else if (cmd == "new-folder") {
            CreateNewDesktopEntry(true, L"新建文件夹");
        } else if (cmd == "new-txt") {
            CreateNewDesktopEntry(false, L"新建文本文档.txt");
        } else if (cmd == "new-md") {
            CreateNewDesktopEntry(false, L"新建笔记.md");
        } else if (cmd == "open-desktop-pulse") {
            OpenPathInPulseOrShell(mount::ResolveKnownFolder(FOLDERID_Desktop));
        } else if (cmd == "open-terminal") {
            const std::wstring desk = mount::ResolveKnownFolder(FOLDERID_Desktop);
            std::thread([desk] {
                const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                if (FAILED(com) || FAILED(LaunchViaDesktopExplorer(L"wt.exe", L"", desk))) {
                    if (ShellExecuteW(nullptr, L"open", L"wt.exe", nullptr, desk.c_str(), SW_SHOWNORMAL) <=
                        reinterpret_cast<HINSTANCE>(32)) {
                        ShellExecuteW(nullptr, L"open", L"powershell.exe", nullptr, desk.c_str(), SW_SHOWNORMAL);
                    }
                }
                if (SUCCEEDED(com)) CoUninitialize();
            }).detach();
        } else if (cmd == "sys-display") {
            ShellExecuteW(nullptr, L"open", L"ms-settings:display", nullptr, nullptr, SW_SHOWNORMAL);
        } else if (cmd == "sys-personalize") {
            ShellExecuteW(nullptr, L"open", L"ms-settings:personalization", nullptr, nullptr, SW_SHOWNORMAL);
        } else if (cmd == "open-pack-settings") {
            AllowSetForegroundWindow(ASFW_ANY);
            if (pipe_.IsConnected()) {
                pipe_.Send(ipc::MessageType::OpenInPulseReq, ++next_req_id_,
                           "{\"mode\":\"settings\",\"path\":\"pulse:settings:native-extensions/pulse.desktop\"}");
            } else {
                LaunchPulseUriFallback(L"pulse:settings:native-extensions/pulse.desktop");
            }
        } else if (cmd == "open-item" && !spec.target_path.empty()) {
            LaunchAsync(spec.target_path);
        } else if (cmd == "run-as-admin" && !spec.target_path.empty()) {
            ShellExecuteW(nullptr, L"runas", spec.target_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (cmd == "reveal-shortcut-target" && !spec.target_path.empty()) {
            const std::wstring target = ResolveShortcutTargetPath(spec.target_path);
            if (pipe_.IsConnected()) {
                AllowSetForegroundWindow(ASFW_ANY);
                pipe_.Send(ipc::MessageType::OpenInPulseReq, ++next_req_id_,
                           "{\"mode\":\"reveal\",\"path\":" + ipc::JsonQuoteWide(target) + "}");
            } else {
                const std::wstring arg = L"/select,\"" + target + L"\"";
                LaunchDetached(L"explorer.exe", arg);
            }
        } else if (cmd == "quick-look" && !spec.target_path.empty()) {
            OpenQuickLookForPath(spec.target_path);
        } else if (cmd == "reveal-in-pulse" && !spec.target_path.empty()) {
            AllowSetForegroundWindow(ASFW_ANY);
            pipe_.Send(ipc::MessageType::OpenInPulseReq, ++next_req_id_,
                       "{\"mode\":\"reveal\",\"path\":" + ipc::JsonQuoteWide(spec.target_path) + "}");
        } else if ((cmd == "copy-path" || cmd == "copy-item" || cmd == "cut-item") && !target_paths.empty()) {
            std::wstring joined;
            for (size_t i = 0; i < target_paths.size(); ++i) {
                if (i > 0) joined += L"\r\n";
                joined += target_paths[i];
            }
            CopyWideTextToClipboard(mount_.Hwnd(), joined);
            ShowUndoToast(cmd == "copy-path" ? L"已复制路径到剪贴板" : L"已复制到剪贴板");
        } else if (cmd == "properties" && !spec.target_path.empty()) {
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.fMask = SEE_MASK_INVOKEIDLIST;
            sei.lpVerb = L"properties";
            sei.lpFile = spec.target_path.c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
        } else if (cmd == "open-tag-pulse" && !spec.current_tag_id.empty()) {
            if (parent_pid_ != 0) AllowSetForegroundWindow(parent_pid_);
            pipe_.Send(ipc::MessageType::OpenInPulseReq, ++next_req_id_,
                       "{\"mode\":\"open\",\"path\":" + ipc::JsonQuoteWide(L"pulse:tag:" + spec.current_tag_id) + "}");
        } else if (cmd == "manage-tags-pulse") {
            if (parent_pid_ != 0) AllowSetForegroundWindow(parent_pid_);
            pipe_.Send(ipc::MessageType::OpenInPulseReq, ++next_req_id_,
                       "{\"mode\":\"open\",\"path\":\"pulse:tag-manager\"}");
        } else if (cmd.starts_with("tag:") && !target_paths.empty()) {
            const std::wstring tid = ipc::Utf8ToWide(cmd.substr(4));
            const bool clear_mode = (tid == L"clear");
            for (auto& it : state_.items) {
                if (std::find(target_paths.begin(), target_paths.end(), it.path) == target_paths.end()) continue;
                const std::wstring prev_tid = it.tag_id;
                const bool clear = clear_mode || (prev_tid == tid);
                it.tag_id = clear ? L"" : tid;
                it.tag_argb = 0;
                if (!clear) {
                    for (const auto& opt : tag_palette_) {
                        if (opt.id == tid) it.tag_argb = opt.argb;
                    }
                }
                if (clear_mode) {
                    pipe_.Send(ipc::MessageType::SetTagReq, ++next_req_id_,
                               "{\"path\":" + ipc::JsonQuoteWide(it.path) +
                               ",\"tag_id\":\"*\",\"clear_all\":true,\"tagged\":false}");
                } else if (clear) {
                    pipe_.Send(ipc::MessageType::SetTagReq, ++next_req_id_,
                               "{\"path\":" + ipc::JsonQuoteWide(it.path) +
                               ",\"tag_id\":" + ipc::JsonQuoteWide(tid) +
                               ",\"tagged\":false}");
                } else {
                    pipe_.Send(ipc::MessageType::SetTagReq, ++next_req_id_,
                               "{\"path\":" + ipc::JsonQuoteWide(it.path) +
                               ",\"tag_id\":" + ipc::JsonQuoteWide(tid) +
                               ",\"prev_tag_id\":" + ipc::JsonQuoteWide(prev_tid) +
                               ",\"tagged\":true}");
                }
            }
            RecomputeLayout();
            SaveModel();
            renderer_.InvalidateAll();
        } else if (cmd == "add-to-tray" && !target_paths.empty()) {
            StagePathsInTray(target_paths);
        } else if (cmd == "rename-item" && !spec.target_path.empty()) {
            StartInlineRenameForPath(spec.target_path);
        } else if (cmd == "delete-item" && !target_paths.empty()) {
            RecyclePathsWithUndoToast(target_paths);
        } else if (!spec.target_group_id.empty()) {
            for (size_t i = 0; i < state_.groups.size(); ++i) {
                auto& g = state_.groups[i];
                if (g.id != spec.target_group_id) continue;
                if (cmd.starts_with("group-view:") && cmd.size() == 12 && cmd[11] >= '0' && cmd[11] <= '3') {
                    const auto view = static_cast<model::GroupViewMode>(cmd[11] - '0');
                    if (g.view_mode == view) return;
                    PushUndo(l10n::Get(l10n::StringId::DesktopGroupChangeView));
                    g.view_mode = view;
                    interaction_.group_pages.erase(g.id);
                } else if (cmd == "group-detach-tab") {
                    auto detached = state_.groups;
                    if (!model::DetachGroupTab(detached, g.id, PrimaryWorkAreaClient(), {},
                                              state_.settings.icon_size, CurrentTileObstacles())) {
                        ShowUndoToast(l10n::Get(l10n::StringId::DesktopGroupDetachNoSpace));
                        return;
                    }
                    PushUndo(l10n::Get(l10n::StringId::DesktopGroupDetachTab));
                    state_.groups = std::move(detached);
                    group_hover_.Reset();
                } else if (cmd == "toggle-collapse") {
                    g.collapsed = !g.collapsed;
                } else if (cmd == "rename-group") {
                    const model::RectI r{g.x + 34, g.y + 6, std::max(120, group_views_[i].frame.w - 96), 28};
                    overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
                    overlays_.BeginInlineRename(mount_.Hwnd(), r, g.title, false,
                                                [this, gid = g.id](const std::wstring& txt) {
                                                    PushUndo(L"重命名分组");
                                                    for (auto& gg : state_.groups) {
                                                        if (gg.id == gid) gg.title = txt;
                                                    }
                                                    SaveModel();
                                                    renderer_.InvalidateAll();
                                                });
                    return;
                } else if (cmd == "edit-rule") {
                    OpenGroupRulePopoverForIndex(i);
                    return;
                } else if (cmd == "ai-organize-group") {
                    OpenAiOrganizeDrawer(g.id);
                    return;
                } else if (cmd == "sort-name") g.sort_by = model::GroupSortBy::Name;
                else if (cmd == "sort-mtime") g.sort_by = model::GroupSortBy::ModifiedTime;
                else if (cmd == "sort-size") g.sort_by = model::GroupSortBy::Size;
                else if (cmd == "sort-type") g.sort_by = model::GroupSortBy::Type;
                else if (cmd == "sort-manual") g.sort_by = model::GroupSortBy::Manual;
                else if (cmd.starts_with("group-cols:") && cmd.size() >= 12) {
                    PushUndo(L"调整分组列数");
                    const int c = std::clamp(cmd[11] - '0', model::kGroupMinCols, model::kGroupMaxCols);
                    model::ResizeGroupColumns(state_.groups, i, c, g.visible_rows,
                                              PrimaryWorkAreaClient(), model::RectI{},
                                              state_.settings.icon_size, CurrentTileObstacles());
                } else if (cmd == "mat-follow") g.material = model::CardMaterial::FollowHost;
                else if (cmd == "mat-acrylic") g.material = model::CardMaterial::Acrylic;
                else if (cmd == "mat-liquid") g.material = model::CardMaterial::LiquidGlass;
                else if (cmd == "mat-mica") g.material = model::CardMaterial::Mica;
                else if (cmd == "mat-mica-alt") g.material = model::CardMaterial::MicaAlt;
                else if (cmd == "mat-solid") g.material = model::CardMaterial::Solid;
                else if (cmd == "toggle-focus") g.focus_visible = !g.focus_visible;
                else if (cmd == "open-group-pulse") {
                    OpenPathInPulseOrShell(g.mapped_folder_path.empty()
                                               ? mount::ResolveKnownFolder(FOLDERID_Desktop)
                                               : g.mapped_folder_path,
                                           &group_views_[i].frame);
                    return;
                } else if (cmd == "map-single-child-folder") {
                    if (i < group_views_.size() && !group_views_[i].single_subfolder_path.empty()) {
                        BindGroupToFolder(i, group_views_[i].single_subfolder_path, /*push_undo=*/true);
                        return;
                    }
                } else if (cmd == "map-real-folder") {
                    const std::wstring dir = PickFolderDialog(L"选择要作为此分组内容显示的文件夹");
                    if (!dir.empty()) {
                        BindGroupToFolder(i, dir, /*push_undo=*/true);
                    }
                    return;
                } else if (cmd == "unmap-real-folder") {
                    if (!g.mapped_folder_path.empty()) {
                        PushUndo(L"解除文件夹关联");
                        g.mapped_folder_path.clear();
                        interaction_.group_pages.erase(g.id);
                        SyncWatcherDirectories();
                        RefreshDesktopItems();
                        RefreshHealthStats();
                        ShowUndoToast(L"已恢复仅显示分组内引用，不再直接映射文件夹内容");
                    }
                } else if (cmd == "delete-group") {
                    PushUndo(L"删除分组");
                    const std::string gid = g.id;
                    const bool was_mapped = !g.mapped_folder_path.empty();
                    interaction_.group_pages.erase(gid);
                    for (auto& it : state_.items) {
                        if (it.pinned_group_id == gid) it.pinned_group_id.clear();
                    }
                    state_.groups.erase(state_.groups.begin() + static_cast<ptrdiff_t>(i));
                    if (was_mapped) {
                        SyncWatcherDirectories();
                        RefreshDesktopItems();
                        RefreshHealthStats();
                    }
                }
                break;
            }
            RecomputeLayout();
            SaveModel();
            renderer_.InvalidateAll();
        } else if (!spec.target_tile_id.empty()) {
            for (size_t i = 0; i < state_.tiles.size(); ++i) {
                auto& t = state_.tiles[i];
                if (t.id != spec.target_tile_id) continue;
                if (cmd == "toggle-clock") {
                    if (t.stack.empty()) t.analog_clock = !t.analog_clock;
                    else { auto& m = t.stack[static_cast<size_t>(std::clamp(t.stack_index, 0, static_cast<int>(t.stack.size()) - 1))]; m.analog_clock = !m.analog_clock; }
                    RefreshWidgetDetail(model::TileKind::Clock);
                } else if (cmd.starts_with("tile-stack-sel:")) {
                    t.stack_index = std::clamp(std::atoi(cmd.c_str() + 15), 0, std::max(0, static_cast<int>(t.stack.size()) - 1));
                } else if (cmd == "tile-stack-rotate") {
                    t.smart_rotate = !t.smart_rotate;
                } else if (cmd == "tile-unstack") {
                    PushUndo(L"移出叠放");
                    const size_t before = state_.tiles.size();
                    if (!model::UnstackActiveMember(state_.tiles, spec.target_tile_id)) {
                        ShowUndoToast(L"没有足够空间拆分叠放");
                    } else if (state_.tiles.size() > before && state_.tiles.back().free_placed) {
                        auto& extra_tile = state_.tiles.back();
                        const std::vector<model::DesktopTile> prior(state_.tiles.begin(), state_.tiles.end() - 1);
                        const model::PointI spot = model::FindFreeDesktopSpotForTile(
                            prior, CurrentGroupFrames(), PrimaryWorkAreaClient(), tile_board_rect_,
                            extra_tile.w, extra_tile.h);
                        extra_tile.x = spot.x;
                        extra_tile.y = spot.y;
                    }
                    RecomputeLayout();
                    break;
                } else if (cmd.starts_with("tile-mat:")) {
                    PushUndo(L"磁贴效果");
                    const std::string v = cmd.substr(9);
                    t.material = v == "acrylic" ? model::CardMaterial::Acrylic
                               : v == "liquid-glass" ? model::CardMaterial::LiquidGlass
                               : v == "mica" ? model::CardMaterial::Mica
                               : v == "mica-alt" ? model::CardMaterial::MicaAlt : v == "solid" ? model::CardMaterial::Solid
                               : model::CardMaterial::FollowHost;
                } else if (cmd == "tile-bg:material") {
                    PushUndo(L"磁贴背景");
                    t.look.background = model::TileBackground::Material;
                } else if (cmd.starts_with("tile-color:")) {
                    PushUndo(L"磁贴背景");
                    t.look.background = model::TileBackground::Color;
                    t.look.color_argb = static_cast<uint32_t>(std::strtoul(cmd.c_str() + 11, nullptr, 16));
                } else if (cmd == "tile-image-pick" || cmd == "tile-image-photos") {
                    std::wstring path;
                    if (cmd == "tile-image-photos") {
                        if (!live_data_.photo_paths.empty()) path = live_data_.photo_paths[live_data_.active_photo_index % live_data_.photo_paths.size()];
                    } else {
                        const std::string tid = t.id;
                        path = PickImageFile();
                        auto again = std::find_if(state_.tiles.begin(), state_.tiles.end(), [&](const model::DesktopTile& x) { return x.id == tid; });
                        if (again == state_.tiles.end() || path.empty()) return;
                        PushUndo(L"磁贴背景");
                        again->look.background = model::TileBackground::Image;
                        again->look.image_path = path;
                        SaveModel();
                        renderer_.InvalidateAll();
                        return;
                    }
                    if (path.empty()) return;
                    PushUndo(L"磁贴背景");
                    t.look.background = model::TileBackground::Image;
                    t.look.image_path = path;
                } else if (cmd == "tile-show-title") {
                    t.look.show_title = !t.look.show_title;
                } else if (cmd == "tile-rename") {
                    OpenTileTextEditor(t, "@title:" + t.id, t.look.title_override.empty() ? model::ActiveTileView(t).title : t.look.title_override);
                    return;
                } else if (cmd == "tile-reset-look") {
                    PushUndo(L"恢复磁贴默认");
                    t.look = {};
                    t.material = model::CardMaterial::FollowHost;
                } else if (cmd == "todo-add") {
                    std::wstring lines;
                    for (const auto& td : state_.todos) lines += td.text + L"\n";
                    OpenTileTextEditor(t, "@todos", lines);
                    return;
                } else if (cmd.starts_with("todo-toggle:")) {
                    // From the 日历 detail card: tick a to-do off (or back on) without leaving the card.
                    const size_t todo_i = static_cast<size_t>(std::max(0, std::atoi(cmd.c_str() + 12)));
                    if (todo_i < state_.todos.size()) {
                        state_.todos[todo_i].done = !state_.todos[todo_i].done;
                        SaveModel();
                        RefreshWidgetDetail(model::TileKind::Calendar);
                        for (const auto& x : state_.tiles) {
                            const auto k = model::ActiveTileView(x).kind;
                            if (k == model::TileKind::Todo || k == model::TileKind::Calendar)
                                renderer_.InvalidateRect(render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, x));
                        }
                    }
                    return;
                } else if (cmd == "todo-clear-done") {
                    PushUndo(L"清除已完成");
                    std::erase_if(state_.todos, [](const model::TodoItem& td) { return td.done; });
                } else if (cmd == "weather-refresh") {
                    RequestWeather(true);
                } else if (cmd.starts_with("weather-city:")) {
                    state_.settings.weather_city = ipc::Utf8ToWide(cmd.substr(13));
                    RequestWeather(true);
                    SyncSettingsToPlugin();
                } else if (cmd == "weather-city-custom") {
                    OpenTileTextEditor(t, "@weather_city", state_.settings.weather_city);
                    return;
                } else if (cmd == "photo-next") {
                    AdvancePhoto(t.photo_shuffle);
                    photo_last_switch_ms_ = GetTickCount64();
                } else if (cmd.starts_with("photo-interval:")) {
                    const int seconds = std::clamp(std::atoi(cmd.c_str() + 15), 0, 24 * 3600);
                    for (auto& x : state_.tiles)
                        if (x.id == t.id) x.photo_interval_s = seconds;
                    photo_last_switch_ms_ = GetTickCount64();
                    SaveModel();
                    EnsurePhotoTimer();
                    return;
                } else if (cmd == "open-clock-app") {
                    ShellExecuteW(nullptr, L"open", L"ms-clock:", nullptr, nullptr, SW_SHOWNORMAL);
                    return;
                } else if (cmd == "open-this-pc") {
                    OpenPathInPulseOrShell(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", nullptr);
                    return;
                } else if (cmd == "disk-cleanup") {
                    ShellExecuteW(nullptr, L"open", L"cleanmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
                    return;
                } else if (cmd.starts_with("note-color:")) {
                    const int colour = std::clamp(std::atoi(cmd.c_str() + 11), 0, 5);
                    for (auto& x : state_.tiles)
                        if (x.id == t.id) x.note_color = colour;
                    SaveModel();
                    renderer_.InvalidateAll();
                    return;
                } else if (cmd == "photo-shuffle") {
                    for (auto& x : state_.tiles)
                        if (x.id == t.id) x.photo_shuffle = !x.photo_shuffle;
                    SaveModel();
                    return;
                } else if (cmd == "photo-folder-reset") {
                    for (auto& x : state_.tiles)
                        if (x.id == t.id) x.target_path.clear();
                    SaveModel();
                    RefreshLiveTileData();
                    renderer_.InvalidateAll();
                    return;
                } else if (cmd == "pick-photo-folder") {
                    const std::string tid = t.id;
                    const std::wstring dir = PickFolderDialog(L"选择照片轮播文件夹");
                    if (dir.empty()) return;
                    for (auto& x : state_.tiles) {
                        if (x.id == tid) x.target_path = dir;
                    }
                    RefreshLiveTileData();
                } else if (cmd == "open-latest-download") {
                    if (!live_data_.download_items.empty()) {
                        LaunchAsync(live_data_.download_items.front().path);
                    }
                    return;
                } else if (cmd == "clear-recent") {
                    live_data_.recent_items.clear();
                    std::thread([]() { SHAddToRecentDocs(SHARD_PIDL, nullptr); }).detach();
                    ShowUndoToast(L"已清除最近访问记录");
                } else if (cmd == "change-tile-target") {
                    const std::string tid = t.id;
                    const std::wstring dir = PickFolderDialog(L"选择磁贴目标文件夹");
                    if (dir.empty()) return;
                    for (auto& x : state_.tiles) {
                        if (x.id == tid) {
                            x.target_path = dir;
                            const std::wstring fn = std::filesystem::path(dir).filename().wstring();
                            if (!fn.empty()) x.title = fn;
                        }
                    }
                    RefreshLiveTileData();
                } else if (cmd == "empty-recycle-bin") {
                    const HWND notify_hwnd = mount_.Hwnd();
                    std::thread([notify_hwnd]() {
                        SHEmptyRecycleBinW(notify_hwnd, nullptr, 0);
                        if (notify_hwnd && IsWindow(notify_hwnd)) {
                            PostMessageW(notify_hwnd, kMsgFolderChanged, 0, 0);
                        }
                    }).detach();
                    return;
                } else if (cmd == "open-storage-settings") {
                    ShellExecuteW(nullptr, L"open", L"ms-settings:storagesense", nullptr, nullptr, SW_SHOWNORMAL);
                    return;
                } else if (cmd == "open-clock-app") {
                    ShellExecuteW(nullptr, L"open", L"ms-clock:", nullptr, nullptr, SW_SHOWNORMAL);
                    return;
                } else if (cmd == "open-calendar-app") {
                    ShellExecuteW(nullptr, L"open", L"outlookcal:", nullptr, nullptr, SW_SHOWNORMAL);
                    return;
                } else if (cmd == "copy-note") {
                    CopyWideTextToClipboard(mount_.Hwnd(), model::ActiveTileView(t).note_body);
                    ShowUndoToast(L"已复制便笺内容");
                    return;
                } else if (cmd == "clear-note") {
                    PushUndo(L"清空便笺");
                    t.note_body.clear();
                } else if (cmd == "tray-open-pulse") {
                    const std::wstring desk = mount::ResolveKnownFolder(FOLDERID_Desktop);
                    pipe_.Send(ipc::MessageType::OpenInPulseReq, ++next_req_id_,
                               "{\"mode\":\"tray\",\"path\":" + ipc::JsonQuoteWide(desk) + "}");
                    return;
                } else if (cmd == "tray-release-desktop") {
                    const std::wstring desk = mount::ResolveKnownFolder(FOLDERID_Desktop);
                    if (!desk.empty()) {
                        pipe_.Send(ipc::MessageType::TrayMutateReq, ++next_req_id_,
                                   "{\"op\":\"release\",\"dest\":" + ipc::JsonQuoteWide(desk) + "}");
                    }
                    return;
                } else if (cmd == "tray-clear") {
                    live_data_.tray_entries.clear();
                    live_data_.tray_total_bytes = 0;
                    pipe_.Send(ipc::MessageType::TrayMutateReq, ++next_req_id_, "{\"op\":\"clear\"}");
                } else if (cmd == "open-taskmgr") {
                    ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
                    return;
                } else if (cmd == "open-resmon") {
                    ShellExecuteW(nullptr, L"open", L"resmon.exe", nullptr, nullptr, SW_SHOWNORMAL);
                    return;
                } else if (cmd == "open-sys-about") {
                    ShellExecuteW(nullptr, L"open", L"ms-settings:about", nullptr, nullptr, SW_SHOWNORMAL);
                    return;
                } else if (cmd == "apps-all-out") {
                    PushUndo(L"全部放回桌面");
                    for (auto& it : state_.items) if (it.in_apps_tile) { it.pinned_group_id.clear(); it.force_loose = true; }
                    RecomputeLayout();
                } else if (cmd == "tiles-edit" || cmd == "tiles-edit-done") {
                    SetTileEditMode(cmd == "tiles-edit");
                    return;
                } else if (cmd == "toggle-tile-live") {
                    t.live_content = !t.live_content;
                } else if (cmd == "edit-note") {
                    const model::RectI tr = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t);
                    OpenStickyNotePopoverForTile(t, tr);
                    return;
                } else if (cmd == "remove-tile") {
                    PushUndo(L"取消固定磁贴");
                    state_.tiles.erase(state_.tiles.begin() + static_cast<ptrdiff_t>(i));
                } else if (cmd.starts_with("tile-size:") && cmd.size() >= 13) {
                    const size_t xp = cmd.find('x', 10);
                    if (xp != std::string::npos) {
                        const int c = std::clamp(std::stoi(cmd.substr(10, xp - 10)), 1, model::kTileMaxCols);
                        const int r = std::clamp(std::stoi(cmd.substr(xp + 1)), 1, model::kTileMaxRows);
                        const bool any_free = std::any_of(state_.tiles.begin(), state_.tiles.end(),
                                                          [](const model::DesktopTile& x) { return x.free_placed; });
                        auto res = !any_free
                            ? model::PreviewTilePlacement(state_.tiles, t.id, std::min(t.col, model::kTileCols - c),
                                                          std::min(t.row, model::kTileRows - r), {c, r})
                            : model::TilePlacementResult{};
                        PushUndo(L"调整磁贴大小");
                        if (res.valid) {
                            state_.tiles = std::move(res.updated_tiles);
                        } else {
                            model::ResizeTileFreeInteractive(
                                state_.tiles, i,
                                model::DefaultTileSpanPx(c), model::DefaultTileSpanPx(r),
                                PrimaryWorkAreaClient(), tile_board_rect_, CurrentGroupFrames());
                            model::CommitTileRestOrigins(state_.tiles);
                        }
                        RecomputeLayout();
                    }
                } else if (cmd == "open-tile") {
                    const model::RectI tr = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t);
                    ActivateTileClick(i, tr);
                    return;
                }
                break;
            }
            SaveModel();
            renderer_.InvalidateAll();
        }
    }

    std::vector<std::pair<model::TileKind, bool>> WidgetCatalog() const {
        static constexpr model::TileKind kWidgets[] = {
            model::TileKind::Photos, model::TileKind::Weather, model::TileKind::Calendar, model::TileKind::Todo,
            model::TileKind::Clock, model::TileKind::StickyNote, model::TileKind::Apps, model::TileKind::StagingTray,
            model::TileKind::Downloads, model::TileKind::Recent, model::TileKind::System, model::TileKind::DiskRing,
            model::TileKind::RecycleBin, model::TileKind::ThisPC, model::TileKind::PulseLauncher};
        std::vector<std::pair<model::TileKind, bool>> out;
        for (const auto k : kWidgets) out.push_back({k, model::BoardHasKind(state_.tiles, k)});
        return out;
    }
    void OpenTileMenu(size_t ti, int x, int y, std::string initial_submenu = {}) {
        if (ti >= state_.tiles.size()) return;
        const auto& t = state_.tiles[ti];
        const auto view = model::ActiveTileView(t);
        POINT scr{x, y};
        mount_.ClientToScreenDip(&scr);
        overlays::ContextMenuSpec spec{};
        spec.screen_anchor = scr;
        spec.target_kind = overlays::ContextMenuTargetKind::LiveTile;
        spec.target_tile_id = t.id;
        spec.tile_kind = view.kind;
        spec.tile_size = t.size;
        spec.tile_live_content = t.live_content;
        spec.tile_analog_clock = view.analog_clock;
        spec.tile_material = t.material;
        spec.tile_look = t.look;
        for (const auto& m : t.stack) spec.tile_stack_names.push_back(m.title);
        spec.tile_stack_index = t.stack_index;
        spec.tile_smart_rotate = t.smart_rotate;
        spec.tile_photo_interval_s = t.photo_interval_s;
        spec.tile_photo_shuffle = t.photo_shuffle;
        spec.tile_photo_custom_folder = !t.target_path.empty();
        spec.tile_note_color = t.note_color;
        spec.tile_edit_mode = interaction_.tile_edit_mode;
        spec.tile_has_photos = !live_data_.photo_paths.empty();
        spec.current_weather_city = state_.settings.weather_city;
        spec.recycle_bin_empty = (live_data_.recycle_bin.item_count == 0 && live_data_.recycle_bin.total_bytes == 0);
        spec.undo_label = undo_.NextUndoLabel();
        spec.initial_open_submenu_id = std::move(initial_submenu);
        overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
        overlays_.OpenContextMenu(spec, state_.settings);
    }

    void OpenGroupRulePopoverForIndex(size_t group_idx) {
        if (group_idx >= state_.groups.size() || group_idx >= group_views_.size()) return;
        POINT tl{group_views_[group_idx].frame.x, group_views_[group_idx].frame.y};
        POINT br{group_views_[group_idx].frame.right(),
                 group_views_[group_idx].frame.y + model::kGroupHeaderHeight};
        mount_.ClientToScreenDip(&tl);
        mount_.ClientToScreenDip(&br);
        overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
        overlays_.OpenGroupRulePopover(RECT{tl.x, tl.y, br.x, br.y}, state_.groups[group_idx]);
        SyncOverlayInteractionFlags();
    }

    void OpenStickyNotePopoverForTile(const model::DesktopTile& tile, const model::RectI& tile_rc) {
        POINT tl{tile_rc.x, tile_rc.y};
        POINT br{tile_rc.right(), tile_rc.bottom()};
        mount_.ClientToScreenDip(&tl);
        mount_.ClientToScreenDip(&br);
        overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
        overlays_.OpenStickyNotePopover(RECT{tl.x, tl.y, br.x, br.y}, tile.id, tile.note_body);
        SyncOverlayInteractionFlags();
    }

    // ----- widgets: edit mode, stacks, the 应用 and 待办 tiles -----
    // 天气: fetched off the UI thread at most every 30 minutes (or now, when asked or the city changed).
    void RequestWeather(bool force) {
        const std::wstring city = state_.settings.weather_city;
        if (city.empty()) {
            live_data_.weather = {};
            live_data_.weather.status = L"在设置里选一个城市";
            return;
        }
        const uint64_t now = GetTickCount64();
        if (!force && city == weather_fetched_city_ && weather_fetched_tick_ && now - weather_fetched_tick_ < 30ull * 60 * 1000) return;
        if (weather_inflight_->exchange(true)) return;
        weather_fetched_city_ = city;
        weather_fetched_tick_ = now;
        if (!live_data_.weather.valid) live_data_.weather.status = L"正在获取天气…";
        const HWND target = mount_.Hwnd();
        auto inflight = weather_inflight_;
        std::thread([city, target, inflight]() {
            auto* result = new live::WeatherResult(live::FetchWeather(city));
            inflight->store(false);
            if (!PostMessageW(target, kMsgWeatherReady, 0, reinterpret_cast<LPARAM>(result))) delete result;
        }).detach();
    }
    void ApplyWeather(std::unique_ptr<live::WeatherResult> r) {
        auto& w = live_data_.weather;
        if (!r->ok) {
            // Keep the last good forecast and say why it is old only when there is none.
            if (!w.valid) w.status = r->error;
            weather_fetched_tick_ = GetTickCount64() - 25ull * 60 * 1000;  // retry in five minutes
        } else {
            w.valid = true;
            w.status.clear();
            w.city = r->city.empty() ? state_.settings.weather_city : r->city;
            w.temp_c = r->temp_c; w.code = r->code; w.hi_c = r->hi_c; w.lo_c = r->lo_c;
            w.feels_c = r->feels_c; w.humidity_pct = r->humidity_pct; w.wind_kmh = r->wind_kmh;
            w.utc_offset_s = r->utc_offset_s; w.has_utc_offset = r->has_utc_offset;
            SYSTEMTIME now{};
            GetLocalTime(&now);
            wchar_t at[8]{};
            std::swprintf(at, ARRAYSIZE(at), L"%02u:%02u", now.wHour, now.wMinute);
            w.updated = at;
            w.hours.clear();
            for (const auto& h : r->hours) w.hours.push_back({h.label, h.code, h.temp_c});
            w.days.clear();
            for (const auto& d : r->days) w.days.push_back({d.label, d.code, d.lo_c, d.hi_c});
        }
        for (const auto& t : state_.tiles)
            if (model::ActiveTileView(t).kind == model::TileKind::Weather)
                renderer_.InvalidateRect(render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t));
        RefreshWidgetDetail(model::TileKind::Weather);
    }
    void SampleSystemMetrics() {
        const auto s = system_sampler_.Sample();
        auto& v = live_data_.system;
        v.valid = s.valid; v.cpu_pct = s.cpu_pct; v.mem_used = s.mem_used; v.mem_total = s.mem_total;
        v.disk_used_pct = s.disk_used_pct; v.disk_free = s.disk_free; v.net_down_bps = s.net_down_bps;
        for (const auto& t : state_.tiles)
            if (model::ActiveTileView(t).kind == model::TileKind::System)
                renderer_.InvalidateRect(render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t));
        RefreshWidgetDetail(model::TileKind::System);
    }
    // The detail card's data for a widget kind, from what the tiles show.
    overlays::WidgetDetailData MakeWidgetDetail(model::TileKind kind) const {
        overlays::WidgetDetailData d;
        d.kind = kind;
        d.weather = live_data_.weather;
        d.system = live_data_.system;
        d.todos = state_.todos;
        d.weather_city = state_.settings.weather_city;
        d.drives = live_data_.drives;
        for (const auto& t : state_.tiles)
            if (t.id == detail_tile_id_) d.analog_clock = model::ActiveTileView(t).analog_clock;
        return d;
    }
    void RefreshWidgetDetail(model::TileKind kind) {
        if (overlays_.WidgetDetailShows(kind)) overlays_.UpdateWidgetDetail(MakeWidgetDetail(kind));
    }
    void OpenWidgetDetailForTile(const model::DesktopTile& tile, model::TileKind kind, const model::RectI& tr) {
        POINT tl{tr.x, tr.y};
        POINT br{tr.right(), tr.bottom()};
        mount_.ClientToScreenDip(&tl);
        mount_.ClientToScreenDip(&br);
        overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
        if (kind == model::TileKind::System) SampleSystemMetrics();
        detail_tile_id_ = tile.id;
        overlays_.OpenWidgetDetail(RECT{tl.x, tl.y, br.x, br.y}, tile.id, MakeWidgetDetail(kind));
        SyncOverlayInteractionFlags();
    }

    void SetTileEditMode(bool on) {
        if (interaction_.tile_edit_mode == on) return;
        interaction_.tile_edit_mode = on;
        interaction_.edit_phase = 0.0f;
        interaction_.hovered_tile_header_btn = 0;
        if (on) {
            SetTimer(mount_.Hwnd(), kTimerEditJiggle, 33, nullptr);
        } else {
            KillTimer(mount_.Hwnd(), kTimerEditJiggle);
            if (overlays_.ActiveKind() == overlays::OverlayKind::WidgetGalleryDrawer) {
                overlays_.Dismiss();
                SyncOverlayInteractionFlags();
            }
        }
        renderer_.InvalidateAll();
    }
    // The shortcuts the 应用 tile shows, in its order (by name, as the renderer draws them).
    std::vector<size_t> AppsInTile() const {
        std::vector<size_t> idx;
        for (size_t i = 0; i < state_.items.size(); ++i) if (state_.items[i].in_apps_tile) idx.push_back(i);
        std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return state_.items[a].name < state_.items[b].name; });
        return idx;
    }
    // Press feel: in over ~90 ms (ease-out), back over ~280 ms with a small spring overshoot.
    void AnimateTilePress(const std::string& id, bool down) {
        if (id.empty() || (!down && interaction_.pressed_tile_id != id)) return;
        if (down) interaction_.pressed_tile_id = id;
        press_from_ = interaction_.tile_press;
        press_to_ = down ? 1.0f : 0.0f;
        press_start_ms_ = GetTickCount64();
        SetTimer(mount_.Hwnd(), kTimerTilePress, 15, nullptr);
    }
    void ReleaseTilePress() {
        if (!interaction_.pressed_tile_id.empty()) AnimateTilePress(interaction_.pressed_tile_id, false);
        if (interaction_.pressed_app_index >= 0) {
            interaction_.pressed_app_index = -1;
            renderer_.InvalidateRect(model::RectI{tile_board_rect_.x - 16, tile_board_rect_.y - 16,
                                                  tile_board_rect_.w + 32, tile_board_rect_.h + 32});
        }
    }
    void StepTilePress() {
        const bool down = press_to_ > 0.5f;
        const float duration = down ? 90.0f : 280.0f;
        const float t = std::clamp(static_cast<float>(GetTickCount64() - press_start_ms_) / duration, 0.0f, 1.0f);
        float eased;
        if (down) {
            eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        } else {
            constexpr float c1 = 1.70158f, c3 = c1 + 1.0f;  // ease-out-back
            eased = 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
        }
        interaction_.tile_press = press_from_ + (press_to_ - press_from_) * eased;
        for (const auto& tile : state_.tiles) {
            if (tile.id != interaction_.pressed_tile_id) continue;
            const model::RectI tr = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, tile);
            renderer_.InvalidateRect(model::RectI{tr.x - 12, tr.y - 12, tr.w + 24, tr.h + 24});
        }
        if (t >= 1.0f) {
            KillTimer(mount_.Hwnd(), kTimerTilePress);
            interaction_.tile_press = press_to_;
            if (!down) interaction_.pressed_tile_id.clear();
        }
    }
    // The first live 照片 tile (they share one slideshow), stacked ones included when showing.
    const model::DesktopTile* PhotoTile() const {
        for (const auto& t : state_.tiles)
            if (model::ActiveTileView(t).kind == model::TileKind::Photos && t.live_content) return &t;
        return nullptr;
    }
    // Opens a file or app via Explorer's desktop shell (so launched apps never inherit Pulse's job object),
    // on a worker thread so slow start-up never freezes the desktop. Falls back to ShellExecuteExW.
    static void LaunchAsync(const std::wstring& target) {
        LaunchDetached(target);
    }
    // 在文件夹中显示: Pulse selects it when connected, otherwise Explorer.
    void RevealPath(const std::wstring& path) {
        if (pipe_.IsConnected()) {
            pipe_.Send(ipc::MessageType::OpenInPulseReq, ++next_req_id_,
                       "{\"mode\":\"reveal\",\"path\":" + ipc::JsonQuoteWide(path) + "}");
            return;
        }
        const std::wstring args = L"/select,\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
    // Windows' own confirmation, then the emptying, on a worker; the tile refreshes when it is done.
    void EmptyRecycleBinAsync() {
        const HWND hwnd = mount_.Hwnd();
        std::thread([hwnd] {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            SHEmptyRecycleBinW(nullptr, nullptr, 0);
            if (SUCCEEDED(com)) CoUninitialize();
            PostMessageW(hwnd, kMsgFolderChanged, 0, 0);
        }).detach();
    }
    // One picture back or forward (back is always the previous in order).
    void StepPhoto(int dir, bool shuffle) {
        if (dir >= 0) { AdvancePhoto(shuffle); return; }
        const size_t n = live_data_.photo_paths.size() > 1 ? live_data_.photo_paths.size() : 3u;
        live_data_.prev_photo_index = live_data_.active_photo_index;
        live_data_.active_photo_index = (live_data_.active_photo_index + n - 1) % n;
        live_data_.photo_crossfade = 1.0f;
        for (const auto& t : state_.tiles)
            if (model::ActiveTileView(t).kind == model::TileKind::Photos)
                renderer_.InvalidateRect(render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t));
    }
    // Next picture: in order, or a different random one.
    void AdvancePhoto(bool shuffle) {
        const size_t n = live_data_.photo_paths.size() > 1 ? live_data_.photo_paths.size() : 3u;
        live_data_.prev_photo_index = live_data_.active_photo_index;
        size_t next = (live_data_.active_photo_index + 1) % n;
        if (shuffle && n > 2) {
            photo_rng_ = photo_rng_ * 6364136223846793005ull + 1442695040888963407ull;
            next = (live_data_.active_photo_index + 1 + static_cast<size_t>((photo_rng_ >> 33) % (n - 1))) % n;
        }
        live_data_.active_photo_index = next;
        live_data_.photo_crossfade = 1.0f;
        for (const auto& t : state_.tiles)
            if (model::ActiveTileView(t).kind == model::TileKind::Photos)
                renderer_.InvalidateRect(render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t));
    }
    // Keeps the slideshow timer at the photo tile's interval (none without a live photo tile or at 0).
    void EnsurePhotoTimer() {
        const model::DesktopTile* photo = PhotoTile();
        const UINT period = photo && photo->photo_interval_s > 0 ? static_cast<UINT>(photo->photo_interval_s) * 1000u : 0u;
        if (period == photo_timer_ms_) return;
        photo_timer_ms_ = period;
        if (period) SetTimer(mount_.Hwnd(), kTimerPhotos, period, nullptr);
        else KillTimer(mount_.Hwnd(), kTimerPhotos);
    }
    void ClearTileDrag() {
        KillTimer(mount_.Hwnd(), kTimerTileReorder);
        tile_drag_session_.Reset();
        dragging_tile_idx_.reset();
        drag_tile_undo_snapshot_ = {};
        interaction_.dragging_tile = false;
        interaction_.dragging_tile_id.clear();
        interaction_.stack_drop_target_tile_id.clear();
        interaction_.drag_tile_preview_rect = {};
        interaction_.drag_tile_valid = false;
    }

    void CancelTileDrag(bool recompute = true) {
        if (!tile_drag_session_.Active()) return;
        tile_drag_session_.Cancel(state_.tiles);
        ClearTileDrag();
        if (mount_.Hwnd() && GetCapture() == mount_.Hwnd()) {
            releasing_capture_ = true;
            ReleaseCapture();
            releasing_capture_ = false;
        }
        if (recompute) RecomputeLayout();
        renderer_.InvalidateAll();
    }

    void UpdateTileDrag(int x, int y) {
        if (!tile_drag_session_.Active() || !dragging_tile_idx_) return;
        const auto before = CaptureDragDamage();
        drag_tile_cursor_ = {x, y};
        if (tile_drag_session_.Update({x - drag_offset_.x, y - drag_offset_.y},
                                     drag_tile_cursor_, GetTickCount64())) {
            state_.tiles = tile_drag_session_.Preview();
            if (tile_drag_session_.Phase() != model::TileDragPhase::Reorder &&
                *dragging_tile_idx_ < state_.tiles.size()) {
                // Only the dragged card follows the pointer while a merge target
                // is reserved. The session owns a separate insertion commit plan.
                auto& moving = state_.tiles[*dragging_tile_idx_];
                const auto& rect = tile_drag_session_.ProposalRect();
                moving.free_placed = true;
                moving.x = rect.x;
                moving.y = rect.y;
            }
            interaction_.stack_drop_target_tile_id = tile_drag_session_.StackTarget();
            interaction_.drag_tile_preview_rect = tile_drag_session_.Target();
            interaction_.drag_tile_valid = tile_drag_session_.Valid();
            InvalidateDragDamage(before);
        }
        if (mount_.Hwnd()) {
            if (tile_drag_session_.NeedsDwellTimer()) SetTimer(mount_.Hwnd(), kTimerTileReorder, 50, nullptr);
            else KillTimer(mount_.Hwnd(), kTimerTileReorder);
        }
    }

    void FinishTileDrag(int x, int y) {
        UpdateTileDrag(x, y);
        if (!tile_drag_session_.Active() || !dragging_tile_idx_) return;
        if (!tile_drag_session_.Valid()) { CancelTileDrag(); return; }
        const auto moving_id = tile_drag_session_.Original()[*dragging_tile_idx_].id;
        const auto stack_on = tile_drag_session_.StackTarget();
        const bool changed = tile_drag_session_.PlacementChanged();
        auto before = std::move(drag_tile_undo_snapshot_);
        if (!stack_on.empty()) {
            auto stacked = tile_drag_session_.Original();
            if (!model::StackTiles(stacked, moving_id, stack_on)) { CancelTileDrag(); return; }
            state_.tiles = std::move(stacked);
        } else if (!tile_drag_session_.Commit(state_.tiles)) {
            CancelTileDrag();
            return;
        }
        ClearTileDrag();
        if (changed || !stack_on.empty()) {
            undo_.Push(before, stack_on.empty() ? L"移动磁贴" : L"叠放");
            last_undo_is_ai_archive_ = false;
            RecomputeLayout();
            SaveModel();
            if (!stack_on.empty()) ShowUndoToast(L"已叠放 · 滚动滚轮切换");
        }
        renderer_.InvalidateAll();
    }

    // Index into AppsInTile() under (x, y) for an 应用 tile drawn at tr, or -1 (mirrors DrawLiveTile).
    int AppCellAt(const model::DesktopTile& t, const model::RectI& tr, int x, int y) const {
        const auto view = model::ActiveTileView(t);
        if (view.kind != model::TileKind::Apps || t.size.cols < 2) return -1;
        const size_t n = AppsInTile().size();
        const D2D1_RECT_F rf = D2D1::RectF(static_cast<float>(tr.x), static_cast<float>(tr.y),
                                           static_cast<float>(tr.right()), static_cast<float>(tr.bottom()));
        const auto pg = render::DesktopRenderer::AppsTilePaging(t, rf, n);
        const float cell_w = (static_cast<float>(tr.w) - 16.0f) / static_cast<float>(pg.cols);
        if (x < tr.x + 8 || x >= tr.right() - 8 || y < pg.top) return -1;
        const int c = static_cast<int>((static_cast<float>(x - tr.x) - 8.0f) / cell_w);
        const int r = static_cast<int>((static_cast<float>(y) - pg.top) / pg.cell_h);
        if (c < 0 || c >= pg.cols || r < 0 || r >= pg.rows) return -1;
        const size_t i = static_cast<size_t>(std::min(interaction_.apps_page, pg.pages - 1)) * pg.room +
                         static_cast<size_t>(r * pg.cols + c);
        return i < n ? static_cast<int>(i) : -1;
    }
    int TodoRowAt(const model::DesktopTile& t, const model::RectI& tr, int y) const {
        const auto view = model::ActiveTileView(t);
        if (view.kind != model::TileKind::Todo || t.size.cols < 2) return -1;
        const int top = tr.y + (t.look.show_title ? 46 : 12);
        if (y < top) return -1;
        const int i = (y - top) / 29;
        return i < static_cast<int>(state_.todos.size()) && top + i * 29 + 26 <= tr.bottom() - 6 ? i : -1;
    }
    bool IsTodoAddButtonAt(const model::DesktopTile& t, const model::RectI& tr, int x, int y) const {
        const auto view = model::ActiveTileView(t);
        if (view.kind != model::TileKind::Todo || t.size.cols < 2) return false;
        if (x < tr.x + 10 || x > tr.right() - 10) return false;
        const int top = tr.y + (t.look.show_title ? 46 : 12);
        int shown = 0;
        for (size_t i = 0; i < state_.todos.size(); ++i) {
            if (top + static_cast<int>(i) * 29 + 26 > tr.bottom() - 6) break;
            ++shown;
        }
        const int add_y = top + shown * 29;
        return add_y + 24 <= tr.bottom() - 6 && y >= add_y && y <= add_y + 24;
    }
    int ListTileRowAt(const model::DesktopTile& t, const model::RectI& tr, int x, int y,
                      std::wstring* out_path = nullptr, bool* out_is_folder = nullptr, bool* out_action = nullptr) const {
        const auto view = model::ActiveTileView(t);
        if (out_action) *out_action = false;
        if (view.kind == model::TileKind::Downloads || view.kind == model::TileKind::Recent) {
            if (t.size.cols < 2 || !t.live_content) return -1;
            const auto& rows = (view.kind == model::TileKind::Downloads) ? live_data_.download_items : live_data_.recent_items;
            const D2D1_RECT_F rf = D2D1::RectF(static_cast<float>(tr.x), static_cast<float>(tr.y),
                                               static_cast<float>(tr.right()), static_cast<float>(tr.bottom()));
            const float fx = static_cast<float>(x), fy = static_cast<float>(y);
            for (size_t i = 0; i < rows.size(); ++i) {
                const D2D1_RECT_F row = render::DesktopRenderer::ListRowRect(view.kind, rf, i, t.look.show_title);
                if (row.bottom <= row.top) break;
                if (fx < row.left || fx >= row.right || fy < row.top || fy >= row.bottom) continue;
                if (out_path) *out_path = rows[i].path;
                if (out_is_folder) *out_is_folder = rows[i].is_folder;
                if (out_action) {
                    const D2D1_RECT_F b = render::DesktopRenderer::ListRowActionRect(row);
                    *out_action = fx >= b.left && fx < b.right && fy >= b.top && fy < b.bottom;
                }
                return static_cast<int>(i);
            }
            return -1;
        }
        if (view.kind == model::TileKind::PinnedFolder) {
            if (t.size.cols < 2 || t.size.rows < 2 || !t.live_content) return -1;
            auto it = live_data_.pinned_folder_items.find(view.target_path);
            if (it == live_data_.pinned_folder_items.end() || it->second.empty()) return -1;
            if (x < tr.x + 10 || x > tr.right() - 10) return -1;
            const int top = tr.y + (t.look.show_title ? 46 : 12);
            if (y < top) return -1;
            const int idx = (y - top) / 29;
            if (idx >= 0 && static_cast<size_t>(idx) < it->second.size() && top + idx * 29 + 26 <= tr.bottom() - 8) {
                if (out_path) *out_path = it->second[static_cast<size_t>(idx)].path;
                if (out_is_folder) *out_is_folder = it->second[static_cast<size_t>(idx)].is_folder;
                return idx;
            }
            return -1;
        }
        if (view.kind == model::TileKind::StagingTray) {
            if (live_data_.tray_entries.empty()) return -1;
            // The chips the renderer draws (TrayChipRect), minus the "+N" cell when they do not all fit.
            const D2D1_RECT_F rf = D2D1::RectF(static_cast<float>(tr.x), static_cast<float>(tr.y),
                                               static_cast<float>(tr.right()), static_cast<float>(tr.bottom()));
            const size_t room = render::DesktopRenderer::TrayChipCapacity(t, rf);
            const size_t shown = live_data_.tray_entries.size() > room ? (room > 0 ? room - 1 : 0) : live_data_.tray_entries.size();
            for (size_t i = 0; i < shown; ++i) {
                const D2D1_RECT_F c = render::DesktopRenderer::TrayChipRect(t, rf, i);
                if (static_cast<float>(x) >= c.left && static_cast<float>(x) < c.right &&
                    static_cast<float>(y) >= c.top && static_cast<float>(y) < c.bottom) {
                    if (out_path) *out_path = live_data_.tray_entries[i].path;
                    if (out_is_folder) *out_is_folder = live_data_.tray_entries[i].is_folder;
                    return static_cast<int>(i);
                }
            }
        }
        return -1;
    }
    void CycleTileSize(size_t i) {
        if (i >= state_.tiles.size()) return;
        const auto sizes = model::AllowedTileSizesFor(model::ActiveTileView(state_.tiles[i]).kind);
        if (sizes.size() < 2) return;
        auto cur = std::find(sizes.begin(), sizes.end(), state_.tiles[i].size);
        size_t at = cur == sizes.end() ? 0 : static_cast<size_t>(cur - sizes.begin());
        const auto next_sz = sizes[(at + 1) % sizes.size()];
        const bool any_free = std::any_of(state_.tiles.begin(), state_.tiles.end(),
                                          [](const model::DesktopTile& t) { return t.free_placed; });
        if (!any_free) {
            for (size_t step = 1; step < sizes.size(); ++step) {
                const auto s = sizes[(at + step) % sizes.size()];
                auto res = model::PreviewTilePlacement(state_.tiles, state_.tiles[i].id,
                                                       std::min(state_.tiles[i].col, model::kTileCols - s.cols),
                                                       std::min(state_.tiles[i].row, model::kTileRows - s.rows), s);
                if (res.valid) {
                    PushUndo(L"调整磁贴大小");
                    state_.tiles = std::move(res.updated_tiles);
                    RecomputeLayout();
                    SaveModel();
                    renderer_.InvalidateAll();
                    return;
                }
            }
        }
        PushUndo(L"调整磁贴大小");
        model::ResizeTileFreeInteractive(
            state_.tiles, i,
            model::DefaultTileSpanPx(next_sz.cols), model::DefaultTileSpanPx(next_sz.rows),
            PrimaryWorkAreaClient(), tile_board_rect_, CurrentGroupFrames());
        model::CommitTileRestOrigins(state_.tiles);
        RecomputeLayout();
        SaveModel();
        renderer_.InvalidateAll();
    }
    void RotateStacks() {
        if (interaction_.tile_edit_mode) return;
        for (auto& t : state_.tiles) {
            if (t.stack.size() < 2 || !t.smart_rotate || interaction_.hovered_tile_id == t.id) continue;
            t.stack_index = (t.stack_index + 1) % static_cast<int>(t.stack.size());
            renderer_.InvalidateRect(render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t));
        }
    }
    void OpenTileTextEditor(const model::DesktopTile& t, const std::string& token, const std::wstring& text) {
        const model::RectI tr = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t);
        POINT tl{tr.x, tr.y}, br{tr.right(), tr.bottom()};
        mount_.ClientToScreenDip(&tl);
        mount_.ClientToScreenDip(&br);
        overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
        overlays_.OpenStickyNotePopover(RECT{tl.x, tl.y, br.x, br.y}, token, text);
        SyncOverlayInteractionFlags();
    }
    std::wstring PickImageFile() {
        std::wstring out;
        Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
        static const COMDLG_FILTERSPEC kTypes[] = {{L"图片", L"*.jpg;*.jpeg;*.png;*.webp;*.bmp"}};
        dlg->SetFileTypes(1, kTypes);
        dlg->SetTitle(L"选择磁贴的背景图片");
        Microsoft::WRL::ComPtr<IShellItem> pics;
        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&pics)))) dlg->SetFolder(pics.Get());
        if (FAILED(dlg->Show(mount_.Hwnd()))) return out;
        Microsoft::WRL::ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
            out = path;
            CoTaskMemFree(path);
        }
        return out;
    }

    void ActivateTileClick(size_t idx, const model::RectI& tr) {
        if (idx >= state_.tiles.size()) return;
        auto& t_in = state_.tiles[idx];
        const model::DesktopTile t = model::ActiveTileView(t_in);
        switch (t.kind) {
        case model::TileKind::Apps:
            OpenPathInPulseOrShell(mount::ResolveKnownFolder(FOLDERID_Desktop), &tr);
            return;
        case model::TileKind::Weather:
            if (state_.settings.weather_city.empty()) {
                OpenTileTextEditor(t_in, "@weather_city", L"北京");
            } else {
                // The forecast card; a stale forecast refreshes behind it.
                RequestWeather(false);
                OpenWidgetDetailForTile(t_in, t.kind, tr);
            }
            return;
        case model::TileKind::System:
        case model::TileKind::Calendar:
        case model::TileKind::Clock:
        case model::TileKind::DiskRing:
            OpenWidgetDetailForTile(t_in, t.kind, tr);
            return;
        case model::TileKind::Todo:
            return;
        default:
            break;
        }
        switch (t.kind) {
        case model::TileKind::StagingTray: {
            POINT tl{tr.x, tr.y};
            POINT br{tr.right(), tr.bottom()};
            mount_.ClientToScreenDip(&tl);
            mount_.ClientToScreenDip(&br);
            overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
            overlays_.OpenStagingTrayPopover(RECT{tl.x, tl.y, br.x, br.y},
                                             live_data_.tray_entries, live_data_.tray_total_bytes);
            SyncOverlayInteractionFlags();
            break;
        }
        case model::TileKind::StickyNote:
            OpenStickyNotePopoverForTile(t, tr);
            break;
        case model::TileKind::Clock:
            if (t_in.stack.empty()) t_in.analog_clock = !t_in.analog_clock;
            else if (t_in.stack_index >= 0 && t_in.stack_index < static_cast<int>(t_in.stack.size()))
                t_in.stack[static_cast<size_t>(t_in.stack_index)].analog_clock = !t_in.stack[static_cast<size_t>(t_in.stack_index)].analog_clock;
            SaveModel();
            renderer_.InvalidateRect(tr);
            break;
        case model::TileKind::Downloads:
            OpenPathInPulseOrShell(mount::ResolveKnownFolder(FOLDERID_Downloads), &tr);
            break;
        case model::TileKind::Recent:
            OpenPathInPulseOrShell(mount::ResolveKnownFolder(FOLDERID_Recent), &tr);
            break;
        case model::TileKind::Photos:
            OpenPathInPulseOrShell(t.target_path.empty()
                                       ? mount::ResolveKnownFolder(FOLDERID_Pictures)
                                       : t.target_path, &tr);
            break;
        case model::TileKind::RecycleBin:
            ShellExecuteW(nullptr, L"open", L"shell:RecycleBinFolder", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case model::TileKind::ThisPC:
            OpenPathInPulseOrShell(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", &tr);
            break;
        case model::TileKind::DiskRing:
            OpenPathInPulseOrShell(t.target_path.empty() ? L"C:\\" : t.target_path, &tr);
            break;
        case model::TileKind::PulseLauncher:
            OpenPathInPulseOrShell(mount::ResolveKnownFolder(FOLDERID_Desktop), &tr);
            break;
        case model::TileKind::PinnedFolder:
        case model::TileKind::PinnedItem:
            OpenPathInPulseOrShell(t.target_path, &tr);
            break;
        }
    }

    void OpenSearchOverlay(const std::wstring& initial_query) {
        // With Pulse running, desktop search is Pulse's quick search. Keys typed before its window takes
        // focus land here and are re-sent with everything typed so far.
        if (pipe_.IsConnected()) {
            const uint64_t now = GetTickCount64();
            if (initial_query.empty() || now - quick_search_tick_ > 1500) quick_search_seed_.clear();
            quick_search_seed_ += initial_query;
            quick_search_tick_ = now;
            if (parent_pid_ != 0) AllowSetForegroundWindow(parent_pid_);
            pipe_.Send(ipc::MessageType::OpenInPulseReq, ++next_req_id_,
                       "{\"mode\":\"quick_search\",\"query\":" + ipc::JsonQuoteWide(quick_search_seed_) + "}");
            return;
        }
        interaction_.search_highlight_query = initial_query;
        overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
        overlays_.OpenSearchPopup(PrimaryWorkAreaScreen(), initial_query, BuildLocalSearchHits(initial_query));
        SyncOverlayInteractionFlags();
        if (!initial_query.empty()) {
            pipe_.Send(ipc::MessageType::SearchQueryReq, ++next_req_id_,
                       "{\"query\":" + ipc::JsonQuoteWide(initial_query) + "}");
        }
        renderer_.InvalidateAll();
    }

    void OpenHealthCheckDrawer() {
        installed_apps_cache_ = model::QueryInstalledAppsFromRegistry();
        RefreshHealthStats();
        overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
        overlays_.OpenHealthDrawer(PrimaryWorkAreaScreen(), cached_health_report_, state_.items.size());
        RecomputeLayout();
        renderer_.InvalidateAll();
    }

    void OpenBeautifyOverlay() {
        overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
        overlays_.OpenBeautifyDrawer(PrimaryWorkAreaScreen(), state_.settings, ActiveWidgetKinds());
        RecomputeLayout();
        renderer_.InvalidateAll();
    }

    bool AiOrganizerEnabled() {
        if (!assistant_availability_) assistant_availability_.emplace(parent_proc_);
        return assistant_availability_->Enabled();
    }

    bool RequireAiOrganizerEnabled() {
        if (AiOrganizerEnabled()) return true;
        if (active_ai_session_ || overlays_.IsAiOrganizeUiOpen()) {
            ++ai_req_seq_;
            if (active_ai_session_) {
                active_ai_session_->Cancel();
                active_ai_session_.reset();
            }
            if (overlays_.IsAiOrganizeUiOpen()) overlays_.Dismiss();
        }
        return false;
    }

    void OpenAiOrganizeDrawer(std::string scope_group_id = {}) {
        if (!RequireAiOrganizerEnabled()) return;
        if (active_ai_session_) {
            active_ai_session_->Cancel();
            active_ai_session_.reset();
        }
        ++ai_req_seq_;
        RefreshDesktopItems();

        ai_view_state_.connection = ai::ProbePulseAiAssistantConnection();
        ai_view_state_.scope_group_id = std::move(scope_group_id);
        ai_view_state_.scope_group_title.clear();
        ai_view_state_.error_banner.clear();
        for (const auto& g : state_.groups) {
            if (g.id == ai_view_state_.scope_group_id) {
                ai_view_state_.scope_group_title = g.title;
                break;
            }
        }
        if (!ai_view_state_.scope_group_id.empty()) {
            ai_view_state_.strategy = ai::AiOrganizeStrategy::ByProjectTopic;
        }

        if (!ai_view_state_.connection.configured) {
            ai_view_state_.phase = overlays::AiDrawerPhase::UnconfiguredGuide;
            ai_view_state_.plan = {};
            overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
            overlays_.OpenAiOrganizeDrawer(PrimaryWorkAreaScreen(), ai_view_state_);
            RecomputeLayout();
            renderer_.InvalidateAll();
            return;
        }

        ai_view_state_.phase = overlays::AiDrawerPhase::Loading;
        ai_view_state_.plan = {};
        overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
        overlays_.OpenAiOrganizeDrawer(PrimaryWorkAreaScreen(), ai_view_state_);
        RecomputeLayout();
        renderer_.InvalidateAll();
        StartAiPlanRequest(ai_view_state_.strategy, ai_view_state_.custom_instruction, false);
    }

    void StartAiPlanRequest(ai::AiOrganizeStrategy strategy,
                            const std::wstring& custom_instruction,
                            bool force_local) {
        if (!RequireAiOrganizerEnabled()) return;
        if (active_ai_session_) {
            active_ai_session_->Cancel();
            active_ai_session_.reset();
        }
        const uint64_t seq = ++ai_req_seq_;
        ai_view_state_.connection = ai::ProbePulseAiAssistantConnection();
        ai_view_state_.strategy = strategy;
        ai_view_state_.custom_instruction = custom_instruction;
        ai_view_state_.error_banner.clear();
        ai_view_state_.phase = overlays::AiDrawerPhase::Loading;
        if (overlays_.ActiveKind() == overlays::OverlayKind::AiOrganizeDrawer) {
            overlays_.UpdateAiOrganizeDrawer(ai_view_state_);
        }

        ai::AiOrganizeSession::Request req{};
        req.desktop_dir = mount::ResolveKnownFolder(FOLDERID_Desktop);
        req.strategy = strategy;
        req.custom_instruction = custom_instruction;
        if (req.custom_instruction.empty() && !ai_view_state_.scope_group_title.empty()) {
            req.custom_instruction = L"聚焦归档「" + ai_view_state_.scope_group_title + L"」分组内的文件";
        }
        req.force_local_fallback = force_local;
        req.groups = state_.groups;
        req.now_unix_ms = NowUnixMs();

        if (ai_view_state_.scope_group_id.empty()) {
            req.items = state_.items;
        } else {
            std::optional<size_t> target_gi;
            for (size_t gi = 0; gi < state_.groups.size(); ++gi) {
                if (state_.groups[gi].id == ai_view_state_.scope_group_id) {
                    target_gi = gi;
                    break;
                }
            }
            for (const auto& it : state_.items) {
                if (target_gi && model::ClassifyItemToGroup(it, state_.groups, req.now_unix_ms) == *target_gi) {
                    req.items.push_back(it);
                }
            }
            if (req.items.empty()) {
                req.items = state_.items;
            }
        }

        const HWND target_hwnd = mount_.Hwnd();
        active_ai_session_ = ai::AiOrganizeSession::Start(
            std::move(req),
            [this, seq, target_hwnd](ai::AiOrganizeSession::Outcome outcome) {
                {
                    std::lock_guard<std::mutex> lk(ai_mutex_);
                    pending_ai_seq_ = seq;
                    pending_ai_outcome_ = std::move(outcome);
                }
                if (target_hwnd && IsWindow(target_hwnd)) {
                    PostMessageW(target_hwnd, kMsgAiPlanReady, 0, 0);
                }
            });
    }

    void OnAiPlanReady() {
        if (!RequireAiOrganizerEnabled()) return;
        uint64_t seq = 0;
        ai::AiOrganizeSession::Outcome outcome{};
        {
            std::lock_guard<std::mutex> lk(ai_mutex_);
            seq = pending_ai_seq_;
            outcome = std::move(pending_ai_outcome_);
        }
        if (seq == 0 || seq != ai_req_seq_ || outcome.cancelled) {
            return;
        }
        active_ai_session_.reset();

        if (outcome.ok) {
            ai_view_state_.plan = std::move(outcome.plan);
            if (!ai_view_state_.scope_group_id.empty() &&
                state_.items.size() > ai_view_state_.plan.desktop_before_count) {
                ai_view_state_.plan.desktop_before_count = state_.items.size();
            }
            ai_view_state_.error_banner.clear();
            ai_view_state_.phase = overlays::AiDrawerPhase::PlanReady;
        } else if (outcome.unconfigured) {
            ai_view_state_.connection = ai::ProbePulseAiAssistantConnection();
            ai_view_state_.error_banner = outcome.error_message;
            ai_view_state_.phase = overlays::AiDrawerPhase::UnconfiguredGuide;
        } else {
            std::vector<ai::DesktopAiCandidate> cands;
            std::vector<ai::ExistingDesktopFolder> existing_dirs;
            const std::wstring desk = mount::ResolveKnownFolder(FOLDERID_Desktop);
            ai::CollectDesktopAiContext(desk, state_.items, state_.groups, NowUnixMs(), cands, existing_dirs);
            ai_view_state_.plan = ai::BuildSmartLocalOrganizePlan(
                desk, ai_view_state_.strategy, ai_view_state_.custom_instruction, cands, existing_dirs);
            ai_view_state_.error_banner = outcome.error_message.empty()
                                              ? L"AI 服务暂不可用，已切换为本地智能规则预览"
                                              : (outcome.error_message + L"（已展示本地智能预览）");
            ai_view_state_.phase = overlays::AiDrawerPhase::PlanReady;
        }

        if (overlays_.ActiveKind() == overlays::OverlayKind::AiOrganizeDrawer) {
            overlays_.UpdateAiOrganizeDrawer(ai_view_state_);
        }
    }

    void ExecuteAiArchiveAndRecordUndo(const ai::AiOrganizePlan& plan, bool pin_created_folders) {
        if (!RequireAiOrganizerEnabled()) return;
        model::DesktopModelState updated_model = state_;
        ai::AiArchiveExecutionResult res =
            ai::ExecuteAiArchivePlan(plan, state_, pin_created_folders, &updated_model);
        if (!res.ok) {
            ai_view_state_.error_banner =
                res.error_message.empty() ? L"归档执行失败，未改动桌面原文件" : res.error_message;
            ai_view_state_.phase = overlays::AiDrawerPhase::PlanReady;
            if (overlays_.ActiveKind() == overlays::OverlayKind::AiOrganizeDrawer) {
                overlays_.UpdateAiOrganizeDrawer(ai_view_state_);
            }
            return;
        }

        state_ = std::move(updated_model);
        last_ai_archive_undo_ = std::move(res.undo_tx);
        last_undo_is_ai_archive_ = !last_ai_archive_undo_.empty();
        pending_ai_archive_toast_count_ = res.moved_count;

        RefreshDesktopItems();
        RefreshLiveTileData();
        RefreshHealthStats();
        RecomputeLayout();
        SaveModel();
        renderer_.InvalidateAll();

        ai_view_state_.phase = overlays::AiDrawerPhase::Completed;
        ai_view_state_.completed_moved_count = res.moved_count;
        ai_view_state_.completed_folder_count = std::max(res.created_folder_count, plan.ActiveFolderCount());
        ai_view_state_.error_banner.clear();
        if (overlays_.ActiveKind() == overlays::OverlayKind::AiOrganizeDrawer) {
            overlays_.UpdateAiOrganizeDrawer(ai_view_state_);
        }
    }

    void ExecuteHealthRemediation(bool archive, const std::vector<std::wstring>& paths) {
        if (paths.empty()) return;
        PushUndo(L"桌面体检清理");
        if (archive) {
            const std::wstring desk = mount::ResolveKnownFolder(FOLDERID_Desktop);
            const std::filesystem::path archive_dir = std::filesystem::path(desk) / L"桌面归档";
            std::error_code ec;
            std::filesystem::create_directories(archive_dir, ec);
            size_t moved = 0;
            for (const auto& p : paths) {
                std::filesystem::path sp(p);
                std::filesystem::path dp = archive_dir / sp.filename();
                if (MoveFileExW(p.c_str(), dp.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                    ++moved;
                }
            }
            RefreshDesktopItems();
            RefreshHealthStats();
            RecomputeLayout();
            renderer_.InvalidateAll();
            ShowUndoToast(L"已归档 " + std::to_wstring(moved) + L" 个桌面项目至「桌面归档」");
        } else {
            RecyclePathsWithUndoToast(paths);
        }
    }

    void SendHoverPreview(const std::string& request) {
        if (request.empty()) return;
#ifdef PULSE_DESKTOP_PREVIEW_TEST
        test_hover_request_ = request;
#else
        pipe_.Send(ipc::MessageType::OpenInPulseReq, ++next_req_id_, request);
#endif
    }

    void CancelHoverPreview() { SendHoverPreview(hover_preview_.Cancel()); }

    void UpdateHoverPreviewAt(int x, int y) {
        if (state_.settings.icons_hidden || state_.settings.active_scene == model::DesktopScene::Present ||
            overlays_.IsOpen() || interaction_.tile_edit_mode || pending_item_drag_ ||
            pending_tile_idx_ || interaction_.marquee_active ||
            interaction_.dragging_items || interaction_.drop_hint_visible || group_drag_session_.Pending() ||
            dragging_tile_idx_ || resizing_tile_idx_ || resizing_group_idx_) {
            CancelHoverPreview();
            return;
        }
        std::wstring path;
        model::RectI rect{};
        bool tile_hit = false;
        if (state_.settings.organize_enabled && state_.settings.tiles_enabled) {
            for (const auto& tile : state_.tiles) {
                const auto tr = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, tile);
                if (!tr.contains(x, y)) continue;
                tile_hit = true;
                rect = tr;
                bool folder = false;
                bool action = false;
                ListTileRowAt(tile, tr, x, y, &path, &folder, &action);
                if (folder || action) path.clear();
                const int app = AppCellAt(tile, tr, x, y);
                if (app >= 0) {
                    const auto& item = state_.items[AppsInTile()[static_cast<size_t>(app)]];
                    if (!item.is_folder) path = item.path;
                }
                break;
            }
        }
        if (!tile_hit) {
            if (const auto hit = HitTestItem(x, y, &rect)) {
                if (!state_.items[*hit].is_folder) path = state_.items[*hit].path;
            }
        }
        const POINT tl = mount_.ClientDipToScreenPx({rect.x, rect.y});
        const POINT br = mount_.ClientDipToScreenPx({rect.right(), rect.bottom()});
        SendHoverPreview(hover_preview_.Update(path, tl.x, tl.y, br.x - tl.x, br.y - tl.y));
    }

    void OpenQuickLookForPath(const std::wstring& path) {
        const auto item = std::find_if(state_.items.begin(), state_.items.end(),
            [&](const auto& entry) { return entry.path == path; });
        if (item == state_.items.end() || !item->is_folder) {
            RECT anchor{};
            const auto cell = FindItemCellRect(path);
            if (cell) {
                const auto tl = mount_.ClientDipToScreenPx({cell->x, cell->y});
                const auto br = mount_.ClientDipToScreenPx({cell->right(), cell->bottom()});
                anchor = {tl.x, tl.y, br.x, br.y};
            }
            const auto request = pulse::desktop::PulseFilePreviewRequest(path, cell ? &anchor : nullptr);
            if (request.empty()) return;
            CancelHoverPreview();
            interaction_.focused_path = path;
            if (overlays_.ActiveKind() == overlays::OverlayKind::QuickLook) overlays_.Dismiss();
            SyncOverlayInteractionFlags();
#ifdef PULSE_DESKTOP_PREVIEW_TEST
            test_preview_request_ = request;
#else
            pipe_.Send(ipc::MessageType::OpenInPulseReq, ++next_req_id_, request);
#endif
            return;
        }
        // Folder listings retain the desktop overlay; files use Pulse's full engine.
        for (const auto& it : state_.items) {
            if (it.path == path) {
                interaction_.focused_path = path;
                overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
                overlays_.OpenQuickLook(PrimaryWorkAreaScreen(), it, nullptr);
                SyncOverlayInteractionFlags();
                pipe_.Send(ipc::MessageType::PreviewReq, ++next_req_id_,
                           "{\"path\":" + ipc::JsonQuoteWide(path) + "}");
                break;
            }
        }
    }

    void CycleQuickLook(int delta) {
        if (state_.items.empty()) return;
        size_t cur = 0;
        for (size_t i = 0; i < state_.items.size(); ++i) {
            if (state_.items[i].path == interaction_.focused_path) {
                cur = i;
                break;
            }
        }
        const size_t n = state_.items.size();
        const size_t next = (cur + n + static_cast<size_t>(delta % static_cast<int>(n))) % n;
        OpenQuickLookForPath(state_.items[next].path);
    }

    void StartInlineRenameForPath(const std::wstring& path) {
        for (size_t i = 0; i < state_.items.size(); ++i) {
            if (_wcsicmp(state_.items[i].path.c_str(), path.c_str()) != 0) continue;
            const auto cell_opt = FindItemCellRect(path);
            const model::RectI cell = cell_opt.value_or(
                model::RectI{state_.items[i].loose_pos.x, state_.items[i].loose_pos.y,
                             model::kLooseCellW, model::kLooseCellH});
            const model::RectI label_rc{cell.x + 2, cell.bottom() - 32, cell.w - 4, 26};

            int stem_len = -1;
            if (!state_.items[i].is_folder) {
                const auto dot = state_.items[i].name.rfind(L'.');
                if (dot != std::wstring::npos && dot > 0) {
                    stem_len = static_cast<int>(dot);
                }
            }

            overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
            overlays_.BeginInlineRename(
                mount_.Hwnd(), label_rc, state_.items[i].name, false,
                [this, old_path = path](const std::wstring& new_name) {
                    if (new_name.empty()) return;
                    std::filesystem::path sp(old_path);
                    std::filesystem::path dp = sp.parent_path() / new_name;
                    if (_wcsicmp(old_path.c_str(), dp.c_str()) != 0 &&
                        MoveFileExW(old_path.c_str(), dp.c_str(), 0)) {
                        RefreshDesktopItems();
                        RecomputeLayout();
                        renderer_.InvalidateAll();
                    }
                },
                stem_len);
            break;
        }
    }

    void SyncSettingsToPlugin() {
        char buf[1024]{};
        std::snprintf(
            buf, sizeof(buf),
            "{\"enabled\":true,\"material\":%u,"
            "\"glass_preset\":%u,\"glass_thickness\":%u,\"glass_ior\":%u,"
            "\"glass_disp_gain\":%u,\"glass_glare\":%u,\"glass_angle\":%d,\"glass_frost\":%u,"
            "\"glass_disp\":%u,\"glass_rate\":%u,"
            "\"scene\":%u,\"label_style\":%u,"
            "\"icon_size\":%u,\"wallpaper_preset\":%u,"
            "\"opacity\":%u,\"auto_accent\":%s,\"accent\":%u,"
            "\"live_tiles\":%s,\"sync_tray\":%s,\"type_search\":%s,"
            "\"dbl_hide\":%s,\"icons_hidden\":%s,\"auto_sleep\":%s}",
            static_cast<unsigned>(state_.settings.default_material),
            static_cast<unsigned>(std::min<uint8_t>(static_cast<uint8_t>(state_.settings.glass_preset), 5)),
            static_cast<unsigned>(std::clamp<uint8_t>(state_.settings.glass_thickness, 1, 80)),
            static_cast<unsigned>(std::clamp<uint16_t>(state_.settings.glass_ior_x100, 100, 400)),
            static_cast<unsigned>(std::min<uint8_t>(state_.settings.glass_dispersion, 50)),
            static_cast<unsigned>(std::min<uint8_t>(state_.settings.glass_glare, 120)),
            static_cast<int>(std::clamp<int16_t>(state_.settings.glass_glare_angle, -180, 180)),
            static_cast<unsigned>(std::min<uint8_t>(state_.settings.glass_frost, 100)),
            static_cast<unsigned>(model::MapGlassGainToLegacyTier(state_.settings.glass_dispersion)),
            static_cast<unsigned>(state_.settings.glass_live_rate),
            static_cast<unsigned>(state_.settings.active_scene),
            static_cast<unsigned>(state_.settings.label_style),
            static_cast<unsigned>(state_.settings.icon_size),
            static_cast<unsigned>(state_.settings.wallpaper_preset),
            static_cast<unsigned>(state_.settings.card_opacity_percent),
            state_.settings.auto_wallpaper_accent ? "true" : "false",
            state_.settings.accent_argb,
            state_.settings.live_tiles_enabled ? "true" : "false",
            state_.settings.sync_staging_tray ? "true" : "false",
            state_.settings.type_to_search ? "true" : "false",
            state_.settings.double_click_hide_icons ? "true" : "false",
            state_.settings.icons_hidden ? "true" : "false",
            state_.settings.pause_on_fullscreen_or_battery ? "true" : "false");
        std::string synced = buf;
        synced.pop_back();
        synced += GlassOpticalJson(state_.settings.glass_options);
        synced += ",\"glass_reference\":" + std::string(state_.settings.glass_reference ? "true" : "false") +
            ",\"glass_blur_radius\":" + std::to_string(state_.settings.glass_blur_radius) +
            ",\"glass_blur_edge\":" + (state_.settings.glass_blur_edge ? "true" : "false") + "}";
        pipe_.Send(ipc::MessageType::SettingsSync, ++next_req_id_, synced);
    }

    // -----------------------------------------------------------------------
    // IPC Drain on Main UI Thread
    // -----------------------------------------------------------------------
    void DrainIpcInbox() {
        std::deque<ipc::Frame> batch;
        {
            std::lock_guard<std::mutex> lk(ipc_mutex_);
            batch.swap(ipc_inbox_);
        }

        for (auto& f : batch) {
            const std::string_view json = f.PayloadText();
            switch (f.type) {
            case ipc::MessageType::AppearanceSync: {
                uint64_t eff = 1;
                if (ipc::JsonFindUint64(json, "effect", eff)) {
                    interaction_.host_material = (eff == 0) ? model::CardMaterial::Solid
                                               : (eff == 2) ? model::CardMaterial::Mica
                                               : (eff == 3) ? model::CardMaterial::MicaAlt
                                                            : model::CardMaterial::Acrylic;
                }
                ApplySystemAppearance();
                renderer_.ReloadWallpaper(state_.settings.wallpaper_preset, state_.settings.dark_mode);
                renderer_.InvalidateAll();
                break;
            }
            case ipc::MessageType::SettingsSync: {
                const model::BeautifySettings prev_settings = state_.settings;
                uint64_t u = 0;
                bool b = false;
                if (ipc::JsonFindUint64(json, "material", u)) {
                    state_.settings.default_material =
                        (u <= 5) ? static_cast<model::CardMaterial>(u) : model::CardMaterial::Acrylic;
                }
                if (ipc::JsonFindUint64(json, "glass_preset", u)) {
                    state_.settings.glass_preset = static_cast<uint8_t>(std::min<uint64_t>(u, 5));
                }
                bool glass_preview = false;
                ipc::JsonFindBool(json,"glass_preview",glass_preview);
                if (ipc::JsonFindUint64(json,"glass_schema",u) && u >= 2) {
                    for (const auto& field : pulse::desktop::kGlassOpticalFields) {
                        double value=0;
                        if(ipc::JsonFindDouble(json,field.key,value)) state_.settings.glass_options.*(field.member)=pulse::desktop::ClampGlassValue(field,value);
                    }
                    if(ipc::JsonFindBool(json,"glass2_blur_edge",b)) state_.settings.glass_options.blur_edge=b;
                }
                if (ipc::JsonFindBool(json, "glass_reference", b)) state_.settings.glass_reference = b;
                else if (ipc::JsonFindUint64(json, "glass_preset", u)) state_.settings.glass_reference = (u == 5);
                if (ipc::JsonFindBool(json, "glass_blur_edge", b)) state_.settings.glass_blur_edge = b;
                if (ipc::JsonFindUint64(json, "glass_blur_radius", u)) state_.settings.glass_blur_radius = static_cast<uint8_t>(std::clamp<uint64_t>(u, 1, 200));
                if (ipc::JsonFindUint64(json, "glass_thickness", u)) {
                    state_.settings.glass_thickness = static_cast<uint8_t>(std::clamp<uint64_t>(u, 1, 80));
                }
                if (ipc::JsonFindUint64(json, "glass_ior", u)) {
                    state_.settings.glass_ior_x100 = static_cast<uint16_t>(std::clamp<uint64_t>(u, 100, 400));
                }
                if (ipc::JsonFindUint64(json, "glass_disp_gain", u)) {
                    state_.settings.glass_dispersion = static_cast<uint8_t>(std::min<uint64_t>(u, 50));
                } else if (ipc::JsonFindUint64(json, "glass_disp", u)) {
                    state_.settings.glass_dispersion = model::MapLegacyGlassDispersionToGain(static_cast<uint8_t>(u));
                }
                if (ipc::JsonFindUint64(json, "glass_glare", u)) {
                    state_.settings.glass_glare = static_cast<uint8_t>(std::min<uint64_t>(u, 120));
                }
                if (int64_t s = 0; ipc::JsonFindInt64(json, "glass_angle", s)) {
                    state_.settings.glass_glare_angle = static_cast<int16_t>(std::clamp<int64_t>(s, -180, 180));
                }
                if (ipc::JsonFindUint64(json, "glass_frost", u)) {
                    state_.settings.glass_frost = static_cast<uint8_t>(std::min<uint64_t>(u, 100));
                }
                if (ipc::JsonFindUint64(json, "glass_rate", u)) {
                    state_.settings.glass_live_rate = static_cast<uint8_t>(std::min<uint64_t>(u, 2));
                }
                if (ipc::JsonFindUint64(json, "scene", u)) state_.settings.active_scene = static_cast<model::DesktopScene>(std::min<uint64_t>(u, 2));
                if (ipc::JsonFindUint64(json, "label_style", u)) state_.settings.label_style = static_cast<model::IconLabelStyle>(std::min<uint64_t>(u, static_cast<uint64_t>(model::IconLabelStyle::Normal)));
                if (ipc::JsonFindUint64(json, "icon_size", u)) state_.settings.icon_size = static_cast<model::DesktopIconSize>(std::min<uint64_t>(u, 2));
                if (ipc::JsonFindUint64(json, "wallpaper_preset", u)) {
                    const auto np = static_cast<model::WallpaperPreset>(std::min<uint64_t>(u, 4));
                    if (np != state_.settings.wallpaper_preset) {
                        state_.settings.wallpaper_preset = np;
                        renderer_.ReloadWallpaper(np, state_.settings.dark_mode);
                        SyncSystemDesktopWallpaper(np, state_.settings.dark_mode, false);
                    }
                }
                if (ipc::JsonFindUint64(json, "opacity", u)) state_.settings.card_opacity_percent = static_cast<uint8_t>(std::min<uint64_t>(u, 100));
                if (ipc::JsonFindBool(json, "live_tiles", b)) state_.settings.live_tiles_enabled = b;
                if (ipc::JsonFindBool(json, "sync_tray", b)) state_.settings.sync_staging_tray = b;
                if (ipc::JsonFindBool(json, "type_search", b)) state_.settings.type_to_search = b;
                if (ipc::JsonFindBool(json, "dbl_hide", b)) state_.settings.double_click_hide_icons = b;
                if (ipc::JsonFindBool(json, "auto_sleep", b)) state_.settings.pause_on_fullscreen_or_battery = b;
                if (std::wstring city; ipc::JsonFindWide(json, "weather_city", city) && city != state_.settings.weather_city) {
                    state_.settings.weather_city = city;
                    live_data_.weather = {};
                    live_data_.weather.status = city.empty() ? L"在设置里选一个城市" : L"正在获取天气…";
                    SetTimer(mount_.Hwnd(), kTimerWeatherCity, 1200, nullptr);
                }
                if (overlays_.ActiveKind() == overlays::OverlayKind::BeautifyDrawer &&
                    !overlays_.IsDraggingBeautifySlider() &&
                    !(state_.settings == prev_settings)) {
                    overlays_.OpenBeautifyDrawer(PrimaryWorkAreaScreen(), state_.settings, ActiveWidgetKinds());
                }
                if (!glass_preview) { RecomputeLayout(); SaveModel(); }
                renderer_.InvalidateAll();
                break;
            }
            case ipc::MessageType::TraySnapshotSync: {
                uint64_t tot = 0;
                ipc::JsonFindUint64(json, "total_bytes", tot);
                live_data_.tray_total_bytes = tot;
                live_data_.tray_entries.clear();
                size_t pos = json.find("\"entries\"");
                if (pos != std::string_view::npos) {
                    pos = json.find('[', pos);
                    while (pos != std::string_view::npos) {
                        const size_t ob = json.find('{', pos);
                        if (ob == std::string_view::npos) break;
                        const size_t cb = json.find('}', ob);
                        if (cb == std::string_view::npos) break;
                        std::string_view obj = json.substr(ob, cb - ob + 1);
                        render::TrayViewEntry e{};
                        ipc::JsonFindUint64(obj, "id", e.item_id);
                        ipc::JsonFindUint64(obj, "size", e.size_bytes);
                        ipc::JsonFindBool(obj, "folder", e.is_folder);
                        ipc::JsonFindBool(obj, "move", e.move_intent);
                        ipc::JsonFindWide(obj, "path", e.path);
                        ipc::JsonFindWide(obj, "name", e.name);
                        live_data_.tray_entries.push_back(std::move(e));
                        pos = cb + 1;
                    }
                }
                renderer_.InvalidateAll();
                break;
            }
            case ipc::MessageType::SearchReplyMsg: {
                std::vector<overlays::SearchHitEntry> hits;
                size_t pos = json.find("\"hits\"");
                if (pos != std::string_view::npos) {
                    pos = json.find('[', pos);
                    while (pos != std::string_view::npos) {
                        const size_t ob = json.find('{', pos);
                        if (ob == std::string_view::npos) break;
                        const size_t cb = json.find('}', ob);
                        if (cb == std::string_view::npos) break;
                        std::string_view obj = json.substr(ob, cb - ob + 1);
                        overlays::SearchHitEntry h{};
                        ipc::JsonFindWide(obj, "path", h.path);
                        ipc::JsonFindWide(obj, "name", h.name);
                        ipc::JsonFindUint64(obj, "size", h.size_bytes);
                        ipc::JsonFindBool(obj, "folder", h.is_folder);
                        h.from_local_desktop = false;
                        hits.push_back(std::move(h));
                        pos = cb + 1;
                    }
                }
                overlays_.UpdateRemoteSearchHits(std::move(hits));
                break;
            }
            case ipc::MessageType::ShellMenuReplyMsg: {
                uint64_t sid = 0;
                ipc::JsonFindUint64(json, "session_id", sid);
                std::vector<overlays::ShellVerbEntry> verbs;
                size_t pos = json.find("\"items\"");
                if (pos != std::string_view::npos) {
                    pos = json.find('[', pos);
                    while (pos != std::string_view::npos) {
                        const size_t ob = json.find('{', pos);
                        if (ob == std::string_view::npos) break;
                        const size_t cb = json.find('}', ob);
                        if (cb == std::string_view::npos) break;
                        std::string_view obj = json.substr(ob, cb - ob + 1);
                        overlays::ShellVerbEntry v{};
                        uint64_t id = 0;
                        ipc::JsonFindUint64(obj, "id", id);
                        v.item_id = static_cast<uint32_t>(id);
                        ipc::JsonFindBool(obj, "enabled", v.enabled);
                        ipc::JsonFindBool(obj, "sep", v.separator_after);
                        ipc::JsonFindBool(obj, "submenu", v.has_children);
                        uint64_t depth = 0;
                        if (ipc::JsonFindUint64(obj, "depth", depth)) v.depth = static_cast<uint32_t>(depth);
                        ipc::JsonFindWide(obj, "handler", v.handler);
                        ipc::JsonFindWide(obj, "verb", v.verb);
                        ipc::JsonFindWide(obj, "label", v.label);
                        verbs.push_back(std::move(v));
                        pos = cb + 1;
                    }
                }
                if (shell_prewarm_pending_) {
                    // Start-up warm-up: fills the cache so the first right-click opens complete.
                    shell_prewarm_pending_ = false;
                    overlays_.CacheShellVerbs("bg", std::move(verbs));
                    if (sid != 0) {
                        pipe_.Send(ipc::MessageType::ShellMenuInvokeReq, ++next_req_id_,
                                   "{\"session_id\":" + std::to_string(sid) + ",\"close\":true}");
                    }
                    break;
                }
                overlays_.SetAsyncShellVerbs(sid, std::move(verbs));
                break;
            }
            case ipc::MessageType::PreviewReplyMsg: {
                ipc::PreviewPayloadData prev{};
                if (ipc::DecodePreviewPayload(f.payload, prev)) {
                    overlays_.UpdateQuickLookPreview(prev);
                }
                break;
            }
            case ipc::MessageType::TagSyncMsg: {
                std::vector<overlays::ColorTagOption> incoming;
                size_t pos = json.find("\"tags\"");
                if (pos != std::string_view::npos) {
                    const size_t arr_open = json.find('[', pos);
                    const size_t arr_close = (arr_open != std::string_view::npos) ? json.find(']', arr_open) : std::string_view::npos;
                    if (arr_open != std::string_view::npos && arr_close != std::string_view::npos) {
                        size_t cur = arr_open;
                        while (cur < arr_close) {
                            const size_t ob = json.find('{', cur);
                            if (ob == std::string_view::npos || ob > arr_close) break;
                            const size_t cb = json.find('}', ob);
                            if (cb == std::string_view::npos || cb > arr_close) break;
                            std::string_view obj = json.substr(ob, cb - ob + 1);
                            overlays::ColorTagOption t{};
                            uint64_t argb = 0;
                            ipc::JsonFindWide(obj, "id", t.id);
                            ipc::JsonFindWide(obj, "name", t.name);
                            ipc::JsonFindUint64(obj, "argb", argb);
                            t.argb = static_cast<uint32_t>(argb);
                            if (!t.id.empty()) incoming.push_back(std::move(t));
                            cur = cb + 1;
                        }
                    }
                }
                if (!incoming.empty()) {
                    tag_palette_ = std::move(incoming);
                    overlays_.SetTagPalette(tag_palette_);
                }

                const size_t ti_pos = json.find("\"tagged_items\"");
                if (ti_pos != std::string_view::npos) {
                    const size_t arr_open = json.find('[', ti_pos);
                    const size_t arr_close = (arr_open != std::string_view::npos) ? json.find(']', arr_open) : std::string_view::npos;
                    if (arr_open != std::string_view::npos && arr_close != std::string_view::npos) {
                        auto norm_key = [](std::wstring s) {
                            for (auto& ch : s) {
                                if (ch == L'/') ch = L'\\';
                                ch = static_cast<wchar_t>(std::towlower(ch));
                            }
                            return s;
                        };
                        std::unordered_map<std::wstring, std::pair<std::wstring, uint32_t>> tagged_map;
                        size_t cur = arr_open;
                        while (cur < arr_close) {
                            const size_t ob = json.find('{', cur);
                            if (ob == std::string_view::npos || ob > arr_close) break;
                            const size_t cb = json.find('}', ob);
                            if (cb == std::string_view::npos || cb > arr_close) break;
                            std::string_view obj = json.substr(ob, cb - ob + 1);
                            std::wstring ipath, tid;
                            uint64_t argb = 0;
                            ipc::JsonFindWide(obj, "path", ipath);
                            ipc::JsonFindWide(obj, "id", tid);
                            ipc::JsonFindUint64(obj, "argb", argb);
                            if (!ipath.empty() && !tid.empty()) {
                                tagged_map[norm_key(std::move(ipath))] = {std::move(tid), static_cast<uint32_t>(argb)};
                            }
                            cur = cb + 1;
                        }

                        bool any_changed = false;
                        for (auto& it : state_.items) {
                            const auto found = tagged_map.find(norm_key(it.path));
                            const std::wstring next_id = (found != tagged_map.end()) ? found->second.first : std::wstring{};
                            const uint32_t next_argb = (found != tagged_map.end()) ? found->second.second : 0u;
                            if (it.tag_id != next_id || it.tag_argb != next_argb) {
                                it.tag_id = next_id;
                                it.tag_argb = next_argb;
                                any_changed = true;
                            }
                        }
                        if (any_changed) {
                            RecomputeLayout();
                            SaveModel();
                            renderer_.InvalidateAll();
                        }
                    }
                }
                break;
            }
            case ipc::MessageType::CommandActionMsg: {
                std::string act;
                ipc::JsonFindString(json, "action", act);
                if (act == "desktop-ai-organize") {
                    OpenAiOrganizeDrawer();
                } else if (act == "desktop-health") {
                    OpenHealthCheckDrawer();
                } else if (act == "desktop-beautify") {
                    OpenBeautifyOverlay();
                } else if (act == "desktop-toggle-icons") {
                    state_.settings.icons_hidden = !state_.settings.icons_hidden;
            if (state_.settings.icons_hidden) CancelHoverPreview();
                    SaveModel();
                    renderer_.InvalidateAll();
                } else if (act == "restore-native-icons") {
                    mount::RestoreNativeIconSnapshot(mount_.InitialSnapshot());
                }
                break;
            }
            case ipc::MessageType::ShutdownClean: {
                bool restore = true;
                ipc::JsonFindBool(json, "restore", restore);
                restore_on_exit_ = restore;
                PostQuitMessage(0);
                return;
            }
            default:
                break;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Win32 Surface Mouse / Keyboard Dispatch
    // -----------------------------------------------------------------------
    LRESULT OnSurfaceMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
        if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
            msg == kMsgFolderChanged || msg == kMsgIconsReady) {
            last_activity_tick_ = GetTickCount64();
            working_set_trimmed_ = false;
        }
        switch (msg) {
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            CancelHoverPreview();
            break;
        case WM_SHOWWINDOW:
            if (!wp) CancelHoverPreview();
            break;
        case WM_POWERBROADCAST:
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            ApplySystemAppearance();
            RefreshLiveWallpaperState();
            renderer_.ReloadWallpaper(state_.settings.wallpaper_preset, state_.settings.dark_mode);
            renderer_.InvalidateAll();
            break;

        case WM_WTSSESSION_CHANGE:
            if (wp == WTS_SESSION_LOCK) {
                session_locked_ = true;
                renderer_.OnLiveWallpaperFrameNotification(true);
            } else if (wp == WTS_SESSION_UNLOCK) {
                session_locked_ = false;
                RefreshLiveWallpaperState();
                renderer_.InvalidateGlassOnly();
            }
            break;

        case kMsgLiveWallpaperProbe:
            handled = true;
            RefreshLiveWallpaperState();
            return 0;

        case kMsgLiveGlassFrame: {
            handled = true;
            const bool paused = session_locked_ ||
                render::DesktopRenderer::ShouldPauseLiveActivity(state_.settings.pause_on_fullscreen_or_battery);
            renderer_.OnLiveWallpaperFrameNotification(paused);
            return 0;
        }

        case kMsgDrainIpc:
            handled = true;
            DrainIpcInbox();
            return 0;

        case kMsgIconsReady:
            // A batch of shell icons and thumbnails finished decoding: draw them in.
            handled = true;
            icons_ready_posted_.store(false);
            renderer_.InvalidateAll();
            return 0;
        case kMsgFolderChanged:
            handled = true;
            RefreshDesktopItems();
            RefreshLiveTileData();
            RefreshHealthStats();
            RecomputeLayout();
            renderer_.InvalidateAll();
            return 0;

        case kMsgAiPlanReady:
            handled = true;
            OnAiPlanReady();
            return 0;

        case kMsgWeatherReady:
            handled = true;
            ApplyWeather(std::unique_ptr<live::WeatherResult>(reinterpret_cast<live::WeatherResult*>(lp)));
            return 0;

        case WM_TIMER:
            if (wp == kTimerHoverGlass) {
                handled = true;
                return 0;
            }
            if (wp == kTimerGroupInteraction) {
                handled = true;
                const std::string merge_target = group_drag_session_.MergeReady(GetTickCount64())
                    ? group_drag_session_.MergeTarget() : std::string();
                if (group_drag_session_.Pending() && interaction_.drop_target_group_id != merge_target) {
                    interaction_.drop_target_group_id = merge_target;
                    renderer_.InvalidateAll();
                }
                POINT screen{};
                if (GetCursorPos(&screen)) {
                    const POINT client = mount_.ScreenPxToClientDip(screen);
                    const HWND hit = WindowFromPoint(screen);
                    const bool on_desktop = hit == hwnd || (hit && IsChild(hwnd, hit));
                    if (!on_desktop) CancelHoverPreview();
                    UpdateGroupHoverAt(client.x, client.y, on_desktop, GetTickCount64());
                }
                return 0;
            }
            if (wp == kTimerHeartbeat) {
                handled = true;
                if (active_ai_session_ || overlays_.IsAiOrganizeUiOpen()) RequireAiOrganizerEnabled();
                RefreshWidgetDetail(model::TileKind::Clock);
                // Release temporary overlay contexts and D2D/DXGI staging buffers after idle,
                // without evicting working-set pages via SetProcessWorkingSetSize.
                if (!working_set_trimmed_ && !overlays_.IsOpen() && GetTickCount64() - last_activity_tick_ > 8000) {
                    overlays_.TrimIdleResources();
                    renderer_.Backdrop().ReleaseMenuMaterialCache();
                    renderer_.TrimDxgiMemory();
                    _heapmin();
                    working_set_trimmed_ = true;
                }
                if (parent_proc_ && WaitForSingleObject(parent_proc_, 0) == WAIT_OBJECT_0) {
                    PostQuitMessage(0);
                    return 0;
                }
                ++heartbeat_tick_;
                if (pipe_.IsConnected() && (items_paths_dirty_ || (heartbeat_tick_ % 3 == 0))) {
                    if (items_paths_dirty_) {
                        pipe_.Send(ipc::MessageType::Ping, ++next_req_id_, BuildWatchedPathsPayload());
                        items_paths_dirty_ = false;
                    } else {
                        pipe_.Send(ipc::MessageType::Ping, ++next_req_id_, "{\"tick\":1}");
                    }
                }
                if (!tile_board_rect_.empty() &&
                    !render::DesktopRenderer::ShouldPauseLiveActivity(state_.settings.pause_on_fullscreen_or_battery)) {
                    SYSTEMTIME st{};
                    GetLocalTime(&st);
                    for (const auto& t : state_.tiles) {
                        const auto tv = model::ActiveTileView(t);
                        if (tv.kind != model::TileKind::Clock) continue;
                        const bool is_analog = tv.analog_clock;
                        if (is_analog || st.wMinute != last_clock_minute_) {
                            renderer_.InvalidateRect(
                                render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t));
                        }
                    }
                    last_clock_minute_ = st.wMinute;
                }
                return 0;
            }
            if (wp == kTimerWeatherCity) {
                handled = true;
                KillTimer(hwnd, kTimerWeatherCity);
                RequestWeather(true);
                renderer_.InvalidateAll();
                return 0;
            }
            if (wp == kTimerTileReorder) {
                handled = true;
                UpdateTileDrag(drag_tile_cursor_.x, drag_tile_cursor_.y);
                return 0;
            }
            if (wp == kTimerTilePress) {
                handled = true;
                StepTilePress();
                return 0;
            }
            if (wp == kTimerLongPress) {
                handled = true;
                KillTimer(hwnd, kTimerLongPress);
                if (pending_tile_idx_ && !dragging_tile_idx_) {
                    tile_long_pressed_ = true;
                    ReleaseTilePress();
                    SetTileEditMode(true);
                }
                return 0;
            }
            if (wp == kTimerEditJiggle) {
                handled = true;
                interaction_.edit_phase += 0.033f / 0.56f;
                if (interaction_.edit_phase >= 1.0f) interaction_.edit_phase -= 1.0f;
                if (!tile_board_rect_.empty()) {
                    renderer_.InvalidateRect(model::RectI{tile_board_rect_.x - 16, tile_board_rect_.y - 40,
                                                          tile_board_rect_.w + 32, tile_board_rect_.h + 56});
                }
                for (const auto& t : state_.tiles) {
                    const model::RectI tr = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t);
                    renderer_.InvalidateRect(model::RectI{tr.x - 20, tr.y - 20, tr.w + 40, tr.h + 40});
                }
                return 0;
            }
            if (wp == kTimerLiveTiles) {
                handled = true;
                if (tile_board_rect_.empty()) return 0;
                ++live_tick_;
                if (live_tick_ % 3 == 0 && state_.settings.live_tiles_enabled &&
                    !render::DesktopRenderer::ShouldPauseLiveActivity(state_.settings.pause_on_fullscreen_or_battery))
                    RotateStacks();
                if (model::BoardHasKind(state_.tiles, model::TileKind::System)) SampleSystemMetrics();
                if (model::BoardHasKind(state_.tiles, model::TileKind::Weather)) RequestWeather(false);
                EnsurePhotoTimer();
                return 0;
            }
            if (wp == kTimerPhotos) {
                handled = true;
                const model::DesktopTile* photo = PhotoTile();
                if (photo && state_.settings.live_tiles_enabled &&
                    !render::DesktopRenderer::ShouldPauseLiveActivity(state_.settings.pause_on_fullscreen_or_battery)) {
                    AdvancePhoto(photo->photo_shuffle);
                    photo_last_switch_ms_ = GetTickCount64();
                }
                return 0;
            }
            break;

        case WM_SETCURSOR: {
            if (LOWORD(lp) == HTCLIENT) {
                handled = true;
                LPCTSTR cursor_id = IDC_ARROW;
                if (resizing_group_idx_ || interaction_.hovered_group_zone == render::GroupHeaderHitZone::ResizeGrip) {
                    cursor_id = IDC_SIZENWSE;
                } else if (resizing_tile_idx_) {
                    if (resizing_tile_zone_ == render::TileResizeZone::Right) cursor_id = IDC_SIZEWE;
                    else if (resizing_tile_zone_ == render::TileResizeZone::Bottom) cursor_id = IDC_SIZENS;
                    else cursor_id = IDC_SIZENWSE;
                } else if (interaction_.hovered_tile_resize == render::TileResizeZone::Right) {
                    cursor_id = IDC_SIZEWE;
                } else if (interaction_.hovered_tile_resize == render::TileResizeZone::Bottom) {
                    cursor_id = IDC_SIZENS;
                } else if (interaction_.hovered_tile_resize == render::TileResizeZone::BottomRight) {
                    cursor_id = IDC_SIZENWSE;
                }
                SetCursor(LoadCursorW(nullptr, cursor_id));
                return TRUE;
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            CancelHoverPreview();
            handled = true;
            overlays_.Dismiss();
            SetFocus(hwnd);
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            drag_start_pt_ = {x, y};
            pending_tile_idx_.reset();
            pending_item_drag_ = false;

            // 0. Check bottom-left #banner action button ("退出专注" / "退出演示" / "显示图标")
            const model::RectI banner_btn = renderer_.LastBannerActionRect();
            if (!banner_btn.empty() && banner_btn.contains(x, y)) {
                if (state_.settings.icons_hidden) {
                    state_.settings.icons_hidden = false;
                } else {
                    state_.settings.active_scene = model::DesktopScene::Work;
                }
                RecomputeLayout();
                SaveModel();
                SyncSettingsToPlugin();
                renderer_.InvalidateAll();
                return 0;
            }

            // 1. Check group header chips, action buttons, footer & resize grips
            if (state_.settings.organize_enabled && !state_.settings.icons_hidden &&
                state_.settings.active_scene != model::DesktopScene::Present) {
                const bool focus_scene = (state_.settings.active_scene == model::DesktopScene::Focus);
                for (const size_t i : GroupHitOrder()) {
                    const auto& gv = group_views_[i];
                    if (!gv.frame.contains(x, y)) continue;

                    const auto zone = renderer_.HitTestGroupCard(
                        state_.groups[i], gv, focus_scene, interaction_.cleanable_installer_bytes,
                        interaction_.hovered_group_id == state_.groups[i].id, x, y);

                    if (zone == render::GroupHeaderHitZone::Tab ||
                        zone == render::GroupHeaderHitZone::TabPrev || zone == render::GroupHeaderHitZone::TabNext) {
                        const auto target = render::HitTestGroupTab(gv, x, y);
                        if (target.empty()) return 0;
                        if (zone == render::GroupHeaderHitZone::Tab) {
                            // Defer selection until release so an inactive-tab drag
                            // remains one transaction, not selection + movement undo.
                            for (size_t ti = 0; ti < state_.groups.size(); ++ti) {
                                if (state_.groups[ti].id != target) continue;
                                BeginGroupDrag(ti, x, y, false);
                                pending_group_tab_id_ = target;
                                SetCapture(hwnd); break;
                            }
                        } else if (target != state_.groups[i].id) {
                            PushUndo(l10n::Get(l10n::StringId::DesktopGroupSelectTab));
                            model::SelectGroupTab(state_.groups, target);
                            group_hover_.Reset();
                            RecomputeLayout(); SaveModel(); renderer_.InvalidateAll();
                        }
                        return 0;
                    }
                    if (zone == render::GroupHeaderHitZone::CleanChip) {
                        OpenHealthCheckDrawer();
                        return 0;
                    }
                    if (zone == render::GroupHeaderHitZone::MapSingleFolderChip) {
                        if (!gv.single_subfolder_path.empty()) {
                            BindGroupToFolder(i, gv.single_subfolder_path, /*push_undo=*/true);
                        }
                        return 0;
                    }
                    if (zone == render::GroupHeaderHitZone::RuleButton) {
                        if (!state_.groups[i].mapped_folder_path.empty()) {
                            OpenPathInPulseOrShell(state_.groups[i].mapped_folder_path, &gv.frame);
                        } else {
                            OpenGroupRulePopoverForIndex(i);
                        }
                        return 0;
                    }
                    if (zone == render::GroupHeaderHitZone::CollapseChevron) {
                        state_.groups[i].collapsed = !state_.groups[i].collapsed;
                        RecomputeLayout();
                        SaveModel();
                        renderer_.InvalidateAll();
                        return 0;
                    }
                    if (zone == render::GroupHeaderHitZone::MenuButton) {
                        POINT scr{gv.frame.right() - 28, gv.frame.y + model::kGroupHeaderHeight};
                        mount_.ClientToScreenDip(&scr);
                        overlays::ContextMenuSpec spec{};
                        spec.screen_anchor = scr;
                        FillGroupContextMenuSpec(i, spec);
                        overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
                        overlays_.OpenContextMenu(spec, state_.settings);
                        return 0;
                    }
                    if (zone == render::GroupHeaderHitZone::FooterPrevPage) {
                        if (gv.page_count > 1) {
                            const int cur = std::clamp(gv.page_index, 0, gv.page_count - 1);
                            const int next = std::clamp(cur - 1, 0, gv.page_count - 1);
                            if (next != cur) {
                                interaction_.group_pages[state_.groups[i].id] = next;
                                RefreshGroupViews();
                                renderer_.InvalidateRect(gv.frame);
                            }
                        }
                        return 0;
                    }
                    if (zone == render::GroupHeaderHitZone::FooterNextPage) {
                        if (gv.page_count > 1) {
                            const int cur = std::clamp(gv.page_index, 0, gv.page_count - 1);
                            const int next = std::clamp(cur + 1, 0, gv.page_count - 1);
                            if (next != cur) {
                                interaction_.group_pages[state_.groups[i].id] = next;
                                RefreshGroupViews();
                                renderer_.InvalidateRect(gv.frame);
                            }
                        }
                        return 0;
                    }
                    if (zone == render::GroupHeaderHitZone::FooterPagerDots) {
                        const int dot = renderer_.HitTestGroupFooterDot(gv, x, y);
                        if (dot >= 0 && dot < gv.page_count && dot != gv.page_index) {
                            interaction_.group_pages[state_.groups[i].id] = dot;
                            RefreshGroupViews();
                            renderer_.InvalidateRect(gv.frame);
                        }
                        return 0;
                    }
                    if (zone == render::GroupHeaderHitZone::FooterViewAll) {
                        OpenPathInPulseOrShell(
                            state_.groups[i].mapped_folder_path.empty()
                                ? mount::ResolveKnownFolder(FOLDERID_Desktop)
                                : state_.groups[i].mapped_folder_path,
                            &gv.frame);
                        return 0;
                    }
                    if (zone == render::GroupHeaderHitZone::ResizeGrip) {
                        BeginGroupDrag(i, x, y, true);
                        SetCapture(hwnd);
                        return 0;
                    }
                    if (y < gv.frame.y + model::kGroupHeaderHeight) {
                        BeginGroupDrag(i, x, y, false);
                        SetCapture(hwnd);
                        return 0;
                    }
                    break; // Expanded card content occludes headers on lower cards.
                }
            }

            // 2. Check desktop items (both inside groups and loose)
            if (auto hit = HitTestItem(x, y)) {
                const std::wstring& p = state_.items[*hit].path;
                if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                    if (interaction_.selected_paths.contains(p)) interaction_.selected_paths.erase(p);
                    else interaction_.selected_paths.insert(p);
                } else if (!interaction_.selected_paths.contains(p)) {
                    interaction_.selected_paths.clear();
                    interaction_.selected_paths.insert(p);
                }
                interaction_.focused_path = p;
                pending_item_drag_ = true;
                SetCapture(hwnd);
                renderer_.InvalidateAll();
                return 0;
            }

            if (state_.settings.organize_enabled) {
                for (const size_t i : GroupHitOrder())
                    if (group_views_[i].frame.contains(x, y)) return 0;
            }

            // 3. Check live tiles (edge/corner resize, pending click, drag >= 6px)
            if (state_.settings.tiles_enabled && state_.settings.organize_enabled &&
                !interaction_.side_drawer_open) {
                const model::RectI hdr_anchor = !tile_board_rect_.empty()
                    ? tile_board_rect_
                    : render::DesktopRenderer::ComputeBoardRectFallback(PrimaryWorkAreaClient(), true);
                const bool any_free_tiles = std::any_of(state_.tiles.begin(), state_.tiles.end(),
                                                        [](const model::DesktopTile& t) { return t.free_placed; });
                if (interaction_.tile_edit_mode && !hdr_anchor.empty()) {
                    if (render::DesktopRenderer::TileHeaderAddWidgetRect(hdr_anchor).contains(x, y)) {
                        OpenWidgetGallery();
                        return 0;
                    }
                    if (render::DesktopRenderer::TileHeaderDoneRect(hdr_anchor).contains(x, y)) {
                        SetTileEditMode(false);
                        return 0;
                    }
                    // − badges (outside the corner) and the size label of the tile under the pointer.
                    for (size_t i = 0; i < state_.tiles.size(); ++i) {
                        const model::RectI tr = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, state_.tiles[i]);
                        const D2D1_RECT_F trf = D2D1::RectF(static_cast<float>(tr.x), static_cast<float>(tr.y),
                                                            static_cast<float>(tr.right()), static_cast<float>(tr.bottom()));
                        const D2D1_RECT_F b = render::DesktopRenderer::TileRemoveBadgeRect(trf);
                        if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) {
                            PushUndo(L"移除磁贴");
                            state_.tiles.erase(state_.tiles.begin() + static_cast<ptrdiff_t>(i));
                            RecomputeLayout();
                            SaveModel();
                            if (overlays_.ActiveKind() == overlays::OverlayKind::WidgetGalleryDrawer) {
                                overlays_.UpdateWidgetGalleryTiles(state_.tiles);
                            }
                            renderer_.InvalidateAll();
                            return 0;
                        }
                        const bool px_pill = state_.tiles[i].w > 0 || state_.tiles[i].h > 0;
                        const D2D1_RECT_F p = render::DesktopRenderer::TileSizePillRect(trf, px_pill);
                        if (interaction_.hovered_tile_id == state_.tiles[i].id && x >= p.left && x < p.right && y >= p.top && y < p.bottom) {
                            CycleTileSize(i);
                            return 0;
                        }
                    }
                }
                // The 照片 card's hover arrows: the previous / next picture.
                for (const auto& pt : state_.tiles) {
                    if (model::ActiveTileView(pt).kind != model::TileKind::Photos || interaction_.hovered_tile_id != pt.id ||
                        live_data_.photo_paths.size() < 2 || interaction_.tile_edit_mode)
                        continue;
                    const model::RectI tr = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, pt);
                    const D2D1_RECT_F rf = D2D1::RectF(static_cast<float>(tr.x), static_cast<float>(tr.y),
                                                       static_cast<float>(tr.right()), static_cast<float>(tr.bottom()));
                    for (const bool next : {false, true}) {
                        const D2D1_RECT_F b = render::DesktopRenderer::PhotoNavRect(pt, rf, next);
                        if (x < b.left || x >= b.right || y < b.top || y >= b.bottom) continue;
                        StepPhoto(next ? 1 : -1, pt.photo_shuffle);
                        photo_last_switch_ms_ = GetTickCount64();
                        photo_timer_ms_ = 0;  // restart the interval from this picture
                        EnsurePhotoTimer();
                        return 0;
                    }
                }
                // A stack's dots and hover arrows switch the widget shown, as on a phone's smart stack.
                for (auto& st : state_.tiles) {
                    if (st.stack.size() < 2 || interaction_.hovered_tile_id != st.id) continue;
                    const model::RectI tr = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, st);
                    const D2D1_RECT_F rf = D2D1::RectF(static_cast<float>(tr.x), static_cast<float>(tr.y),
                                                       static_cast<float>(tr.right()), static_cast<float>(tr.bottom()));
                    const size_t n = st.stack.size();
                    auto hit = [&](D2D1_RECT_F b, float pad) {
                        return b.right > b.left && x >= b.left - pad && x < b.right + pad && y >= b.top - 1.0f && y < b.bottom + 1.0f;
                    };
                    int target = -1;
                    if (hit(render::DesktopRenderer::StackArrowRect(rf, n, true), 2.0f)) target = (st.stack_index + static_cast<int>(n) - 1) % static_cast<int>(n);
                    else if (hit(render::DesktopRenderer::StackArrowRect(rf, n, false), 2.0f)) target = (st.stack_index + 1) % static_cast<int>(n);
                    else
                        for (size_t d = 0; d < n; ++d)
                            if (hit(render::DesktopRenderer::StackDotRect(rf, n, st.stack_index, d), 5.0f)) target = static_cast<int>(d);
                    if (target < 0) continue;
                    st.stack_index = target;
                    SaveModel();
                    renderer_.InvalidateRect(model::RectI{tr.x - 12, tr.y - 30, tr.w + 24, tr.h + 60});
                    return 0;
                }
                for (size_t i = 0; i < state_.tiles.size(); ++i) {
                    const model::RectI tr = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, state_.tiles[i]);
                    if (!tr.contains(x, y)) continue;
                    const render::TileResizeZone rzone = render::DesktopRenderer::HitTestTileResize(state_.tiles[i], tr, x, y);
                    if (rzone != render::TileResizeZone::None) {
                        PushUndo(L"调整磁贴大小");
                        resizing_tile_idx_ = i;
                        resizing_tile_zone_ = rzone;
                        resize_tile_start_rect_ = tr;
                        interaction_.resizing_tile_id = state_.tiles[i].id;
                        SetCapture(hwnd);
                        renderer_.InvalidateAll();
                        return 0;
                    }
                    pending_tile_idx_ = i;
                    tile_long_pressed_ = false;
                    {
                        const int app_cell = interaction_.tile_edit_mode ? -1 : AppCellAt(state_.tiles[i], tr, x, y);
                        const bool inner_control = !interaction_.tile_edit_mode &&
                            (app_cell >= 0 || TodoRowAt(state_.tiles[i], tr, y) >= 0 || IsTodoAddButtonAt(state_.tiles[i], tr, x, y) ||
                             ListTileRowAt(state_.tiles[i], tr, x, y) >= 0);
                        if (app_cell >= 0) {
                            interaction_.pressed_app_index = app_cell;
                            renderer_.InvalidateRect(tr);
                        }
                        if (!inner_control) AnimateTilePress(state_.tiles[i].id, true);
                    }
                    drag_offset_ = {x - tr.x, y - tr.y};
                    SetCapture(hwnd);
                    if (!interaction_.tile_edit_mode) SetTimer(hwnd, kTimerLongPress, 550, nullptr);
                    return 0;
                }
                // An empty + cell while editing on default 5x7 board: open the visual "+ 添加小组件" picker targeting this cell!
                if (interaction_.tile_edit_mode && !any_free_tiles && tile_board_rect_.contains(x, y)) {
                    const int col = std::clamp((x - tile_board_rect_.x) / (model::kTileUnitPx + model::kTileGapPx),
                                               0, model::kTileCols - 1);
                    const int row = std::clamp((y - tile_board_rect_.y) / (model::kTileUnitPx + model::kTileGapPx),
                                               0, model::kTileRows - 1);
                    OpenWidgetGallery(std::nullopt, model::PointI{col, row});
                    return 0;
                }
            }
            if (interaction_.tile_edit_mode) SetTileEditMode(false);  // a click elsewhere finishes editing

            // 4. Bare desktop rubber-band marquee
            marquee_base_selection_ = ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
                                          ? interaction_.selected_paths
                                          : std::unordered_set<std::wstring>{};
            interaction_.selected_paths = marquee_base_selection_;
            interaction_.marquee_active = true;
            interaction_.marquee_rect = {x, y, 0, 0};
            SetCapture(hwnd);
            renderer_.InvalidateAll();
            return 0;
        }

        case WM_MOUSEMOVE: {
            handled = true;
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            UpdateHoverPreviewAt(x, y);
            interaction_.cursor_pos = {x, y};
            interaction_.cursor_inside = true;
            if (!tracking_mouse_leave_) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                if (TrackMouseEvent(&tme)) tracking_mouse_leave_ = true;
            }
            UpdateGroupHoverAt(x, y, true, GetTickCount64());
            if (group_drag_session_.Pending()) {
                if ((wp & MK_LBUTTON) != 0) UpdateGroupDrag(x, y, GetTickCount64());
                return 0;
            }

            // Start internal item drag ghost (.dghost) after >= 6px threshold
            if (pending_item_drag_ && (wp & MK_LBUTTON) != 0 &&
                (std::abs(x - drag_start_pt_.x) >= 6 || std::abs(y - drag_start_pt_.y) >= 6)) {
                pending_item_drag_ = false;
                if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
                    if (GetCapture() == hwnd) ReleaseCapture();
                    std::vector<std::wstring> drag_paths(interaction_.selected_paths.begin(),
                                                         interaction_.selected_paths.end());
                    overlays::DesktopDropManager::StartFileDragOut(drag_paths, DROPEFFECT_COPY | DROPEFFECT_MOVE);
                    return 0;
                }
                interaction_.dragging_items = true;
                interaction_.drag_item_count = interaction_.selected_paths.size();
                interaction_.drag_primary_ext.clear();
                interaction_.drag_primary_folder = false;
                for (const auto& it : state_.items) {
                    if (it.path == interaction_.focused_path || interaction_.selected_paths.contains(it.path)) {
                        interaction_.drag_primary_ext = it.extension.empty() ? it.name : it.extension;
                        interaction_.drag_primary_folder = it.is_folder;
                        break;
                    }
                }
            }

            if (interaction_.dragging_items) {
                const POINT screen = mount_.ClientDipToScreenPx(POINT{x, y});
                if ((wp & MK_LBUTTON) != 0 && overlays::DesktopDropManager::IsExternalDragWindow(
                        hwnd, WindowFromPoint(screen))) {
                    const std::vector<std::wstring> paths(interaction_.selected_paths.begin(),
                                                          interaction_.selected_paths.end());
                    interaction_.dragging_items = false;
                    interaction_.drag_item_count = 0;
                    interaction_.drop_target_group_id.clear();
                    interaction_.drop_target_tile_id.clear();
                    renderer_.InvalidateAll();
                    // OLE owns capture and the release event from here. Clear the
                    // internal target first so cancellation cannot commit a move.
                    releasing_capture_ = true;
                    if (GetCapture() == hwnd) ReleaseCapture();
                    releasing_capture_ = false;
                    overlays::DesktopDropManager::StartFileDragOut(paths, DROPEFFECT_COPY | DROPEFFECT_MOVE);
                    return 0;
                }
                interaction_.drag_cursor_pos = {x, y};
                interaction_.drop_target_group_id.clear();
                interaction_.drop_target_tile_id.clear();
                for (const size_t gi : GroupHitOrder()) {
                    if (group_views_[gi].frame.contains(x, y)) {
                        interaction_.drop_target_group_id = state_.groups[gi].id;
                        break;
                    }
                }
                if (interaction_.drop_target_group_id.empty() &&
                    state_.settings.tiles_enabled && !interaction_.side_drawer_open) {
                    for (const auto& t : state_.tiles) {
                        const model::RectI tr = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t);
                        const auto vk = model::ActiveTileView(t).kind;
                        bool shortcuts = false;
                        for (const auto& it : state_.items)
                            if (it.is_shortcut && interaction_.selected_paths.contains(it.path)) shortcuts = true;
                        if (tr.contains(x, y) &&
                            (vk == model::TileKind::StagingTray || vk == model::TileKind::RecycleBin ||
                             (vk == model::TileKind::Apps && shortcuts))) {
                            interaction_.drop_target_tile_id = t.id;
                            break;
                        }
                    }
                }
                renderer_.InvalidateAll();
                return 0;
            }

            // A refreshed tile list can invalidate the pressed index before movement.
            if (pending_tile_idx_ && *pending_tile_idx_ >= state_.tiles.size()) {
                pending_tile_idx_.reset();
                KillTimer(hwnd, kTimerLongPress);
                ReleaseTilePress();
            }
            // Start live tile drag ONLY when cursor moves >= 6px from press!
            if (pending_tile_idx_ && (wp & MK_LBUTTON) != 0 && !tile_long_pressed_ &&
                (std::abs(x - drag_start_pt_.x) >= 6 || std::abs(y - drag_start_pt_.y) >= 6)) {
                KillTimer(hwnd, kTimerLongPress);
                ReleaseTilePress();
                drag_tile_undo_snapshot_ = state_;
                dragging_tile_idx_ = *pending_tile_idx_;
                tile_drag_session_.Begin(state_.tiles, *dragging_tile_idx_, TileDragWorkAreaClient(drag_start_pt_),
                                         tile_board_rect_, CurrentGroupFrames());
                pending_tile_idx_.reset();
                const auto& t = state_.tiles[*dragging_tile_idx_];
                interaction_.dragging_tile = true;
                interaction_.dragging_tile_id = t.id;
                interaction_.drag_tile_preview_rect =
                    render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t);
                interaction_.drag_tile_valid = true;
            }

            if (resizing_tile_idx_ && *resizing_tile_idx_ < state_.tiles.size()) {
                const auto before = CaptureDragDamage();
                const int dx = x - drag_start_pt_.x;
                const int dy = y - drag_start_pt_.y;
                const int want_w = (resizing_tile_zone_ == render::TileResizeZone::Right ||
                                    resizing_tile_zone_ == render::TileResizeZone::BottomRight)
                    ? (resize_tile_start_rect_.w + dx)
                    : resize_tile_start_rect_.w;
                const int want_h = (resizing_tile_zone_ == render::TileResizeZone::Bottom ||
                                    resizing_tile_zone_ == render::TileResizeZone::BottomRight)
                    ? (resize_tile_start_rect_.h + dy)
                    : resize_tile_start_rect_.h;
                model::ResizeTileFreeInteractive(state_.tiles, *resizing_tile_idx_, want_w, want_h,
                                                 PrimaryWorkAreaClient(), tile_board_rect_, CurrentGroupFrames());
                InvalidateDragDamage(before);
                return 0;
            }

            if (dragging_tile_idx_) {
                UpdateTileDrag(x, y);
                return 0;
            }

            if (interaction_.marquee_active) {
                const int x0 = std::min(drag_start_pt_.x, x);
                const int y0 = std::min(drag_start_pt_.y, y);
                interaction_.marquee_rect = {x0, y0, std::abs(x - drag_start_pt_.x), std::abs(y - drag_start_pt_.y)};
                interaction_.selected_paths = marquee_base_selection_;

                const model::PointI cm = model::IconCellMetrics(state_.settings.icon_size);
                std::unordered_set<size_t> claimed;
                for (size_t ai = 0; ai < state_.items.size(); ++ai)
                    if (state_.items[ai].in_apps_tile) claimed.insert(ai);
                if (state_.settings.organize_enabled) {
                    for (size_t gi = 0; gi < group_views_.size() && gi < state_.groups.size(); ++gi) {
                        const auto& gv = group_views_[gi];
                        for (size_t idx : gv.member_item_indices) claimed.insert(idx);
                        if (!gv.expanded || gv.frame.empty()) continue;
                        for (size_t ci = 0; ci < gv.visible_item_indices.size(); ++ci) {
                            const model::RectI cr = model::GroupItemRect(gv, ci);
                            if (interaction_.marquee_rect.intersects(cr)) {
                                interaction_.selected_paths.insert(state_.items[gv.visible_item_indices[ci]].path);
                            }
                        }
                    }
                }
                const int lw = std::max(model::kLooseCellW, cm.x + 6);
                const int lh = std::max(model::kLooseCellH, cm.y + 6);
                for (size_t i = 0; i < state_.items.size(); ++i) {
                    if (claimed.contains(i)) continue;
                    const auto& it = state_.items[i];
                    if (it.hidden_by_mapped_group || !it.mapped_owner_group_id.empty()) continue;
                    if (state_.settings.organize_enabled && !it.force_loose && !it.pinned_group_id.empty()) continue;
                    const model::RectI cr{it.loose_pos.x, it.loose_pos.y, lw, lh};
                    if (interaction_.marquee_rect.intersects(cr)) {
                        interaction_.selected_paths.insert(it.path);
                    }
                }
                renderer_.InvalidateAll();
                return 0;
            }

            // Hover state update (invalidates ONLY when hover target actually changes!)
            std::wstring new_hover_path;
            if (auto hit = HitTestItem(x, y)) {
                new_hover_path = state_.items[*hit].path;
            }
            std::string new_hover_grp;
            render::GroupHeaderHitZone new_hover_zone = render::GroupHeaderHitZone::None;
            int new_hover_dot = -1;
            const bool focus_scene = (state_.settings.active_scene == model::DesktopScene::Focus);
            for (const size_t i : GroupHitOrder()) {
                if (group_views_[i].frame.contains(x, y)) {
                    new_hover_grp = state_.groups[i].id;
                    new_hover_zone = renderer_.HitTestGroupCard(
                        state_.groups[i], group_views_[i], focus_scene,
                        interaction_.cleanable_installer_bytes, true, x, y);
                    if (new_hover_zone == render::GroupHeaderHitZone::FooterPagerDots) {
                        new_hover_dot = renderer_.HitTestGroupFooterDot(group_views_[i], x, y);
                    }
                    break;
                }
            }
            std::string new_hover_tile;
            int new_todo = -1;
            int new_tile_row = -1;
            bool new_tile_action = false;
            bool new_tile_edit_btn = false;
            render::TileResizeZone new_tile_resize = render::TileResizeZone::None;
            int new_tile_hdr_btn = 0;
            if (state_.settings.tiles_enabled && state_.settings.organize_enabled && !interaction_.side_drawer_open) {
                const model::RectI hdr_anchor = !tile_board_rect_.empty()
                    ? tile_board_rect_
                    : render::DesktopRenderer::ComputeBoardRectFallback(PrimaryWorkAreaClient(), true);
                if (interaction_.tile_edit_mode && !hdr_anchor.empty()) {
                    if (render::DesktopRenderer::TileHeaderAddWidgetRect(hdr_anchor).contains(x, y)) {
                        new_tile_hdr_btn = 1;
                    } else if (render::DesktopRenderer::TileHeaderDoneRect(hdr_anchor).contains(x, y)) {
                        new_tile_hdr_btn = 2;
                    }
                }
                for (const auto& t : state_.tiles) {
                    const model::RectI tr = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t);
                    if (tr.contains(x, y)) {
                        new_hover_tile = t.id;
                        new_tile_resize = render::DesktopRenderer::HitTestTileResize(t, tr, x, y);
                        if (!interaction_.tile_edit_mode && new_tile_resize == render::TileResizeZone::None) {
                            const D2D1_RECT_F trf = D2D1::RectF(static_cast<float>(tr.x), static_cast<float>(tr.y),
                                                                static_cast<float>(tr.right()), static_cast<float>(tr.bottom()));
                            const D2D1_RECT_F eb = render::DesktopRenderer::TileEditButtonRect(trf, t.size.cols == 1);
                            if (x >= eb.left && x < eb.right && y >= eb.top && y < eb.bottom) {
                                new_tile_edit_btn = true;
                            } else {
                                const int app = AppCellAt(t, tr, x, y);
                                if (app >= 0) new_hover_path = state_.items[AppsInTile()[static_cast<size_t>(app)]].path;
                                new_todo = TodoRowAt(t, tr, y);
                                if (new_todo < 0 && IsTodoAddButtonAt(t, tr, x, y)) {
                                    new_tile_action = true;
                                }
                                new_tile_row = ListTileRowAt(t, tr, x, y);
                            }
                        }
                        break;
                    }
                }
            }
            auto invalidate_hover_tiles = [&] {
                for (const auto& tile : state_.tiles) {
                    if (tile.id != interaction_.hovered_tile_id && tile.id != new_hover_tile) continue;
                    const auto r = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, tile);
                    renderer_.InvalidateRect({r.x - 20, r.y - 20, r.w + 40, r.h + 40});
                }
            };
            if (new_todo != interaction_.hovered_todo_index ||
                new_tile_row != interaction_.hovered_tile_row ||
                new_tile_action != interaction_.hovered_tile_action ||
                new_tile_edit_btn != interaction_.hovered_tile_edit_btn ||
                new_tile_resize != interaction_.hovered_tile_resize ||
                new_tile_hdr_btn != interaction_.hovered_tile_header_btn) {
                interaction_.hovered_todo_index = new_todo;
                interaction_.hovered_tile_row = new_tile_row;
                interaction_.hovered_tile_action = new_tile_action;
                interaction_.hovered_tile_edit_btn = new_tile_edit_btn;
                interaction_.hovered_tile_resize = new_tile_resize;
                interaction_.hovered_tile_header_btn = new_tile_hdr_btn;
                invalidate_hover_tiles();
            }

            auto invalidate_group_by_id = [&](const std::string& gid) {
                if (gid.empty()) return;
                for (size_t i = 0; i < group_views_.size() && i < state_.groups.size(); ++i) {
                    if (state_.groups[i].id != gid) continue;
                    model::RectI f = group_views_[i].frame;
                    if (!group_views_[i].expanded) {
                        f.h = model::kGroupHeaderHeight;
                    }
                    renderer_.InvalidateRect({f.x - 20, f.y - 20, f.w + 40, f.h + 40});
                    break;
                }
            };
            auto invalidate_item_by_path = [&](const std::wstring& path) {
                if (path.empty()) return;
                const model::PointI cm = model::IconCellMetrics(state_.settings.icon_size);
                const int lw = std::max(model::kLooseCellW, cm.x + 6);
                const int lh = std::max(model::kLooseCellH, cm.y + 6);
                for (const auto& it : state_.items) {
                    if (it.path != path) continue;
                    renderer_.InvalidateRect({it.loose_pos.x - 8, it.loose_pos.y - 8, lw + 16, lh + 16});
                    break;
                }
                for (size_t i = 0; i < group_views_.size() && i < state_.groups.size(); ++i) {
                    const auto& gv = group_views_[i];
                    for (size_t idx : gv.visible_item_indices) {
                        if (idx < state_.items.size() && state_.items[idx].path == path) {
                            const auto& f = gv.frame;
                            renderer_.InvalidateRect({f.x - 20, f.y - 20, f.w + 40, f.h + 40});
                            return;
                        }
                    }
                }
            };

            if (new_hover_path != interaction_.hovered_path ||
                new_hover_grp != interaction_.hovered_group_id ||
                new_hover_zone != interaction_.hovered_group_zone ||
                new_hover_dot != interaction_.hovered_group_dot ||
                new_hover_tile != interaction_.hovered_tile_id) {
                if (new_hover_path != interaction_.hovered_path) {
                    invalidate_item_by_path(interaction_.hovered_path);
                    invalidate_item_by_path(new_hover_path);
                }
                if (new_hover_grp != interaction_.hovered_group_id ||
                    new_hover_zone != interaction_.hovered_group_zone ||
                    new_hover_dot != interaction_.hovered_group_dot) {
                    invalidate_group_by_id(interaction_.hovered_group_id);
                    invalidate_group_by_id(new_hover_grp);
                }
                if (new_hover_tile != interaction_.hovered_tile_id) {
                    invalidate_hover_tiles();
                }
                interaction_.hovered_path = std::move(new_hover_path);
                interaction_.hovered_group_id = std::move(new_hover_grp);
                interaction_.hovered_group_zone = new_hover_zone;
                interaction_.hovered_group_dot = new_hover_dot;
                interaction_.hovered_tile_id = std::move(new_hover_tile);
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            CancelHoverPreview();
            handled = true;
            tracking_mouse_leave_ = false;
            interaction_.cursor_inside = false;
            return 0;
        }

        case WM_LBUTTONUP: {
            handled = true;
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            pending_item_drag_ = false;
            releasing_capture_ = true;
            if (GetCapture() == hwnd) ReleaseCapture();
            releasing_capture_ = false;
            if (group_drag_session_.Pending()) {
                FinishGroupDrag(x, y);
                return 0;
            }

            if (interaction_.dragging_items) {
                const std::vector<std::wstring> paths(interaction_.selected_paths.begin(),
                                                      interaction_.selected_paths.end());
                const std::string dst_grp = interaction_.drop_target_group_id;
                const std::string dst_tile = interaction_.drop_target_tile_id;
                interaction_.dragging_items = false;
                interaction_.drop_target_group_id.clear();
                interaction_.drop_target_tile_id.clear();

                if (!dst_grp.empty()) {
                    std::wstring mapped_dst_dir;
                    for (const auto& g : state_.groups) {
                        if (g.id == dst_grp) {
                            mapped_dst_dir = g.mapped_folder_path;
                            break;
                        }
                    }
                    if (!mapped_dst_dir.empty()) {
                        PushUndo(L"移动到关联文件夹");
                        for (const auto& p : paths) {
                            MoveOrCopyToDirectoryCollisionSafe(p, mapped_dst_dir, false);
                        }
                        interaction_.selected_paths.clear();
                        RefreshDesktopItems();
                        RefreshHealthStats();
                        RecomputeLayout();
                        SaveModel();
                    } else {
                        PushUndo(L"移入分组");
                        const auto dest_keys = EvacuateMappedItemsToDesktop(paths);
                        for (auto& it : state_.items) {
                            if (dest_keys.contains(NormalizeFolderKey(it.path))) {
                                it.pinned_group_id = dst_grp;
                                it.force_loose = false;
                            }
                        }
                        RecomputeLayout();
                        SaveModel();
                    }
                } else if (!dst_tile.empty()) {
                    for (const auto& t : state_.tiles) {
                        if (t.id != dst_tile) continue;
                        const auto vk = model::ActiveTileView(t).kind;
                        if (vk == model::TileKind::StagingTray) StagePathsInTray(paths);
                        else if (vk == model::TileKind::RecycleBin) RecyclePathsWithUndoToast(paths);
                        else if (vk == model::TileKind::Apps) {
                            PushUndo(L"收进应用");
                            for (auto& it : state_.items) {
                                if (!it.is_shortcut || !interaction_.selected_paths.contains(it.path)) continue;
                                it.pinned_group_id = model::kAppsTileGroupId;
                                it.force_loose = false;
                            }
                            RecomputeLayout();
                            SaveModel();
                        }
                        break;
                    }
                } else {
                    PushUndo(L"放到桌面");
                    const auto dest_keys = EvacuateMappedItemsToDesktop(paths);
                    int idx = 0;
                    for (auto& it : state_.items) {
                        if (dest_keys.contains(NormalizeFolderKey(it.path))) {
                            it.pinned_group_id.clear();
                            it.force_loose = true;
                            it.loose_pos = {x + (idx % 3) * model::kLooseCellW,
                                            y + (idx / 3) * model::kLooseCellH};
                            ++idx;
                        }
                    }
                    RecomputeLayout();
                    SaveModel();
                }
                renderer_.InvalidateAll();
                return 0;
            }

            // Tile click (< 6px without dragging!)
            KillTimer(hwnd, kTimerLongPress);
            ReleaseTilePress();
            if (pending_tile_idx_ && *pending_tile_idx_ < state_.tiles.size()) {
                const size_t ti = *pending_tile_idx_;
                pending_tile_idx_.reset();
                if (tile_long_pressed_) { tile_long_pressed_ = false; return 0; }
                const model::RectI tr = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, state_.tiles[ti]);
                if (!tr.contains(x, y)) return 0;
                if (interaction_.tile_edit_mode) {
                    // Editing: a tap opens the tile's menu (自定义磁贴, 大小, 叠放).
                    OpenTileMenu(ti, x, y);
                    return 0;
                }
                {
                    const D2D1_RECT_F trf = D2D1::RectF(static_cast<float>(tr.x), static_cast<float>(tr.y),
                                                        static_cast<float>(tr.right()), static_cast<float>(tr.bottom()));
                    const D2D1_RECT_F eb = render::DesktopRenderer::TileEditButtonRect(trf, state_.tiles[ti].size.cols == 1);
                    if (x >= eb.left && x < eb.right && y >= eb.top && y < eb.bottom) {
                        OpenTileMenu(ti, x, y, "sub:tile-custom");
                        return 0;
                    }
                }
                if (model::ActiveTileView(state_.tiles[ti]).kind == model::TileKind::RecycleBin &&
                    live_data_.recycle_bin.item_count > 0) {
                    const D2D1_RECT_F trf3 = D2D1::RectF(static_cast<float>(tr.x), static_cast<float>(tr.y),
                                                         static_cast<float>(tr.right()), static_cast<float>(tr.bottom()));
                    const D2D1_RECT_F eb3 = render::DesktopRenderer::RecycleEmptyRect(trf3);
                    if (x >= eb3.left && x < eb3.right && y >= eb3.top && y < eb3.bottom) {
                        EmptyRecycleBinAsync();
                        return 0;
                    }
                }
                const int app = AppCellAt(state_.tiles[ti], tr, x, y);
                if (app >= 0) {
                    const auto& it = state_.items[AppsInTile()[static_cast<size_t>(app)]];
                    LaunchAsync(it.path);
                    return 0;
                }
                const int todo = TodoRowAt(state_.tiles[ti], tr, y);
                if (todo >= 0 && interaction_.hovered_todo_index == todo) {
                    const D2D1_RECT_F trf2 = D2D1::RectF(static_cast<float>(tr.x), static_cast<float>(tr.y),
                                                         static_cast<float>(tr.right()), static_cast<float>(tr.bottom()));
                    const D2D1_RECT_F xb = render::DesktopRenderer::TodoDeleteRect(state_.tiles[ti], trf2, static_cast<size_t>(todo));
                    if (x >= xb.left && x < xb.right && y >= xb.top && y < xb.bottom) {
                        PushUndo(L"删除待办");
                        state_.todos.erase(state_.todos.begin() + todo);
                        interaction_.hovered_todo_index = -1;
                        SaveModel();
                        ShowUndoToast(L"已删除待办");
                        renderer_.InvalidateAll();
                        return 0;
                    }
                }
                if (todo >= 0) {
                    state_.todos[static_cast<size_t>(todo)].done = !state_.todos[static_cast<size_t>(todo)].done;
                    SaveModel();
                    renderer_.InvalidateRect(tr);
                    return 0;
                }
                if (IsTodoAddButtonAt(state_.tiles[ti], tr, x, y)) {
                    std::wstring lines;
                    for (const auto& td : state_.todos) lines += td.text + L"\n";
                    OpenTileTextEditor(state_.tiles[ti], "@todos", lines);
                    return 0;
                }
                std::wstring row_path;
                bool row_is_folder = false, row_action = false;
                if (ListTileRowAt(state_.tiles[ti], tr, x, y, &row_path, &row_is_folder, &row_action) >= 0 && !row_path.empty()) {
                    if (row_action) RevealPath(row_path);
                    else if (row_is_folder) OpenPathInPulseOrShell(row_path, &tr);
                    else LaunchAsync(row_path);
                    return 0;
                }
                ActivateTileClick(ti, tr);
                return 0;
            }
            pending_tile_idx_.reset();

            if (resizing_tile_idx_) {
                model::CommitTileRestOrigins(state_.tiles);
                resizing_tile_idx_.reset();
                resizing_tile_zone_ = render::TileResizeZone::None;
                interaction_.resizing_tile_id.clear();
                RecomputeLayout();
                SaveModel();
                renderer_.InvalidateAll();
            }
            if (dragging_tile_idx_) FinishTileDrag(x, y);

            if (interaction_.marquee_active) {
                interaction_.marquee_active = false;
                renderer_.InvalidateAll();
            }
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            handled = true;
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);

            // Double-click group header -> collapse / expand!
            if (state_.settings.organize_enabled && !state_.settings.icons_hidden &&
                state_.settings.active_scene != model::DesktopScene::Present) {
                for (const size_t i : GroupHitOrder()) {
                    const auto& gv = group_views_[i];
                    if (gv.frame.contains(x, y) && y < gv.frame.y + model::kGroupHeaderHeight) {
                        state_.groups[i].collapsed = !state_.groups[i].collapsed;
                        RecomputeLayout();
                        SaveModel();
                        renderer_.InvalidateAll();
                        return 0;
                    }
                    if (gv.frame.contains(x, y)) break;
                }
            }

            model::RectI cell_rc{};
            if (auto hit = HitTestItem(x, y, &cell_rc)) {
                const auto& it = state_.items[*hit];
                if (it.is_folder) {
                    OpenPathInPulseOrShell(it.path, &cell_rc);
                } else {
                    LaunchAsync(it.path);
                }
                return 0;
            }

            if (state_.settings.tiles_enabled && !interaction_.side_drawer_open) {
                for (const auto& t : state_.tiles) {
                    if (render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t).contains(x, y)) {
                        return 0;
                    }
                }
            }

            if (state_.settings.active_scene == model::DesktopScene::Present) {
                state_.settings.active_scene = model::DesktopScene::Work;
                RecomputeLayout();
                SaveModel();
                SyncSettingsToPlugin();
                renderer_.InvalidateAll();
                return 0;
            }

            if (state_.settings.double_click_hide_icons) {
                state_.settings.icons_hidden = !state_.settings.icons_hidden;
            if (state_.settings.icons_hidden) CancelHoverPreview();
                SaveModel();
                SyncSettingsToPlugin();
                renderer_.InvalidateAll();
            }
            return 0;
        }

        case WM_CANCELMODE:
            CancelHoverPreview();
            if (group_drag_session_.Pending()) {
                handled = true;
                CancelGroupDrag();
                if (GetCapture() == hwnd) ReleaseCapture();
            }
            if (tile_drag_session_.Active()) {
                handled = true;
                CancelTileDrag();
                if (GetCapture() == hwnd) ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            CancelHoverPreview();
            // Capture taken away mid-press or mid-drag (Alt+Tab, a dialog): cancel cleanly and restore layout state.
            ReleaseTilePress();
            if (!releasing_capture_) {
                bool need_recompute = false;
                if (resizing_tile_idx_) {
                    resizing_tile_idx_.reset();
                    resizing_tile_zone_ = render::TileResizeZone::None;
                    interaction_.resizing_tile_id.clear();
                    model::CommitTileRestOrigins(state_.tiles);
                    need_recompute = true;
                }
                if (dragging_tile_idx_) {
                    CancelTileDrag(false);
                    need_recompute = true;
                }
                if (group_drag_session_.Pending()) CancelGroupDrag();
                pending_tile_idx_.reset();
                pending_item_drag_ = false;
                interaction_.dragging_items = false;
                interaction_.marquee_active = false;
                if (need_recompute) {
                    RecomputeLayout();
                    renderer_.InvalidateAll();
                }
            }
            break;

        case WM_MOUSEWHEEL: {
            // Over a group card or tile stack, the wheel turns its pages.
            POINT scr{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const POINT c = mount_.ScreenPxToClientDip(scr);
            if (state_.settings.organize_enabled && !state_.settings.icons_hidden &&
                state_.settings.active_scene != model::DesktopScene::Present &&
                !interaction_.side_drawer_open && !overlays_.IsOpen()) {
                for (const size_t i : GroupHitOrder()) {
                    const auto& g = state_.groups[i];
                    const auto& gv = group_views_[i];
                    if (!gv.expanded || gv.page_count < 2 || !gv.frame.contains(c.x, c.y)) continue;
                    handled = true;
                    const uint64_t now = GetTickCount64();
                    if (wheel_group_id_ != g.id || (now - last_group_wheel_tick_) > 450) {
                        wheel_group_id_ = g.id;
                        wheel_group_accum_ = 0;
                    }
                    last_group_wheel_tick_ = now;
                    wheel_group_accum_ += GET_WHEEL_DELTA_WPARAM(wp);
                    int steps = 0;
                    while (wheel_group_accum_ <= -WHEEL_DELTA) {
                        ++steps;
                        wheel_group_accum_ += WHEEL_DELTA;
                    }
                    while (wheel_group_accum_ >= WHEEL_DELTA) {
                        --steps;
                        wheel_group_accum_ -= WHEEL_DELTA;
                    }
                    if (steps != 0) {
                        const int cur = std::clamp(gv.page_index, 0, gv.page_count - 1);
                        const int next = std::clamp(cur + steps, 0, gv.page_count - 1);
                        if (next != cur) {
                            interaction_.group_pages[g.id] = next;
                            RefreshGroupViews();
                            renderer_.InvalidateRect(gv.frame);
                        }
                    }
                    return 0;
                }
            }
            // Over the 应用 tile (not stacked) the wheel turns its pages.
            for (const auto& t : state_.tiles) {
                if (!t.stack.empty() || t.kind != model::TileKind::Apps) continue;
                const model::RectI tr = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t);
                if (!tr.contains(c.x, c.y)) continue;
                const D2D1_RECT_F rf = D2D1::RectF(static_cast<float>(tr.x), static_cast<float>(tr.y),
                                                   static_cast<float>(tr.right()), static_cast<float>(tr.bottom()));
                const int pages = render::DesktopRenderer::AppsTilePaging(t, rf, AppsInTile().size()).pages;
                if (pages < 2) break;
                handled = true;
                const uint64_t now = GetTickCount64();
                if (now - last_wheel_tick_ < 220) return 0;
                last_wheel_tick_ = now;
                const int next = std::clamp(interaction_.apps_page + (GET_WHEEL_DELTA_WPARAM(wp) < 0 ? 1 : -1), 0, pages - 1);
                if (next != interaction_.apps_page) { interaction_.apps_page = next; renderer_.InvalidateRect(tr); }
                return 0;
            }
            for (auto& t : state_.tiles) {
                if (t.stack.size() < 2) continue;
                const model::RectI tr = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t);
                if (!tr.contains(c.x, c.y)) continue;
                handled = true;
                const uint64_t now = GetTickCount64();
                if (now - last_wheel_tick_ < 280) return 0;
                last_wheel_tick_ = now;
                const int dir = GET_WHEEL_DELTA_WPARAM(wp) < 0 ? 1 : -1;
                const int n = static_cast<int>(t.stack.size());
                t.stack_index = ((t.stack_index + dir) % n + n) % n;
                SaveModel();
                renderer_.InvalidateRect(tr);
                return 0;
            }
            break;
        }

        case WM_RBUTTONUP: {
            CancelHoverPreview();
            handled = true;
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            POINT scr{x, y};
            mount_.ClientToScreenDip(&scr);

            overlays::ContextMenuSpec spec{};
            spec.screen_anchor = scr;
            spec.tag_palette = tag_palette_;
            spec.undo_label = (last_undo_is_ai_archive_ && !last_ai_archive_undo_.empty())
                                  ? std::wstring(L"AI 智能归档")
                                  : undo_.NextUndoLabel();
            spec.health_issue_count = cached_health_report_.issues.size();
            for (const auto& g : state_.groups) {
                spec.groups_list.push_back({g.id, g.title, g.icon_glyph});
            }

            std::string hit_grp_id;
            if (auto hit = HitTestItem(x, y, nullptr, &hit_grp_id)) {
                const auto& it = state_.items[*hit];
                if (!interaction_.selected_paths.contains(it.path)) {
                    interaction_.selected_paths.clear();
                    interaction_.selected_paths.insert(it.path);
                    interaction_.focused_path = it.path;
                    renderer_.InvalidateAll();
                }
                spec.target_kind = overlays::ContextMenuTargetKind::FileIcon;
                spec.target_path = it.path;
                spec.target_name = it.name;
                spec.target_is_folder = it.is_folder;
                spec.selected_count = interaction_.selected_paths.size();
                spec.item_in_tray = IsPathInTray(it.path);
                spec.current_group_id = hit_grp_id;
                spec.current_tag_id = it.tag_id;
                if (it.is_folder) {
                    const std::wstring item_key = NormalizeFolderKey(it.path);
                    for (const auto& g : state_.groups) {
                        if (!g.mapped_folder_path.empty() && NormalizeFolderKey(g.mapped_folder_path) == item_key) {
                            spec.target_folder_already_mapped = true;
                        }
                        if (!hit_grp_id.empty() && g.id == hit_grp_id && !g.rule.fallback_catch_all && g.mapped_folder_path.empty()) {
                            spec.current_group_can_map_folder = true;
                        }
                    }
                }
                overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
                overlays_.OpenContextMenu(spec, state_.settings);
                pipe_.Send(ipc::MessageType::ShellMenuReq, ++next_req_id_,
                           "{\"background\":false,\"paths\":[" + ipc::JsonQuoteWide(spec.target_path) + "]}");
                return 0;
            }

            if (state_.settings.organize_enabled && !state_.settings.icons_hidden &&
                state_.settings.active_scene != model::DesktopScene::Present) {
                for (const size_t i : GroupHitOrder()) {
                    if (group_views_[i].frame.contains(x, y)) {
                        FillGroupContextMenuSpec(i, spec);
                        overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
                        overlays_.OpenContextMenu(spec, state_.settings);
                        return 0;
                    }
                }
            }

            if (state_.settings.tiles_enabled && state_.settings.organize_enabled &&
                !interaction_.side_drawer_open) {
                for (size_t ti = 0; ti < state_.tiles.size(); ++ti) {
                    const auto& t = state_.tiles[ti];
                    const model::RectI tr = render::DesktopRenderer::ComputeTileRectInline(tile_board_rect_, t);
                    if (!tr.contains(x, y)) continue;
                    // A shortcut in the 应用 tile: dedicated clean shortcut menu (打开 / 管理员运行 / 所在位置 / 放回桌面 / 固定 / 属性 / 删除).
                    const int app = interaction_.tile_edit_mode ? -1 : AppCellAt(t, tr, x, y);
                    if (app >= 0) {
                        const auto& it = state_.items[AppsInTile()[static_cast<size_t>(app)]];
                        interaction_.selected_paths = {it.path};
                        interaction_.focused_path = it.path;
                        spec.target_kind = overlays::ContextMenuTargetKind::AppTileShortcut;
                        spec.target_path = it.path;
                        spec.target_name = it.name;
                        spec.selected_count = 1;
                        spec.item_in_apps_tile = true;
                        overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
                        overlays_.OpenContextMenu(spec, state_.settings);
                        return 0;
                    }
                    OpenTileMenu(ti, x, y);
                    return 0;
                }
            }
            spec.widget_catalog = WidgetCatalog();

            spec.target_kind = overlays::ContextMenuTargetKind::BlankDesktop;
            spec.ai_organize_enabled = AiOrganizerEnabled();
            overlays_.SetAppearance(state_.settings.dark_mode, EffectiveAccentArgb());
            overlays_.OpenContextMenu(spec, state_.settings);
            const std::wstring desk = mount::ResolveKnownFolder(FOLDERID_Desktop);
            if (!desk.empty()) {
                pipe_.Send(ipc::MessageType::ShellMenuReq, ++next_req_id_,
                           "{\"background\":true,\"paths\":[" + ipc::JsonQuoteWide(desk) + "]}");
            }
            return 0;
        }

        case WM_KEYDOWN: {
            handled = true;
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

            if (wp == VK_SPACE && !interaction_.focused_path.empty()) {
                OpenQuickLookForPath(interaction_.focused_path);
            } else if (wp == VK_F2 && !interaction_.focused_path.empty()) {
                StartInlineRenameForPath(interaction_.focused_path);
            } else if (wp == VK_F5) {
                RefreshDesktopItems();
                RefreshLiveTileData();
                RefreshHealthStats();
                RecomputeLayout();
                renderer_.InvalidateAll();
            } else if (wp == VK_RETURN && !interaction_.selected_paths.empty()) {
                for (const auto& p : interaction_.selected_paths) {
                    LaunchAsync(p);
                }
            } else if (wp == VK_DELETE && !interaction_.selected_paths.empty()) {
                std::vector<std::wstring> paths(interaction_.selected_paths.begin(),
                                                interaction_.selected_paths.end());
                interaction_.selected_paths.clear();
                RecyclePathsWithUndoToast(paths);
            } else if (ctrl && shift && wp == 'S' && !interaction_.selected_paths.empty()) {
                std::vector<std::wstring> paths(interaction_.selected_paths.begin(),
                                                interaction_.selected_paths.end());
                StagePathsInTray(paths);
            } else if (ctrl && shift && wp == 'N') {
                CreateNewDesktopEntry(true, L"新建文件夹");
            } else if (ctrl && shift && wp == 'C' && !interaction_.selected_paths.empty()) {
                std::wstring joined;
                for (const auto& p : interaction_.selected_paths) {
                    if (!joined.empty()) joined += L"\r\n";
                    joined += p;
                }
                CopyWideTextToClipboard(hwnd, joined);
                ShowUndoToast(L"已复制路径到剪贴板");
            } else if (ctrl && shift && wp == 'A') {
                OpenAiOrganizeDrawer();
            } else if (ctrl && wp == 'F') {
                OpenSearchOverlay(L"");
            } else if (ctrl && wp == 'A') {
                interaction_.selected_paths.clear();
                for (const auto& it : state_.items) {
                    if (it.hidden_by_mapped_group) continue;
                    if (!state_.settings.organize_enabled && !it.mapped_owner_group_id.empty()) continue;
                    interaction_.selected_paths.insert(it.path);
                }
                renderer_.InvalidateAll();
            } else if (ctrl && wp == 'Z') {
                CancelGroupDrag();
                CancelTileDrag();
                PerformUndo();
            } else if (ctrl && wp == 'Y') {
                CancelGroupDrag();
                CancelTileDrag();
                if (undo_.Redo(state_)) {
                    renderer_.ReloadWallpaper(state_.settings.wallpaper_preset, state_.settings.dark_mode);
                    SyncWatcherDirectories();
                    RefreshDesktopItems();
                    RefreshHealthStats();
                    RecomputeLayout();
                    SaveModel();
                    renderer_.InvalidateAll();
                }
            } else if (wp == VK_ESCAPE) {
                if (group_drag_session_.Pending()) {
                    CancelGroupDrag();
                    if (GetCapture() == hwnd) ReleaseCapture();
                } else if (tile_drag_session_.Active()) {
                    CancelTileDrag();
                    if (GetCapture() == hwnd) ReleaseCapture();
                } else if (overlays_.IsOpen()) {
                    overlays_.Dismiss();
                } else if (interaction_.tile_edit_mode) {
                    SetTileEditMode(false);
                } else if (state_.settings.active_scene != model::DesktopScene::Work) {
                    state_.settings.active_scene = model::DesktopScene::Work;
                    RecomputeLayout();
                    SaveModel();
                    SyncSettingsToPlugin();
                    renderer_.InvalidateAll();
                } else {
                    interaction_.search_highlight_query.clear();
                    interaction_.selected_paths.clear();
                    renderer_.InvalidateAll();
                }
            }
            return 0;
        }

        case WM_IME_STARTCOMPOSITION: {
            if (state_.settings.type_to_search && !overlays_.IsOpen() && !overlays_.IsInlineRenaming()) {
                if (HIMC imc = ImmGetContext(hwnd)) {
                    const RECT wa = PrimaryWorkAreaScreen();
                    POINT dip_pt{wa.left + ((wa.right - wa.left) - 540) / 2 + 42, wa.top + 132};
                    mount_.ScreenDipToClient(&dip_pt);
                    const float s = mount_.DipScale();
                    const POINT px_pt{
                        static_cast<LONG>(std::lround(static_cast<float>(dip_pt.x) * s)),
                        static_cast<LONG>(std::lround(static_cast<float>(dip_pt.y) * s)),
                    };
                    COMPOSITIONFORM cf{};
                    cf.dwStyle = CFS_POINT;
                    cf.ptCurrentPos = px_pt;
                    ImmSetCompositionWindow(imc, &cf);
                    CANDIDATEFORM cand{};
                    cand.dwStyle = CFS_CANDIDATEPOS;
                    cand.ptCurrentPos = POINT{px_pt.x, px_pt.y + static_cast<LONG>(std::lround(24.0f * s))};
                    ImmSetCandidateWindow(imc, &cand);
                    ImmReleaseContext(hwnd, imc);
                }
            }
            break;
        }

        case WM_CHAR: {
            const wchar_t ch = static_cast<wchar_t>(wp);
            if (state_.settings.type_to_search && ch > 0x20 && ch != 0x7F &&
                (GetKeyState(VK_CONTROL) & 0x8000) == 0 && !overlays_.IsInlineRenaming()) {
                handled = true;
                if (overlays_.ActiveKind() == overlays::OverlayKind::SearchPopup) {
                    overlays_.AppendSearchChar(ch);
                } else {
                    OpenSearchOverlay(std::wstring(1, ch));
                }
                return 0;
            }
            break;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            handled = true;
            return 0;

        default:
            break;
        }
        return 0;
    }

    std::wstring pipe_name_;
    DWORD parent_pid_ = 0;
    HANDLE parent_proc_ = nullptr;
    bool restore_on_exit_ = true;
    WORD last_clock_minute_ = 0xFFFFu;

    mount::DesktopMountHost mount_;
    mount::DesktopFolderWatcher watcher_;
    host::LiveWallpaperWatcher live_watcher_;
    render::DesktopRenderer renderer_;
    overlays::DesktopOverlayController overlays_;
    overlays::DesktopDropManager drop_mgr_;
    ipc::AsyncPipeEndpoint pipe_;
    uint64_t next_req_id_ = 1;

    std::mutex ipc_mutex_;
    std::deque<ipc::Frame> ipc_inbox_;

    model::DesktopModelState state_{};
    model::UndoHistory undo_{32};
    model::RectI tile_board_rect_{};
    std::vector<model::GroupComputedView> group_views_;
    render::LiveTileRuntimeData live_data_{};
    render::DesktopInteractionState interaction_{};
    std::vector<overlays::ColorTagOption> tag_palette_;
    std::vector<model::InstalledAppRecord> installed_apps_cache_;
    model::HealthReport cached_health_report_{};

    std::optional<ai::AssistantAvailability> assistant_availability_;
    overlays::AiDrawerViewState ai_view_state_{};
    std::shared_ptr<ai::AiOrganizeSession> active_ai_session_;
    uint64_t ai_req_seq_ = 0;
    std::mutex ai_mutex_;
    uint64_t pending_ai_seq_ = 0;
    ai::AiOrganizeSession::Outcome pending_ai_outcome_{};
    ai::AiArchiveUndoTransaction last_ai_archive_undo_{};
    bool last_undo_is_ai_archive_ = false;
    size_t pending_ai_archive_toast_count_ = 0;

    bool releasing_capture_ = false;
    model::PointI drag_start_pt_{0, 0};
    model::PointI drag_offset_{0, 0};
    bool pending_item_drag_ = false;
    std::optional<size_t> pending_tile_idx_;
    bool tile_long_pressed_ = false;
    live::SystemSampler system_sampler_;
    std::shared_ptr<std::atomic<bool>> weather_inflight_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<uint64_t>> wallpaper_apply_seq_ = std::make_shared<std::atomic<uint64_t>>(0);
    bool items_paths_dirty_ = true;
    uint32_t heartbeat_tick_ = 0;
    uint64_t weather_fetched_tick_ = 0;
    std::wstring weather_fetched_city_;
    uint32_t live_tick_ = 0;
    uint64_t last_wheel_tick_ = 0;
    std::vector<std::wstring> watched_extra_dirs_;
    std::string wheel_group_id_;
    int wheel_group_accum_ = 0;
    uint64_t last_group_wheel_tick_ = 0;
    pulse::desktop::HoverPreviewSession hover_preview_;
    pulse::desktop::interaction::GroupHoverSession group_hover_;
    pulse::desktop::interaction::GroupDragSession group_drag_session_;
    model::DesktopModelState drag_group_undo_snapshot_{};
    std::optional<size_t> pending_group_idx_;
    std::string pending_group_tab_id_;
    std::optional<size_t> dragging_group_idx_;
    std::optional<size_t> resizing_group_idx_;
    std::optional<size_t> dragging_tile_idx_;
    std::optional<size_t> resizing_tile_idx_;
    render::TileResizeZone resizing_tile_zone_ = render::TileResizeZone::None;
    model::RectI resize_tile_start_rect_{};
    model::DesktopModelState drag_tile_undo_snapshot_{};
    model::TileDragSession tile_drag_session_;
#ifdef PULSE_DESKTOP_DRAG_TEST
    std::string test_saved_model_;
#endif
#ifdef PULSE_DESKTOP_PERSISTENCE_TEST
    std::string test_model_input_;
#endif
#ifdef PULSE_DESKTOP_PREVIEW_TEST
    std::string test_preview_request_;
    std::string test_hover_request_;
#endif
    model::PointI drag_tile_cursor_{};
    UINT photo_timer_ms_ = 0;
    std::string detail_tile_id_;   // the tile whose detail card is (or was last) open
    float press_from_ = 0.0f, press_to_ = 0.0f;
    uint64_t press_start_ms_ = 0;
    uint64_t photo_last_switch_ms_ = 0;
    uint64_t photo_rng_ = 0x9E3779B97F4A7C15ull ^ GetTickCount64();
    std::atomic<bool> icons_ready_posted_{false};
    std::wstring quick_search_seed_;
    bool shell_prewarm_pending_ = false;
    uint64_t quick_search_tick_ = 0;
    uint64_t last_activity_tick_ = 0;
    bool working_set_trimmed_ = false;
    bool hover_glass_timer_active_ = false;
    bool tracking_mouse_leave_ = false;
    bool session_locked_ = false;
    std::unordered_set<std::wstring> marquee_base_selection_;
};

} // namespace

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    bool mode_probe = false;
    bool mode_bench = false;
    bool mode_bench_live = false;
    bool mode_restore_icons = false;
    bool mode_verify_hosted_edit = false;
    std::wstring snapshots_dir;
    std::wstring pipe_name;
    DWORD parent_pid = 0;
    bool mode_check_lumatext = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--probe-hierarchy") {
            mode_probe = true;
        } else if (arg == L"--restore-icons") {
            mode_restore_icons = true;
        } else if (arg == L"--verify-hosted-edit") {
            mode_verify_hosted_edit = true;
        } else if (arg == L"--benchmark-4k") {
            mode_bench = true;
        } else if (arg == L"--benchmark-live-memory") {
            mode_bench_live = true;
        } else if (arg == L"--render-snapshots" && i + 1 < argc) {
            snapshots_dir = argv[++i];
        } else if (arg == L"--pipe" && i + 1 < argc) {
            pipe_name = argv[++i];
        } else if (arg == L"--parent-pid" && i + 1 < argc) {
            parent_pid = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (arg == L"--check-lumatext") {
            mode_check_lumatext = true;
        }
    }
    LocalFree(argv);

    // Pulse's text engine ships with Pulse itself, beside the pulse.exe that started this host: load that copy
    // (by full path, its own imports from System32 only) so the plugin's delay-loaded lumatext.dll binds to it
    // instead of carrying a second copy. Without it LumaText stays off and the edits fall back to native drawing.
    if (parent_pid != 0 && !GetModuleHandleW(L"lumatext.dll")) {
        if (HANDLE parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent_pid)) {
            wchar_t image[MAX_PATH * 2]{};
            DWORD len = ARRAYSIZE(image);
            if (QueryFullProcessImageNameW(parent, 0, image, &len)) {
                std::wstring dll(image, len);
                const size_t slash = dll.find_last_of(L"\\/");
                if (slash != std::wstring::npos) {
                    dll.resize(slash + 1);
                    dll += L"lumatext.dll";
                    if (GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES)
                        LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                }
            }
            CloseHandle(parent);
        }
    }
    if (mode_check_lumatext) {
        // Diagnostic: which lumatext.dll this host would use (prints the path, exit 0 when found).
        wchar_t path[MAX_PATH * 2]{};
        const HMODULE luma = GetModuleHandleW(L"lumatext.dll");
        if (luma) GetModuleFileNameW(luma, path, ARRAYSIZE(path));
        std::fwprintf(stdout, L"lumatext: %ls\n", luma ? path : L"(not loaded)");
        return luma ? 0 : 1;
    }

    if (mode_probe) {
        return RunProbeHierarchyMode();
    }
    if (mode_restore_icons) {
        desktop_takeover::RecoverOrphanedTakeover(false);
        return 0;
    }
    if (mode_verify_hosted_edit) {
        return pulse::desktop::tests::RunHostedEditVerification();
    }
    if (mode_bench) {
        return RunBenchmark4KMode();
    }
    if (mode_bench_live) {
        return RunBenchmarkLiveMemoryMode();
    }
    if (!snapshots_dir.empty()) {
        return RunRenderSnapshotsMode(snapshots_dir);
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int code = 0;
    {
        DesktopHostSession session(std::move(pipe_name), parent_pid);
        code = session.Run();
    }
    CoUninitialize();
    return code;
}
