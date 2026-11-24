#include "host/render/desktop_renderer.h"
#include "host/render/desktop_appearance.h"
#include <dwmapi.h>
#include <wincodec.h>
#include <filesystem>
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

using namespace pulse::desktop;
using Microsoft::WRL::ComPtr;
namespace {
constexpr int width = 520, height = 360;
COLORREF fixture_background = RGB(0, 0, 0);
bool fixture_split_background = false;
LRESULT CALLBACK FixtureProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        HBRUSH brush = CreateSolidBrush(fixture_background);
        FillRect(dc, &paint.rcPaint, brush);
        DeleteObject(brush);
        if (fixture_split_background) {
            RECT lower{0, height / 2, width, height};
            FillRect(dc, &lower, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
void Pump() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
struct Window {
    HWND hwnd = nullptr;
    ~Window() { if (hwnd) DestroyWindow(hwnd); }
};
bool Capture(HWND hwnd, const std::filesystem::path& path, std::vector<unsigned char>& pixels) {
    POINT origin{};
    if (!ClientToScreen(hwnd, &origin)) return false;
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = bitmap ? SelectObject(memory, bitmap) : nullptr;
    bool ok = bitmap && BitBlt(memory, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT);
    if (ok) {
        GdiFlush();
        pixels.assign(static_cast<unsigned char*>(bits), static_cast<unsigned char*>(bits) + width * height * 4);
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    }
    if (old) SelectObject(memory, old);
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (!ok) return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(width, height))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat32bppBGRA &&
        SUCCEEDED(frame->WritePixels(height, width * 4, static_cast<UINT>(pixels.size()), pixels.data())) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}
double Difference(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b) {
    if (a.size() != width * height * 4 || a.size() != b.size()) return -1;
    // Interior material only, excluding all text, borders, and empty-group hint.
    double total = 0;
    for (int y = 240; y < 280; ++y) for (int x = 70; x < 430; ++x) {
        const size_t p = static_cast<size_t>((y * width + x) * 4);
        for (int c = 0; c < 3; ++c) total += std::abs(int(a[p + c]) - int(b[p + c]));
    }
    return total / (40 * 360 * 3);
}
double PatchBrightness(const std::vector<unsigned char>& pixels, int y0) {
    if (pixels.size() != width * height * 4) return -1;
    double total = 0;
    for (int y = y0; y < y0 + 30; ++y) for (int x = 70; x < 130; ++x) {
        const size_t offset = static_cast<size_t>((y * width + x) * 4);
        for (int c = 0; c < 3; ++c) total += pixels[offset + c];
    }
    return total / (30 * 60 * 3);
}
// Removes each channel's mean so a uniform tint change cannot masquerade as texture.
double SpatialRms(const std::vector<unsigned char>& a, const std::vector<unsigned char>* b = nullptr) {
    if (a.size() != width * height * 4 || (b && b->size() != a.size())) return -1;
    double sum[3]{}, squares[3]{};
    constexpr double samples = 40 * 360;
    for (int y = 240; y < 280; ++y) for (int x = 70; x < 430; ++x) {
        const size_t p = static_cast<size_t>((y * width + x) * 4);
        for (int c = 0; c < 3; ++c) {
            const double value = double(a[p + c]) - (b ? double((*b)[p + c]) : 0.0);
            sum[c] += value;
            squares[c] += value * value;
        }
    }
    double variance = 0;
    for (int c = 0; c < 3; ++c) variance += (std::max)(0.0, squares[c] / samples - std::pow(sum[c] / samples, 2));
    return std::sqrt(variance / 3);
}

}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::printf("usage: pulse_desktop_material_live_test OUTPUT_DIRECTORY\n"); return 2; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 3;
    int failed = 0;
    auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failed += !ok; };
    const auto thin_light = render::CardAcrylicProfile(false, 80);
    const auto thin_dark = render::CardAcrylicProfile(true, 80);
    check(std::abs(thin_light.tint_opacity - 0.186048f) < 0.00001f &&
          std::abs(thin_light.luminosity_opacity - 0.402288f) < 0.00001f,
          "thin light separates tint and luminosity instead of a solid alpha wash");
    check(std::abs(thin_dark.tint_opacity - 0.234192f) < 0.00001f &&
          std::abs(thin_dark.luminosity_opacity - 0.342720f) < 0.00001f,
          "thin dark separates tint and luminosity");
    check(render::CardMicaProfile(true, false).luminosity_opacity < 0.65f &&
          render::CardAcrylicProfile(false, 30).tint_opacity < thin_light.tint_opacity,
          "desktop MicaAlt retains wallpaper contrast; Acrylic slider remains effective");
    {
        std::filesystem::create_directories(argv[1]);
        WNDCLASSW wc{};
        wc.lpfnWndProc = FixtureProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"PulseDesktopMaterialLiveFixture";
        RegisterClassW(&wc);
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int x = (std::max)(work.left, work.right - width - 40);
        const int y = (std::max)(work.top, work.bottom - height - 40);
        std::printf("FIXTURE screen_x=%d screen_y=%d width=%d height=%d\n", x, y, width, height);
        Window backing{CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            wc.lpszClassName, L"Pulse material fixture backing", WS_POPUP, x, y, width, height,
            nullptr, nullptr, wc.hInstance, nullptr)};
        Window window{CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
            wc.lpszClassName, L"Pulse live material fixture", WS_POPUP, x, y, width, height,
            backing.hwnd, nullptr, wc.hInstance, nullptr)};
        render::DesktopRenderer renderer;
        const bool initialized = backing.hwnd && window.hwnd && renderer.InitializeForHwnd(window.hwnd, width, height, true);
        check(initialized, "InitializeForHwnd real hardware window");
        if (initialized) {
            check(renderer.UsingComposition(), "DirectComposition active (no HWND fallback)");
            std::printf("PATH HWND=%p composition=%d virtual_surface=%d capture=screen-BitBlt-CAPTUREBLT\n",
                static_cast<void*>(window.hwnd), renderer.UsingComposition(), renderer.UsingVirtualSurface());
            ComPtr<IDXGIFactory1> factory;
            ComPtr<IDXGIAdapter1> adapter;
            DXGI_ADAPTER_DESC1 desc{};
            const bool adapter_ok = SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
                SUCCEEDED(factory->EnumAdapters1(0, &adapter)) && SUCCEEDED(adapter->GetDesc1(&desc));
            check(adapter_ok && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE), "default hardware adapter available");
            if (adapter_ok) std::printf("DEFAULT_ADAPTER %ls vendor=%04x device=%04x\n", desc.Description, desc.VendorId, desc.DeviceId);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(window.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            renderer.SetSurfaceGeometry(96, {mi.rcMonitor.left, mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top}, {x, y, width, height});
            check(renderer.ReloadWallpaper(model::WallpaperPreset::System, true), "load real system wallpaper");
            check(!renderer.Backdrop().WallpaperPath().empty(), "system wallpaper path resolved");
            std::printf("WALLPAPER %ls\n", renderer.Backdrop().WallpaperPath().c_str());
            ShowWindow(backing.hwnd, SW_SHOWNOACTIVATE);
            UpdateWindow(backing.hwnd);
            ShowWindow(window.hwnd, SW_SHOWNOACTIVATE);
            model::DesktopModelState state;
            state.settings.tiles_enabled = false;
            state.settings.default_material = model::CardMaterial::FollowHost;
            model::DesktopGroup group;
            group.id = "material-fixture";
            group.title = L"Live material fixture";
            group.x = 30; group.y = 30; group.cols = 5; group.visible_rows = 2;
            state.groups.push_back(group);
            model::GroupComputedView view;
            view.group_id = group.id;
            view.frame = {30, 30, 460, 290};
            std::vector<model::GroupComputedView> views{view};
            render::DesktopInteractionState interaction;
            interaction.banner_suppressed = true;
            std::vector<unsigned char> themes[2];
            for (int dark = 0; dark <= 1; ++dark) {
                state.settings.dark_mode = dark != 0;
                interaction.host_dark_mode = dark != 0;
                std::vector<unsigned char> captures[7];
                for (int mode = 0; mode < 7; ++mode) {
                    state.groups[0].material = mode == 0 ? model::CardMaterial::Mica : mode == 1 ? model::CardMaterial::MicaAlt :
                        mode == 2 ? model::CardMaterial::FollowHost : mode == 3 ? model::CardMaterial::Mica :
                        mode == 4 ? model::CardMaterial::Solid : model::CardMaterial::MicaAlt;
                    state.settings.card_opacity_percent = mode == 5 ? 30 : mode == 6 ? 100 : 78;
                    interaction.host_material = model::CardMaterial::Mica;
                    interaction.transparency_enabled = mode != 3;
                    renderer.InvalidateAll();
                    check(renderer.Tick(state, views, {}, {}, interaction), "Tick presented actual fixture");
                    for (int n = 0; n < 12; ++n) { Pump(); Sleep(16); }
                    check(SUCCEEDED(DwmFlush()), "DWM composition completed");
                    const auto path = std::filesystem::path(argv[1]) / (L"live-" + std::to_wstring(dark) + L"-" + std::to_wstring(mode) + L".png");
                    check(Capture(window.hwnd, path, captures[mode]), "screen PNG captured");
                }
                const double alt = Difference(captures[0], captures[1]);
                const double follow = Difference(captures[0], captures[2]);
                const double opaque = Difference(captures[3], captures[4]);
                std::printf("PIXELS dark=%d mica_alt_mean_delta=%.6f follow_mean_delta=%.6f opaque_mean_delta=%.6f\n", dark, alt, follow, opaque);
                check(alt > 0.5, "Mica and MicaAlt visibly distinct");
                check(follow >= 0 && follow < 0.05, "FollowHost matches explicit Mica");
                check(opaque >= 0 && opaque < 0.05, "transparency disabled matches Solid");
                const double opacity = Difference(captures[5], captures[6]);
                std::printf("PIXELS dark=%d mica_alt_opacity30_100_mean_delta=%.6f\n", dark, opacity);
                // Official Mica recipe is opaque; the Acrylic strength slider does not alter it.
                check(opacity >= 0 && opacity < 0.05, "opaque MicaAlt remains independent of Acrylic opacity slider");
                themes[dark] = std::move(captures[0]);
            }
            check(Difference(themes[0], themes[1]) > 20, "light and dark Mica visibly distinct");
            state.settings.dark_mode = false;
            state.settings.card_opacity_percent = 78;
            state.groups[0].material = model::CardMaterial::MicaAlt;
            interaction.host_dark_mode = false;
            interaction.transparency_enabled = true;
            const int center_x = work.left + ((work.right - work.left) - width) / 2;
            const int center_y = work.top + ((work.bottom - work.top) - height) / 2;
            std::vector<unsigned char> positions[3];
            for (int location = 0; location < 3; ++location) {
                const int sx = location == 0 ? x : location == 1 ? work.left + 40 : center_x;
                const int sy = location == 0 ? y : location == 1 ? work.top + 40 : center_y;
                std::printf("WALLPAPER_POSITION index=%d sx=%d sy=%d width=%d height=%d\n", location, sx, sy, width, height);
                SetWindowPos(backing.hwnd, HWND_TOPMOST, sx, sy, width, height, SWP_NOACTIVATE);
                SetWindowPos(window.hwnd, HWND_TOPMOST, sx, sy, width, height, SWP_NOACTIVATE);
                renderer.SetSurfaceGeometry(96, {mi.rcMonitor.left, mi.rcMonitor.top,
                    mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top}, {sx, sy, width, height});
                renderer.InvalidateAll();
                check(renderer.Tick(state, views, {}, {}, interaction), "position-mapped live MicaAlt Tick");
                for (int n = 0; n < 12; ++n) { Pump(); Sleep(16); }
                check(SUCCEEDED(DwmFlush()), "position-mapped DWM flush");
                const auto path = std::filesystem::path(argv[1]) / (L"live-wallpaper-position-" + std::to_wstring(location) + L".png");
                check(Capture(window.hwnd, path, positions[location]), "position-mapped real-screen capture");
            }
            const double local_rms = SpatialRms(positions[0]);
            const double position_delta = Difference(positions[0], positions[1]);
            const double pattern_delta = SpatialRms(positions[0], &positions[1]);
            std::printf("TEXTURE local_rms=%.6f position_mean_delta=%.6f position_centered_rms=%.6f\n",
                local_rms, position_delta, pattern_delta);
            check(local_rms > 0.1, "wallpaper variation visible inside card (not uniform fill)");
            check(position_delta > 0.5 && pattern_delta > 0.1, "screen position changes sampled wallpaper pattern, not merely mean tint");
            const double center_rms = SpatialRms(positions[2]);
            std::printf("TEXTURE_CENTER local_rms=%.6f sx=%d sy=%d wallpaper=%ls\n",
                center_rms, center_x, center_y, renderer.Backdrop().WallpaperPath().c_str());
            // One foreground frame per material. Only the owned, nonactivating backing
            // repaints below it: this distinguishes live host backdrop from wallpaper.
            check(GetWindow(window.hwnd, GW_OWNER) == backing.hwnd, "fixture foreground is owned above nonactivating backing");
            for (int material = 0; material < 3; ++material) {
                state.groups[0].material = material == 0 ? model::CardMaterial::Mica :
                    material == 1 ? model::CardMaterial::MicaAlt : model::CardMaterial::Acrylic;
                renderer.InvalidateAll();
                check(renderer.Tick(state, views, {}, {}, interaction), "single foreground frame before dynamic backing changes");
                const auto presents_before = renderer.Metrics().total_presents;
                std::vector<unsigned char> backgrounds[3];
                for (int background = 0; background < 3; ++background) {
                    fixture_background = background == 0 ? RGB(255, 255, 255) : RGB(0, 0, 0);
                    fixture_split_background = background == 2;
                    // No foreground Tick, invalidation, position or activation operation here.
                    RedrawWindow(backing.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
                    for (int n = 0; n < 12; ++n) { Pump(); Sleep(16); }
                    check(SUCCEEDED(DwmFlush()), "dynamic backing DWM flush");
                    const auto path = std::filesystem::path(argv[1]) / (L"live-backing-" + std::to_wstring(material) + L"-" + std::to_wstring(background) + L".png");
                    check(Capture(window.hwnd, path, backgrounds[background]), "dynamic backing real-screen capture");
                }
                const double backing_delta = Difference(backgrounds[0], backgrounds[1]);
                const double split_delta = Difference(backgrounds[0], backgrounds[2]);
                const double split_top = PatchBrightness(backgrounds[2], 90);
                const double split_bottom = PatchBrightness(backgrounds[2], 250);
                bool backing_changed = false;
                if (backgrounds[0].size() == width * height * 4 && backgrounds[1].size() == backgrounds[0].size()) {
                    constexpr size_t corner = (5 * width + 5) * 4;
                    backing_changed = int(backgrounds[0][corner]) - int(backgrounds[1][corner]) > 200;
                }
                std::printf("DYNAMIC_BACKING material=%d white_black_delta=%.6f white_split_bottom_delta=%.6f split_top=%.6f split_bottom=%.6f backing_changed=%d\n",
                    material, backing_delta, split_delta, split_top, split_bottom, backing_changed);
                check(backing_changed, "owned backing visibly changes white to black outside card");
                check(renderer.Metrics().total_presents == presents_before, "dynamic backing test performs zero foreground presents");
                if (material < 2) {
                    check(backing_delta >= 0 && backing_delta < 0.05 && split_delta >= 0 && split_delta < 0.05,
                        "opaque Mica ignores live white/black/split behind-window changes");
                } else {
                    check(backing_delta > 20, "Acrylic follows live behind-window change without foreground redraw");
                    check(split_top >= 0 && split_bottom - split_top > 20,
                        "Acrylic spatial source is correct: lower white half is brighter than upper black half");
                    check(split_delta >= 0 && split_delta < 10,
                        "Acrylic lower-half sample agrees with white backing rather than stale black backing");
                    check(split_bottom > 240,
                        "Acrylic white backing is not darkened by filled foreground shadow rectangles");
                }
            }
            fixture_split_background = false;
            fixture_background = RGB(0, 0, 0);
            RedrawWindow(backing.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
            check(renderer.NativeAcrylicActive() && renderer.NativeAcrylicVisualCount() == 1,
                "Acrylic owns exactly one native backdrop visual");
            const auto native_updates = renderer.NativeAcrylicUpdateCount();
            int forced_frames = 0;
            for (int n = 0; n < 100; ++n) {
                renderer.InvalidateAll();
                forced_frames += renderer.Tick(state, views, {}, {}, interaction) ? 1 : 0;
            }
            check(forced_frames == 100 && renderer.NativeAcrylicUpdateCount() == native_updates,
                "100 unchanged-geometry foreground redraws do not rebuild native Acrylic visuals");
            const auto unchanged_updates_after = renderer.NativeAcrylicUpdateCount();
            state.groups[0].material = model::CardMaterial::Solid;
            renderer.InvalidateAll();
            check(renderer.Tick(state, views, {}, {}, interaction) && renderer.NativeAcrylicVisualCount() == 0,
                "switching Acrylic to Solid removes native backdrop visual");
            state.groups[0].material = model::CardMaterial::Acrylic;
            renderer.InvalidateAll();
            check(renderer.Tick(state, views, {}, {}, interaction) && renderer.NativeAcrylicVisualCount() == 1,
                "switching Solid back to Acrylic restores one native visual");
            interaction.transparency_enabled = false;
            renderer.InvalidateAll();
            check(renderer.Tick(state, views, {}, {}, interaction) && renderer.NativeAcrylicVisualCount() == 0,
                "disabling transparency removes native Acrylic visual");
            interaction.transparency_enabled = true;
            renderer.InvalidateAll();
            check(renderer.Tick(state, views, {}, {}, interaction) && renderer.NativeAcrylicVisualCount() == 1,
                "restoring transparency restores one native Acrylic visual");
            const auto saved_groups = state.groups;
            const auto saved_views = views;
            state.groups.clear();
            views.clear();
            renderer.InvalidateAll();
            check(renderer.Tick(state, views, {}, {}, interaction) && renderer.NativeAcrylicVisualCount() == 0,
                "removing every group removes every native Acrylic visual");
            for (int n = 0; n < 12; ++n) { Pump(); Sleep(16); }
            check(SUCCEEDED(DwmFlush()), "removed group DWM flush");
            std::vector<unsigned char> removed;
            check(Capture(window.hwnd, std::filesystem::path(argv[1]) / L"live-acrylic-removed.png", removed),
                "removed Acrylic real-screen capture");
            const double removed_top = PatchBrightness(removed, 90);
            const double removed_bottom = PatchBrightness(removed, 250);
            check(removed_top >= 0 && removed_top < 1 && removed_bottom >= 0 && removed_bottom < 1,
                "removed card exposes black backing without orphan Acrylic regions");
            std::printf("ACRYLIC_LIFECYCLE unchanged_geometry_updates=%llu->%llu removed_top=%.6f removed_bottom=%.6f\n",
                static_cast<unsigned long long>(native_updates), static_cast<unsigned long long>(unchanged_updates_after),
                removed_top, removed_bottom);
            state.groups = saved_groups;
            views = saved_views;
            renderer.InvalidateAll();
            check(renderer.Tick(state, views, {}, {}, interaction) && renderer.NativeAcrylicVisualCount() == 1,
                "restoring group restores exactly one native Acrylic visual");
            // Warm up before resource accounting; budgets permit lazy runtime allocation,
            // not persistent per-tick growth. These are process metrics, not VRAM claims.
            for (int n = 0; n < 32; ++n) renderer.Tick(state, views, {}, {}, interaction);
            PROCESS_MEMORY_COUNTERS_EX before{}, after{};
            before.cb = sizeof(before); after.cb = sizeof(after);
            DWORD handles_before = 0, handles_after = 0;
            const bool before_ok = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof(before)) &&
                GetProcessHandleCount(GetCurrentProcess(), &handles_before);
            const auto metrics_before = renderer.Metrics();
            const auto material_before = renderer.Backdrop().MaterialStats();
            auto* const mica_bitmap = renderer.Backdrop().MicaBlurBitmap();
            auto* const mica_brush = renderer.Backdrop().MicaBrush();
            FILETIME created{}, exited{}, kernel_before{}, user_before{}, kernel_after{}, user_after{};
            LARGE_INTEGER qpc_before{}, qpc_after{}, qpc_frequency{};
            const bool cpu_before_ok = GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel_before, &user_before) != FALSE;
            const bool frequency_ok = QueryPerformanceFrequency(&qpc_frequency) != FALSE;
            QueryPerformanceCounter(&qpc_before);
            int idle_draws = 0;
            for (int n = 0; n < 1000; ++n) idle_draws += renderer.Tick(state, views, {}, {}, interaction) ? 1 : 0;
            QueryPerformanceCounter(&qpc_after);
            const bool cpu_after_ok = GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel_after, &user_after) != FALSE;
            auto ticks = [](FILETIME ft) { return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; };
            check(cpu_before_ok && cpu_after_ok && frequency_ok, "idle CPU and elapsed measurements available");
            if (cpu_before_ok && cpu_after_ok && frequency_ok) {
                const double cpu_ms = static_cast<double>(ticks(kernel_after) + ticks(user_after) - ticks(kernel_before) - ticks(user_before)) / 10000.0;
                const double elapsed_ms = 1000.0 * static_cast<double>(qpc_after.QuadPart - qpc_before.QuadPart) / static_cast<double>(qpc_frequency.QuadPart);
                std::printf("IDLE_CPU ticks=1000 process_kernel_user_ms=%.6f elapsed_ms=%.6f\n", cpu_ms, elapsed_ms);
            }
            const auto metrics_after = renderer.Metrics();
            const auto material_after = renderer.Backdrop().MaterialStats();
            const bool after_ok = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after), sizeof(after)) &&
                GetProcessHandleCount(GetCurrentProcess(), &handles_after);
            check(before_ok && after_ok, "process memory and handle measurements available");
            check(idle_draws == 0 && metrics_after.total_presents == metrics_before.total_presents &&
                metrics_after.idle_ticks_skipped - metrics_before.idle_ticks_skipped == 1000, "1000 idle Tick calls perform zero presents");
            check(mica_bitmap == renderer.Backdrop().MicaBlurBitmap() && mica_brush == renderer.Backdrop().MicaBrush(),
                "idle Tick retains wallpaper bitmap and brush identities");
            check(material_after.card_builds == material_before.card_builds &&
                material_after.menu_builds == material_before.menu_builds &&
                material_after.menu_uploads == material_before.menu_uploads,
                "1000 idle Tick calls rebuild/upload zero material resources");
            std::printf("IDLE_MATERIAL card_builds=%llu->%llu menu_builds=%llu->%llu menu_uploads=%llu->%llu cpu_wallpaper_bytes=%zu\n",
                static_cast<unsigned long long>(material_before.card_builds), static_cast<unsigned long long>(material_after.card_builds),
                static_cast<unsigned long long>(material_before.menu_builds), static_cast<unsigned long long>(material_after.menu_builds),
                static_cast<unsigned long long>(material_before.menu_uploads), static_cast<unsigned long long>(material_after.menu_uploads),
                material_after.cpu_wallpaper_bytes);
            check(material_after.cpu_wallpaper_bytes <= 4u * 1024u * 1024u &&
                material_after.card_bitmap_bytes <= 16u * 1024u * 1024u &&
                material_after.menu_bitmap_bytes <= 8u * 1024u * 1024u,
                "explicit backing budgets: CPU<=4MiB cards<=16MiB menu<=8MiB (not driver total VRAM)");
            std::printf("MATERIAL_BYTES cpu_wallpaper=%zu card_bitmap=%zu menu_bitmap=%zu accounting=explicit_buffers_not_driver_total_VRAM\n",
                material_after.cpu_wallpaper_bytes, material_after.card_bitmap_bytes, material_after.menu_bitmap_bytes);
            constexpr SIZE_T memory_tolerance = 8u * 1024u * 1024u;
            check(before_ok && after_ok && after.PrivateUsage <= before.PrivateUsage + memory_tolerance &&
                after.WorkingSetSize <= before.WorkingSetSize + memory_tolerance && handles_after <= handles_before + 8,
                "idle process private/working-set growth <=8MiB and handle growth <=8");
            std::printf("IDLE private_before=%zu private_after=%zu working_set_before=%zu working_set_after=%zu handles_before=%lu handles_after=%lu presents_delta=%llu\n",
                before.PrivateUsage, after.PrivateUsage, before.WorkingSetSize, after.WorkingSetSize, handles_before, handles_after,
                static_cast<unsigned long long>(metrics_after.total_presents - metrics_before.total_presents));
            SetWindowPos(backing.hwnd, HWND_TOPMOST, center_x, center_y, width, height, SWP_NOACTIVATE);
            SetWindowPos(window.hwnd, HWND_TOPMOST, center_x, center_y, width, height, SWP_NOACTIVATE);
            renderer.Shutdown();
            // Virtual desktop surfaces intentionally omit the wallpaper plane. Use the
            // real HWND swapchain path for this supplementary mapping screenshot.
            const bool composite_ready = renderer.InitializeForHwnd(window.hwnd, width, height, false);
            check(composite_ready, "supplementary real HWND wallpaper composite initialized");
            if (composite_ready) {
                renderer.SetSurfaceGeometry(96, {mi.rcMonitor.left, mi.rcMonitor.top,
                    mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top}, {center_x, center_y, width, height});
                std::printf("COMPOSITE_POSITION sx=%d sy=%d\n", center_x, center_y);
                check(renderer.ReloadWallpaper(model::WallpaperPreset::System, false), "composite loads same system wallpaper");
                interaction.force_opaque_wallpaper = true;
                interaction.transparency_enabled = true;
                state.settings.card_opacity_percent = 78;
                state.groups[0].material = model::CardMaterial::MicaAlt;
                for (int dark = 0; dark <= 1; ++dark) {
                    state.settings.dark_mode = dark != 0;
                    interaction.host_dark_mode = dark != 0;
                    renderer.InvalidateAll();
                    check(renderer.Tick(state, views, {}, {}, interaction), "supplementary wallpaper composite Tick");
                    for (int n = 0; n < 12; ++n) { Pump(); Sleep(16); }
                    check(SUCCEEDED(DwmFlush()), "supplementary composite DWM flush");
                    std::vector<unsigned char> composite;
                    const auto path = std::filesystem::path(argv[1]) / (L"live-wallpaper-composite-" + std::to_wstring(dark) + L".png");
                    check(Capture(window.hwnd, path, composite), "supplementary actual-screen wallpaper composite PNG");
                }
                std::printf("COMPOSITE_PATH real_HWND_swapchain force_opaque_wallpaper=1 capture=screen-BitBlt\n");
                renderer.Shutdown();
            }
        }
    }
    CoUninitialize();
    std::printf("RESULT failed=%d windows destroyed; no desktop settings changed\n", failed);
    return failed ? 1 : 0;
}


