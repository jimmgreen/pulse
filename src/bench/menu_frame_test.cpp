// Real menu render workload; compile the same harness against saved/new menu sources.
#include "../ui/fluent_menu.h"
#include "../ui/text_contrast_scope.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
namespace pulse::ui {
struct FluentMenuTestPeer {
    struct OutsideFocusFixture {
        FluentMenu* menu = nullptr;
        HWND other = nullptr;
        bool switched = false;
        bool watchdog = false;
    };
    static void CALLBACK TransferFocus(HWND owner, UINT, UINT_PTR id, DWORD) {
        KillTimer(owner, id);
        auto* fixture = reinterpret_cast<OutsideFocusFixture*>(GetWindowLongPtrW(owner, GWLP_USERDATA));
        if (!fixture) return;
        if (id == 1) {
            SetForegroundWindow(fixture->other);
            fixture->switched = GetForegroundWindow() == fixture->other;
        } else {
            fixture->watchdog = true;
            fixture->menu->Dismiss();
        }
    }
    static int OutsideFocus() {
        const HWND previous = GetForegroundWindow();
        WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PulseMenuOutsideFocusFixture"; RegisterClassW(&wc);
        const auto window = [&] {
            return CreateWindowExW(0, wc.lpszClassName, L"Fixture", WS_POPUP | WS_VISIBLE,
                -30000, -30000, 360, 220, nullptr, nullptr, wc.hInstance, nullptr);
        };
        const HWND other = window(), owner = window();
        if (!other || !owner) return 2;
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        int result = 2;
        {
            Compositor compositor;
            FluentMenu menu;
            SetForegroundWindow(owner);
            if (GetForegroundWindow() == owner && compositor.Init(owner) && menu.Create(owner, &compositor, 1.0f)) {
                OutsideFocusFixture fixture{&menu, other};
                SetWindowLongPtrW(owner, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&fixture));
                FluentMenuItem item;
                item.text = L"Fixture action";
                item.command = 101;
                SetTimer(owner, 1, 125, &TransferFocus);
                SetTimer(owner, 2, 650, &TransferFocus);
                const int command = menu.TrackPopup(POINT{220, 180}, {std::move(item)});
                KillTimer(owner, 1);
                KillTimer(owner, 2);
                const bool retained = GetForegroundWindow() == other;
                const bool hidden = !IsWindowVisible(menu.hwnd_);
                std::printf("foreground switched=%d watchdog needed=%d foreground retained=%d popup hidden=%d command=%d\n",
                    fixture.switched, fixture.watchdog, retained, hidden, command);
                result = fixture.switched && !fixture.watchdog && retained && hidden && command == 0 ? 0 : 1;
                SetWindowLongPtrW(owner, GWLP_USERDATA, 0);
            }
        }
        if (SUCCEEDED(apartment)) CoUninitialize();
        const HWND foreground = GetForegroundWindow();
        if ((foreground == owner || foreground == other) && IsWindow(previous)) SetForegroundWindow(previous);
        DestroyWindow(owner);
        DestroyWindow(other);
        return result;
    }
    static int FocusReturn(bool isolated) {
        const HWND previous = GetForegroundWindow();
        const auto name = L"PulseMenuFocus_" + std::to_wstring(GetCurrentProcessId());
        if (isolated) {
            HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
            if (!desktop || !SetThreadDesktop(desktop)) return 3;
        }
        WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PulseMenuFocusFixture"; RegisterClassW(&wc);
        const auto window = [&](HWND parent) {
            return CreateWindowExW(0, wc.lpszClassName, L"Fixture", WS_POPUP | WS_VISIBLE,
                -30000, -30000, 200, 100, parent, nullptr, wc.hInstance, nullptr);
        };
        const HWND other = window(nullptr), owner = window(nullptr);
        FluentMenu menu; menu.owner_ = owner; menu.hwnd_ = window(owner);
        int failures = 0;
        const auto check = [&](bool ok, const char* label) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures;
        };
        SetForegroundWindow(menu.hwnd_);
        check(GetForegroundWindow() == menu.hwnd_, "fixture popup owns foreground");
        menu.HideRootWindow();
        check(GetForegroundWindow() == owner && !IsWindowVisible(menu.hwnd_), "closing active popup returns foreground to owner");
        ShowWindow(menu.hwnd_, SW_SHOWNOACTIVATE); SetForegroundWindow(other);
        check(GetForegroundWindow() == other, "fixture user switched to another window");
        menu.HideRootWindow();
        check(GetForegroundWindow() == other, "closing inactive popup does not steal foreground");
        const HWND foreground = GetForegroundWindow();
        if (!isolated && (foreground == owner || foreground == other || foreground == menu.hwnd_) && IsWindow(previous))
            SetForegroundWindow(previous);
        DestroyWindow(owner); DestroyWindow(other);
        return failures ? 1 : 0;
    }
    static void Prepare(FluentMenu& menu, const std::vector<FluentMenuItem>& items) {
        menu.model_.SetItems(items);
        menu.model_.Layout(menu.compositor_->DwriteFactory(), menu.scale_, 280.0f * menu.scale_);
        menu.first_sub_.model.SetItems(items);
        menu.first_sub_.model.Layout(menu.compositor_->DwriteFactory(), menu.scale_, 280.0f * menu.scale_);
        menu.motion_ = false;
        menu.open_t_ms_ = -1.0f;
        menu.hover_row_ = 1;
        menu.first_sub_.hover = 1;
    }
    static bool Draw(FluentMenu& menu, int kind) {
        if (kind == 0) return menu.Render();
        if (kind == 1) return menu.RenderSub();
        return menu.RenderTip(L"Menu tooltip\nSecond line\n\x6587\x4ef6\x64cd\x4f5c");
    }
};
}
int wmain(int argc, wchar_t** argv) {
    using namespace pulse::ui;
    if (argc == 2 && std::wstring_view(argv[1]) == L"--focus-return") return FluentMenuTestPeer::FocusReturn(true);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--focus-return-live") return FluentMenuTestPeer::FocusReturn(false);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--outside-focus-live") return FluentMenuTestPeer::OutsideFocus();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseMenuFrameBench";
    RegisterClassW(&wc);
    HWND owner = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"", WS_POPUP, -30000, -30000, 700, 400, nullptr, nullptr, wc.hInstance, nullptr);
    int failures = 0;
    {
        Compositor compositor;
        if (!owner || !compositor.Init(owner)) return 2;
        FluentMenu menu;
        const float scale = argc > 2 ? static_cast<float>(_wtof(argv[2])) : 1.0f;
        compositor.RecreateTextFormats(scale);
        if (!compositor.SmallFormat() || !compositor.DwriteFactory() || !compositor.Dc()) {
            std::printf("[FAIL] menu resources: dc=%p dwrite=%p small_format=%p\n",
                static_cast<void*>(compositor.Dc()), static_cast<void*>(compositor.DwriteFactory()),
                static_cast<void*>(compositor.SmallFormat()));
            return 2;
        }
        if (!menu.Create(owner, &compositor, scale)) {
            std::printf("[FAIL] menu.Create\n");
            return 2;
        }
        const bool dark = argc > 3 && std::wstring(argv[3]) == L"dark";
        const bool dpi_only = argc > 4 && std::wstring(argv[4]) == L"dpi-only";
        menu.SetTheme(dark, D2D1::ColorF(0.2f, 0.4f, 0.8f));
        std::vector<FluentMenuItem> items;
        for (int i = 0; i < 16; ++i) {
            FluentMenuItem item;
            item.command = i + 1;
            item.text = L"Menu action \x6587\x4ef6 " + std::to_wstring(i);
            item.enabled = i != 3;
            items.push_back(std::move(item));
        }
        if (argc > 1 && !menu.SaveDebugSnapshot(argv[1], items, 1)) { ++failures; std::printf("[FAIL] main snapshot\n"); }
        if (dpi_only && argc > 1) {
            const auto bytes = [](const std::filesystem::path& path) {
                std::ifstream stream(path, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>{});
            };
            const auto baseline = bytes(argv[1]);
            for (const float dpi : {144.0f, 192.0f}) {
                compositor.Dc()->SetDpi(dpi, dpi);
                const auto path = std::wstring(argv[1]) + L".dpi-" + std::to_wstring(static_cast<int>(dpi)) + L".png";
                const bool saved = menu.SaveDebugSnapshot(path.c_str(), items, 1);
                float x = 0, y = 0; compositor.Dc()->GetDpi(&x, &y);
                const bool same = saved && !baseline.empty() && bytes(path) == baseline;
                const bool restored = x == dpi && y == dpi;
                if (!same || !restored) ++failures;
                std::printf("[%s] inherited DPI %.0f: identical snapshot=%d restored=%d\n",
                    same && restored ? "PASS" : "FAIL", dpi, same, restored);
            }
        }
        FluentMenuTestPeer::Prepare(menu, items);
        const char* names[] = {"main", "submenu", "tooltip"};
        for (int kind = 0; kind < 3; ++kind) {
            if (dpi_only) {
                compositor.Dc()->SetDpi(144, 192);
                const bool drawn = FluentMenuTestPeer::Draw(menu, kind);
                float x = 0, y = 0; compositor.Dc()->GetDpi(&x, &y);
                const bool restored = x == 144 && y == 192;
                if (!drawn || !restored) ++failures;
                std::printf("[%s] %s non-square owner DPI restored\n", drawn && restored ? "PASS" : "FAIL", names[kind]);
                continue;
            }
            int warmup_failures = 0;
            for (int i = 0; i < 10; ++i) if (!FluentMenuTestPeer::Draw(menu, kind)) ++warmup_failures;
            if (warmup_failures) {
                failures += warmup_failures;
                std::printf("[FAIL] %s warmup: %d/10; timing skipped\n", names[kind], warmup_failures);
                continue;
            }
            for (int round = 0; round < 5; ++round) {
                int draw_failures = 0;
                const auto start = std::chrono::steady_clock::now();
                for (int i = 0; i < 100; ++i) if (!FluentMenuTestPeer::Draw(menu, kind)) ++draw_failures;
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (draw_failures) {
                    failures += draw_failures;
                    std::printf("[FAIL] %s round=%d render failures=%d/100; invalid timing\n", names[kind], round, draw_failures);
                } else {
                    std::printf("menu_frame,%s,%d,100,%.6f\n", names[kind], round, ms);
                }
            }
        }
        // The menu must restore an outer frame snapshot, including tooltip draws.
        const TextContrastScope outer(true);
        for (int kind = 0; kind < 3; ++kind) {
            const bool drawn = FluentMenuTestPeer::Draw(menu, kind);
            const bool restored = TextHighContrast();
            if (!drawn || !restored) {
                ++failures;
                std::printf("[FAIL] %s nested render=%d restored=%d\n", names[kind], drawn, restored);
            }
        }
    }
    DestroyWindow(owner);
    CoUninitialize();
    std::printf("[%s] real menu render paths and scope restoration\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
