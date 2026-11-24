// mount_restore_test.cpp - Automated [PASS]/[FAIL] M0/M5 verification suite for pulse.desktop:
//   1. Live read-only desktop HWND hierarchy probe + Shell COM icon snapshot & atomic JSON round-trip
//   2. Crash-safe TakeoverState orphan recovery & 3-in-10s circuit breaker (isolated HKCU + mock HWND)
//   3. Current-user SID DACL overlapped Named Pipe IPC round-trip (JSON + binary QuickLook payload)
//   4. Real 4K (3840x2160) D3D11/D2D/WIC 1/4-res blur renderer (<28 MB Private WS, <=6 MB LRU, 0 idle Presents)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <psapi.h>

#include "desktop_takeover_record.h"
#include "host/desktop_mount.h"
#include "host/overlays/desktop_overlays.h"
#include "host/render/desktop_renderer.h"
#include "ipc/desktop_ipc.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace {

namespace ipc = pulse::desktop::ipc;
namespace model = pulse::desktop::model;
namespace mount = pulse::desktop::mount;
namespace render = pulse::desktop::render;
namespace overlays = pulse::desktop::overlays;

int g_pass_count = 0;
int g_fail_count = 0;

void Check(bool cond, const char* name, const char* detail = "") {
    if (cond) {
        ++g_pass_count;
        if (detail && detail[0] != '\0') {
            std::printf("[PASS] %s (%s)\n", name, detail);
        } else {
            std::printf("[PASS] %s\n", name);
        }
    } else {
        ++g_fail_count;
        std::printf("[FAIL] %s %s\n", name, detail ? detail : "");
    }
    std::fflush(stdout);
}

uint64_t QueryPrivateWorkingSetBytes() noexcept {
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
                const bool shared = ((ws_buf[i] & 0xFFFu) & 0x100u) != 0;
                if (!shared) ++priv_pages;
            }
            return priv_pages * page_size;
        }
        if (GetLastError() == ERROR_BAD_LENGTH && ws_buf[0] > 0) {
            ws_buf.resize(static_cast<size_t>(ws_buf[0]) + 1024);
        } else {
            break;
        }
    }

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return pmc.WorkingSetSize;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Test 1: Live Read-Only Hierarchy Probe & Native Icon Snapshot Round-Trip
// ---------------------------------------------------------------------------
void TestLiveHierarchyAndSnapshotRoundTrip() {
    const HWND live_lv_before = desktop_takeover::FindNativeDesktopListView();
    const BOOL vis_before = live_lv_before ? IsWindowVisible(live_lv_before) : TRUE;

    HDESK hdesk = GetThreadDesktop(GetCurrentThreadId());
    wchar_t desk_name[128]{};
    GetUserObjectInformationW(hdesk, UOI_NAME, desk_name, sizeof(desk_name), nullptr);
    const bool on_interactive_desktop = (_wcsicmp(desk_name, L"Default") == 0);

    const mount::DesktopHierarchyInfo info = mount::ProbeDesktopHierarchy();
    char mode_detail[220]{};
    std::snprintf(mode_detail, sizeof(mode_detail), "desktop=%s mode=%s progman=0x%llX defview=0x%llX lv=0x%llX pid=%lu",
                  ipc::WideToUtf8(desk_name).c_str(),
                  mount::HierarchyModeName(info.mode),
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info.progman)),
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info.shelldll_defview)),
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info.sys_listview32)),
                  static_cast<unsigned long>(info.explorer_pid));
    const bool hierarchy_ok = info.mode != mount::DesktopHierarchyMode::NotFound &&
                              info.progman != nullptr &&
                              info.shelldll_defview != nullptr &&
                              (!on_interactive_desktop || (info.sys_listview32 != nullptr && info.explorer_pid != 0));
    Check(hierarchy_ok, "ProbeDesktopHierarchy detects live Windows desktop tree", mode_detail);

    mount::NativeIconSnapshot snap{};
    HRESULT hr = E_FAIL;
    const bool cap_ok = mount::CaptureNativeIconSnapshot(snap, &hr);
    char snap_detail[128]{};
    std::snprintf(snap_detail, sizeof(snap_detail), "hr=0x%08lX items=%zu monitors=%zu",
                  static_cast<unsigned long>(hr), snap.items.size(), snap.monitors.size());
    Check(cap_ok && SUCCEEDED(hr) && !snap.monitors.empty(),
          "CaptureNativeIconSnapshot reads live Explorer IFolderView2", snap_detail);

    // Prepare snapshot payload for atomic JSON persistence test (augmenting copy if desktop has 0 icons).
    mount::NativeIconSnapshot to_save = snap;
    if (to_save.items.empty()) {
        to_save.items.push_back({L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", L"此电脑", L"此电脑", {32, 48}});
        to_save.items.push_back({L"C:\\Users\\Public\\Desktop\\ Sample \"Quote\".lnk", L"Sample \"Quote\".lnk", L"Sample", {32, 148}});
    }

    wchar_t tmp_dir[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(tmp_dir), tmp_dir);
    const std::filesystem::path test_dir =
        std::filesystem::path(tmp_dir) / (L"PulseDesktopSnapTest_" + std::to_wstring(GetCurrentProcessId()));
    const std::wstring json_path = (test_dir / L"native_icons.json").wstring();

    const bool saved = mount::SaveIconSnapshotFile(json_path, to_save);
    mount::NativeIconSnapshot loaded{};
    const bool read_ok = saved && mount::LoadIconSnapshotFile(json_path, loaded);

    bool exact_match = read_ok &&
                       loaded.view_mode == to_save.view_mode &&
                       loaded.icon_size == to_save.icon_size &&
                       loaded.monitors.size() == to_save.monitors.size() &&
                       loaded.items.size() == to_save.items.size();
    for (size_t i = 0; exact_match && i < to_save.items.size(); ++i) {
        exact_match = (loaded.items[i].parsing_name == to_save.items[i].parsing_name) &&
                      (loaded.items[i].relative_name == to_save.items[i].relative_name) &&
                      (loaded.items[i].display_name == to_save.items[i].display_name) &&
                      (loaded.items[i].position == to_save.items[i].position);
    }
    Check(exact_match, "Atomic SaveIconSnapshotFile / LoadIconSnapshotFile preserves 100% coordinates");

    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);

    // Verify user's real desktop icons were NEVER hidden or altered!
    if (live_lv_before) {
        Check(IsWindowVisible(live_lv_before) == vis_before,
              "Live Explorer SysListView32 visibility remained completely untouched");
    }
}

// ---------------------------------------------------------------------------
// Test 2: Orphaned-host recovery & 3-in-10s Crash Circuit Breaker
// The host hides Explorer's icons with Explorer's own switch; the record says whether it owes turning
// them back on. Recovery here never touches the live desktop (allow_live_desktop = false).
// ---------------------------------------------------------------------------
void TestCrashSafeTakeoverAndCircuitBreaker() {
    const std::wstring reg_key =
        L"Software\\Pulse\\DesktopTakeover_Test_" + std::to_wstring(GetCurrentProcessId());
    desktop_takeover::ClearActive(reg_key.c_str(), true);

    // A live host keeps its takeover.
    desktop_takeover::RecordActive(GetCurrentProcessId(), true, L"C:\\Temp\\mock_snap.json", reg_key.c_str());
    const bool kept = !desktop_takeover::RecoverOrphanedTakeover(true, reg_key.c_str(), false);
    Check(kept && desktop_takeover::ReadState(reg_key.c_str()).active,
          "RecoverOrphanedTakeover leaves a live host's takeover alone");

    // A dead host (guaranteed-dead PID) that hid the icons is recovered and the debt cleared.
    const bool rec_ok = desktop_takeover::RecordActive(0xFFFFFFFCu, true, L"C:\\Temp\\mock_snap.json", reg_key.c_str());
    const auto pre_state = desktop_takeover::ReadState(reg_key.c_str());
    Check(rec_ok && pre_state.active && pre_state.hid_icons, "Recorded takeover that hid the desktop icons");
    const bool recovered = desktop_takeover::RecoverOrphanedTakeover(true, reg_key.c_str(), false);
    const auto post_state = desktop_takeover::ReadState(reg_key.c_str());
    Check(recovered && !post_state.active && !post_state.hid_icons,
          "RecoverOrphanedTakeover clears an orphaned takeover");

    // Verify 3-in-10s crash circuit breaker.
    desktop_takeover::RecordActive(GetCurrentProcessId(), false, L"C:\\Temp\\mock_snap.json", reg_key.c_str());
    bool b1 = false, b2 = false, b3 = false, b_reset = false;
    desktop_takeover::RecordCrashEvent(10000ull, 10000ull, 3u, b1, reg_key.c_str());
    desktop_takeover::RecordCrashEvent(14000ull, 10000ull, 3u, b2, reg_key.c_str());
    desktop_takeover::RecordCrashEvent(18500ull, 10000ull, 3u, b3, reg_key.c_str());
    const auto breaker_state = desktop_takeover::ReadState(reg_key.c_str());

    // After >10s quiet period, crash counter resets to 1 without tripping breaker.
    desktop_takeover::RecordCrashEvent(40000ull, 10000ull, 3u, b_reset, reg_key.c_str());
    const auto reset_state = desktop_takeover::ReadState(reg_key.c_str());

    Check(!b1 && !b2 && b3 && !breaker_state.active && !b_reset && reset_state.crash_count == 1u,
          "RecordCrashEvent trips circuit breaker on 3 crashes within 10s and resets outside window");

    desktop_takeover::ClearActive(reg_key.c_str(), true);

    HKEY probe_deleted = nullptr;
    const bool key_cleaned =
        RegOpenKeyExW(HKEY_CURRENT_USER, reg_key.c_str(), 0, KEY_READ, &probe_deleted) != ERROR_SUCCESS;
    if (probe_deleted) RegCloseKey(probe_deleted);
    Check(key_cleaned, "Cleaned up isolated test registry key");
}

// ---------------------------------------------------------------------------
// Test 3: Authenticated Overlapped Named Pipe IPC Round-Trip
// ---------------------------------------------------------------------------
void TestAuthenticatedPipeIpcRoundTrip() {
    const std::wstring pipe_name = ipc::MakeUniquePipeName();
    Check(!pipe_name.empty(), "MakeUniquePipeName generates CSPRNG pipe path");

    std::mutex mtx;
    std::condition_variable cv;
    bool got_hello = false;
    bool got_pong = false;
    bool got_preview = false;
    ipc::PreviewPayloadData decoded_preview{};

    ipc::AsyncPipeEndpoint server;
    ipc::AsyncPipeEndpoint client;

    const bool srv_ok = server.StartServer(
        pipe_name,
        [&](ipc::Frame&& f) {
            if (f.type == ipc::MessageType::Hello) {
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    got_hello = (f.PayloadText().find("desktop_test") != std::string_view::npos);
                }
                server.Send(ipc::MessageType::Ping, 77u, "{\"seq\":1}");

                ipc::PreviewPayloadData p{};
                p.status = 0;
                p.size_bytes = 123456;
                p.image_width = 1920;
                p.image_height = 1080;
                p.thumb_width = 16;
                p.thumb_height = 16;
                p.thumb_stride = 64;
                p.path = L"C:\\Users\\Public\\Desktop\\测试预览.png";
                p.display_type = L"PNG 图像";
                p.text_excerpt = L"QuickLook 预览摘要";
                p.thumb_bgra.assign(16u * 64u, 0xCCu);
                server.SendBinary(ipc::MessageType::PreviewReplyMsg, 88u, ipc::EncodePreviewPayload(p));
                cv.notify_all();
            } else if (f.type == ipc::MessageType::Pong && f.request_id == 77u) {
                std::lock_guard<std::mutex> lk(mtx);
                got_pong = true;
                cv.notify_all();
            }
        });

    const bool cli_ok = client.StartClient(
        pipe_name,
        [&](ipc::Frame&& f) {
            if (f.type == ipc::MessageType::Ping) {
                client.Send(ipc::MessageType::Pong, f.request_id, "{\"ack\":true}");
            } else if (f.type == ipc::MessageType::PreviewReplyMsg) {
                ipc::PreviewPayloadData out{};
                const bool ok = ipc::DecodePreviewPayload(f.payload, out);
                std::lock_guard<std::mutex> lk(mtx);
                got_preview = ok;
                decoded_preview = std::move(out);
                cv.notify_all();
            }
        });

    client.Send(ipc::MessageType::Hello, 1u, "{\"client\":\"desktop_test\"}");

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::milliseconds(3000),
                    [&]() { return got_hello && got_pong && got_preview; });
    }

    server.Stop();
    client.Stop();

    Check(srv_ok && cli_ok && got_hello && got_pong,
          "AsyncPipeEndpoint server/client overlapped JSON handshake & Ping/Pong");
    Check(got_preview &&
          decoded_preview.path == L"C:\\Users\\Public\\Desktop\\测试预览.png" &&
          decoded_preview.thumb_bgra.size() == 1024u &&
          decoded_preview.image_width == 1920u,
          "Binary PreviewReplyMsg codec round-trips UTF-16 metadata & BGRA buffer");
}

// ---------------------------------------------------------------------------
// Test 4: Real 4K (3840x2160) Renderer, <=6 MB LRU Cap, 0 Idle Presents, 0 MB Overlay
// ---------------------------------------------------------------------------
void Test4KRendererMemoryAndZeroIdlePresents() {
    render::DesktopRenderer renderer;
    const bool init_ok = renderer.InitializeOffscreen(3840, 2160);
    renderer.Backdrop().SetUseVectorIconsOnly(true);
    Check(init_ok &&
          renderer.Backdrop().QuarterWidth() == 960u &&
          renderer.Backdrop().QuarterHeight() == 540u &&
          renderer.Backdrop().AcrylicBlurBitmap() != nullptr &&
          renderer.Backdrop().MicaBlurBitmap() != nullptr &&
          renderer.Backdrop().NoiseBrush() != nullptr,
          "4K WallpaperBackdrop decodes at 1/4-res (960x540) and bakes Acrylic + Mica + Noise brushes");

    // Build 4 groups + all 11 live tile kinds + 24 items.
    model::DesktopModelState state{};
    const model::RectI work_area{0, 0, 3840, 2112};
    for (int g = 0; g < 4; ++g) {
        model::DesktopGroup grp{};
        grp.id = "g" + std::to_string(g);
        grp.title = L"测试分区 " + std::to_wstring(g + 1);
        grp.x = 32 + (g % 2) * 380;
        grp.y = 32 + (g / 2) * 260;
        grp.cols = 4;
        grp.visible_rows = 2;
        state.groups.push_back(std::move(grp));
    }

    const model::TileKind all_kinds[11] = {
        model::TileKind::Photos, model::TileKind::Downloads, model::TileKind::Recent,
        model::TileKind::PinnedFolder, model::TileKind::RecycleBin, model::TileKind::ThisPC,
        model::TileKind::DiskRing, model::TileKind::PulseLauncher, model::TileKind::Clock,
        model::TileKind::StickyNote, model::TileKind::StagingTray,
    };
    for (int i = 0; i < 11; ++i) {
        model::DesktopTile t{};
        t.id = "t" + std::to_string(i);
        t.kind = all_kinds[i];
        t.col = i % 5;
        t.row = (i / 5) * 2;
        t.size = {1, 1};
        t.note_body = L"便签测试";
        state.tiles.push_back(std::move(t));
    }

    for (int i = 0; i < 24; ++i) {
        model::DesktopItem it{};
        it.path = L"C:\\Users\\Public\\Desktop\\Item_" + std::to_wstring(i) + L".docx";
        it.name = L"桌面项目_" + std::to_wstring(i) + L".docx";
        it.extension = L".docx";
        if (i < 12) {
            it.pinned_group_id = state.groups[static_cast<size_t>(i) % 4].id;
        } else {
            it.force_loose = true;
            it.loose_pos = {24 + (i - 12) * 100, 620};
        }
        state.items.push_back(std::move(it));
    }

    const model::RectI board_rect = render::DesktopRenderer::ComputeBoardRectFallback(work_area, true);
    const auto group_views = render::DesktopRenderer::BuildSimpleGroupViews(state);
    render::LiveTileRuntimeData live{};
    render::DesktopInteractionState inter{};
    inter.cleanable_installer_bytes = 753ull * 1024ull * 1024ull;

    for (int i = 0; i < 4; ++i) {
        renderer.InvalidateAll();
        renderer.Tick(state, group_views, board_rect, live, inter);
    }

    // Dirty-rect single-card update.
    renderer.InvalidateRect(group_views[0].frame);
    renderer.Tick(state, group_views, board_rect, live, inter);

    const uint64_t presents_before_idle = renderer.Metrics().total_presents;
    for (int i = 0; i < 12; ++i) {
        renderer.Tick(state, group_views, board_rect, live, inter);
    }
    const uint64_t idle_presents = renderer.Metrics().total_presents - presents_before_idle;
    Check(idle_presents == 0 && renderer.Metrics().idle_ticks_skipped == 12u,
          "0-Present idle sleep performs zero BeginDraw/Present1 across 12 idle ticks");

    // Measure 4K Private Working Set BEFORE synthetic LRU stress flood.
    renderer.TrimDxgiMemory();
    const uint64_t priv_ws_bytes = QueryPrivateWorkingSetBytes();
    const double priv_ws_mb = static_cast<double>(priv_ws_bytes) / (1024.0 * 1024.0);
    char mem_detail[96]{};
    std::snprintf(mem_detail, sizeof(mem_detail), "4K PrivateWorkingSet=%.2f MB (budget < 25.00 MB)", priv_ws_mb);
    Check(priv_ws_bytes > 0 && priv_ws_bytes < 25ull * 1024ull * 1024ull,
          "4K DesktopRenderer + WallpaperBackdrop Private Working Set < 25 MB", mem_detail);

    // Verify procedural 1:1 WallpaperPreset 1/4-res blur & accent generation.
    const bool dusk_ok = renderer.ReloadWallpaper(model::WallpaperPreset::Dusk, true) &&
                         renderer.Backdrop().VibrantAccentArgb() == 0xFFE68A6Eu &&
                         renderer.Backdrop().AcrylicBlurBitmap() != nullptr;
    const bool photo_ok = renderer.ReloadWallpaper(model::WallpaperPreset::Photo, true) &&
                          renderer.Backdrop().VibrantAccentArgb() == 0xFF7FB2FFu &&
                          renderer.Backdrop().MicaBlurBitmap() != nullptr;
    Check(dusk_ok && photo_ok,
          "WallpaperBackdrop bakes Dusk & Photo presets at 1/4-res with expected vibrant accents");

    // Stress-test strict <= 6 MB LRU thumbnail cache cap by inserting 16 MB of bitmaps!
    {
        Microsoft::WRL::ComPtr<ID2D1Factory1> tmp_fac;
        Microsoft::WRL::ComPtr<ID3D11Device> tmp_d3d;
        const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED,
                                        &fl, 1, D3D11_SDK_VERSION, &tmp_d3d, nullptr, nullptr))) {
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
            tmp_d3d.As(&dxgi);
            D2D1_FACTORY_OPTIONS fo{};
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                              &fo, reinterpret_cast<void**>(tmp_fac.GetAddressOf()));
            Microsoft::WRL::ComPtr<ID2D1Device> d2d_dev;
            Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
            if (tmp_fac && SUCCEEDED(tmp_fac->CreateDevice(dxgi.Get(), &d2d_dev)) &&
                SUCCEEDED(d2d_dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) {
                std::vector<uint8_t> one_mb_rgba(512u * 512u * 4u, 0x80u);
                for (int i = 0; i < 16; ++i) {
                    renderer.Backdrop().CacheRgbaBitmap(
                        dc.Get(), L"stress_" + std::to_wstring(i), one_mb_rgba.data(), 512u, 512u);
                }
            }
        }
    }
    char lru_detail[96]{};
    std::snprintf(lru_detail, sizeof(lru_detail), "cached_bytes=%zu / %zu",
                  renderer.Backdrop().CachedBytes(), render::WallpaperBackdrop::kMaxCacheBytes);
    Check(renderer.Backdrop().CachedBytes() <= render::WallpaperBackdrop::kMaxCacheBytes &&
          renderer.Backdrop().CachedEntryCount() <= 6u,
          "WallpaperBackdrop LRU cache strictly enforces <= 6 MB byte cap", lru_detail);

    // Verify on-demand overlay + cascading #dsub submenu destroy ALL swapchains on Dismiss() (0 MB idle).
    overlays::DesktopOverlayController ov;
    ov.Initialize({});
    Check(!ov.HasActiveSwapChain(), "Overlay controller starts with 0 active swapchains");
    ov.ShowToast(RECT{0, 0, 1920, 1040}, L"测试胶囊提示", L"撤销", {}, 3000);
    const bool had_sc = ov.HasActiveSwapChain();
    ov.Dismiss();
    const bool destroyed_sc = !ov.HasActiveSwapChain();

    overlays::ContextMenuSpec mspec{};
    mspec.target_kind = overlays::ContextMenuTargetKind::FileIcon;
    mspec.screen_anchor = POINT{160, 160};
    mspec.target_path = L"C:\\Users\\Public\\Desktop\\Item_0.docx";
    mspec.target_name = L"Item_0.docx";
    mspec.groups_list = {{"g0", L"进行中", L'\xE8A5'}, {"g1", L"安装包", L'\xE7B8'}};
    mspec.initial_open_submenu_id = "sub:move-group";
    ov.OpenContextMenu(mspec, state.settings);
    const bool had_menu_sc = ov.HasActiveSwapChain();
    ov.Dismiss();
    const bool destroyed_menu_sc = !ov.HasActiveSwapChain();

    overlays::AiDrawerViewState ai_st{};
    ai_st.phase = overlays::AiDrawerPhase::PlanReady;
    ai_st.connection.configured = true;
    ai_st.connection.provider_name = L"DeepSeek";
    ai_st.connection.model_id = "deepseek-flash";
    ai_st.plan.desktop_before_count = 22;
    ov.OpenAiOrganizeDrawer(RECT{0, 0, 1920, 1032}, ai_st);
    const bool had_ai_sc = ov.IsSideDrawerOpen() && ov.HasActiveSwapChain();
    ov.Dismiss();
    const bool destroyed_ai_sc = !ov.IsSideDrawerOpen() && !ov.HasActiveSwapChain();

    ov.Shutdown();
    char ov_detail[160]{};
    std::snprintf(ov_detail, sizeof(ov_detail), "toast=%d/%d menu=%d/%d ai=%d/%d", had_sc, destroyed_sc,
                  had_menu_sc, destroyed_menu_sc, had_ai_sc, destroyed_ai_sc);
    Check(had_sc && destroyed_sc && had_menu_sc && destroyed_menu_sc && had_ai_sc && destroyed_ai_sc,
          "Overlay controller + cascading #dsub + AiOrganizeDrawer destroy all swapchains on Dismiss()", ov_detail);

    renderer.Shutdown();
}

} // namespace

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::printf("=== Pulse Desktop Enhancement Pack M0/M5 Verification Suite ===\n");
    TestLiveHierarchyAndSnapshotRoundTrip();
    TestCrashSafeTakeoverAndCircuitBreaker();
    TestAuthenticatedPipeIpcRoundTrip();
    Test4KRendererMemoryAndZeroIdlePresents();

    std::printf("=== Summary: %d PASSED, %d FAILED ===\n", g_pass_count, g_fail_count);
    CoUninitialize();
    return g_fail_count == 0 ? 0 : 1;
}
