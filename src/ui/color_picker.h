// color_picker.h — QFluent DropDownColorPickerButton popup replica.
//
// 1:1 recreation of QFluentKit's DropDownColorPickerButton popup (hue/sat
// square, brightness slider, RGB/HSV mode combo, hex field, per-channel
// gradient sliders, accept/cancel buttons) rendered with Direct2D on a
// layered window, following the FluentMenu surface/present pattern.
// Layout metrics and visuals mirror QFluentKit sources (ColorPicker.cpp,
// Dialog/ColorDialog.cpp, Slider.cpp). The tag editor only stores RGB, so
// the alpha channel edited in the UI is not part of the result.
#pragma once

#include "FluentTokens.h"

#include <windows.h>
#include <cstdint>
#include <string>

namespace pulse::ui {

class Compositor;
class FluentMenu;

// Qt-style HSV units used by the picker UI: h 0..359, s/v 0..255.
struct HsvColor {
    int h = 0;
    int s = 0;
    int v = 255;
};

HsvColor HsvFromRgb(uint32_t rgb) noexcept;
uint32_t RgbFromHsv(int h, int s, int v) noexcept;

// Parses "#rrggbb" (alpha = 255) or "#aarrggbb" (case-insensitive, leading
// '#' optional). Returns false for anything else.
bool ParseHexColor(const wchar_t* text, uint32_t& argb_out) noexcept;
// "#rrggbb" or "#aarrggbb", lowercase like QColor::name().
std::wstring FormatHexColor(uint32_t argb, bool with_alpha);

class ColorPickerPopup {
public:
    // Modal; returns true when the user accepts (check button / Enter).
    // initial_rgb / result_rgb are 0xRRGGBB. The popup appears at screen_pt
    // (tag menu position), clamped to the work area. menu supplies the
    // RGB/HSV mode dropdown (shared app FluentMenu); may be nullptr, in
    // which case the combo toggles the mode directly.
    static bool Pick(HWND owner, Compositor* compositor, FluentMenu* menu, float scale,
                     POINT screen_pt, uint32_t initial_rgb, bool dark, uint32_t& result_rgb);

    // GUI verification: render the popup frame to a PNG without showing it.
    static bool SaveDebugSnapshot(Compositor* compositor, const wchar_t* png_path,
                                  bool dark, D2D1_COLOR_F accent, float scale);
};

} // namespace pulse::ui
