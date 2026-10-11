// FluentTokens.h — Win11 Fluent design tokens (ui.md §3).
// Dark / light / high-contrast instances; runtime switches by pointer.
#pragma once
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <windows.h>
#include <dwmapi.h>
#include <cmath>
#include <algorithm>

namespace pulse::ui {

inline D2D1_COLOR_F HexColor(uint32_t rgb, float a = 1.0f) noexcept {
    return D2D1::ColorF(
        ((rgb >> 16) & 0xFF) / 255.0f,
        ((rgb >> 8) & 0xFF) / 255.0f,
        (rgb & 0xFF) / 255.0f,
        a);
}

inline D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) noexcept {
    c.a = a;
    return c;
}

inline D2D1_COLOR_F BlendOver(D2D1_COLOR_F src, D2D1_COLOR_F dst) noexcept {
    const float a = std::clamp(src.a, 0.0f, 1.0f);
    const float ia = 1.0f - a;
    D2D1_COLOR_F out;
    out.r = src.r * a + dst.r * ia;
    out.g = src.g * a + dst.g * ia;
    out.b = src.b * a + dst.b * ia;
    out.a = 1.0f;
    return out;
}

// Per-channel mix of two colors; t = 0 gives a, 1 gives b. The result is opaque.
inline D2D1_COLOR_F MixColor(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) noexcept {
    D2D1_COLOR_F out;
    out.r = a.r + (b.r - a.r) * t;
    out.g = a.g + (b.g - a.g) * t;
    out.b = a.b + (b.b - a.b) * t;
    out.a = 1.0f;
    return out;
}

struct AccentShades {
    D2D1_COLOR_F dark3;
    D2D1_COLOR_F dark2;
    D2D1_COLOR_F dark1;
    D2D1_COLOR_F light1;
    D2D1_COLOR_F light2;
    D2D1_COLOR_F light3;
};

inline void RgbToHsv(D2D1_COLOR_F c, float& h, float& s, float& v) noexcept {
    float r = c.r, g = c.g, b = c.b;
    float mx = std::max({r, g, b});
    float mn = std::min({r, g, b});
    float d = mx - mn;
    v = mx;
    s = mx == 0.0f ? 0.0f : d / mx;
    if (d == 0.0f) {
        h = 0.0f;
    } else if (mx == r) {
        h = std::fmod((g - b) / d + 6.0f, 6.0f);
    } else if (mx == g) {
        h = (b - r) / d + 2.0f;
    } else {
        h = (r - g) / d + 4.0f;
    }
    h *= 60.0f;
}

inline D2D1_COLOR_F HsvToRgb(float h, float s, float v) noexcept {
    float c = v * s;
    float x = c * (1.0f - std::abs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    D2D1_COLOR_F r{};
    r.a = 1.0f;
    if (h < 60.0f)       { r.r = c; r.g = x; r.b = 0; }
    else if (h < 120.0f) { r.r = x; r.g = c; r.b = 0; }
    else if (h < 180.0f) { r.r = 0; r.g = c; r.b = x; }
    else if (h < 240.0f) { r.r = 0; r.g = x; r.b = c; }
    else if (h < 300.0f) { r.r = x; r.g = 0; r.b = c; }
    else                 { r.r = c; r.g = 0; r.b = x; }
    r.r += m; r.g += m; r.b += m;
    return r;
}

inline AccentShades DeriveAccentShades(D2D1_COLOR_F accent) noexcept {
    float h, s, v;
    RgbToHsv(accent, h, s, v);
    AccentShades a{};
    // Darken/lighten by value; keep saturation somewhat.
    a.dark3  = HsvToRgb(h, std::min(1.0f, s * 1.05f), std::max(0.0f, v * 0.55f));
    a.dark2  = HsvToRgb(h, std::min(1.0f, s * 1.02f), std::max(0.0f, v * 0.70f));
    a.dark1  = HsvToRgb(h, s, std::max(0.0f, v * 0.85f));
    a.light1 = HsvToRgb(h, std::max(0.0f, s * 0.95f), std::min(1.0f, v * 1.12f));
    a.light2 = HsvToRgb(h, std::max(0.0f, s * 0.90f), std::min(1.0f, v * 1.25f));
    a.light3 = HsvToRgb(h, std::max(0.0f, s * 0.85f), std::min(1.0f, v * 1.40f));
    return a;
}

inline D2D1_COLOR_F AutoAccentText(D2D1_COLOR_F accent) noexcept {
    // Perceived luminance; return white or black.
    float y = 0.2126f * accent.r + 0.7152f * accent.g + 0.0722f * accent.b;
    return y < 0.5f ? HexColor(0xFFFFFF) : HexColor(0x000000);
}

// Layout metrics in DIPs at 100% scale. Scaled at use (SetScale, etc.).
// Compact classic heights: tab bar 36, toolbar 64 (was 48/88).
inline constexpr float kTitleBarHeight = 36.0f;

// ---------------------------------------------------------------------------
// Resolved runtime theme (colors already premultiplied by desired alpha).
// ---------------------------------------------------------------------------
struct Theme {
    D2D1_COLOR_F bg;
    D2D1_COLOR_F text;
    D2D1_COLOR_F text_secondary;
    D2D1_COLOR_F text_disabled;

    D2D1_COLOR_F fill_hover;
    D2D1_COLOR_F fill_pressed;
    D2D1_COLOR_F fill_selected;
    // Selection in a split pane that does not have focus: neutral, so only the
    // focused pane carries the accent.
    D2D1_COLOR_F fill_selected_inactive;
    // File list selection in the focused pane (#78): an accent gradient that is
    // strongest beside the indicator bar and fades toward the row's right edge,
    // plus the stroke for Settings > File list > Outline selected items.
    D2D1_COLOR_F list_selected_start;
    D2D1_COLOR_F list_selected_end;
    D2D1_COLOR_F list_selected_outline;
    D2D1_COLOR_F fill_input;
    D2D1_COLOR_F fill_input_hover;
    D2D1_COLOR_F fill_input_focus;
    D2D1_COLOR_F fill_input_disabled;

    D2D1_COLOR_F stroke_card;
    D2D1_COLOR_F stroke_divider;
    D2D1_COLOR_F stroke_input_bottom;

    D2D1_COLOR_F surface_flyout;
    D2D1_COLOR_F surface_pane_inactive;
    // Window layers: the title strip, one sheet shared by the active tab,
    // toolbar, sidebar and status bar, and cards for panes and details.
    D2D1_COLOR_F surface_title;
    D2D1_COLOR_F surface_sheet;
    D2D1_COLOR_F surface_card;

    D2D1_COLOR_F accent;
    D2D1_COLOR_F accent_hover;
    D2D1_COLOR_F accent_pressed;
    D2D1_COLOR_F accent_text;

    D2D1_COLOR_F danger;
    D2D1_COLOR_F danger_hover;

    // Legacy convenience aliases used by older code paths.
    D2D1_COLOR_F header_bg;
    D2D1_COLOR_F header_sep;
    D2D1_COLOR_F hover_bg;
    D2D1_COLOR_F selection_bg;
    D2D1_COLOR_F scrollbar_thumb;
    D2D1_COLOR_F tab_bg;
    D2D1_COLOR_F tab_active_bg;
    D2D1_COLOR_F address_bg;
    D2D1_COLOR_F status_bg;
    D2D1_COLOR_F icon_folder;
    D2D1_COLOR_F icon_file;
    D2D1_COLOR_F fps_bg;
    D2D1_COLOR_F fps_text;

    // Geometric tokens (DIPs at 100% scale).
    float radius_window = 0.0f;
    float radius_flyout = 0.0f;
    float radius_control = 0.0f;
    float spacing_unit = 4.0f;
    float row_list = 28.0f;
    float row_header = 32.0f;
    float row_menu = 36.0f;
    float padding_cell = 12.0f;
    float titlebar_height = kTitleBarHeight;
    float toolbar_height = 40.0f;
    float statusbar_height = 24.0f;
    float sidebar_width = 224.0f;
    float scrollbar_silent = 2.0f;
    float scrollbar_hover = 6.0f;
    float focus_ring = 2.0f;
};

enum class ThemeMode { Auto, Light, Dark };

inline bool ShouldUseDarkMode(ThemeMode overrideMode) noexcept {
    if (overrideMode == ThemeMode::Dark) return true;
    if (overrideMode == ThemeMode::Light) return false;
    using Fn = bool (WINAPI*)();
    HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
    bool dark = false;
    if (uxtheme) {
        Fn should = reinterpret_cast<Fn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(132)));
        if (should) dark = should();
        FreeLibrary(uxtheme);
        if (should) return dark;
    }
    DWORD value = 1;
    DWORD size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

inline D2D1_COLOR_F GetAccentColor() noexcept {
    DWORD color = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque))) {
        return HexColor(color, 1.0f);
    }
    return HexColor(0x0078D4);
}

#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_COLOR_NONE
#define DWMWA_COLOR_NONE 0xFFFFFFFEu
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_NONE
#define DWMSBT_NONE 1
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2
#endif
#ifndef DWMSBT_TRANSIENTWINDOW
#define DWMSBT_TRANSIENTWINDOW 3
#endif
#ifndef DWMSBT_TABBEDWINDOW
#define DWMSBT_TABBEDWINDOW 4
#endif

inline bool IsHighContrast() noexcept {
    HIGHCONTRASTW hc{ sizeof(hc) };
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) {
        return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    }
    return false;
}

// Text boxes in high contrast use the system window colors, like the rest of the
// high-contrast theme; otherwise the window's own colors.
inline COLORREF HcEditText(COLORREF themed) noexcept {
    return IsHighContrast() ? GetSysColor(COLOR_WINDOWTEXT) : themed;
}

inline COLORREF HcEditBack(COLORREF themed) noexcept {
    return IsHighContrast() ? GetSysColor(COLOR_WINDOW) : themed;
}

// WM_CTLCOLOREDIT brush: the system window brush in high contrast (system-owned,
// never deleted; follows a high-contrast switch while open), else `themed`.
inline HBRUSH EditBackBrush(HBRUSH themed) noexcept {
    return IsHighContrast() ? GetSysColorBrush(COLOR_WINDOW) : themed;
}

// The common light/dark text-box pair.
inline COLORREF EditTextColor(bool dark) noexcept {
    return HcEditText(dark ? RGB(255, 255, 255) : RGB(26, 26, 26));
}

inline COLORREF EditBackColor(bool dark) noexcept {
    return HcEditBack(dark ? RGB(30, 30, 30) : RGB(255, 255, 255));
}

inline D2D1_COLOR_F ColorFromRef(COLORREF color) noexcept {
    return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                        GetBValue(color) / 255.0f);
}

inline void UpdateWindowTheme(HWND hwnd, bool dark) noexcept {
    BOOL darkValue = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkValue, sizeof(darkValue));
    const COLORREF border = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
}

inline Theme MakeTheme(bool dark, D2D1_COLOR_F accent) noexcept {
    Theme t{};
    // Keep the stored accent hue in both themes.
    t.accent = accent;
    AccentShades shades = DeriveAccentShades(t.accent);
    t.accent_hover = shades.light1;
    t.accent_pressed = shades.dark1;
    t.accent_text = AutoAccentText(t.accent);
    t.danger = HexColor(0xC42B1C);
    t.danger_hover = WithAlpha(HexColor(0xC42B1C), 0.85f);

    if (dark) {
        // 去饱和深色主题：深灰底 + 浅灰字，无彩色点缀。
        const D2D1_COLOR_F muted = MixColor(accent, HexColor(0x808080), 0.65f);
        t.accent = muted;
        t.accent_hover = MixColor(muted, HexColor(0xFFFFFF), 0.15f);
        t.accent_pressed = MixColor(muted, HexColor(0x000000), 0.20f);
        t.bg = HexColor(0x202020);
        t.text = HexColor(0xDCDCDC);
        t.text_secondary = WithAlpha(HexColor(0xDCDCDC), 0.65f);
        t.text_disabled = WithAlpha(HexColor(0xDCDCDC), 0.40f);

        t.fill_hover = WithAlpha(HexColor(0xFFFFFF), 0.08f);
        t.fill_pressed = WithAlpha(HexColor(0xFFFFFF), 0.12f);
        t.fill_selected = WithAlpha(muted, 0.55f);
        t.fill_selected_inactive = WithAlpha(HexColor(0x808080), 0.35f);
        t.list_selected_start = WithAlpha(muted, 0.55f);
        t.list_selected_end = WithAlpha(muted, 0.55f);
        t.list_selected_outline = muted;
        t.fill_input = HexColor(0x2A2A2A);
        t.fill_input_hover = HexColor(0x303030);
        t.fill_input_focus = HexColor(0x2A2A2A);
        t.fill_input_disabled = HexColor(0x262626);

        t.stroke_card = HexColor(0x3A3A3A);
        t.stroke_divider = HexColor(0x333333);
        t.stroke_input_bottom = HexColor(0x6E6E6E);

        t.surface_flyout = HexColor(0x2A2A2A);
        t.surface_title = HexColor(0x202020);
        t.surface_sheet = HexColor(0x242424);
        t.surface_card = HexColor(0x2A2A2A);
        t.surface_pane_inactive = WithAlpha(HexColor(0x808080), 0.50f);

        t.header_bg = HexColor(0x242424);
        t.header_sep = HexColor(0x3A3A3A);
        t.hover_bg = t.fill_hover;
        t.selection_bg = t.fill_selected;
        t.scrollbar_thumb = HexColor(0x6E6E6E);
        t.tab_bg = HexColor(0x242424);
        t.tab_active_bg = HexColor(0x2E2E2E);
        t.address_bg = HexColor(0x2A2A2A);
        t.status_bg = HexColor(0x1E1E1E);
        t.icon_folder = HexColor(0xB0B0B0);
        t.icon_file = HexColor(0x909090);
        t.fps_bg = WithAlpha(HexColor(0x000000), 0.60f);
        t.fps_text = HexColor(0xDCDCDC);
    } else {
        // Win10 经典浅色主题：#F5F5F5 底、深灰字、Win10 蓝 #0078D4 点缀、选中项淡蓝。
        t.accent = HexColor(0x0078D4);
        t.accent_hover = HexColor(0x0067B8);
        t.accent_pressed = HexColor(0x005A9E);
        t.accent_text = HexColor(0xFFFFFF);
        t.bg = HexColor(0xF5F5F5);
        t.text = HexColor(0x1A1A1A);
        t.text_secondary = HexColor(0x666666);
        t.text_disabled = HexColor(0x999999);

        t.fill_hover = HexColor(0xE5E5E5);
        t.fill_pressed = HexColor(0xD0D0D0);
        t.fill_selected = HexColor(0xE5F1FB);
        t.fill_selected_inactive = HexColor(0xCCCCCC);
        t.list_selected_start = HexColor(0xE5F1FB);
        t.list_selected_end = HexColor(0xE5F1FB);
        t.list_selected_outline = HexColor(0x0078D4);
        t.fill_input = HexColor(0xFFFFFF);
        t.fill_input_hover = HexColor(0xFFFFFF);
        t.fill_input_focus = HexColor(0xFFFFFF);
        t.fill_input_disabled = HexColor(0xF5F5F5);

        t.stroke_card = HexColor(0xD0D0D0);
        t.stroke_divider = HexColor(0xE0E0E0);
        t.stroke_input_bottom = HexColor(0x7A7A7A);

        t.surface_flyout = HexColor(0xFFFFFF);
        t.surface_pane_inactive = WithAlpha(HexColor(0x808080), 0.50f);
        t.surface_title = HexColor(0xF5F5F5);
        t.surface_sheet = HexColor(0xF5F5F5);
        t.surface_card = HexColor(0xFFFFFF);

        t.header_bg = HexColor(0xF5F5F5);
        t.header_sep = HexColor(0xD0D0D0);
        t.hover_bg = t.fill_hover;
        t.selection_bg = t.fill_selected;
        t.scrollbar_thumb = HexColor(0xC1C1C1);
        t.tab_bg = HexColor(0xE8E8E8);
        t.tab_active_bg = HexColor(0x0078D4);
        t.address_bg = HexColor(0xFFFFFF);
        t.status_bg = HexColor(0xF5F5F5);
        t.icon_folder = HexColor(0xBCA270);
        t.icon_file = HexColor(0x808080);
        t.fps_bg = WithAlpha(HexColor(0x000000), 0.60f);
        t.fps_text = HexColor(0xFFFFFF);
    }
    return t;
}

inline Theme MakeHighContrastTheme() noexcept {
    Theme t{};
    t.accent = HexColor(0x0078D4);
    t.accent_hover = HexColor(0x005A9E);
    t.accent_pressed = HexColor(0x004578);
    t.accent_text = HexColor(0xFFFFFF);
    t.danger = HexColor(0xC42B1C);
    t.danger_hover = HexColor(0xA51910);

    // High contrast uses system colors without transparency.
    auto sys = [](int idx) { return HexColor((uint32_t)GetSysColor(idx)); };
    t.bg = sys(COLOR_WINDOW);
    t.text = sys(COLOR_WINDOWTEXT);
    t.text_secondary = sys(COLOR_GRAYTEXT);
    t.text_disabled = sys(COLOR_GRAYTEXT);

    t.fill_hover = sys(COLOR_HIGHLIGHT);
    t.fill_pressed = sys(COLOR_HIGHLIGHT);
    t.fill_selected = sys(COLOR_HIGHLIGHT);
    t.fill_selected_inactive = sys(COLOR_HIGHLIGHT);
    t.list_selected_start = t.fill_selected;
    t.list_selected_end = t.fill_selected;
    t.list_selected_outline = sys(COLOR_HIGHLIGHTTEXT);
    t.fill_input = sys(COLOR_WINDOW);
    t.fill_input_hover = sys(COLOR_WINDOW);
    t.fill_input_focus = sys(COLOR_WINDOW);
    t.fill_input_disabled = sys(COLOR_BTNFACE);

    t.stroke_card = sys(COLOR_WINDOWFRAME);
    t.stroke_divider = sys(COLOR_WINDOWFRAME);
    t.stroke_input_bottom = sys(COLOR_HIGHLIGHT);

    t.surface_flyout = sys(COLOR_WINDOW);
    t.surface_pane_inactive = sys(COLOR_GRAYTEXT);
    t.surface_title = sys(COLOR_WINDOW);
    t.surface_sheet = sys(COLOR_WINDOW);
    t.surface_card = sys(COLOR_WINDOW);

    t.header_bg = sys(COLOR_WINDOW);
    t.header_sep = sys(COLOR_WINDOWFRAME);
    t.hover_bg = t.fill_hover;
    t.selection_bg = t.fill_selected;
    t.scrollbar_thumb = sys(COLOR_WINDOWTEXT);
    t.tab_bg = sys(COLOR_BTNFACE);
    t.tab_active_bg = sys(COLOR_WINDOW);
    t.address_bg = sys(COLOR_WINDOW);
    t.status_bg = sys(COLOR_BTNFACE);
    t.icon_folder = sys(COLOR_WINDOWTEXT);
    t.icon_file = sys(COLOR_WINDOWTEXT);
    t.fps_bg = sys(COLOR_WINDOWTEXT);
    t.fps_text = sys(COLOR_WINDOW);
    return t;
}

} // namespace pulse::ui
