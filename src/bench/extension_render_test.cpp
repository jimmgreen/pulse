#include "../ui/ui_compositor.h"
#include <cstdio>
#include <string>

int wmain(int argc, wchar_t** argv) {
    std::puts("start"); std::fflush(stdout);
    if (argc < 2) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"",
        WS_POPUP, 0, 0, 1000, 880, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    pulse::ui::Compositor compositor;
    if (!compositor.Init(window)) { std::puts("init failed"); return 3; }
    std::puts("initialized"); std::fflush(stdout);
    compositor.RecreateTextFormats(argc > 2 ? static_cast<float>(_wtof(argv[2])) : 1.0f);
    auto* dc = compositor.Dc();
    const bool dark = argc > 4 && std::wstring(argv[4]) == L"dark";
    const auto bg = D2D1::ColorF(dark ? 0x18202a : 0xf8fafc);
    const auto fg = D2D1::ColorF(dark ? 0xf0f4fa : 0x17283f);
    auto dim = fg; dim.a = 0.62f;
    pulse::ui::ComPtr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(dim, &brush);
    bool ok = true;
    float expected_n = -1.0f, expected_g = -1.0f;
    for (int pass = 0; pass < 2; ++pass) {
        dc->BeginDraw();
        dc->Clear(bg);
        for (int row = 0; row < 24; ++row) {
            std::wstring name(static_cast<size_t>(row % 12), L'i');
            if (row >= 12) name.insert(0, L"测试");
            name += L"_00001_";
            name += argc > 3 ? argv[3] : L".png";
            const float y = 10.0f + row * 35.0f;
            auto* format = compositor.FileNameFormat();
            if (!pass) {
                float before_n = 0, before_g = 0, full = 0;
                ok &= compositor.MeasureLumaText(name.substr(0, name.size() - 2), format, before_n);
                ok &= compositor.MeasureLumaText(name.substr(0, name.size() - 1), format, before_g);
                ok &= compositor.MeasureLumaText(name, format, full);
                if (!row) { expected_n = before_g - before_n; expected_g = full - before_g; }
                ok &= before_g - before_n == expected_n && full - before_g == expected_g;
                if (!row) std::printf("n_advance=%.4f g_advance=%.4f\n", expected_n, expected_g);
            }
            ok &= compositor.DrawLumaTextSplit(name, format, D2D1::RectF(10, y, 320, y + 32),
                fg, static_cast<UINT32>(name.find_last_of(L'.')), dim, bg);
            ok &= compositor.DrawLumaText(name, format, D2D1::RectF(340, y, 650, y + 32), dim, bg);
            dc->DrawText(name.c_str(), static_cast<UINT32>(name.size()), format,
                D2D1::RectF(670, y, 990, y + 32), brush.get());
        }
        ok &= SUCCEEDED(dc->EndDraw());
        const std::wstring output = std::wstring(argv[1]) + (pass ? L".cached.png" : L".png");
        ok &= compositor.SaveSnapshot(output.c_str());
    }
    std::printf("render=%d\n", ok);
    compositor.Shutdown();
    DestroyWindow(window);
    CoUninitialize();
    return ok ? 0 : 1;
}
