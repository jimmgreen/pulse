#include <cstdint>
#include <cstdio>
#ifdef PULSE_THEME_BASELINE_HEADER
#include PULSE_THEME_BASELINE_HEADER
#else
#include "../ui/FluentTokens.h"
#endif

int main() {
    using namespace pulse::ui;
    int failures = 0;
    const auto check = [&](const char* label, D2D1_COLOR_F color, COLORREF reference) {
        const int red = static_cast<int>(std::lround(color.r * 255));
        const int green = static_cast<int>(std::lround(color.g * 255));
        const int blue = static_cast<int>(std::lround(color.b * 255));
        const bool equal = red == GetRValue(reference) && green == GetGValue(reference) &&
            blue == GetBValue(reference) && color.a == 1.0f;
        std::printf("[%s] %s actual_rgb=%d,%d,%d expected_rgb=%u,%u,%u\n", equal ? "PASS" : "FAIL",
            label, red, green, blue, static_cast<unsigned>(GetRValue(reference)),
            static_cast<unsigned>(GetGValue(reference)), static_cast<unsigned>(GetBValue(reference)));
        if (!equal) ++failures;
    };
    check("COLORREF helper preserves asymmetric channels", ColorFromRef(RGB(38, 59, 80)), RGB(38, 59, 80));
    check("ordinary RGB hex tokens remain unchanged", HexColor(0x3399ff), RGB(51, 153, 255));
    const auto theme = MakeHighContrastTheme();
    check("HC window", theme.bg, GetSysColor(COLOR_WINDOW));
    check("HC text", theme.text, GetSysColor(COLOR_WINDOWTEXT));
    check("HC selection", theme.selection_bg, GetSysColor(COLOR_HIGHLIGHT));
    check("HC hover", theme.fill_hover, GetSysColor(COLOR_HIGHLIGHT));
    check("HC selected outline", theme.list_selected_outline, GetSysColor(COLOR_HIGHLIGHTTEXT));
    std::printf("[RESULT] failures=%d system_settings_modified=0\n", failures);
    return failures ? 1 : 0;
}
