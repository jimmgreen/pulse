// Model-only regression checks for the menu measurement cache.
#include "../ui/fluent_menu.h"
#include "../ui/typography.h"
#include <cstdio>

namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }
int main() {
    using namespace pulse::ui;
    ComPtr<IDWriteFactory2> factory;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory2),
            reinterpret_cast<IUnknown**>(&factory)))) return 2;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    FluentMenuItem row;
    row.command = 1;
    row.text = L"Menu measurement sample";
    FluentMenuModel model;
    const auto width = [&](float scale) {
        model.SetItems({row});
        model.Layout(factory.get(), scale);
        return model.WidthPx();
    };
    typography::SetUiFontScale(100);
    const int original = width(1);
    check(width(1) == original, "repeated label keeps width");
    check(width(2) >= original * 2 - 1, "DPI changes invalidate cached widths");
    check(width(1) == original, "returning to original DPI restores width");
    typography::SetUiFontScale(125);
    check(width(1) > original, "font-size generation invalidates cached widths");
    typography::SetUiFontScale(100);
    check(width(1) == original, "font-size restoration restores width");
    typography::InvalidateMeasurements();
    check(width(1) == original, "explicit generation invalidation preserves geometry");
    const std::wstring label = row.text;
    for (int i = 0; i < 600; ++i) {
        row.text = label + std::to_wstring(i);
        width(1);
    }
    row.text = label;
    check(width(1) == original, "bounded cache eviction preserves geometry");
    row.text = std::wstring(2000, L'W');
    check(width(1) == 320 && model.Truncated(0), "oversized uncached text retains truncation");
    row.text = label;
    check(width(1) == original, "long text does not contaminate regular label");
    ComPtr<IDWriteFactory2> second;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory2),
            reinterpret_cast<IUnknown**>(&second)))) return 2;
    model.Layout(second.get(), 1);
    check(model.WidthPx() == original, "new factory retains correct geometry");
    model.Layout(nullptr, 1);
    model.Layout(factory.get(), 1);
    check(model.WidthPx() == original, "null factory does not cache failed widths");
    check(model.HitTestRow(model.RowTopPx(0) + model.RowHeightPx(0) / 2) == 0 &&
        model.FirstEnabled() == 0 && model.At(0)->command == 1, "hit testing keyboard navigation and command identity unchanged");
    std::printf("RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}
