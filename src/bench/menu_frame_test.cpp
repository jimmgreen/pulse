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
    static void Prepare(FluentMenu& menu, const std::vector<FluentMenuItem>& items) {
        menu.model_.SetItems(items);
        menu.model_.Layout(menu.compositor_->DwriteFactory(), menu.scale_, 280.0f * menu.scale_);
        menu.sub_model_.SetItems(items);
        menu.sub_model_.Layout(menu.compositor_->DwriteFactory(), menu.scale_, 280.0f * menu.scale_);
        menu.motion_ = false;
        menu.open_t_ms_ = -1.0f;
        menu.hover_row_ = 1;
        menu.sub_hover_ = 1;
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
