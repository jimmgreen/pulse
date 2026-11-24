#include "host/render/desktop_renderer.h"
#include "desktop_takeover_record.h"
#include "../../../src/ui/ui_compositor.h"
#include <cstdio>
#include <filesystem>
#include <wrl/client.h>
using namespace pulse::desktop;
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int failures = 0;
    auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; };
    auto pump = [] { MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } };
    const auto card = D2D1::RectF(20, 30, 320, 210);
    for (auto kind : {model::TileKind::Downloads, model::TileKind::Recent}) {
        const auto titled = render::DesktopRenderer::ListRowRect(kind, card, 0, true);
        const auto hidden = render::DesktopRenderer::ListRowRect(kind, card, 0, false);
        check(hidden.top == 42 && titled.top - hidden.top == 34, "hidden header gives 34 DIP back to first row");
        check(hidden.bottom - hidden.top == titled.bottom - titled.top, "row hit box preserves height");
    }
    model::DesktopTile tray{}; tray.size = {2, 2}; tray.look.show_title = false;
    check(render::DesktopRenderer::TrayChipRect(tray, card, 0).top == 42, "tray drawing and hit testing collapse title");
    const std::wstring key = L"Software\\PulseDesktopFeedbackTest_" + std::to_wstring(GetCurrentProcessId());
    HWND mock = CreateWindowExW(0, L"STATIC", L"fixture", WS_POPUP | WS_VISIBLE, -3000,-3000,10,10,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    HKEY probe_key = nullptr;
    const LSTATUS registry_status = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &probe_key, nullptr);
    std::printf("registry fixture status=%ld\n", registry_status);
    if (probe_key) RegCloseKey(probe_key);
    const bool recorded = desktop_takeover::RecordActive(GetCurrentProcessId(), false, L"fixture", key.c_str());
    std::printf("fixture hwnd=%p style=%llx recorded=%d error=%lu\n", mock, static_cast<unsigned long long>(GetWindowLongPtrW(mock, GWL_STYLE)), recorded, GetLastError());
    check(desktop_takeover::HideNativeDesktopListView(mock, key.c_str()), "hide request succeeds on isolated native view");
    pump();
    check(!IsWindowVisible(mock), "actual view becomes hidden");
    auto record = desktop_takeover::ReadState(key.c_str());
    check(record.hid_icons && record.listview_hwnd == reinterpret_cast<uintptr_t>(mock), "persist exact view ownership before hiding");
    check(!desktop_takeover::HideNativeDesktopListView(mock, key.c_str()), "already hidden view is not toggled visible");
    const bool restore_requested = desktop_takeover::RestoreOwnedListView(mock, record);
    pump();
    check(restore_requested && IsWindowVisible(mock), "owned hidden view restores its visible state");
    ShowWindow(mock, SW_HIDE);
    record.hid_icons = false;
    check(!desktop_takeover::RestoreOwnedListView(mock, record) && !IsWindowVisible(mock), "user-hidden view stays hidden when not owned");
    ShowWindow(mock, SW_SHOWNOACTIVATE);
    const bool hide_requested = ShowWindowAsync(mock, SW_HIDE) != FALSE;
    pump();
    check(hide_requested && !IsWindowVisible(mock), "isolated view hides without registry dependency");
    desktop_takeover::TakeoverState owned{};
    owned.hid_icons = true; owned.listview_hwnd = reinterpret_cast<uintptr_t>(mock);
    const bool isolated_restore_requested = desktop_takeover::RestoreOwnedListView(mock, owned);
    pump();
    check(isolated_restore_requested && IsWindowVisible(mock), "owned view visibility restores without registry dependency");
    DestroyWindow(mock);
    desktop_takeover::ClearActive(key.c_str(), true);
    {
        pulse::ui::Compositor::SharedThreadDevices shared{};
        const bool got_shared = pulse::ui::Compositor::AcquireSharedThreadDevices(shared);
        check(got_shared && shared.d3d != nullptr && shared.dcomp != nullptr,
              "shared thread graphics device binds IDCompositionDevice for Win10 secondary surfaces");
        Microsoft::WRL::ComPtr<IDCompositionDevice> comp_only;
        const HRESULT comp_hr = pulse::ui::Compositor::CreateCompositionOnlyDevice(&comp_only);
        check(SUCCEEDED(comp_hr) && comp_only != nullptr,
              "CreateCompositionOnlyDevice succeeds while thread DXGI device already has bound composition");
        comp_only.Reset();
        if (got_shared) {
            pulse::ui::Compositor::ReleaseSharedThreadDevices(shared.d3d);
        }

        HWND live_hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, L"STATIC", L"", WS_POPUP,
            -30000, -30000, 480, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(live_hwnd != nullptr, "create hidden offscreen live composition HWND");
        if (live_hwnd) {
            render::DesktopRenderer live_renderer;
            check(live_renderer.InitializeForHwnd(live_hwnd, 480, 320, true, false),
                  "DesktopRenderer initializes composition surface for live HWND");
            check(live_renderer.UsingComposition(), "DesktopRenderer uses DirectComposition surface");
            live_renderer.SetSurfaceGeometry(96, {0, 0, 480, 320}, {-30000, -30000, 480, 320});
            check(live_renderer.ReloadWallpaper(model::WallpaperPreset::Bloom, true),
                  "reload preset wallpaper for live renderer");

            model::DesktopModelState live_state;
            live_state.settings.organize_enabled = true;
            live_state.settings.tiles_enabled = true;
            live_state.settings.wallpaper_preset = model::WallpaperPreset::Bloom;

            model::DesktopItem g1_item{};
            g1_item.name = L"DocA.txt";
            g1_item.path = L"C:\\fixture\\DocA.txt";
            g1_item.extension = L".txt";
            g1_item.pinned_group_id = "g1";
            live_state.items.push_back(g1_item);

            model::DesktopItem g2_item{};
            g2_item.name = L"DocB.txt";
            g2_item.path = L"C:\\fixture\\DocB.txt";
            g2_item.extension = L".txt";
            g2_item.pinned_group_id = "g2";
            live_state.items.push_back(g2_item);

            model::DesktopItem loose1{};
            loose1.name = L"LooseA.txt";
            loose1.path = L"C:\\fixture\\LooseA.txt";
            loose1.extension = L".txt";
            loose1.force_loose = true;
            loose1.loose_pos = {160, 40};
            live_state.items.push_back(loose1);

            model::DesktopItem loose2{};
            loose2.name = L"LooseB.txt";
            loose2.path = L"C:\\fixture\\LooseB.txt";
            loose2.extension = L".txt";
            loose2.force_loose = true;
            loose2.loose_pos = {360, 200};
            live_state.items.push_back(loose2);

            model::DesktopGroup g1{};
            g1.id = "g1";
            g1.title = L"GroupOne";
            g1.x = 16;
            g1.y = 16;
            live_state.groups.push_back(g1);

            model::DesktopGroup g2{};
            g2.id = "g2";
            g2.title = L"GroupTwo";
            g2.x = 280;
            g2.y = 170;
            live_state.groups.push_back(g2);

            model::GroupComputedView v1{};
            v1.group_id = "g1";
            v1.frame = {16, 16, 150, 140};
            v1.visible_item_indices = {0};
            v1.member_item_indices = {0};
            v1.total_item_count = 1;

            model::GroupComputedView v2{};
            v2.group_id = "g2";
            v2.frame = {280, 170, 460, 300};
            v2.visible_item_indices = {1};
            v2.member_item_indices = {1};
            v2.total_item_count = 1;

            std::vector<model::GroupComputedView> views = {v1, v2};
            render::LiveTileRuntimeData live_data;
            render::DesktopInteractionState interaction;
            check(live_renderer.Tick(live_state, views, {}, live_data, interaction),
                  "live composition Tick renders initial multi-group scene");
            live_renderer.InvalidateRect({20, 20, 120, 120});
            check(live_renderer.Tick(live_state, views, {}, live_data, interaction),
                  "partial dirty-rect Tick succeeds with clip culling");
            live_renderer.Shutdown();
            DestroyWindow(live_hwnd);
        }
    }
    if (argc > 1) {
        render::DesktopRenderer renderer;
        check(renderer.InitializeOffscreen(1000, 520), "offscreen renderer initializes");
        model::DesktopModelState state; state.settings.organize_enabled = true; state.settings.tiles_enabled = true;
        state.settings.wallpaper_preset = model::WallpaperPreset::Bloom;
        render::LiveTileRuntimeData live;
        mount::MiniFolderItem item{}; item.name=L"Example report.txt"; item.path=L"C:\\fixture\\Example report.txt"; item.extension=L".txt";
        live.download_items.push_back(item); live.recent_items.push_back(item);
        for (int i=0;i<6;++i) {
            model::DesktopTile t{}; t.id=std::to_string(i); t.title=i%3==0?L"Downloads":i%3==1?L"Recent":L"Staging";
            t.kind=i%3==0?model::TileKind::Downloads:i%3==1?model::TileKind::Recent:model::TileKind::StagingTray;
            t.size={2,2}; t.free_placed=true; t.x=24+(i%3)*324; t.y=24+(i/3)*240; t.w=300;t.h=208;t.live_content=true;t.look.show_title=i<3;
            state.tiles.push_back(t);
        }
        for (bool dark : {false,true}) {
            state.settings.dark_mode=dark;
            const auto path=std::filesystem::path(argv[1])/(dark?L"titles-dark.png":L"titles-light.png");
            check(renderer.RenderCompositePng(path.wstring(), state, {}, {}, live, {}), "render title shown/hidden comparison PNG");
        }
    }
    std::printf("six-card hover invalidation DIP area: baseline=505920 modified=84320 reduction=83.33%% (geometry, not Win10 timing)\n");
    CoUninitialize();
    return failures?1:0;
}
