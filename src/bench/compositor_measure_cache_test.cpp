#include "../ui/ui_compositor.h"
#include "../ui/typography.h"
#include "../ui/text_contrast_scope.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    const bool require_preserved = argc > 1 && wcscmp(argv[1], L"--expect-preserve") == 0;
    const int count = argc > 2 ? _wtoi(argv[2]) : 1024;
    if (count < 1 || count > 4096) return 2;
    using namespace pulse::ui;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto make_window = [] {
        return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"measure cache",
            WS_POPUP, -32000, -32000, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    };
    HWND main_window = make_window(), auxiliary_window = make_window();
    int failures = 0;
    {
        Compositor main;
        if (!main_window || !auxiliary_window || !main.Init(main_window)) return 2;
        main.RecreateTextFormats(1.0f);
        std::vector<std::wstring> text;
        for (int i = 0; i < count; ++i) text.push_back(L"Report document file " + std::to_wstring(i));
        for (int round = 0; round < 5; ++round) {
            double expected = 0;
            for (const auto& line : text) expected += typography::MeasureLine(&main, main.TextFormat(), line);
            const auto generation = typography::Generation();
            Compositor auxiliary;
            if (!auxiliary.Init(auxiliary_window)) return 2;
            auxiliary.RecreateTextFormats(1.0f);
            const bool preserved = generation == typography::Generation();
            TextContrastScope text_contrast_scope(IsHighContrast());
            const auto start = std::chrono::steady_clock::now();
            double actual = 0;
            for (const auto& line : text) actual += typography::MeasureLine(&main, main.TextFormat(), line);
            const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            const auto hot_start = std::chrono::steady_clock::now();
            double hot_sum = 0;
            for (int repeat = 0; repeat < 50; ++repeat)
                for (const auto& line : text) hot_sum += typography::MeasureLine(&main, main.TextFormat(), line);
            const double hot_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hot_start).count();
            if (std::abs(hot_sum - expected * 50) > 0.001) ++failures;
            std::printf("hot_lookups=%d hot_ms=%.6f\n", count * 50, hot_ms);
            const bool equal = std::abs(actual - expected) < 0.001;
            if (!equal || (require_preserved && !preserved)) ++failures;
            std::printf("round=%d preserved=%d widths_equal=%d measure_ms=%.6f checksum=%.6f\n",
                round, preserved, equal, elapsed, actual);
        }
    }
    DestroyWindow(auxiliary_window); DestroyWindow(main_window);
    if (SUCCEEDED(com)) CoUninitialize();
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
