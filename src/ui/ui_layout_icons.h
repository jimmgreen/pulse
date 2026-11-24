#pragma once
// 设置 › 界面布局: the mockup's own line icons (ui_modes_mockup.html CUSTOM), drawn
// with DrawSvgParts on the 24-unit canvas the command icons use.
//
// Settings rows take a glyph string; a string that starts with "\x1" picks one of
// these instead: code < 0x100 is a UilIcon, 0x100 + command_icons::Icon a Pulse
// command icon (MainRenderer::DrawUilIcon).
#include "svg_path_icon.h"
#include "command_icons.h"
#include <d2d1_1.h>
#include <cstdint>
#include <iterator>

namespace pulse::ui {

enum class UilIcon : uint16_t {
    None, Layout, Single, Status, Tabs, Addr, Cmdbar, Zen, Rows, RowsZebra, Wall, Import, Export,
    Palette, Power, Keyboard, List2, Count
};

struct UilGlyph {
    wchar_t s[3];
    operator const wchar_t*() const { return s; }
};
inline UilGlyph UilG(UilIcon icon) { return {{L'\x1', static_cast<wchar_t>(icon), 0}}; }
inline UilGlyph UilG(command_icons::Icon icon) {
    return {{L'\x1', static_cast<wchar_t>(0x100 + static_cast<int>(icon)), 0}};
}

// Base parts in `color`, the mockup's tone part (class "a") in `accent`.
inline void DrawUiLayoutIcon(ID2D1DeviceContext* dc, UilIcon icon, D2D1_RECT_F box,
                             D2D1_COLOR_F color, D2D1_COLOR_F accent) {
    constexpr float w = 1.75f;
#define PULSE_UIL_FRAME "M5.5 4.75h13a2.25 2.25 0 0 1 2.25 2.25v10a2.25 2.25 0 0 1-2.25 2.25h-13A2.25 2.25 0 0 1 3.25 17V7A2.25 2.25 0 0 1 5.5 4.75Z"
    static constexpr SvgPart kFrame[] = {{PULSE_UIL_FRAME, false, w, 1.0f}};
    static constexpr SvgPart kLayoutA[] = {{"M3.25 9h17.5M9 9v10.25", false, w, 1.0f}};
    static constexpr SvgPart kStatusA[] = {{"M3.25 15.25h17.5M6.5 17.3h3", false, w, 1.0f}};
    static constexpr SvgPart kCmdbarA[] = {{"M6.5 9h3M12 9h1M15.5 9h1", false, w, 1.0f}};
#undef PULSE_UIL_FRAME
    static constexpr SvgPart kTabs[] = {
        {"M3.25 9.5V7A2.25 2.25 0 0 1 5.5 4.75h4.25A2.25 2.25 0 0 1 12 7v2.5", false, w, 1.0f},
        {"M3.25 9.5h17.5V17a2.25 2.25 0 0 1-2.25 2.25h-13A2.25 2.25 0 0 1 3.25 17Z", false, w, 1.0f}};
    static constexpr SvgPart kTabsA[] = {{"M5.75 7.6h3.5", false, w, 1.0f}};
    static constexpr SvgPart kAddr[] = {{"M5 7.25h14A2.25 2.25 0 0 1 21.25 9.5v5A2.25 2.25 0 0 1 19 16.75H5A2.25 2.25 0 0 1 2.75 14.5v-5A2.25 2.25 0 0 1 5 7.25Z", false, w, 1.0f}};
    static constexpr SvgPart kAddrA[] = {{"M6.5 12h2.5M11.5 12h2.5", false, w, 1.0f}};
    static constexpr SvgPart kZen[] = {{"M20.5 12a8.5 8.5 0 1 1-17 0 8.5 8.5 0 0 1 17 0Z", false, w, 1.0f}};
    static constexpr SvgPart kZenA[] = {{"M15.25 12a3.25 3.25 0 1 1-6.5 0 3.25 3.25 0 0 1 6.5 0Z", false, w, 1.0f}};
    static constexpr SvgPart kRows[] = {{"M4 7h16M4 12h16M4 17h16", false, w, 1.0f}};
    static constexpr SvgPart kRowsZ[] = {{"M4 6h16M4 10h16M4 14h16M4 18h16", false, w, 1.0f}};
    static constexpr SvgPart kWall[] = {{"M4.5 4.75h7a1.25 1.25 0 0 1 1.25 1.25v4.5A1.25 1.25 0 0 1 11.5 11.75h-7A1.25 1.25 0 0 1 3.25 10.5V6A1.25 1.25 0 0 1 4.5 4.75ZM16 4.75h3.5a1.25 1.25 0 0 1 1.25 1.25v4.5a1.25 1.25 0 0 1-1.25 1.25H16a1.25 1.25 0 0 1-1.25-1.25V6A1.25 1.25 0 0 1 16 4.75ZM4.5 14.25h3.5a1.25 1.25 0 0 1 1.25 1.25V18a1.25 1.25 0 0 1-1.25 1.25H4.5A1.25 1.25 0 0 1 3.25 18v-2.5a1.25 1.25 0 0 1 1.25-1.25ZM12.5 14.25h7a1.25 1.25 0 0 1 1.25 1.25V18a1.25 1.25 0 0 1-1.25 1.25h-7A1.25 1.25 0 0 1 11.25 18v-2.5a1.25 1.25 0 0 1 1.25-1.25Z", false, w, 1.0f}};
    static constexpr SvgPart kImport[] = {{"M13 3.5h5.5a2 2 0 0 1 2 2v13a2 2 0 0 1-2 2H13", false, w, 1.0f}};
    static constexpr SvgPart kImportA[] = {{"M3.5 12h11M11 8.5l3.5 3.5-3.5 3.5", false, w, 1.0f}};
    static constexpr SvgPart kExport[] = {{"M11 3.5H5.5a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2H11", false, w, 1.0f}};
    static constexpr SvgPart kExportA[] = {{"M9.5 12h11M17 8.5l3.5 3.5-3.5 3.5", false, w, 1.0f}};
    static constexpr SvgPart kPalette[] = {{"M12 3.5a8.5 8.5 0 0 0 0 17c1 0 1.7-.8 1.7-1.7 0-.45-.17-.86-.45-1.16a1.7 1.7 0 0 1 1.27-2.84H16.5a4 4 0 0 0 4-4C20.5 6.97 16.7 3.5 12 3.5Z", false, w, 1.0f}};
    static constexpr SvgPart kPaletteA[] = {
        {"M8 11a1 1 0 1 1-2 0 1 1 0 0 1 2 0Z", true, 0.0f, 1.0f},
        {"M11 7.5a1 1 0 1 1-2 0 1 1 0 0 1 2 0Z", true, 0.0f, 1.0f},
        {"M16 8a1 1 0 1 1-2 0 1 1 0 0 1 2 0Z", true, 0.0f, 1.0f}};
    static constexpr SvgPart kPower[] = {{"M12 3v8", false, w, 1.0f}, {"M6.4 6.6a7.5 7.5 0 1 0 11.2 0", false, w, 1.0f}};
    static constexpr SvgPart kKeyboard[] = {
        {"M4.5 6.5h15a2 2 0 0 1 2 2v7a2 2 0 0 1-2 2h-15a2 2 0 0 1-2-2v-7a2 2 0 0 1 2-2Z", false, w, 1.0f},
        {"M6.5 10h.01M9.5 10h.01M12.5 10h.01M15.5 10h.01M8 14h8", false, w, 1.0f}};
    static constexpr SvgPart kList2[] = {{"M8.5 6.5h12M8.5 12h12M8.5 17.5h12M4 6.5h.01M4 12h.01M4 17.5h.01", false, w, 1.0f}};
    const SvgPart* base = nullptr; size_t base_n = 0;
    const SvgPart* tone = nullptr; size_t tone_n = 0;
    auto set = [&](const SvgPart* b, size_t bn, const SvgPart* t = nullptr, size_t tn = 0) { base = b; base_n = bn; tone = t; tone_n = tn; };
    switch (icon) {
    case UilIcon::Layout: set(kFrame, 1, kLayoutA, 1); break;
    case UilIcon::Single: set(kFrame, 1); break;
    case UilIcon::Status: set(kFrame, 1, kStatusA, 1); break;
    case UilIcon::Tabs: set(kTabs, std::size(kTabs), kTabsA, 1); break;
    case UilIcon::Addr: set(kAddr, 1, kAddrA, 1); break;
    case UilIcon::Cmdbar: set(kFrame, 1, kCmdbarA, 1); break;
    case UilIcon::Zen: set(kZen, 1, kZenA, 1); break;
    case UilIcon::Rows: set(kRows, 1); break;
    case UilIcon::RowsZebra: set(kRowsZ, 1); break;
    case UilIcon::Wall: set(kWall, 1); break;
    case UilIcon::Import: set(kImport, 1, kImportA, 1); break;
    case UilIcon::Export: set(kExport, 1, kExportA, 1); break;
    case UilIcon::Palette: set(kPalette, 1, kPaletteA, std::size(kPaletteA)); break;
    case UilIcon::Power: set(kPower, std::size(kPower)); break;
    case UilIcon::Keyboard: set(kKeyboard, std::size(kKeyboard)); break;
    case UilIcon::List2: set(kList2, 1); break;
    default: return;
    }
    DrawSvgParts(dc, base, base_n, 24.0f, box, color);
    if (tone) DrawSvgParts(dc, tone, tone_n, 24.0f, box, accent);
}

} // namespace pulse::ui
