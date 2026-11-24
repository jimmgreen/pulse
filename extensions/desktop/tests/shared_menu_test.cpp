#include "host/overlays/desktop_overlays.h"
#include <filesystem>
#include <cstdio>
#include <dwmapi.h>
#include <fstream>
#include <psapi.h>
#include <tlhelp32.h>
#include <map>
#include <vector>
#include "host/render/wallpaper_backdrop.h"
#pragma comment(lib, "psapi.lib")

namespace {
std::filesystem::path output_dir;
bool expect_material = false;
bool expect_native_acrylic = true;
uint64_t native_upload_start = 0;
bool background_dark = false;
int contrast_stage = 0;
bool contrast_submenu = false;
COLORREF light_backdrop_sample = 0;
int material_failures = 0;
int live_index = 0;
int submenu_stage = 0;
bool capture_enabled = true;
bool deactivate_test = false;
HWND competing_window = nullptr;
pulse::desktop::render::WallpaperBackdrop* test_backdrop = nullptr;
struct Usage { SIZE_T private_bytes; DWORD handles, gdi, user, threads; };
Usage ReadUsage() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    DWORD handles = 0, threads = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot, &entry)) do {
            if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++threads;
        } while (Thread32Next(snapshot, &entry));
        CloseHandle(snapshot);
    }
    return {memory.PrivateUsage, handles, GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
        GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS), threads};
}
Usage ReadSettledUsage(int cycle) {
    Usage previous = ReadUsage();
    int stable = 0;
    for (int attempt = 0; attempt < 20; ++attempt) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        DwmFlush();
        Sleep(50);
        const Usage current = ReadUsage();
        if (current.handles == previous.handles && current.gdi == previous.gdi &&
            current.user == previous.user && current.threads == previous.threads) ++stable;
        else stable = 0;
        previous = current;
        if (stable >= 2) {
            std::printf("[INFO] cycle=%d sample_settled_ms=%d\n", cycle, (attempt + 1) * 50);
            return current;
        }
    }
    std::printf("[INFO] cycle=%d sample_not_settled_after_ms=1000\n", cycle);
    return previous;
}
void PrintHandleTypes(int cycle) {
    using QuerySystem = LONG (WINAPI*)(ULONG, void*, ULONG, ULONG*);
    const auto query = reinterpret_cast<QuerySystem>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!query) return;
    struct Entry {
        void* object;
        ULONG_PTR pid, handle;
        ULONG access;
        USHORT backtrace, type;
        ULONG attributes, reserved;
    };
    struct Table { ULONG_PTR count, reserved; Entry entries[1]; };
    constexpr ULONG capacity = 32u * 1024u * 1024u;
    void* buffer = VirtualAlloc(nullptr, capacity, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buffer) return;
    ULONG needed = 0;
    const LONG status = query(64, buffer, capacity, &needed);
    if (status < 0) {
        std::printf("[INFO] cycle=%d handle_types_status=0x%08lX\n", cycle, static_cast<unsigned long>(status));
        VirtualFree(buffer, 0, MEM_RELEASE);
        return;
    }
    const auto* table = reinterpret_cast<const Table*>(buffer);
    std::map<USHORT, size_t> types;
    for (ULONG_PTR i = 0; i < table->count; ++i)
        if (table->entries[i].pid == GetCurrentProcessId()) ++types[table->entries[i].type];
    VirtualFree(buffer, 0, MEM_RELEASE);
    std::printf("[INFO] cycle=%d own_handle_type_counts", cycle);
    for (const auto& [type, count] : types) std::printf(" %u:%zu", type, count);
    std::printf("\n");
}
HWND OwnWindow(const wchar_t* title, bool visible_only = true) {
    struct Search { const wchar_t* title; HWND result; bool visible_only; } search{title, nullptr, visible_only};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM parameter) -> BOOL {
        auto* value = reinterpret_cast<Search*>(parameter);
        wchar_t text[80]{};
        GetWindowTextW(hwnd, text, 80);
        if (wcscmp(text, value->title) == 0 && (!value->visible_only || IsWindowVisible(hwnd))) {
            value->result = hwnd; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

bool SnapshotHasContent(const std::filesystem::path& path, bool dark) {
    pulse::ui::ComPtr<IWICImagingFactory> factory;
    pulse::ui::ComPtr<IWICBitmapDecoder> decoder;
    pulse::ui::ComPtr<IWICBitmapFrameDecode> frame;
    pulse::ui::ComPtr<IWICFormatConverter> converter;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    UINT width = 0, height = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || width < 80 || height < 80 || width > 8192 || height > 8192) return false;
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) hr = converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
    if (SUCCEEDED(hr)) hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(hr)) return false;
    size_t opaque = 0, bright = 0, dim = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (pixels[i + 3] < 240) continue;
        ++opaque;
        const unsigned luminance = (static_cast<unsigned>(pixels[i]) + pixels[i + 1] + pixels[i + 2]) / 3;
        bright += luminance > 180;
        dim += luminance < 120;
    }
    const size_t area = static_cast<size_t>(width) * height;
    const bool valid = opaque > area / 3 && bright > 100 && dim > 100 &&
        (dark ? dim > opaque / 2 : bright > opaque / 2);
    std::printf("[%s] decoded menu PNG nonempty %ux%u opaque=%zu bright=%zu dim=%zu dark=%d\n",
        valid ? "PASS" : "FAIL", width, height, opaque, bright, dim, dark);
    return valid;
}

COLORREF SampleMenuBody(HWND menu) {
    RECT r{};
    GetWindowRect(menu, &r);
    const int margin = (GetWindowLongPtrW(menu, GWL_EXSTYLE) & WS_EX_LAYERED) ? 20 : 0;
    const int x = r.right - margin - 12, y = (r.top + r.bottom) / 2;
    HDC screen = GetDC(nullptr);
    unsigned red = 0, green = 0, blue = 0;
    for (int dy = -3; dy <= 3; ++dy) for (int dx = -3; dx <= 3; ++dx) {
        const COLORREF color = GetPixel(screen, x + dx, y + dy);
        red += GetRValue(color); green += GetGValue(color); blue += GetBValue(color);
    }
    ReleaseDC(nullptr, screen);
    return RGB(red / 49, green / 49, blue / 49);
}
void CaptureMenu(HWND menu) {
    const bool no_material_window = OwnWindow(L"PulseMenuMaterial") == nullptr;
    const bool sampled = test_backdrop && test_backdrop->MenuCacheBytes() != 0;
    BOOL host_backdrop = FALSE;
    const HRESULT host_query = DwmGetWindowAttribute(menu, DWMWA_USE_HOSTBACKDROPBRUSH, &host_backdrop, sizeof(host_backdrop));
    const bool layered = (GetWindowLongPtrW(menu, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
    const bool no_redirection = (GetWindowLongPtrW(menu, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) != 0;
    if (expect_native_acrylic) {
        std::printf("[INFO] native host_attr_query=0x%08X value=%d layered=%d no_redirection=%d uploads=%llu->%llu cache=%zu\n",
            static_cast<unsigned>(host_query), host_backdrop, layered, no_redirection,
            static_cast<unsigned long long>(native_upload_start),
            static_cast<unsigned long long>(test_backdrop ? test_backdrop->MenuUploadCount() : 0),
            test_backdrop ? test_backdrop->MenuCacheBytes() : 0);
    }
    // USE_HOSTBACKDROPBRUSH is documented as a setter. A failed getter is not
    // evidence of a disabled brush; the stationary live-pixel test verifies it.
    const bool host_query_consistent = FAILED(host_query) || host_backdrop;
    const bool valid = no_material_window && (expect_native_acrylic
        ? !layered && no_redirection && host_query_consistent && test_backdrop &&
            test_backdrop->MenuUploadCount() == native_upload_start && test_backdrop->MenuCacheBytes() == 96u * 96u * 4u
        : expect_material == sampled);
    const bool ordinary = (GetWindowLongPtrW(menu, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0;
    std::printf("[%s] menu wallpaper_expected=%d cache_present=%d separate_windows=%d topmost=%d\n",
        valid && ordinary ? "PASS" : "FAIL", expect_material, sampled, !no_material_window, !ordinary);
    material_failures += !(valid && ordinary);
    RECT r{};
    GetWindowRect(menu, &r);
    const POINT hit{r.right - 32, r.top + 50};
    const bool hit_ok = WindowFromPoint(hit) == menu;
    std::printf("[%s] layered menu transparent-area hit testing\n", hit_ok ? "PASS" : "FAIL");
    material_failures += !hit_ok;
    const int w = r.right-r.left, h = r.bottom-r.top;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto old = SelectObject(dc, bitmap);
    DwmFlush();
    BitBlt(dc, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    header.bfSize = header.bfOffBits + w*h*4;
    std::ofstream out(output_dir / (L"live-menu-" + std::to_wstring(live_index++) + L".bmp"), std::ios::binary);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    out.write(static_cast<const char*>(bits), w*h*4);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

pulse::desktop::overlays::DesktopOverlayController* async_owner = nullptr;
void CALLBACK ChooseFirst(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND menu = OwnWindow(L"PulseMenu");
    if (!menu || !IsWindowVisible(menu)) return;
    if (contrast_stage == 1 && contrast_submenu) {
        PostMessageW(menu, WM_KEYDOWN, VK_HOME, 0);
        for (int i = 0; i < 3; ++i) PostMessageW(menu, WM_KEYDOWN, VK_DOWN, 0);
        PostMessageW(menu, WM_KEYDOWN, VK_RIGHT, 0);
        contrast_stage = 3;
        return;
    }
    if (contrast_stage == 1 || contrast_stage == 3) {
        HWND sampled_window = contrast_submenu ? OwnWindow(L"PulseSubMenu") : menu;
        if (!sampled_window || !IsWindowVisible(sampled_window)) {
            ++material_failures;
            contrast_stage = 0;
            KillTimer(nullptr, timer);
            PostMessageW(menu, WM_KEYDOWN, VK_ESCAPE, 0);
            return;
        }
        DwmFlush();
        light_backdrop_sample = SampleMenuBody(sampled_window);
        CaptureMenu(sampled_window);
        background_dark = true;
        InvalidateRect(competing_window, nullptr, FALSE);
        UpdateWindow(competing_window);
        contrast_stage = 2;
        return;
    }
    if (contrast_stage == 2) {
        KillTimer(nullptr, timer);
        DwmFlush();
        HWND sampled_window = contrast_submenu ? OwnWindow(L"PulseSubMenu") : menu;
        const COLORREF dark_sample = SampleMenuBody(sampled_window);
        CaptureMenu(sampled_window);
        const int delta = abs(static_cast<int>(GetRValue(dark_sample)) - GetRValue(light_backdrop_sample)) +
            abs(static_cast<int>(GetGValue(dark_sample)) - GetGValue(light_backdrop_sample)) +
            abs(static_cast<int>(GetBValue(dark_sample)) - GetBValue(light_backdrop_sample));
        const bool reacts = expect_native_acrylic ? delta > 20 : delta <= 3;
        std::printf("[%s] stationary %s white-to-dark underlying-window RGB=%u,%u,%u->%u,%u,%u delta=%d\n",
            reacts ? "PASS" : "FAIL", expect_native_acrylic ?
                (contrast_submenu ? "native Acrylic submenu 150%" : "native Acrylic") : "wallpaper MicaAlt",
            GetRValue(light_backdrop_sample), GetGValue(light_backdrop_sample), GetBValue(light_backdrop_sample),
            GetRValue(dark_sample), GetGValue(dark_sample), GetBValue(dark_sample), delta);
        material_failures += !reacts;
        contrast_stage = 0;
        PostMessageW(menu, WM_KEYDOWN, VK_ESCAPE, 0);
        if (contrast_submenu) PostMessageW(menu, WM_KEYDOWN, VK_ESCAPE, 0);
        return;
    }
    if (submenu_stage == 1) {
        PostMessageW(menu, WM_KEYDOWN, VK_HOME, 0);
        for (int i = 0; i < 3; ++i) PostMessageW(menu, WM_KEYDOWN, VK_DOWN, 0);
        PostMessageW(menu, WM_KEYDOWN, VK_RIGHT, 0);
        submenu_stage = 2;
        return;
    }
    KillTimer(nullptr, timer);
    if (submenu_stage == 2) {
        HWND sub = OwnWindow(L"PulseSubMenu");
        const bool sub_visible = sub && IsWindowVisible(sub);
        std::printf("[%s] native cascading submenu visible\n", sub_visible ? "PASS" : "FAIL");
        material_failures += !sub_visible;
        if (sub_visible) CaptureMenu(sub);
        PostMessageW(menu, WM_KEYDOWN, VK_ESCAPE, 0);
        PostMessageW(menu, WM_KEYDOWN, VK_ESCAPE, 0);
        submenu_stage = 0;
        return;
    }
    if (deactivate_test) {
        deactivate_test = false;
        ShowWindow(competing_window, SW_SHOW);
        SetWindowPos(competing_window, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(competing_window);
        SetFocus(competing_window);
        return;
    }
    if (capture_enabled) CaptureMenu(menu);
    if (async_owner) {
        pulse::desktop::overlays::ShellVerbEntry shell;
        shell.item_id = 91;
        shell.label = L"Fixture Shell action";
        shell.verb = L"fixture";
        async_owner->SetAsyncShellVerbs(73, {shell});
        async_owner = nullptr;
        PostMessageW(menu, WM_KEYDOWN, VK_UP, 0);
    } else PostMessageW(menu, WM_KEYDOWN, VK_HOME, 0);
    PostMessageW(menu, WM_KEYDOWN, VK_RETURN, 0);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    struct ApartmentGuard { ~ApartmentGuard() { CoUninitialize(); } } apartment_guard;
    std::filesystem::create_directories(argv[1]);
    output_dir = argv[1];
    int failed = 0;
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
            if (message == WM_PAINT) {
                PAINTSTRUCT paint{};
                HDC dc = BeginPaint(hwnd, &paint);
                RECT r{}; GetClientRect(hwnd, &r);
                HBRUSH brush = CreateSolidBrush(background_dark ? RGB(16, 16, 16) : RGB(255, 255, 255));
                FillRect(dc, &r, brush);
                DeleteObject(brush);
                EndPaint(hwnd, &paint);
                return 0;
            }
            return DefWindowProcW(hwnd, message, wparam, lparam);
        };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PulseMenuBackdropFixture";
        RegisterClassW(&wc);
        HWND source_window = CreateWindowExW(0, wc.lpszClassName, L"Wallpaper source device",
            WS_OVERLAPPEDWINDOW, 80, 80, 400, 300, nullptr, nullptr, wc.hInstance, nullptr);
        competing_window = CreateWindowExW(0, wc.lpszClassName, L"Competing foreground fixture",
            WS_OVERLAPPEDWINDOW, 200, 200, 420, 320, nullptr, nullptr, wc.hInstance, nullptr);
        pulse::ui::Compositor source_compositor;
        if (!source_compositor.Init(source_window)) return 4;
        pulse::desktop::render::WallpaperBackdrop wallpaper;
        if (!wallpaper.LoadForScreen(source_compositor.Dc(), GetSystemMetrics(SM_CXSCREEN),
            GetSystemMetrics(SM_CYSCREEN), pulse::desktop::model::WallpaperPreset::System, false)) return 5;
        test_backdrop = &wallpaper;
        pulse::desktop::overlays::DesktopOverlayController overlays;
        overlays.SetBackdropSource(&wallpaper, source_compositor.Dc());
        std::string invoked;
        pulse::desktop::overlays::OverlayCallbacks callbacks;
        bool shell_invoked = false;
        int sessions_closed = 0;
        callbacks.on_invoke_shell_verb = [&](uint64_t sid, uint32_t item, const std::wstring& verb) {
            shell_invoked = sid == 73 && item == 91 && verb == L"fixture";
        };
        callbacks.on_close_shell_session = [&](uint64_t) { ++sessions_closed; };
        callbacks.on_menu_command = [&](const std::string& id, const auto&) { invoked = id; };
        if (!overlays.Initialize(std::move(callbacks))) return 3;
        pulse::desktop::model::BeautifySettings settings;
        wchar_t latency[4]{};
        GetEnvironmentVariableW(L"PULSE_MENU_LATENCY", latency, ARRAYSIZE(latency));
        if (latency[0] == L'1') {
            capture_enabled = false;
            pulse::desktop::overlays::ContextMenuSpec spec;
            spec.target_kind = pulse::desktop::overlays::ContextMenuTargetKind::GroupHeader;
            spec.screen_anchor = {GetSystemMetrics(SM_CXSCREEN) / 3, GetSystemMetrics(SM_CYSCREEN) / 4};
            for (int native = 0; native < 2; ++native) {
                settings.default_material = native ? pulse::desktop::model::CardMaterial::Acrylic : pulse::desktop::model::CardMaterial::MicaAlt;
                overlays.TrimIdleResources();
                Usage warm{}, last{};
                for (int cycle = 0; cycle < 40; ++cycle) {
                    std::printf("[CYCLE] material=%d cycle=%d\n", native, cycle);
                    submenu_stage = 1;
                    if (cycle == 39) {
                        overlays.TrimIdleResources();
                        EmptyWorkingSet(GetCurrentProcess());
                        std::printf("[COLD_TRIM] explicit device and working set trim\n");
                    }
                    LARGE_INTEGER request{};
                    QueryPerformanceCounter(&request);
                    SetEnvironmentVariableW(L"PULSE_MENU_REQUEST_QPC", std::to_wstring(request.QuadPart).c_str());
                    overlays.OpenContextMenu(spec, settings);
                    const auto timer = SetTimer(nullptr, 0, 100, ChooseFirst);
                    MSG message{};
                    while (overlays.IsOpen() && GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
                    KillTimer(nullptr, timer);
                    if (cycle == 7) warm = ReadSettledUsage(cycle);
                    if (cycle == 38) last = ReadSettledUsage(cycle);
                }
                ReadSettledUsage(40);
                const bool bounded = last.private_bytes <= warm.private_bytes + 16u * 1024u * 1024u &&
                    last.handles <= warm.handles + 4 && last.gdi <= warm.gdi + 2 && last.user <= warm.user + 2;
                std::printf("[%s] latency resource bounds private=%zu->%zu handles=%lu->%lu GDI=%lu->%lu USER=%lu->%lu\n",
                    bounded ? "PASS" : "FAIL", warm.private_bytes, last.private_bytes, warm.handles, last.handles,
                    warm.gdi, last.gdi, warm.user, last.user);
                failed += !bounded;
            }
            overlays.Shutdown();
            wallpaper.ReleaseDeviceResources();
            source_compositor.Shutdown();
            DestroyWindow(competing_window);
            DestroyWindow(source_window);
            test_backdrop = nullptr;
            return failed || material_failures ? 1 : 0;
        }
        settings.default_material = pulse::desktop::model::CardMaterial::MicaAlt;
        for (int dpi : {100, 150}) for (int dark : {0, 1}) for (int kind = 0; kind < 4; ++kind) {
            pulse::desktop::overlays::ContextMenuSpec spec;
            spec.target_kind = static_cast<pulse::desktop::overlays::ContextMenuTargetKind>(kind);
            spec.target_name = L"Desktop menu fixture";
            spec.tag_palette = {{L"red", L"Red", 0xFFE5484Du}, {L"blue", L"Blue", 0xFF3E8EF7u}};
            overlays.SetDipScale(static_cast<float>(dpi) / 100.0f);
            const auto path = std::filesystem::path(argv[1]) / (L"menu-" + std::to_wstring(kind) + L"-" +
                std::to_wstring(dark) + L"-" + std::to_wstring(dpi) + L".png");
            const bool ok = overlays.SaveContextMenuSnapshot(path.c_str(), spec, settings, dark != 0);
            std::printf("[%s] shared FluentMenu kind=%d dark=%d dpi=%d\n", ok ? "PASS" : "FAIL", kind, dark, dpi);
            failed += !ok;
            failed += ok && !SnapshotHasContent(path, dark != 0);
        }
        wchar_t snapshots_only[4]{};
        GetEnvironmentVariableW(L"PULSE_MENU_SNAPSHOT_ONLY", snapshots_only, ARRAYSIZE(snapshots_only));
        if (snapshots_only[0] == L'1') {
            overlays.Shutdown();
            wallpaper.ReleaseDeviceResources();
            source_compositor.Shutdown();
            DestroyWindow(competing_window);
            DestroyWindow(source_window);
            test_backdrop = nullptr;
            std::printf("[INFO] snapshot-only: live material and resource tests not run\n");
            return failed ? 1 : 0;
        }
        pulse::desktop::overlays::ContextMenuSpec group;
        group.target_kind = pulse::desktop::overlays::ContextMenuTargetKind::GroupHeader;
        group.screen_anchor = {GetSystemMetrics(SM_CXSCREEN) / 3, GetSystemMetrics(SM_CYSCREEN) / 4};
        overlays.SetDipScale(1.0f);
        overlays.SetAppearance(false, 0xFF0078D4u);
        overlays.OpenContextMenu(group, settings);
        const auto timer = SetTimer(nullptr, 0, 500, ChooseFirst);
        MSG tracking{};
        while (overlays.IsOpen() && GetMessageW(&tracking, nullptr, 0, 0) > 0) DispatchMessageW(&tracking);
        KillTimer(nullptr, timer);
        const bool dispatched = invoked == "rename-group";
        std::printf("[%s] actual FluentMenu keyboard command dispatch\n", dispatched ? "PASS" : "FAIL");
        failed += !dispatched;
        overlays.SetAppearance(true, 0xFF0078D4u);
        overlays.OpenContextMenu(group, settings);
        async_owner = &overlays;
        const auto async_timer = SetTimer(nullptr, 0, 500, ChooseFirst);
        while (overlays.IsOpen() && GetMessageW(&tracking, nullptr, 0, 0) > 0) DispatchMessageW(&tracking);
        KillTimer(nullptr, async_timer);
        const bool async_ok = shell_invoked && sessions_closed == 0;
        std::printf("[%s] async Shell row patch preserves session and command identity\n", async_ok ? "PASS" : "FAIL");
        failed += !async_ok;
        submenu_stage = 1;
        overlays.OpenContextMenu(group, settings);
        const auto submenu_timer = SetTimer(nullptr, 0, 500, ChooseFirst);
        while (overlays.IsOpen() && GetMessageW(&tracking, nullptr, 0, 0) > 0) DispatchMessageW(&tracking);
        KillTimer(nullptr, submenu_timer);
        const bool submenu_cleaned = OwnWindow(L"PulseMenuMaterial") == nullptr;
        std::printf("[%s] cascading material surfaces destroyed\n", submenu_cleaned ? "PASS" : "FAIL");
        failed += !submenu_cleaned;
        overlays.SetAppearance(false, 0xFF0078D4u);
        expect_material = false;
        expect_native_acrylic = false;
        settings.default_material = pulse::desktop::model::CardMaterial::Solid;
        overlays.OpenContextMenu(group, settings);
        const auto solid_timer = SetTimer(nullptr, 0, 500, ChooseFirst);
        while (overlays.IsOpen() && GetMessageW(&tracking, nullptr, 0, 0) > 0) DispatchMessageW(&tracking);
        KillTimer(nullptr, solid_timer);
        const bool cleaned = OwnWindow(L"PulseMenuMaterial") == nullptr;
        std::printf("[%s] material windows destroyed after dismiss\n", cleaned ? "PASS" : "FAIL");
        failed += !cleaned;
        overlays.SetAsyncShellVerbs(74, {});
        const bool stale_ok = sessions_closed == 1 && !overlays.IsOpen();
        std::printf("[%s] late Shell reply closes session without reopening menu\n", stale_ok ? "PASS" : "FAIL");
        failed += !stale_ok;
        overlays.SetAppearance(false, 0xFF0078D4u);
        const auto base_anchor = group.screen_anchor;
        for (int variant = 0; variant < 3; ++variant) {
            const bool acrylic = variant != 0;
            contrast_submenu = variant == 2;
            const float menu_scale = contrast_submenu ? 1.5f : 1.0f;
            overlays.SetDipScale(menu_scale);
            group.screen_anchor = {static_cast<int>(base_anchor.x / menu_scale), static_cast<int>(base_anchor.y / menu_scale)};
            background_dark = false;
            expect_native_acrylic = true;
            expect_material = false;
            settings.default_material = acrylic ? pulse::desktop::model::CardMaterial::Acrylic :
                pulse::desktop::model::CardMaterial::MicaAlt;
            SetWindowPos(competing_window, HWND_TOP, base_anchor.x - 60, base_anchor.y - 60,
                1200, 1000, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(competing_window, nullptr, FALSE);
            UpdateWindow(competing_window);
            contrast_stage = 1;
            native_upload_start = wallpaper.MenuUploadCount();
            overlays.OpenContextMenu(group, settings);
            const auto contrast_timer = SetTimer(nullptr, 0, 600, ChooseFirst);
            while (overlays.IsOpen() && GetMessageW(&tracking, nullptr, 0, 0) > 0) DispatchMessageW(&tracking);
            KillTimer(nullptr, contrast_timer);
            ShowWindow(competing_window, SW_HIDE);
            const bool complete = contrast_stage == 0;
            std::printf("[%s] stationary backdrop comparison completed\n", complete ? "PASS" : "FAIL");
            failed += !complete;
        }
        overlays.SetDipScale(1.0f);
        group.screen_anchor = base_anchor;
        expect_native_acrylic = true;
        expect_material = false;
        settings.default_material = pulse::desktop::model::CardMaterial::MicaAlt;
        deactivate_test = true;
        overlays.OpenContextMenu(group, settings);
        const auto deactivate_timer = SetTimer(nullptr, 0, 500, ChooseFirst);
        while (overlays.IsOpen() && GetMessageW(&tracking, nullptr, 0, 0) > 0) DispatchMessageW(&tracking);
        KillTimer(nullptr, deactivate_timer);
        const bool deactivated = !overlays.IsOpen() && OwnWindow(L"PulseMenu") == nullptr &&
            OwnWindow(L"PulseSubMenu") == nullptr && GetForegroundWindow() == competing_window;
        std::printf("[%s] another ordinary window becomes foreground and dismisses menu\n", deactivated ? "PASS" : "FAIL");
        failed += !deactivated;
        ShowWindow(competing_window, SW_HIDE);

        overlays.TrimIdleResources(); // Fault injection must exercise a new presenter.
        SetEnvironmentVariableW(L"PULSE_MENU_NATIVE_FAIL_PRESENT", L"1");
        settings.default_material = pulse::desktop::model::CardMaterial::Acrylic;
        invoked.clear();
        overlays.OpenContextMenu(group, settings);
        while (overlays.IsOpen() && GetMessageW(&tracking, nullptr, 0, 0) > 0) DispatchMessageW(&tracking);
        SetEnvironmentVariableW(L"PULSE_MENU_NATIVE_FAIL_PRESENT", nullptr);
        const bool failed_present_clean = !overlays.IsOpen() && invoked.empty() &&
            OwnWindow(L"PulseMenu") == nullptr && OwnWindow(L"PulseSubMenu") == nullptr &&
            OwnWindow(L"PulseMenuMaterial") == nullptr && wallpaper.MenuCacheBytes() == 0;
        std::printf("[%s] injected native Present failure clears windows/cache and cancels command\n",
            failed_present_clean ? "PASS" : "FAIL");
        failed += !failed_present_clean;
        capture_enabled = false;
        wchar_t skip_stress[4]{};
        GetEnvironmentVariableW(L"PULSE_MENU_SKIP_STRESS", skip_stress, ARRAYSIZE(skip_stress));
        if (skip_stress[0] != L'1') {
            Usage warm{};
            Usage samples[4]{};
            const auto uploads_before = wallpaper.MenuUploadCount();
            bool bounded = true;
            for (int repeat = 0; repeat < 104; ++repeat) {
                settings.default_material = repeat % 2 == 0 ? pulse::desktop::model::CardMaterial::MicaAlt :
                    pulse::desktop::model::CardMaterial::Acrylic;
                overlays.OpenContextMenu(group, settings);
                const auto repeat_timer = SetTimer(nullptr, 0, 120, ChooseFirst);
                while (overlays.IsOpen() && GetMessageW(&tracking, nullptr, 0, 0) > 0) DispatchMessageW(&tracking);
                KillTimer(nullptr, repeat_timer);
                bounded = bounded && wallpaper.MenuCacheBytes() == 0 && OwnWindow(L"PulseMenu") == nullptr;
                if (repeat == 7 || repeat == 39 || repeat == 71 || repeat == 103) {
                    const int index = (repeat - 7) / 32;
                    samples[index] = ReadSettledUsage(repeat + 1);
                    if (index == 0) warm = samples[index];
                    const auto& sample = samples[index];
                    std::printf("[INFO] cycle=%d private=%zu handles=%lu GDI=%lu USER=%lu threads=%lu uploads=%llu cache=%zu\n",
                        repeat + 1, sample.private_bytes, sample.handles, sample.gdi, sample.user, sample.threads,
                        static_cast<unsigned long long>(wallpaper.MenuUploadCount()), wallpaper.MenuCacheBytes());
                    PrintHandleTypes(repeat + 1);
                }
            }
            const auto last = samples[1];
            const auto final_sample = samples[3];
            std::printf("[INFO] additional 64-cycle delta private=%lld handles=%ld threads=%ld\n",
                static_cast<long long>(final_sample.private_bytes) - static_cast<long long>(last.private_bytes),
                static_cast<long>(final_sample.handles) - static_cast<long>(last.handles),
                static_cast<long>(final_sample.threads) - static_cast<long>(last.threads));
            const auto uploads_after = wallpaper.MenuUploadCount();
            const bool plateau = bounded && uploads_after == uploads_before &&
                last.private_bytes <= warm.private_bytes + 16u * 1024u * 1024u &&
                last.handles <= warm.handles + 4 && last.gdi <= warm.gdi + 2 && last.user <= warm.user + 2;
            std::printf("[%s] 32 alternating MicaAlt/Acrylic cycles after warmup private=%zu->%zu handles=%lu->%lu GDI=%lu->%lu USER=%lu->%lu uploads=%llu->%llu idle_cache=%zu\n",
                plateau ? "PASS" : "FAIL", warm.private_bytes, last.private_bytes, warm.handles, last.handles,
                warm.gdi, last.gdi, warm.user, last.user,
                static_cast<unsigned long long>(uploads_before), static_cast<unsigned long long>(uploads_after), wallpaper.MenuCacheBytes());
            failed += !plateau;
            const bool tail_plateau = final_sample.private_bytes <= last.private_bytes + 16u * 1024u * 1024u &&
                final_sample.handles <= last.handles + 4 && final_sample.gdi <= last.gdi + 2 && final_sample.user <= last.user + 2;
            std::printf("[%s] subsequent 64 cycles retain original memory/handle bounds\n", tail_plateau ? "PASS" : "FAIL");
            failed += !tail_plateau;
        } else std::printf("[SKIP] diagnostic PULSE_MENU_SKIP_STRESS=1: 104-cycle resource audit not run\n");
        overlays.TrimIdleResources();
        const bool trimmed = !overlays.HasCachedMenuDevice() && wallpaper.MenuCacheBytes() == 0 &&
            OwnWindow(L"PulseMenu", false) == nullptr && OwnWindow(L"PulseSubMenu", false) == nullptr;
        std::printf("[%s] idle trim releases menu device and wallpaper cache\n", trimmed ? "PASS" : "FAIL");
        failed += !trimmed;
        const auto uploads_trimmed = wallpaper.MenuUploadCount();
        settings.default_material = pulse::desktop::model::CardMaterial::MicaAlt;
        overlays.OpenContextMenu(group, settings);
        const auto reopen_timer = SetTimer(nullptr, 0, 500, ChooseFirst);
        while (overlays.IsOpen() && GetMessageW(&tracking, nullptr, 0, 0) > 0) DispatchMessageW(&tracking);
        KillTimer(nullptr, reopen_timer);
        const bool reopened = overlays.HasCachedMenuDevice() && wallpaper.MenuUploadCount() == uploads_trimmed &&
            wallpaper.MenuCacheBytes() == 0 && OwnWindow(L"PulseMenu") == nullptr;
        std::printf("[%s] menu reopens after idle device trim\n", reopened ? "PASS" : "FAIL");
        failed += !reopened;
        overlays.TrimIdleResources();
        // Dismissing before the posted track message is dispatched must cancel it.
        pulse::desktop::overlays::ContextMenuSpec spec;
        overlays.OpenContextMenu(spec, settings);
        overlays.Dismiss();
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        const bool cancelled = !overlays.IsOpen() && !overlays.HasActiveSwapChain();
        std::printf("[%s] deferred menu cancellation releases surfaces\n", cancelled ? "PASS" : "FAIL");
        failed += !cancelled;
        overlays.Shutdown();
        wallpaper.ReleaseDeviceResources();
        source_compositor.Shutdown();
        DestroyWindow(competing_window);
        DestroyWindow(source_window);
        test_backdrop = nullptr;
    }
    return failed || material_failures ? 1 : 0;
}

