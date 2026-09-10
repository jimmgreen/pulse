#include "edit_host.h"
// color_picker.cpp — QFluent DropDownColorPickerButton popup replica.
//
// 1:1 recreation of QFluentKit's DropDownColorPickerButton popup, rendered
// with Direct2D on a WS_EX_LAYERED window (FluentMenu surface pattern):
//   - 256x256 hue/saturation square (CPU-baked bitmap, V=255), rounded 5.6,
//     inner stroke rgba(0,0,0,15), picker ring r=8/w=3 colored by the
//     QFluent rule (black when s>153 or 40<h<180, else #FFFDFE).
//   - Brightness slider + R/G/B/A (or H/S/V/A) channel sliders with
//     per-channel groove gradients (ColorChannelSlider geometry: groove 10px,
//     radius 5, 11px side insets) and SliderHandle thumbs (outer r=11
//     #454545/white with 1px ring, inner accent dot r=5, 6.5 on hover).
//   - RGB/HSV combo (shared FluentMenu dropdown), lowercase #aarrggbb hex
//     field, native EDIT overlays for text input (FluentMenu filter-edit
//     pattern), separator, transparent Accept/Close glyph buttons.
//   - 120ms fade + 10px horizontal slide, 170ms OutCubic (QFluent execAt).
// Card metrics (DIP): width 292, inner 256, margins 18/14/8, spacing 10.
#include "color_picker.h"

#include "fluent_components.h"
#include "fluent_menu.h"
#include "ui_compositor.h"
#include "lumatext_renderer.h"
#include "typography.h"

#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <d2d1effects.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "uxtheme.lib")

namespace pulse::ui {

HsvColor HsvFromRgb(uint32_t rgb) noexcept {
    const int r = static_cast<int>((rgb >> 16) & 0xff);
    const int g = static_cast<int>((rgb >> 8) & 0xff);
    const int b = static_cast<int>(rgb & 0xff);
    const int mx = (std::max)({r, g, b});
    const int mn = (std::min)({r, g, b});
    const int d = mx - mn;
    HsvColor out;
    out.v = mx;
    out.s = mx == 0 ? 0 : (d * 255 + mx / 2) / mx;
    if (d == 0) return out;
    double h;
    if (mx == r) h = 60.0 * std::fmod(static_cast<double>(g - b) / d, 6.0);
    else if (mx == g) h = 60.0 * (static_cast<double>(b - r) / d + 2.0);
    else h = 60.0 * (static_cast<double>(r - g) / d + 4.0);
    if (h < 0.0) h += 360.0;
    out.h = std::clamp(static_cast<int>(std::lround(h)), 0, 359);
    return out;
}

uint32_t RgbFromHsv(int h, int s, int v) noexcept {
    h = ((h % 360) + 360) % 360;
    s = std::clamp(s, 0, 255);
    v = std::clamp(v, 0, 255);
    const double hf = h / 60.0;
    const double c = static_cast<double>(v) * s / 255.0;
    const double x = c * (1.0 - std::abs(std::fmod(hf, 2.0) - 1.0));
    const double m = v - c;
    double r = 0, g = 0, b = 0;
    if (hf < 1) { r = c; g = x; }
    else if (hf < 2) { r = x; g = c; }
    else if (hf < 3) { g = c; b = x; }
    else if (hf < 4) { g = x; b = c; }
    else if (hf < 5) { r = x; b = c; }
    else { r = c; b = x; }
    auto byte = [](double val) {
        return static_cast<uint32_t>(std::clamp(static_cast<long>(std::lround(val)), 0L, 255L));
    };
    return (byte(r + m) << 16) | (byte(g + m) << 8) | byte(b + m);
}

bool ParseHexColor(const wchar_t* text, uint32_t& argb_out) noexcept {
    if (!text) return false;
    if (*text == L'#') ++text;
    const size_t len = wcslen(text);
    if (len != 6 && len != 8) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < len; ++i) {
        const wchar_t ch = text[i];
        uint32_t d;
        if (ch >= L'0' && ch <= L'9') d = static_cast<uint32_t>(ch - L'0');
        else if (ch >= L'a' && ch <= L'f') d = static_cast<uint32_t>(ch - L'a') + 10;
        else if (ch >= L'A' && ch <= L'F') d = static_cast<uint32_t>(ch - L'A') + 10;
        else return false;
        v = (v << 4) | d;
    }
    if (len == 6) v |= 0xFF000000u;
    argb_out = v;
    return true;
}

std::wstring FormatHexColor(uint32_t argb, bool with_alpha) {
    wchar_t buf[16]{};
    if (with_alpha) swprintf_s(buf, L"#%08x", argb);
    else swprintf_s(buf, L"#%06x", argb & 0x00FFFFFFu);
    return buf;
}

namespace {

// d2d1.lib does not export the effect CLSIDs; define the shadow one locally
// (CLSID_D2D1Shadow from d2d1effects.h), same as fluent_menu.cpp.
constexpr GUID kShadowEffectClsid = { 0xC67EA361, 0x1863, 0x4e69,
    { 0x89, 0xDB, 0x69, 0x5D, 0x3E, 0x9D, 0x5B, 0x53 } };

constexpr wchar_t kClassName[] = L"PulseFluentColorPicker";

// Card metrics in DIPs (QFluent DropDownColorPickerPopup layout).
constexpr float kCardW = 292.0f;
constexpr float kMarginH = 18.0f;
constexpr float kSpacing = 10.0f;
constexpr float kHueSize = 256.0f;
constexpr float kBrightH = 24.0f;
constexpr float kRowH = 36.0f;
constexpr float kComboW = 84.0f;
constexpr float kEditW = 78.0f;
constexpr float kLabelW = 12.0f;
constexpr float kCardH = 14.0f + kHueSize + kSpacing + kBrightH + kSpacing + kRowH
    + kSpacing + (3.0f * kRowH + 2.0f * kSpacing) + kSpacing + kRowH
    + kSpacing + 1.0f + kSpacing + kRowH + 8.0f; // 599
constexpr int kShadow = FluentMenu::kShadowMargin;

// Hit parts.
constexpr int kPartNone = 0;
constexpr int kPartHue = 1;
constexpr int kPartBright = 2;
constexpr int kPartCh0 = 3;   // 3..6 = channel rows
constexpr int kPartCombo = 7;
constexpr int kPartOk = 8;
constexpr int kPartCancel = 9;

struct EditHook {
    struct State* s = nullptr;
    int index = 0; // 0..3 channels, 4 = hex
};

struct State {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    Compositor* compositor = nullptr;
    FluentMenu* menu = nullptr;
    float scale = 1.0f;
    bool dark = true;
    D2D1_COLOR_F accent{};
    Theme theme{};
    fluent::Painter painter;

    uint32_t argb = 0xFF000000u; // source of truth
    HsvColor hsv{};
    bool hsv_mode = false;
    bool syncing = false;
    bool done = false;
    bool accepted = false;

    int base_x = 0;
    int base_y = 0;
    int present_dx = 0;
    int win_w = 0;
    int win_h = 0;

    // Card content rects, window pixels (shadow margin included).
    D2D1_RECT_F r_hue{};
    D2D1_RECT_F r_bright{};
    D2D1_RECT_F r_combo{};
    D2D1_RECT_F r_hex{};
    D2D1_RECT_F r_ch_edit[4]{};
    D2D1_RECT_F r_ch_slider[4]{};
    D2D1_RECT_F r_ch_label[4]{};
    D2D1_RECT_F r_sep{};
    D2D1_RECT_F r_ok{};
    D2D1_RECT_F r_cancel{};

    HWND edits[5]{};
    std::array<EditHook, 5> hooks{};
    HFONT font = nullptr;
    HBRUSH edit_brush = nullptr;
    int line_h = 16;
    int focus_edit = -1;
    bool edits_shown = false;

    int hot = kPartNone;      // hovered part
    int drag = kPartNone;     // captured part
    bool press_ok = false;
    bool press_cancel = false;
    bool press_combo = false;
    bool tracking_leave = false;

    HDC mem_dc = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    int dib_w = 0;
    int dib_h = 0;
    ComPtr<ID2D1Bitmap1> hue_bmp;
    int hue_px = 0;
};

int Red(uint32_t argb) { return static_cast<int>((argb >> 16) & 0xff); }
int Green(uint32_t argb) { return static_cast<int>((argb >> 8) & 0xff); }
int Blue(uint32_t argb) { return static_cast<int>(argb & 0xff); }
int Alpha(uint32_t argb) { return static_cast<int>((argb >> 24) & 0xff); }

uint32_t WithRed(uint32_t c, int v) { return (c & 0xFF00FFFFu) | (static_cast<uint32_t>(v) << 16); }
uint32_t WithGreen(uint32_t c, int v) { return (c & 0xFFFF00FFu) | (static_cast<uint32_t>(v) << 8); }
uint32_t WithBlue(uint32_t c, int v) { return (c & 0xFFFFFF00u) | static_cast<uint32_t>(v); }
uint32_t WithAlpha8(uint32_t c, int v) { return (c & 0x00FFFFFFu) | (static_cast<uint32_t>(v) << 24); }

int ChannelMax(const State& s, int ch) {
    if (ch == 3) return 255;
    if (s.hsv_mode && ch == 0) return 359;
    return 255;
}

int ChannelValue(const State& s, int ch) {
    if (ch == 3) return Alpha(s.argb);
    if (!s.hsv_mode) return ch == 0 ? Red(s.argb) : ch == 1 ? Green(s.argb) : Blue(s.argb);
    return ch == 0 ? s.hsv.h : ch == 1 ? s.hsv.s : s.hsv.v;
}

void SetChannel(State& s, int ch, int value) {
    value = std::clamp(value, 0, ChannelMax(s, ch));
    if (ch == 3) {
        s.argb = WithAlpha8(s.argb, value);
        return;
    }
    if (!s.hsv_mode) {
        if (ch == 0) s.argb = WithRed(s.argb, value);
        else if (ch == 1) s.argb = WithGreen(s.argb, value);
        else s.argb = WithBlue(s.argb, value);
        s.hsv = HsvFromRgb(s.argb & 0x00FFFFFFu);
    } else {
        if (ch == 0) s.hsv.h = value;
        else if (ch == 1) s.hsv.s = value;
        else s.hsv.v = value;
        s.argb = WithAlpha8(RgbFromHsv(s.hsv.h, s.hsv.s, s.hsv.v), Alpha(s.argb));
    }
}

const wchar_t* ChannelLabel(const State& s, int ch) {
    if (ch == 3) return L"A";
    if (!s.hsv_mode) return ch == 0 ? L"R" : ch == 1 ? L"G" : L"B";
    return ch == 0 ? L"H" : ch == 1 ? L"S" : L"V";
}

float Px(const State& s, float dip) { return dip * s.scale; }

bool Contains(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

void Layout(State& s) {
    const float m = static_cast<float>(kShadow);
    auto rect = [&](float l, float t, float r, float b) {
        return D2D1::RectF(m + Px(s, l), m + Px(s, t), m + Px(s, r), m + Px(s, b));
    };
    const float x0 = kMarginH;
    const float x1 = kMarginH + kHueSize;
    float y = 14.0f;
    s.r_hue = rect(x0, y, x1, y + kHueSize);
    y += kHueSize + kSpacing;
    s.r_bright = rect(x0, y, x1, y + kBrightH);
    y += kBrightH + kSpacing;
    s.r_combo = rect(x0, y, x0 + kComboW, y + kRowH);
    s.r_hex = rect(x0 + kComboW + kSpacing, y, x1, y + kRowH);
    y += kRowH + kSpacing;
    for (int i = 0; i < 4; ++i) {
        const float ry = y + i * (kRowH + kSpacing);
        s.r_ch_label[i] = rect(x0, ry, x0 + kLabelW, ry + kRowH);
        s.r_ch_edit[i] = rect(x0 + kLabelW + kSpacing, ry,
                              x0 + kLabelW + kSpacing + kEditW, ry + kRowH);
        s.r_ch_slider[i] = rect(x0 + kLabelW + kSpacing + kEditW + kSpacing, ry, x1, ry + kRowH);
    }
    y += 4.0f * kRowH + 3.0f * kSpacing + kSpacing;
    s.r_sep = rect(x0, y, x1, y + 1.0f);
    y += 1.0f + kSpacing;
    const float half = (x1 - x0) / 2.0f;
    s.r_ok = rect(x0, y, x0 + half, y + kRowH);
    s.r_cancel = rect(x0 + half, y, x1, y + kRowH);
    s.win_w = static_cast<int>(std::lround(Px(s, kCardW))) + kShadow * 2;
    s.win_h = static_cast<int>(std::lround(Px(s, kCardH))) + kShadow * 2;
}

// Channel slider groove (ColorChannelSlider): 10px tall, radius 5, 11px side
// insets so the r=10 handle stays inside the slot.
D2D1_RECT_F GrooveRect(const State& s, const D2D1_RECT_F& slot) {
    const float inset = Px(s, 11.0f);
    const float half = Px(s, 5.0f);
    const float cy = (slot.top + slot.bottom) * 0.5f;
    return D2D1::RectF(slot.left + inset, cy - half, slot.right - inset, cy + half);
}

// Brightness groove (color_dialog.qss): 12px tall, radius 6, full slot width.
D2D1_RECT_F BrightGrooveRect(const State& s, const D2D1_RECT_F& slot) {
    const float half = Px(s, 6.0f);
    const float cy = (slot.top + slot.bottom) * 0.5f;
    return D2D1::RectF(slot.left, cy - half, slot.right, cy + half);
}

float GrooveT(const State& s, const D2D1_RECT_F& slot, float x) {
    const D2D1_RECT_F g = GrooveRect(s, slot);
    if (g.right <= g.left) return 0.0f;
    return std::clamp((x - g.left) / (g.right - g.left), 0.0f, 1.0f);
}

// ClickableSlider maps the click against the full widget width.
float BrightT(const D2D1_RECT_F& slot, float x) {
    if (slot.right <= slot.left) return 0.0f;
    return std::clamp((x - slot.left) / (slot.right - slot.left), 0.0f, 1.0f);
}

// Signed distance to a rounded rect centered on (0,0); for corner coverage.
float RoundedSdf(float px, float py, float hw, float hh, float r) {
    const float qx = std::fabs(px) - (hw - r);
    const float qy = std::fabs(py) - (hh - r);
    const float ax = (std::max)(qx, 0.0f);
    const float ay = (std::max)(qy, 0.0f);
    return std::sqrt(ax * ax + ay * ay) + (std::min)((std::max)(qx, qy), 0.0f) - r;
}

bool EnsureHueBitmap(State& s, ID2D1DeviceContext* dc) {
    const int px = (std::max)(16, static_cast<int>(std::lround(Px(s, kHueSize))));
    if (s.hue_bmp.get() && s.hue_px == px) return true;
    std::vector<uint32_t> buf(static_cast<size_t>(px) * px);
    const float radius = Px(s, 5.6f);
    const float half = px * 0.5f;
    for (int y = 0; y < px; ++y) {
        const int sat = static_cast<int>(std::lround((1.0 - static_cast<double>(y) / (px - 1)) * 255.0));
        for (int x = 0; x < px; ++x) {
            const int h = static_cast<int>(x * 359.0 / (px - 1));
            const uint32_t rgb = RgbFromHsv(h, sat, 255);
            const float d = RoundedSdf(x + 0.5f - half, y + 0.5f - half, half, half, radius);
            const float cov = std::clamp(0.5f - d, 0.0f, 1.0f);
            const uint32_t a = static_cast<uint32_t>(std::lround(cov * 255.0f));
            const uint32_t r = static_cast<uint32_t>(std::lround(Red(rgb) * cov));
            const uint32_t g = static_cast<uint32_t>(std::lround(Green(rgb) * cov));
            const uint32_t b = static_cast<uint32_t>(std::lround(Blue(rgb) * cov));
            buf[static_cast<size_t>(y) * px + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    const D2D1_SIZE_U size{ static_cast<UINT32>(px), static_cast<UINT32>(px) };
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    s.hue_bmp.reset();
    if (FAILED(dc->CreateBitmap(size, buf.data(), static_cast<UINT32>(px * 4), props,
                                &s.hue_bmp))) {
        return false;
    }
    s.hue_px = px;
    return true;
}

struct StopList {
    std::array<D2D1_GRADIENT_STOP, 7> stops{};
    int count = 0;
    void add(float pos, D2D1_COLOR_F c) {
        stops[static_cast<size_t>(count)] = D2D1::GradientStop(pos, c);
        ++count;
    }
};

D2D1_COLOR_F RgbColor(uint32_t rgb, float a = 1.0f) { return HexColor(rgb, a); }

StopList ChannelStops(const State& s, int ch) {
    StopList out;
    const uint32_t rgb = s.argb & 0x00FFFFFFu;
    if (ch == 3) {
        // Premultiplied endpoints: the left stop fades over the checkerboard.
        out.add(0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        out.add(1.0f, RgbColor(rgb));
        return out;
    }
    if (!s.hsv_mode) {
        uint32_t left = rgb, right = rgb;
        if (ch == 0) { left = WithRed(left, 0); right = WithRed(right, 255); }
        else if (ch == 1) { left = WithGreen(left, 0); right = WithGreen(right, 255); }
        else { left = WithBlue(left, 0); right = WithBlue(right, 255); }
        out.add(0.0f, RgbColor(left));
        out.add(1.0f, RgbColor(right));
        return out;
    }
    if (ch == 0) {
        for (int i = 0; i <= 6; ++i) {
            out.add(i / 6.0f, RgbColor(RgbFromHsv(i * 60, s.hsv.s, s.hsv.v)));
        }
    } else if (ch == 1) {
        out.add(0.0f, RgbColor(RgbFromHsv(s.hsv.h, 0, s.hsv.v)));
        out.add(1.0f, RgbColor(RgbFromHsv(s.hsv.h, 255, s.hsv.v)));
    } else {
        out.add(0.0f, RgbColor(RgbFromHsv(s.hsv.h, s.hsv.s, 0)));
        out.add(1.0f, RgbColor(RgbFromHsv(s.hsv.h, s.hsv.s, 255)));
    }
    return out;
}

void FillGroove(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, const StopList& list) {
    ComPtr<ID2D1GradientStopCollection> coll;
    if (FAILED(dc->CreateGradientStopCollection(list.stops.data(),
                                                static_cast<UINT32>(list.count),
                                                D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
                                                &coll))) {
        return;
    }
    ComPtr<ID2D1LinearGradientBrush> brush;
    const auto props = D2D1::LinearGradientBrushProperties(
        D2D1::Point2F(r.left, 0.0f), D2D1::Point2F(r.right, 0.0f));
    if (FAILED(dc->CreateLinearGradientBrush(props, coll.get(), &brush))) return;
    const float radius = (r.bottom - r.top) * 0.5f;
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.get());
}

void FillChecker(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float cell) {
    // QFluent checkerboardPixmap: #CCC / white cells.
    ComPtr<ID2D1SolidColorBrush> a, b;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.8f, 0.8f, 0.8f, 1.0f), &a);
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &b);
    dc->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dc->FillRectangle(r, a.get());
    int row = 0;
    for (float y = r.top; y < r.bottom; y += cell, ++row) {
        int col = row & 1;
        for (float x = r.left + (col ? cell : 0.0f); x < r.right; x += cell * 2.0f, col += 2) {
            const D2D1_RECT_F tile = D2D1::RectF(
                x, y, (std::min)(x + cell, r.right), (std::min)(y + cell, r.bottom));
            dc->FillRectangle(tile, b.get());
        }
    }
    dc->PopAxisAlignedClip();
}

// Channel thumb (QFluent SliderHandle, 22px widget): outer r=10 circle
// (#454545 dark / white light) with a 1px ring, inner accent dot r=5
// (6.5 hovered, 4 pressed).
void DrawChannelThumb(State& s, ID2D1DeviceContext* dc, const D2D1_RECT_F& slot,
                      float t, bool hovered, bool pressed) {
    const D2D1_RECT_F g = GrooveRect(s, slot);
    const float cx = g.left + t * (g.right - g.left);
    const float cy = (g.top + g.bottom) * 0.5f;
    ComPtr<ID2D1SolidColorBrush> fill, ring, dot;
    dc->CreateSolidColorBrush(s.dark ? D2D1::ColorF(0.2706f, 0.2706f, 0.2706f, 1.0f) // #454545
                                     : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &fill);
    const float ring_a = (s.dark ? 90.0f : 25.0f) / 255.0f;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, ring_a), &ring);
    dc->CreateSolidColorBrush(s.accent, &dot);
    const float outer = Px(s, 10.0f);
    const auto ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), outer, outer);
    dc->FillEllipse(ellipse, fill.get());
    dc->DrawEllipse(ellipse, ring.get(), Px(s, 1.0f));
    const float inner = Px(s, pressed ? 4.0f : hovered ? 6.5f : 5.0f);
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), inner, inner), dot.get());
}

// Brightness thumb (color_dialog.qss ::handle, 16px): 1px border ring, then a
// solid ring, then the contrasting center dot (white on dark, black on
// light) at ~55% radius — the QSS radial gradient with hard stops.
void DrawBrightThumb(State& s, ID2D1DeviceContext* dc, const D2D1_RECT_F& slot,
                     float t) {
    const float r = Px(s, 8.0f);
    const float cx = slot.left + r + t * (slot.right - slot.left - 2.0f * r);
    const float cy = (slot.top + slot.bottom) * 0.5f;
    ComPtr<ID2D1SolidColorBrush> border, ring, dot;
    dc->CreateSolidColorBrush(s.dark ? D2D1::ColorF(0.2157f, 0.2157f, 0.2157f, 1.0f) // #373737
                                     : D2D1::ColorF(0.8706f, 0.8706f, 0.8706f, 1.0f), // #DEDEDE
                              &border);
    dc->CreateSolidColorBrush(s.dark ? D2D1::ColorF(0.2706f, 0.2706f, 0.2706f, 1.0f)
                                     : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &ring);
    dc->CreateSolidColorBrush(s.dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)
                                     : D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &dot);
    const auto c = D2D1::Point2F(cx, cy);
    dc->FillEllipse(D2D1::Ellipse(c, r, r), border.get());
    const float inner = r - Px(s, 1.0f);
    dc->FillEllipse(D2D1::Ellipse(c, inner, inner), ring.get());
    const float core = r * 0.55f;
    dc->FillEllipse(D2D1::Ellipse(c, core, core), dot.get());
}

// Popup field (color_picker.qss): dark #2D2D2D / light white, 1px border,
// radius 5, focus replaces the bottom edge with a 2px accent bar. Light
// theme's resting bottom border is darker than the other edges.
void DrawPopupField(State& s, ID2D1DeviceContext* dc, const D2D1_RECT_F& rc,
                    bool focused, bool hovered) {
    const float radius = Px(s, 5.0f);
    D2D1_COLOR_F fill = s.dark ? D2D1::ColorF(0.1765f, 0.1765f, 0.1765f, 1.0f) // #2D2D2D
                               : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
    if (hovered) {
        fill = s.dark ? D2D1::ColorF(0.2471f, 0.2471f, 0.2471f, 1.0f)          // ~#3F3F3F
                      : D2D1::ColorF(0.9804f, 0.9804f, 0.9804f, 1.0f);
    }
    const D2D1_COLOR_F border =
        s.dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.053f)
               : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f);
    s.painter.FillRoundedRect(rc, radius, fill);
    s.painter.StrokeRoundedRect(rc, radius, border, Px(s, 1.0f));

    ComPtr<ID2D1SolidColorBrush> bar;
    if (focused) {
        dc->CreateSolidColorBrush(s.accent, &bar);
        const float h = Px(s, 2.0f);
        dc->PushAxisAlignedClip(D2D1::RectF(rc.left, rc.bottom - h, rc.right, rc.bottom),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(rc.left, rc.bottom - Px(s, 10.0f), rc.right,
                                          rc.bottom),
                              radius, radius),
            bar.get());
        dc->PopAxisAlignedClip();
    } else if (!s.dark) {
        dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.20f), &bar);
        const float h = Px(s, 1.0f);
        dc->PushAxisAlignedClip(D2D1::RectF(rc.left + 1.0f, rc.bottom - h, rc.right - 1.0f,
                                            rc.bottom),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rc, radius, radius), bar.get(),
                                 Px(s, 1.0f));
        dc->PopAxisAlignedClip();
    }
}

int HitTest(const State& s, float x, float y) {
    if (Contains(s.r_hue, x, y)) return kPartHue;
    if (Contains(s.r_bright, x, y)) return kPartBright;
    for (int i = 0; i < 4; ++i) {
        if (Contains(s.r_ch_slider[i], x, y)) return kPartCh0 + i;
    }
    if (Contains(s.r_combo, x, y)) return kPartCombo;
    if (Contains(s.r_ok, x, y)) return kPartOk;
    if (Contains(s.r_cancel, x, y)) return kPartCancel;
    return kPartNone;
}

bool Render(State& s) {
    ID2D1DeviceContext* dc = s.compositor ? s.compositor->Dc() : nullptr;
    if (!dc || s.win_w <= 0 || s.win_h <= 0) return false;
    const D2D1_SIZE_U size{ static_cast<UINT32>(s.win_w), static_cast<UINT32>(s.win_h) };
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> bmp;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &bmp))) return false;

    ComPtr<ID2D1Image> old_target;
    dc->GetTarget(&old_target);
    dc->SetTarget(bmp.get());
    // Grayscale text AA: ClearType coverage survives premultiplied readback as
    // ghosted glyph fringes under UpdateLayeredWindow (same as FluentMenu).
    const D2D1_TEXT_ANTIALIAS_MODE old_text_aa = dc->GetTextAntialiasMode();
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    const auto card = D2D1::RectF(static_cast<float>(kShadow), static_cast<float>(kShadow),
                                  static_cast<float>(kShadow) + Px(s, kCardW),
                                  static_cast<float>(kShadow) + Px(s, kCardH));
    const float radius = Px(s, s.theme.radius_flyout);

    // Drop shadow from the card silhouette (CLSID_D2D1Shadow, like the menu).
    ComPtr<ID2D1CommandList> card_list;
    if (SUCCEEDED(dc->CreateCommandList(&card_list))) {
        dc->SetTarget(card_list.get());
        ComPtr<ID2D1SolidColorBrush> black;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1.0f), &black);
        dc->FillRoundedRectangle(D2D1::RoundedRect(card, radius, radius), black.get());
        card_list->Close();
        dc->SetTarget(bmp.get());
        ComPtr<ID2D1Effect> shadow;
        if (SUCCEEDED(dc->CreateEffect(kShadowEffectClsid, &shadow))) {
            shadow->SetInput(0, card_list.get());
            shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 7.0f);
            shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0.0f, 0.0f, 0.0f, 0.36f));
            dc->DrawImage(shadow.get(), D2D1::Point2F(0.0f, 4.0f * s.scale));
        }
    }

    if (!s.painter.BeginFrame(s.theme, IsHighContrast())) {
        dc->EndDraw();
        dc->SetTextAntialiasMode(old_text_aa);
        dc->SetTarget(old_target.get());
        return false;
    }
    s.painter.DrawMenuSurface(card);

    // Hue/saturation square + inner stroke + picker ring.
    if (EnsureHueBitmap(s, dc)) {
        dc->DrawBitmap(s.hue_bmp.get(), s.r_hue, 1.0f,
                       D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }
    s.painter.StrokeRoundedRect(s.r_hue, Px(s, 5.6f),
                                D2D1::ColorF(0.0f, 0.0f, 0.0f, 15.0f / 255.0f),
                                Px(s, 2.4f));
    {
        const float rx = s.r_hue.left +
            static_cast<float>(s.hsv.h) / 359.0f * (s.r_hue.right - s.r_hue.left - 1.0f);
        const float ry = s.r_hue.top +
            static_cast<float>(255 - s.hsv.s) / 255.0f * (s.r_hue.bottom - s.r_hue.top - 1.0f);
        const bool dark_ring = s.hsv.s > 153 || (s.hsv.h > 40 && s.hsv.h < 180);
        ComPtr<ID2D1SolidColorBrush> ring;
        dc->CreateSolidColorBrush(dark_ring ? D2D1::ColorF(0, 0, 0, 1.0f)
                                            : D2D1::ColorF(1.0f, 0.9922f, 0.9961f, 1.0f),
                                  &ring);
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(rx, ry), Px(s, 8.0f), Px(s, 8.0f)),
                        ring.get(), Px(s, 3.0f));
    }

    // Brightness slider (color_dialog.qss geometry: 12px groove, 16px thumb).
    {
        StopList stops;
        stops.add(0.0f, RgbColor(RgbFromHsv(s.hsv.h, s.hsv.s, 0)));
        stops.add(1.0f, RgbColor(RgbFromHsv(s.hsv.h, s.hsv.s, 255)));
        FillGroove(dc, BrightGrooveRect(s, s.r_bright), stops);
        DrawBrightThumb(s, dc, s.r_bright, s.hsv.v / 255.0f);
    }

    IDWriteTextFormat* body = s.compositor->TextFormat();

    // Mode combo (ComboBox look: field frame + text + chevron).
    {
        DrawPopupField(s, dc, s.r_combo, false, s.hot == kPartCombo);
        const float pad = Px(s, 12.0f);
        const D2D1_RECT_F text_rc = D2D1::RectF(
            s.r_combo.left + pad, s.r_combo.top, s.r_combo.right - Px(s, 26.0f),
            s.r_combo.bottom);
        s.painter.DrawText(s.hsv_mode ? L"HSV" : L"RGB", text_rc, body, s.theme.text);
        const float g = Px(s, 8.0f);
        const float cx = s.r_combo.right - Px(s, 14.0f);
        const float cy = (s.r_combo.top + s.r_combo.bottom) * 0.5f;
        s.painter.DrawGlyph(L"\xE70D",
                            D2D1::RectF(cx - g, cy - g, cx + g, cy + g),
                            s.theme.text_secondary);
    }

    // Hex + channel fields: D2D draws frame and value text; once the native
    // edits appear they cover exactly the text line with a matching solid bg.
    auto draw_field = [&](const D2D1_RECT_F& rc, const std::wstring& text, int edit_index) {
        DrawPopupField(s, dc, rc, s.focus_edit == edit_index, false);
        const float pad = Px(s, 12.0f);
        const D2D1_RECT_F text_rc =
            D2D1::RectF(rc.left + pad, rc.top, rc.right - pad, rc.bottom);
        s.painter.DrawText(text, text_rc, body, s.theme.text);
    };
    draw_field(s.r_hex, FormatHexColor(s.argb, true), 4);
    for (int i = 0; i < 4; ++i) {
        s.painter.DrawText(ChannelLabel(s, i), s.r_ch_label[i], body, s.theme.text);
        draw_field(s.r_ch_edit[i], std::to_wstring(ChannelValue(s, i)), i);
    }

    // Channel sliders; the alpha groove is layer-clipped so the checkerboard
    // respects the rounded groove ends.
    for (int i = 0; i < 4; ++i) {
        const D2D1_RECT_F groove = GrooveRect(s, s.r_ch_slider[i]);
        if (i == 3) {
            ComPtr<ID2D1Factory> factory;
            dc->GetFactory(&factory);
            ComPtr<ID2D1RoundedRectangleGeometry> geo;
            const float gr = (groove.bottom - groove.top) * 0.5f;
            if (factory.get() &&
                SUCCEEDED(factory->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(groove, gr, gr), &geo))) {
                dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geo.get(),
                                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                              nullptr);
                FillChecker(dc, groove, Px(s, 4.0f));
                FillGroove(dc, groove, ChannelStops(s, i));
                dc->PopLayer();
            }
        } else {
            FillGroove(dc, groove, ChannelStops(s, i));
        }
        const float t = static_cast<float>(ChannelValue(s, i)) /
                        static_cast<float>(ChannelMax(s, i));
        DrawChannelThumb(s, dc, s.r_ch_slider[i], t, s.hot == kPartCh0 + i,
                         s.drag == kPartCh0 + i);
    }

    // Separator (color_picker.qss: white 8% dark / black 8% light).
    s.painter.FillRoundedRect(s.r_sep, 0.0f,
                              s.dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f)
                                     : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f));

    // Accept / cancel glyph buttons. The hover pill is inset and rounded so
    // it never pokes past the card's rounded bottom corners.
    auto draw_tool = [&](const D2D1_RECT_F& rc, const wchar_t* glyph, bool hovered,
                         bool pressed) {
        if (hovered || pressed) {
            const float inset = Px(s, 3.0f);
            const D2D1_RECT_F pill = D2D1::RectF(rc.left + inset, rc.top + inset,
                                                 rc.right - inset, rc.bottom - inset);
            s.painter.FillRoundedRect(pill, Px(s, 4.5f),
                                      pressed ? s.theme.fill_pressed : s.theme.fill_hover);
        }
        const float g = Px(s, 8.0f);
        const float cx = (rc.left + rc.right) * 0.5f;
        const float cy = (rc.top + rc.bottom) * 0.5f;
        s.painter.DrawGlyph(glyph, D2D1::RectF(cx - g, cy - g, cx + g, cy + g),
                            s.theme.text);
    };
    // CheckMark E73E + Close E8BB are the matched bold pair in Segoe Fluent
    // Icons (Accept E8FB is visibly thinner than Close, so it is not used).
    draw_tool(s.r_ok, L"\xE73E", s.hot == kPartOk, s.press_ok);
    draw_tool(s.r_cancel, L"\xE8BB", s.hot == kPartCancel, s.press_cancel);

    HRESULT hr = dc->EndDraw();
    dc->SetTextAntialiasMode(old_text_aa);
    dc->SetTarget(old_target.get());
    if (FAILED(hr)) return false;

    // GPU -> CPU readback, then into the staging DIB for UpdateLayeredWindow.
    const auto cpu_props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> cpu;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, cpu_props, &cpu))) return false;
    if (FAILED(cpu->CopyFromBitmap(nullptr, bmp.get(), nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;

    if (!s.mem_dc) s.mem_dc = CreateCompatibleDC(nullptr);
    if (s.dib && (s.win_w != 0 && (s.dib_w != s.win_w || s.dib_h != s.win_h))) {
        DeleteObject(s.dib);
        s.dib = nullptr;
    }
    if (!s.dib) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = s.win_w;
        bmi.bmiHeader.biHeight = -s.win_h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        s.dib = CreateDIBSection(s.mem_dc, &bmi, DIB_RGB_COLORS, &s.bits, nullptr, 0);
        if (!s.dib) {
            cpu->Unmap();
            return false;
        }
        s.dib_w = s.win_w;
        s.dib_h = s.win_h;
    }
    for (int row = 0; row < s.win_h; ++row) {
        std::memcpy(static_cast<uint8_t*>(s.bits) + static_cast<size_t>(row) * s.win_w * 4,
                    static_cast<const uint8_t*>(mapped.bits) +
                        static_cast<size_t>(row) * mapped.pitch,
                    static_cast<size_t>(s.win_w) * 4);
    }
    cpu->Unmap();
    return true;
}

void PlaceEdits(State& s);

void Present(State& s, BYTE alpha, int dx) {
    if (!s.dib || !s.bits) return;
    HGDIOBJ old = SelectObject(s.mem_dc, s.dib);
    POINT dst{ s.base_x + dx, s.base_y };
    POINT src{ 0, 0 };
    SIZE sz{ s.win_w, s.win_h };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
    UpdateLayeredWindow(s.hwnd, nullptr, &dst, &sz, s.mem_dc, &src, 0, &blend, ULW_ALPHA);
    SelectObject(s.mem_dc, old);
    s.present_dx = dx;
    if (s.edits_shown) PlaceEdits(s);
}

void Repaint(State& s) {
    if (Render(s)) Present(s, 255, s.present_dx);
}

void SetEditText(State& s, HWND edit, const std::wstring& text) {
    s.syncing = true;
    SetWindowTextW(edit, text.c_str());
    s.syncing = false;
}

void SyncEdits(State& s, int skip_index) {
    for (int i = 0; i < 4; ++i) {
        if (i == skip_index) continue;
        SetEditText(s, s.edits[i], std::to_wstring(ChannelValue(s, i)));
    }
    if (skip_index != 4) SetEditText(s, s.edits[4], FormatHexColor(s.argb, true));
}

void ShowEdits(State& s) {
    if (s.edits_shown) return;
    s.edits_shown = true;
    PlaceEdits(s);
}

void PlaceEdits(State& s) {
    const float pad = Px(s, 12.0f);
    for (int i = 0; i < 5; ++i) {
        if (!s.edits[i]) continue;
        const D2D1_RECT_F& rc = i < 4 ? s.r_ch_edit[i] : s.r_hex;
        const int x = static_cast<int>(std::lround(rc.left + pad));
        const int w = (std::max)(20, static_cast<int>(std::lround(rc.right - rc.left - pad * 2.0f)));
        const int line = (std::min)(s.line_h, static_cast<int>(rc.bottom - rc.top));
        const int y = static_cast<int>(std::lround(rc.top)) +
                      (std::max)(0, static_cast<int>(rc.bottom - rc.top - line) / 2);
        // SWP_NOACTIVATE is essential: an activating SetWindowPos would focus
        // each edit in turn, whose EN_SETFOCUS triggers another repaint and
        // placement — a focus/repaint storm.
        SetWindowPos(s.edits[i], HWND_TOP, x, y, w, line,
                     SWP_NOACTIVATE |
                         (s.edits_shown ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }
}

int EditIndexFromHwnd(const State& s, HWND hwnd) {
    for (int i = 0; i < 5; ++i) {
        if (s.edits[i] == hwnd) return i;
    }
    return -1;
}

void ApplyPoint(State& s, float x, float y) {
    if (s.drag == kPartHue) {
        const float w = s.r_hue.right - s.r_hue.left - 1.0f;
        const float h = s.r_hue.bottom - s.r_hue.top - 1.0f;
        const int hue = static_cast<int>(
            std::clamp((x - s.r_hue.left) / w, 0.0f, 1.0f) * 359.0f);
        const int sat = static_cast<int>(std::lround(
            (1.0f - std::clamp((y - s.r_hue.top) / h, 0.0f, 1.0f)) * 255.0f));
        s.hsv.h = hue;
        s.hsv.s = sat;
        s.argb = WithAlpha8(RgbFromHsv(s.hsv.h, s.hsv.s, s.hsv.v), Alpha(s.argb));
    } else if (s.drag == kPartBright) {
        s.hsv.v = static_cast<int>(std::lround(BrightT(s.r_bright, x) * 255.0f));
        s.argb = WithAlpha8(RgbFromHsv(s.hsv.h, s.hsv.s, s.hsv.v), Alpha(s.argb));
    } else if (s.drag >= kPartCh0 && s.drag < kPartCh0 + 4) {
        const int ch = s.drag - kPartCh0;
        const float t = GrooveT(s, s.r_ch_slider[ch], x);
        SetChannel(s, ch, static_cast<int>(std::lround(t * ChannelMax(s, ch))));
    } else {
        return;
    }
    SyncEdits(s, -1);
    Repaint(s);
}

void FocusEdit(State& s, int index) {
    if (index >= 0 && index < 5 && s.edits[index]) SetFocus(s.edits[index]);
}

void OpenModeCombo(State& s) {
    if (!s.menu) {
        s.hsv_mode = !s.hsv_mode;
        SyncEdits(s, -1);
        Repaint(s);
        return;
    }
    std::vector<FluentMenuItem> items(2);
    items[0].command = 1;
    items[0].text = L"RGB";
    items[0].checked = !s.hsv_mode;
    items[1].command = 2;
    items[1].text = L"HSV";
    items[1].checked = s.hsv_mode;
    POINT pt{ s.base_x + s.present_dx + static_cast<int>(std::lround(s.r_combo.left)),
              s.base_y + static_cast<int>(std::lround(s.r_combo.bottom)) +
                  static_cast<int>(std::lround(4.0f * s.scale)) };
    const int cmd = s.menu->TrackPopup(pt, std::move(items));
    const bool want_hsv = cmd == 2 ? true : cmd == 1 ? false : s.hsv_mode;
    if (want_hsv != s.hsv_mode) {
        s.hsv_mode = want_hsv;
        SyncEdits(s, -1);
        Repaint(s);
    }
}

bool IsOurs(const State& s, HWND hwnd) {
    if (!hwnd) return false;
    if (hwnd == s.hwnd) return true;
    return EditIndexFromHwnd(s, hwnd) >= 0;
}

constexpr UINT_PTR kEditCaretTimer = 71;

bool PaintLumaEditControl(State& s, HWND hwnd, HDC hdc) {
    (void)hdc;
    if (!s.compositor || !s.compositor->LumaTextEnabled()) return false;
    HideCaret(hwnd);
    const D2D1_COLOR_F fg = s.dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f)
                                   : D2D1::ColorF(32.0f / 255.0f, 32.0f / 255.0f, 32.0f / 255.0f);
    const D2D1_COLOR_F bg = s.dark ? D2D1::ColorF(45.0f / 255.0f, 45.0f / 255.0f, 45.0f / 255.0f)
                                   : D2D1::ColorF(1.0f, 1.0f, 1.0f);
    return s.compositor->PresentLumaEdit(hwnd, s.compositor->TextFormat(), fg, bg);
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR,
                          DWORD_PTR ref) {
    auto* hook = reinterpret_cast<EditHook*>(ref);
    State* s = hook ? hook->s : nullptr;
    if (!s) return DefSubclassProc(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
    case WM_CAPTURECHANGED:
        if (s->compositor && s->compositor->LumaTextEnabled()) {
            const LRESULT result = s->compositor->CallLumaEditMouse(
                hwnd, msg, wp, lp, s->compositor->TextFormat());
            if (msg != WM_MOUSEMOVE || GetCapture() == hwnd)
                PaintLumaEditControl(*s, hwnd, nullptr);
            return result;
        }
        break;
    case WM_PAINT: {
        if (!s->compositor || !s->compositor->LumaTextEnabled()) break;
        if (!PaintLumaEditControl(*s, hwnd, nullptr)) {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, s->edit_brush);
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    case WM_SETFOCUS: {
        LRESULT lr = DefSubclassProc(hwnd, msg, wp, lp);
        HideCaret(hwnd);
        SetTimer(hwnd, kEditCaretTimer, GetCaretBlinkTime(), nullptr);
        if (s->compositor && s->compositor->LumaTextEnabled())
            PaintLumaEditControl(*s, hwnd, nullptr);
        else
            InvalidateRect(hwnd, nullptr, FALSE);
        return lr;
    }
    case WM_KILLFOCUS:
        KillTimer(hwnd, kEditCaretTimer);
        break;
    case WM_TIMER:
        if (wp == kEditCaretTimer) {
            if (GetCapture() != hwnd)
                PaintLumaEditControl(*s, hwnd, nullptr);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            s->done = true;
            return 0;
        }
        if (wp == VK_RETURN) {
            s->accepted = true;
            s->done = true;
            return 0;
        }
        if (wp == VK_TAB) {
            const int dir = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
            FocusEdit(*s, (hook->index + dir + 5) % 5);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB) return 0;
        if (hook->index == 4 && wp >= 0x20) {
            const wchar_t ch = static_cast<wchar_t>(wp);
            const bool ok = (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') ||
                            (ch >= L'A' && ch <= L'F') || ch == L'#';
            if (!ok) return 0;
        }
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void OnEditChange(State& s, int index) {
    if (s.syncing) return;
    wchar_t text[32]{};
    GetWindowTextW(s.edits[index], text, ARRAYSIZE(text));
    if (index == 4) {
        uint32_t argb = 0;
        if (ParseHexColor(text, argb)) {
            s.argb = argb;
            s.hsv = HsvFromRgb(argb & 0x00FFFFFFu);
            SyncEdits(s, 4);
            Repaint(s);
        }
        return;
    }
    if (!text[0]) return;
    const int value = _wtoi(text);
    const int clamped = std::clamp(value, 0, ChannelMax(s, index));
    SetChannel(s, index, clamped);
    // Rewriting the sender's text only when clamping changed it keeps the
    // caret stable while typing in-range values.
    if (clamped != value) SetEditText(s, s.edits[index], std::to_wstring(clamped));
    SyncEdits(s, index);
    Repaint(s);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    State* s = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        s = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        s->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }
    if (!s) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLOREDIT: {
        const int index = EditIndexFromHwnd(*s, reinterpret_cast<HWND>(lp));
        if (index < 0) break;
        HDC hdc = reinterpret_cast<HDC>(wp);
        // QSS text colors: white on dark, rgb(32,32,32) on light.
        SetTextColor(hdc, s->dark ? RGB(255, 255, 255) : RGB(32, 32, 32));
        SetBkColor(hdc, s->dark ? RGB(45, 45, 45) : RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(s->edit_brush);
    }
    case WM_COMMAND: {
        const HWND from = reinterpret_cast<HWND>(lp);
        const int index = from ? EditIndexFromHwnd(*s, from) : -1;
        if (index < 0) break;
        if (HIWORD(wp) == EN_CHANGE) OnEditChange(*s, index);
        else if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) {
            s->focus_edit = HIWORD(wp) == EN_SETFOCUS ? index : -1;
            for (HWND e : s->edits) {
                if (e) InvalidateRect(e, nullptr, FALSE);
            }
            Repaint(*s);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const float x = static_cast<float>(GET_X_LPARAM(lp));
        const float y = static_cast<float>(GET_Y_LPARAM(lp));
        const int part = HitTest(*s, x, y);
        SetFocus(hwnd);
        if (part == kPartHue || part == kPartBright ||
            (part >= kPartCh0 && part < kPartCh0 + 4)) {
            s->drag = part;
            SetCapture(hwnd);
            ApplyPoint(*s, x, y);
        } else if (part == kPartOk) {
            s->press_ok = true;
            SetCapture(hwnd);
            Repaint(*s);
        } else if (part == kPartCancel) {
            s->press_cancel = true;
            SetCapture(hwnd);
            Repaint(*s);
        } else if (part == kPartCombo) {
            s->press_combo = true;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float x = static_cast<float>(GET_X_LPARAM(lp));
        const float y = static_cast<float>(GET_Y_LPARAM(lp));
        if (s->drag != kPartNone) {
            ApplyPoint(*s, x, y);
            return 0;
        }
        const int part = HitTest(*s, x, y);
        if (part != s->hot) {
            s->hot = part;
            Repaint(*s);
        }
        if (!s->tracking_leave) {
            s->tracking_leave = true;
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        s->tracking_leave = false;
        if (s->hot != kPartNone) {
            s->hot = kPartNone;
            Repaint(*s);
        }
        return 0;
    case WM_LBUTTONUP: {
        const float x = static_cast<float>(GET_X_LPARAM(lp));
        const float y = static_cast<float>(GET_Y_LPARAM(lp));
        const int part = HitTest(*s, x, y);
        // Snapshot the press state before ReleaseCapture: it fires
        // WM_CAPTURECHANGED synchronously, which clears these flags.
        const bool was_drag = s->drag != kPartNone;
        const bool was_ok = s->press_ok;
        const bool was_cancel = s->press_cancel;
        const bool was_combo = s->press_combo;
        s->drag = kPartNone;
        s->press_ok = s->press_cancel = s->press_combo = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (was_ok && part == kPartOk) {
            s->accepted = true;
            s->done = true;
            return 0;
        }
        if (was_cancel && part == kPartCancel) {
            s->done = true;
            return 0;
        }
        if (was_combo && part == kPartCombo) {
            OpenModeCombo(*s);
            return 0;
        }
        if (was_drag || was_ok || was_cancel) Repaint(*s);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            s->done = true;
            return 0;
        }
        if (wp == VK_RETURN) {
            s->accepted = true;
            s->done = true;
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (s->drag != kPartNone) s->drag = kPartNone;
        s->press_ok = s->press_cancel = s->press_combo = false;
        return 0;
    case WM_CLOSE:
        s->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void DestroySurface(State& s) {
    if (s.dib) DeleteObject(s.dib);
    if (s.mem_dc) DeleteDC(s.mem_dc);
    s.dib = nullptr;
    s.mem_dc = nullptr;
    s.bits = nullptr;
}

void CreateBrushes(State& s) {
    // Native edit bg matches the QSS field fill exactly (solid #2D2D2D dark /
    // white light; no hover/focus variants in color_picker.qss).
    s.edit_brush = CreateSolidBrush(s.dark ? RGB(45, 45, 45) : RGB(255, 255, 255));
}

void CreateEdits(State& s) {
    const int height = -std::lround(14.0f * s.scale);
    s.font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         typography::PreferredTextFamily());
    if (!s.font) {
        s.font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Segoe UI");
    }
    if (s.font) {
        HDC hdc = GetDC(nullptr);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, s.font));
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        SelectObject(hdc, old);
        ReleaseDC(nullptr, hdc);
        s.line_h = (std::max)(1, static_cast<int>(tm.tmHeight));
    }
    for (int i = 0; i < 5; ++i) {
        HWND e = CreateChildEdit(s.hwnd, L"", i < 4 ? ES_NUMBER : 0);
        if (!e) continue;
        SetWindowTheme(e, L"", L"");
        if (!s.compositor || !s.compositor->LumaTextEnabled())
            SetLayeredWindowAttributes(e, 0, 255, LWA_ALPHA);
        if (s.font) SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(s.font), TRUE);
        SendMessageW(e, EM_SETLIMITTEXT, i < 4 ? 3 : 9, 0);
        s.hooks[static_cast<size_t>(i)].s = &s;
        s.hooks[static_cast<size_t>(i)].index = i;
        SetWindowSubclass(e, EditProc, static_cast<UINT_PTR>(i + 1),
                          reinterpret_cast<DWORD_PTR>(&s.hooks[static_cast<size_t>(i)]));
        s.edits[i] = e;
    }
}

bool SavePng(const wchar_t* path, int w, int h, void* bits) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
        return false;
    }
    ComPtr<IWICStream> stream;
    wic->CreateStream(&stream);
    if (!stream.get() || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) {
        return false;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) return false;
    if (FAILED(frame->Initialize(nullptr))) return false;
    if (FAILED(frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h)))) return false;
    // The staging DIB is premultiplied BGRA; say so or thin strokes darken.
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    if (FAILED(frame->WritePixels(static_cast<UINT>(h), static_cast<UINT>(w * 4),
                                  static_cast<UINT>(w * 4 * h),
                                  static_cast<BYTE*>(bits)))) {
        return false;
    }
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(encoder->Commit());
}

} // namespace

bool ColorPickerPopup::Pick(HWND owner, Compositor* compositor, FluentMenu* menu,
                            float scale, POINT screen_pt, uint32_t initial_rgb, bool dark,
                            uint32_t& result_rgb) {
    if (!compositor || !compositor->Dc()) return false;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        registered = true;
    }

    State s;
    s.owner = owner;
    s.compositor = compositor;
    s.menu = menu;
    s.scale = (std::max)(0.5f, scale);
    s.dark = dark;
    s.accent = GetAccentColor();
    s.theme = IsHighContrast() ? MakeHighContrastTheme() : MakeTheme(dark, s.accent);
    s.argb = 0xFF000000u | (initial_rgb & 0x00FFFFFFu);
    s.hsv = HsvFromRgb(initial_rgb);
    s.painter.SetCompositor(compositor);
    s.painter.SetScale(s.scale);
    Layout(s);

    s.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, kClassName, L"", WS_POPUP,
                             0, 0, s.win_w, s.win_h, owner, nullptr,
                             GetModuleHandleW(nullptr), &s);
    if (!s.hwnd) return false;

    // Anchor at the menu point, clamped to the work area (QFluent execAt).
    HMONITOR monitor = MonitorFromPoint(screen_pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);
    s.base_x = std::clamp(screen_pt.x, mi.rcWork.left, mi.rcWork.right - s.win_w);
    s.base_y = std::clamp(screen_pt.y, mi.rcWork.top, mi.rcWork.bottom - s.win_h);

    CreateBrushes(s);
    CreateEdits(s);
    SyncEdits(s, -1);

    // 120ms fade + 10px slide from the right, 170ms OutCubic (QFluent execAt).
    if (Render(s)) {
        ShowWindow(s.hwnd, SW_SHOW);
        SetForegroundWindow(s.hwnd);
        const auto t0 = std::chrono::steady_clock::now();
        constexpr fluent::MotionSpec slide{ 170.0f, fluent::EasingCurve::OutCubic };
        for (;;) {
            const float elapsed = static_cast<float>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count());
            if (elapsed >= slide.duration_ms) break;
            const float p = fluent::EvaluateMotion(slide, elapsed);
            const int dx = static_cast<int>(std::lround((1.0f - p) * 10.0f * s.scale));
            const BYTE alpha = static_cast<BYTE>(
                std::clamp(elapsed / 120.0f, 0.0f, 1.0f) * 255.0f);
            Present(s, alpha, dx);
            Sleep(10);
        }
        Present(s, 255, 0);
    } else {
        ShowWindow(s.hwnd, SW_SHOW);
        SetForegroundWindow(s.hwnd);
    }
    ShowEdits(s);
    Repaint(s);

    MSG msg{};
    while (!s.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        const bool click = msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN ||
                           msg.message == WM_MBUTTONDOWN || msg.message == WM_NCLBUTTONDOWN ||
                           msg.message == WM_NCRBUTTONDOWN || msg.message == WM_NCMBUTTONDOWN;
        // Click outside the popup or its edit overlays cancels (QFluent
        // eventFilter behavior); the click is still delivered normally.
        if (click && !IsOurs(s, msg.hwnd)) s.done = true;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    for (HWND e : s.edits) {
        if (e) DestroyWindow(e); // subclass is removed with the window
    }
    result_rgb = s.accepted ? (s.argb & 0x00FFFFFFu) : initial_rgb;
    const bool accepted = s.accepted;
    DestroyWindow(s.hwnd);
    DestroySurface(s);
    if (s.font) DeleteObject(s.font);
    if (s.edit_brush) DeleteObject(s.edit_brush);
    return accepted;
}

bool ColorPickerPopup::SaveDebugSnapshot(Compositor* compositor, const wchar_t* png_path,
                                         bool dark, D2D1_COLOR_F accent, float scale) {
    if (!compositor || !compositor->Dc() || !png_path) return false;
    State s;
    s.compositor = compositor;
    s.scale = (std::max)(0.5f, scale);
    s.dark = dark;
    s.accent = accent;
    s.theme = MakeTheme(dark, accent);
    // Sample color from the QFluent reference screenshot.
    s.argb = 0xFF2FDFF1u;
    s.hsv = HsvFromRgb(0x2FDFF1u);
    s.hot = kPartOk; // show the accept-button hover pill in snapshots
    s.painter.SetCompositor(compositor);
    s.painter.SetScale(s.scale);
    Layout(s);
    if (!Render(s)) return false;
    const bool ok = SavePng(png_path, s.win_w, s.win_h, s.bits);
    DestroySurface(s);
    return ok;
}

} // namespace pulse::ui
