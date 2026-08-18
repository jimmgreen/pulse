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
inline constexpr float kTitleBarHeight = 48.0f;

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
    D2D1_COLOR_F fill_input;
    D2D1_COLOR_F fill_input_hover;
    D2D1_COLOR_F fill_input_focus;
    D2D1_COLOR_F fill_input_disabled;

    D2D1_COLOR_F stroke_card;
    D2D1_COLOR_F stroke_divider;
    D2D1_COLOR_F stroke_input_bottom;

    D2D1_COLOR_F surface_flyout;
    D2D1_COLOR_F surface_pane_inactive;

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
    D2D1_COLOR_F border;
    D2D1_COLOR_F icon_folder;
    D2D1_COLOR_F icon_file;
    D2D1_COLOR_F fps_bg;
    D2D1_COLOR_F fps_text;

    // Geometric tokens (DIPs at 100% scale).
    float radius_window = 12.0f;
    float radius_flyout = 12.0f;
    float radius_control = 8.0f;
    float spacing_unit = 4.0f;
    float row_list = 28.0f;
    float row_header = 32.0f;
    float row_menu = 32.0f;
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

inline void UpdateWindowTheme(HWND hwnd, bool dark) noexcept {
    BOOL darkValue = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkValue, sizeof(darkValue));
    const COLORREF border = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
}

inline Theme MakeTheme(bool dark, D2D1_COLOR_F accent) noexcept {
    Theme t{};
    AccentShades shades = DeriveAccentShades(accent);
    t.accent = accent;
    t.accent_hover = shades.light1;
    t.accent_pressed = shades.dark1;
    t.accent_text = AutoAccentText(accent);
    t.danger = HexColor(0xC42B1C);
    t.danger_hover = WithAlpha(HexColor(0xC42B1C), 0.85f);

    if (dark) {
        t.bg = HexColor(0x1A1A1A);
        t.text = HexColor(0xFFFFFF);
        t.text_secondary = WithAlpha(HexColor(0xFFFFFF), 0.6063f);
        t.text_disabled = WithAlpha(HexColor(0xFFFFFF), 0.40f);

        t.fill_hover = WithAlpha(HexColor(0xFFFFFF), 0.08f);
        t.fill_pressed = WithAlpha(HexColor(0xFFFFFF), 0.06f);
        t.fill_selected = WithAlpha(accent, 0.15f);
        t.fill_input = WithAlpha(HexColor(0xFFFFFF), 0.0605f);
        t.fill_input_hover = WithAlpha(HexColor(0xFFFFFF), 0.0837f);
        t.fill_input_focus = WithAlpha(HexColor(0x1E1E1E), 0.70f);
        t.fill_input_disabled = WithAlpha(HexColor(0xFFFFFF), 0.0419f);

        t.stroke_card = WithAlpha(HexColor(0xFFFFFF), 0.045f);
        t.stroke_divider = WithAlpha(HexColor(0xFFFFFF), 0.06f);
        t.stroke_input_bottom = WithAlpha(HexColor(0xFFFFFF), 0.5442f);

        t.surface_flyout = HexColor(0x2B2B2B);
        t.surface_pane_inactive = WithAlpha(HexColor(0xFFFFFF), 0.72f);

        t.header_bg = HexColor(0x1E1E1E);
        t.header_sep = WithAlpha(HexColor(0xFFFFFF), 0.08f);
        t.hover_bg = t.fill_hover;
        t.selection_bg = t.fill_selected;
        t.scrollbar_thumb = WithAlpha(HexColor(0xFFFFFF), 0.35f);
        t.tab_bg = HexColor(0x1E1E1E);
        t.tab_active_bg = HexColor(0x2C2C2C);
        t.address_bg = HexColor(0x202020);
        t.status_bg = HexColor(0x151515);
        t.border = WithAlpha(HexColor(0xFFFFFF), 0.06f);
        t.icon_folder = HexColor(0xFFCD70);
        t.icon_file = HexColor(0x78B0E8);
        t.fps_bg = WithAlpha(HexColor(0x000000), 0.60f);
        t.fps_text = HexColor(0xFFFFFF);
    } else {
        t.bg = HexColor(0xF5F5F5);
        t.text = HexColor(0x1A1A1A);
        t.text_secondary = WithAlpha(HexColor(0x000000), 0.6063f);
        t.text_disabled = WithAlpha(HexColor(0x000000), 0.36f);

        t.fill_hover = WithAlpha(HexColor(0x000000), 0.05f);
        t.fill_pressed = WithAlpha(HexColor(0x000000), 0.03f);
        t.fill_selected = WithAlpha(accent, 0.12f);
        t.fill_input = WithAlpha(HexColor(0xFFFFFF), 0.70f);
        t.fill_input_hover = WithAlpha(HexColor(0xF9F9F9), 0.50f);
        t.fill_input_focus = HexColor(0xFFFFFF);
        t.fill_input_disabled = WithAlpha(HexColor(0xF9F9F9), 0.30f);

        t.stroke_card = WithAlpha(HexColor(0x000000), 0.028f);
        t.stroke_divider = WithAlpha(HexColor(0x000000), 0.045f);
        t.stroke_input_bottom = WithAlpha(HexColor(0x000000), 0.392f);

        t.surface_flyout = HexColor(0xFBFBFB);
        t.surface_pane_inactive = WithAlpha(HexColor(0x000000), 0.61f);

        t.header_bg = HexColor(0xFAFAFA);
        t.header_sep = WithAlpha(HexColor(0x000000), 0.06f);
        t.hover_bg = t.fill_hover;
        t.selection_bg = t.fill_selected;
        t.scrollbar_thumb = WithAlpha(HexColor(0x000000), 0.30f);
        t.tab_bg = HexColor(0xEEEEEE);
        t.tab_active_bg = HexColor(0xFFFFFF);
        t.address_bg = HexColor(0xFFFFFF);
        t.status_bg = HexColor(0xEEEEEE);
        t.border = WithAlpha(HexColor(0x000000), 0.045f);
        t.icon_folder = HexColor(0xF5B041);
        t.icon_file = HexColor(0x2E86DE);
        t.fps_bg = WithAlpha(HexColor(0x000000), 0.55f);
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
    t.fill_input = sys(COLOR_WINDOW);
    t.fill_input_hover = sys(COLOR_WINDOW);
    t.fill_input_focus = sys(COLOR_WINDOW);
    t.fill_input_disabled = sys(COLOR_BTNFACE);

    t.stroke_card = sys(COLOR_WINDOWFRAME);
    t.stroke_divider = sys(COLOR_WINDOWFRAME);
    t.stroke_input_bottom = sys(COLOR_HIGHLIGHT);

    t.surface_flyout = sys(COLOR_WINDOW);
    t.surface_pane_inactive = sys(COLOR_GRAYTEXT);

    t.header_bg = sys(COLOR_WINDOW);
    t.header_sep = sys(COLOR_WINDOWFRAME);
    t.hover_bg = t.fill_hover;
    t.selection_bg = t.fill_selected;
    t.scrollbar_thumb = sys(COLOR_WINDOWTEXT);
    t.tab_bg = sys(COLOR_BTNFACE);
    t.tab_active_bg = sys(COLOR_WINDOW);
    t.address_bg = sys(COLOR_WINDOW);
    t.status_bg = sys(COLOR_BTNFACE);
    t.border = sys(COLOR_WINDOWFRAME);
    t.icon_folder = sys(COLOR_WINDOWTEXT);
    t.icon_file = sys(COLOR_WINDOWTEXT);
    t.fps_bg = sys(COLOR_WINDOWTEXT);
    t.fps_text = sys(COLOR_WINDOW);
    return t;
}

} // namespace pulse::ui
