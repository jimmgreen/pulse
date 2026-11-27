// widget_faces.cpp - The desktop widgets' faces, after docs/design/desktop_widgets_redesign.html.
//
// One rule set for every widget: no title band, one large number, colour only where it means something
// (today in red, the sweep hand in orange, the sky behind the weather, charge in green, load in orange/red),
// one ink per card (light or dark text, picked by DrawLiveTile from what is behind the card), and drawn
// vector icons. The faces draw over the card material DrawLiveTile has already laid down.
#include "host/render/desktop_renderer.h"

#include "host/model/world_cities.h"
#include "host/overlays/lunar_calendar.h"
#include "host/render/vector_glyphs.h"
#include "host/render/widget_painters.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

namespace pulse::desktop::render {
namespace {

constexpr float kPi = 3.14159265f;

// ---- colours that carry meaning ----
constexpr uint32_t kRed = 0xFFFF3B30u;      // today
constexpr uint32_t kOrange = 0xFFFF9500u;   // sweep hand
constexpr uint32_t kWarn = 0xFFFF9F0Au;     // load >= 80%
constexpr uint32_t kAlarm = 0xFFFF453Au;    // load >= 90%, low battery
constexpr uint32_t kGreen = 0xFF30D158u;    // charging, break
constexpr uint32_t kFocus = 0xFFFF6A5Cu;    // focus
constexpr uint32_t kSun = 0xFFFFD60Au;
constexpr uint32_t kDrop = 0xFF63C7FFu;

// ---- drawn icons, 24-unit grid (the mockup's symbols) ----
struct IconPart { const char* d; bool fill; float stroke; };
struct IconDef { const IconPart* parts; size_t count; };
#define PULSE_ICON(name, ...) constexpr IconPart name##_parts[] = {__VA_ARGS__}; \
    constexpr IconDef name = {name##_parts, sizeof(name##_parts) / sizeof(IconPart)};

PULSE_ICON(kIcoLoc, {"M20.2 3.8 3.9 10.6c-.6.25-.55 1.1.07 1.27l6.6 1.6 1.6 6.6c.16.62 1.02.68 1.27.07L20.2 3.8z", true, 0})
PULSE_ICON(kIcoPlay, {"M8 5.6v12.8c0 .8.86 1.28 1.54.86l10.1-6.4a1 1 0 0 0 0-1.72L9.54 4.74C8.86 4.32 8 4.8 8 5.6z", true, 0})
PULSE_ICON(kIcoPause, {"M7.8 5h1.4a1.3 1.3 0 0 1 1.3 1.3v11.4a1.3 1.3 0 0 1-1.3 1.3H7.8a1.3 1.3 0 0 1-1.3-1.3V6.3A1.3 1.3 0 0 1 7.8 5zM14.8 5h1.4a1.3 1.3 0 0 1 1.3 1.3v11.4a1.3 1.3 0 0 1-1.3 1.3h-1.4a1.3 1.3 0 0 1-1.3-1.3V6.3a1.3 1.3 0 0 1 1.3-1.3z", true, 0})
PULSE_ICON(kIcoNext, {"M3.5 6.6v10.8c0 .7.78 1.12 1.36.73L13 12.73a.88.88 0 0 0 0-1.46L4.86 5.87c-.58-.39-1.36.03-1.36.73zM12 6.6v10.8c0 .7.78 1.12 1.36.73l8.14-5.4a.88.88 0 0 0 0-1.46l-8.14-5.4c-.58-.39-1.36.03-1.36.73z", true, 0})
PULSE_ICON(kIcoPrev, {"M20.5 6.6v10.8c0 .7-.78 1.12-1.36.73L11 12.73a.88.88 0 0 1 0-1.46l8.14-5.4c.58-.39 1.36.03 1.36.73zM12 6.6v10.8c0 .7-.78 1.12-1.36.73L2.5 12.73a.88.88 0 0 1 0-1.46l8.14-5.4c.58-.39 1.36.03 1.36.73z", true, 0})
PULSE_ICON(kIcoReset, {"M5 12a7 7 0 1 0 2.1-5", false, 2.0f}, {"M6.2 3.2v4.6h4.6", false, 2.0f})
PULSE_ICON(kIcoLaptop, {"M6.1 5.5h11.8a1.6 1.6 0 0 1 1.6 1.6v6.8a1.6 1.6 0 0 1-1.6 1.6H6.1a1.6 1.6 0 0 1-1.6-1.6V7.1a1.6 1.6 0 0 1 1.6-1.6z", false, 1.8f}, {"M2.5 18.5h19", false, 1.8f})
PULSE_ICON(kIcoPhones, {"M4.5 15v-3a7.5 7.5 0 0 1 15 0v3", false, 1.8f},
           {"M5.1 13.5h1.3a1.6 1.6 0 0 1 1.6 1.6v3.3a1.6 1.6 0 0 1-1.6 1.6H5.1a1.6 1.6 0 0 1-1.6-1.6v-3.3a1.6 1.6 0 0 1 1.6-1.6z", true, 0},
           {"M17.6 13.5h1.3a1.6 1.6 0 0 1 1.6 1.6v3.3a1.6 1.6 0 0 1-1.6 1.6h-1.3a1.6 1.6 0 0 1-1.6-1.6v-3.3a1.6 1.6 0 0 1 1.6-1.6z", true, 0})
PULSE_ICON(kIcoMouse, {"M12 3.5a5.5 5.5 0 0 1 5.5 5.5v6a5.5 5.5 0 0 1-11 0V9A5.5 5.5 0 0 1 12 3.5z", false, 1.8f}, {"M12 7v3.5", false, 1.8f})
PULSE_ICON(kIcoKeyboard, {"M4.5 6.5h15a2 2 0 0 1 2 2v7a2 2 0 0 1-2 2h-15a2 2 0 0 1-2-2v-7a2 2 0 0 1 2-2z", false, 1.8f},
           {"M6 10h.01M9.3 10h.01M12.6 10h.01M15.9 10h.01M18 10h.01M8 14h8", false, 1.9f})
PULSE_ICON(kIcoBluetooth, {"M7 7.5l10 9-5 4.5V3l5 4.5-10 9", false, 1.8f})
PULSE_ICON(kIcoPlug, {"M9 3v4.5M15 3v4.5M6.5 7.5h11v3.5a5.5 5.5 0 0 1-11 0V7.5zM12 16.5V21", false, 1.8f})
PULSE_ICON(kIcoBolt, {"M13.2 2.5 5.4 13.1h5.6l-1.2 8.4 7.8-10.6H12l1.2-8.4z", true, 0})
PULSE_ICON(kIcoCheck, {"M6 12.5l4 4 8-9", false, 2.4f})
PULSE_ICON(kIcoPlus, {"M12 5v14M5 12h14", false, 2.0f})
PULSE_ICON(kIcoNote, {"M9 18V6.5l11-2.3V16", false, 1.8f}, {"M9.1 18a2.6 2.6 0 1 1-5.2 0 2.6 2.6 0 0 1 5.2 0z", true, 0},
           {"M20.1 16a2.6 2.6 0 1 1-5.2 0 2.6 2.6 0 0 1 5.2 0z", true, 0})
PULSE_ICON(kIcoCloudOff, {"M7.2 19h9.9a4.2 4.2 0 0 0 .75-8.33A5.7 5.7 0 0 0 6.8 9.7 4.65 4.65 0 0 0 7.2 19z", false, 1.7f},
           {"M10 12.5l4 4M14 12.5l-4 4", false, 1.7f})
#undef PULSE_ICON

constexpr const char* kCloudPath = "M7.2 19.2h9.9a4.3 4.3 0 0 0 .75-8.53A5.8 5.8 0 0 0 6.7 9.75 4.75 4.75 0 0 0 7.2 19.2z";
constexpr const char* kMoonPath = "M15.6 3.2a8.6 8.6 0 1 0 5.2 15.3A7.2 7.2 0 0 1 15.6 3.2z";
constexpr const char* kBoltSmall = "M12.6 15.2 9.8 19.4h2.6l-1.1 3.4 3.4-4.6h-2.5l.9-3z";

struct Ink {
    uint32_t pri, sec, ter, hair, track, fill, fill2;
};
Ink InkFor(bool light_text) {
    return light_text ? Ink{0xFFFFFFFFu, 0x9EFFFFFFu, 0x61FFFFFFu, 0x21FFFFFFu, 0x29FFFFFFu, 0x1FFFFFFFu, 0x33FFFFFFu}
                      : Ink{0xFF1D1D1Fu, 0x8A000000u, 0x52000000u, 0x14000000u, 0x17000000u, 0x0F000000u, 0x1A000000u};
}

std::wstring WeekdayShort(int dow) {
    static constexpr const wchar_t* k[7] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};
    return k[((dow % 7) + 7) % 7];
}
std::wstring MonthName(int m) {
    static constexpr const wchar_t* k[12] = {L"一月", L"二月", L"三月", L"四月", L"五月", L"六月",
                                             L"七月", L"八月", L"九月", L"十月", L"十一月", L"十二月"};
    return k[std::clamp(m, 1, 12) - 1];
}
// "十月十八" (the lunar date without the 农历 prefix), empty without ICU.
std::wstring LunarShort(const SYSTEMTIME& st) {
    std::wstring s = overlays::LunarFullText(st.wYear, st.wMonth, st.wDay);
    if (s.rfind(L"农历", 0) == 0) s.erase(0, 2);
    return s;
}
int DaysInMonth(int y, int m) {
    static constexpr int k[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : k[std::clamp(m, 1, 12) - 1];
}
int DayOfWeek(int y, int m, int d) {  // 0 = Sunday
    static constexpr int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
int IsoWeek(const SYSTEMTIME& st) {
    // ISO 8601: the week holding the year's first Thursday is week 1.
    const int dow = (st.wDayOfWeek + 6) % 7;  // Monday 0
    int yday = st.wDay - 1;
    for (int m = 1; m < st.wMonth; ++m) yday += DaysInMonth(st.wYear, m);
    int wk = (yday - dow + 10) / 7;
    if (wk < 1) return 52;
    if (wk == 53) {
        const int jan1 = DayOfWeek(st.wYear + 1, 1, 1);
        if (jan1 >= 1 && jan1 <= 4) return 1;  // Mon..Thu next year: this is week 1 of it
    }
    return wk;
}
bool IsNightHour(int hour) { return hour < 6 || hour >= 18; }

std::wstring FormatTime(const SYSTEMTIME& st, bool h24, std::wstring* suffix = nullptr) {
    wchar_t buf[16]{};
    if (h24) {
        std::swprintf(buf, 16, L"%02u:%02u", st.wHour, st.wMinute);
        if (suffix) suffix->clear();
    } else {
        const int h = st.wHour % 12 == 0 ? 12 : st.wHour % 12;
        std::swprintf(buf, 16, L"%d:%02u", h, st.wMinute);
        if (suffix) *suffix = st.wHour < 12 ? L"上午" : L"下午";
    }
    return buf;
}

} // namespace

// =====================================================================================================
class WidgetFacePainter {
public:
    WidgetFacePainter(DesktopRenderer& r, const DesktopRenderer::WidgetFaceInput& in)
        : R(r), in_(in), dc_(r.d2d_dc_.Get()), set_(in.state->settings), live_(*in.live), tile_(*in.tile),
          rc_(in.rect), w_(in.rect.right - in.rect.left), h_(in.rect.bottom - in.rect.top),
          cols_(in.tile_in->size.cols), rows_(in.tile_in->size.rows), light_(in.light_ink), ink_(InkFor(in.light_ink)) {
        pad_ = cols_ == 1 ? 11.0f : 14.0f;
        shadow_ = in.glass_text_shadow;
        // On a 着色 card red would fight the tint: today takes the text colour instead.
        tinted_ = set_.widgets.scheme == 3 && in.tile_in->look.background == model::TileBackground::Material;
    }

    bool Draw() {
        switch (tile_.kind) {
        case model::TileKind::Clock: Clock(); return true;
        case model::TileKind::Weather: Weather(); return true;
        case model::TileKind::Calendar: Calendar(); return true;
        case model::TileKind::System: System(); return true;
        case model::TileKind::Battery: Battery(); return true;
        case model::TileKind::MediaPlayer: Media(); return true;
        case model::TileKind::Pomodoro: Pomodoro(); return true;
        case model::TileKind::Countdown: Countdown(); return true;
        case model::TileKind::Todo: Todo(); return true;
        case model::TileKind::StickyNote: Note(); return true;
        case model::TileKind::DiskRing: Disk(); return true;
        default: return false;
        }
    }

private:
    // ------------------------------------------------------------------ primitives
    enum class Align { Left, Center, Right };

    void SetInk(bool light) { light_ = light; ink_ = InkFor(light); }

    // Text placed on its baseline: x is the left edge, the centre or the right edge per `align`.
    // Returns the drawn width.
    float Text(std::wstring_view s, float px, DWRITE_FONT_WEIGHT wt, bool display, float x, float baseline,
               float max_w, uint32_t argb, Align align = Align::Left, float opacity = 1.0f) {
        if (s.empty() || max_w <= 1.0f) return 0.0f;
        IDWriteTextFormat* fmt = R.FaceFormat(px, wt, display);
        if (!fmt) return 0.0f;
        const DWRITE_TEXT_ALIGNMENT da = align == Align::Left ? DWRITE_TEXT_ALIGNMENT_LEADING
                                         : align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                                  : DWRITE_TEXT_ALIGNMENT_TRAILING;
        R.widget_text_.Bind(R.dwrite_factory_.Get(), dc_);
        auto* layout = R.widget_text_.Layout(R.dwrite_factory_.Get(), s, fmt, max_w, px * 1.6f, true, da);
        if (!layout) return 0.0f;
        DWRITE_LINE_METRICS lm{};
        UINT32 lines = 0;
        layout->GetLineMetrics(&lm, 1, &lines);
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);
        const float left = align == Align::Left ? x : align == Align::Center ? x - max_w * 0.5f : x - max_w;
        const float top = baseline - (lines ? lm.baseline : px * 0.8f);
        if (shadow_ && light_) {
            R.widget_text_.DrawLayout(R.dwrite_factory_.Get(), dc_, D2D1::Point2F(left, top + 1.0f), layout,
                                      R.Brush(0x52000000u, opacity));
        }
        R.widget_text_.DrawLayout(R.dwrite_factory_.Get(), dc_, D2D1::Point2F(left, top), layout, R.Brush(argb, opacity));
        return std::min(tm.widthIncludingTrailingWhitespace, max_w);
    }
    float Measure(std::wstring_view s, float px, DWRITE_FONT_WEIGHT wt, bool display) {
        IDWriteTextFormat* fmt = R.FaceFormat(px, wt, display);
        if (s.empty() || !fmt) return 0.0f;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(R.dwrite_factory_->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), fmt, 4000.0f, px * 2.0f, &layout)) || !layout)
            return 0.0f;
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);
        return tm.widthIncludingTrailingWhitespace;
    }
    // The type scale (DIPs).
    float Label(std::wstring_view s, float x, float base, float max_w, Align a = Align::Left, uint32_t c = 0) {
        return Text(s, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, x, base, max_w, c ? c : ink_.sec, a);
    }
    float Caption(std::wstring_view s, float x, float base, float max_w, Align a = Align::Left, uint32_t c = 0) {
        return Text(s, 11.0f, DWRITE_FONT_WEIGHT_MEDIUM, false, x, base, max_w, c ? c : ink_.sec, a);
    }
    float Body(std::wstring_view s, float x, float base, float max_w, Align a = Align::Left, uint32_t c = 0) {
        return Text(s, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, x, base, max_w, c ? c : ink_.pri, a);
    }
    float Hero(std::wstring_view s, float px, float x, float base, float max_w, Align a = Align::Left, uint32_t c = 0) {
        // SF-style display numerals: semibold, as macOS widgets set temperatures, times and loads.
        return Text(s, px, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, x, base, max_w, c ? c : ink_.pri, a);
    }

    void FillRR(const D2D1_RECT_F& r, float rad, uint32_t argb, float op = 1.0f) {
        dc_->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), R.Brush(argb, op));
    }
    void Bar(float x, float y, float w, float h, float frac, uint32_t argb) {
        if (w <= 0.0f) return;
        FillRR(D2D1::RectF(x, y, x + w, y + h), h * 0.5f, ink_.track);
        const float f = std::clamp(frac, 0.0f, 1.0f);
        if (f > 0.0f) FillRR(D2D1::RectF(x, y, x + std::max(h, w * f), y + h), h * 0.5f, argb);
    }
    void Ring(float cx, float cy, float rad, float sw, float frac, uint32_t argb, uint32_t track) {
        dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), R.Brush(track), sw);
        const float f = std::clamp(frac, 0.0f, 1.0f);
        if (f <= 0.0f) return;
        if (f >= 0.999f) {
            dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), R.Brush(argb), sw);
            return;
        }
        ComPtr<ID2D1PathGeometry> geo;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(R.d2d_factory_->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) return;
        const float a1 = -kPi * 0.5f + f * 2.0f * kPi;
        sink->BeginFigure(D2D1::Point2F(cx, cy - rad), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + rad * std::cos(a1), cy + rad * std::sin(a1)), D2D1::SizeF(rad, rad), 0.0f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE, f > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        dc_->DrawGeometry(geo.Get(), R.Brush(argb), sw, R.round_stroke_.Get());
    }
    void Icon(const IconDef& icon, const D2D1_RECT_F& box, uint32_t argb, float op = 1.0f) {
        for (size_t i = 0; i < icon.count; ++i)
            DrawSvgPath(dc_, icon.parts[i].d, box, 24.0f, R.Brush(argb, op), icon.parts[i].fill, icon.parts[i].stroke);
    }
    static D2D1_RECT_F Box(float cx, float cy, float s) { return D2D1::RectF(cx - s * 0.5f, cy - s * 0.5f, cx + s * 0.5f, cy + s * 0.5f); }
    // A sub-box of a 24-grid icon box, as an SVG translate(tx ty) scale(sc) would place it.
    static D2D1_RECT_F Sub(const D2D1_RECT_F& b, float tx, float ty, float sc) {
        const float k = (b.right - b.left) / 24.0f;
        return D2D1::RectF(b.left + tx * k, b.top + ty * k, b.left + tx * k + 24.0f * sc * k, b.top + ty * k + 24.0f * sc * k);
    }
    void Sparkline(std::span<const float> vals, const D2D1_RECT_F& r, float max_v, float min_v, uint32_t argb, bool area) {
        const size_t n = vals.size();
        if (n < 2 || r.right - r.left < 4.0f) return;
        const float span = std::max(0.001f, max_v - min_v);
        auto pt = [&](size_t i) {
            const float x = r.left + (r.right - r.left) * static_cast<float>(i) / static_cast<float>(n - 1);
            const float v = std::clamp((vals[i] - min_v) / span, 0.0f, 1.0f);
            return D2D1::Point2F(x, r.bottom - 2.0f - v * (r.bottom - r.top - 5.0f));
        };
        ComPtr<ID2D1PathGeometry> line, fill;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(R.d2d_factory_->CreatePathGeometry(&line)) && SUCCEEDED(line->Open(&sink))) {
            sink->BeginFigure(pt(0), D2D1_FIGURE_BEGIN_HOLLOW);
            for (size_t i = 1; i < n; ++i) sink->AddLine(pt(i));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
        }
        if (area && SUCCEEDED(R.d2d_factory_->CreatePathGeometry(&fill))) {
            ComPtr<ID2D1GeometrySink> fs;
            if (SUCCEEDED(fill->Open(&fs))) {
                fs->BeginFigure(D2D1::Point2F(r.left, r.bottom), D2D1_FIGURE_BEGIN_FILLED);
                for (size_t i = 0; i < n; ++i) fs->AddLine(pt(i));
                fs->AddLine(D2D1::Point2F(r.right, r.bottom));
                fs->EndFigure(D2D1_FIGURE_END_CLOSED);
                fs->Close();
                const D2D1_COLOR_F c = ColorOf(argb);
                D2D1_GRADIENT_STOP stops[2] = {{0.0f, D2D1::ColorF(c.r, c.g, c.b, 0.22f * c.a)}, {1.0f, D2D1::ColorF(c.r, c.g, c.b, 0.0f)}};
                ComPtr<ID2D1GradientStopCollection> col;
                ComPtr<ID2D1LinearGradientBrush> br;
                if (SUCCEEDED(dc_->CreateGradientStopCollection(stops, 2, &col)) &&
                    SUCCEEDED(dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, r.top), D2D1::Point2F(0, r.bottom)),
                                                             col.Get(), &br)))
                    dc_->FillGeometry(fill.Get(), br.Get());
            }
        }
        if (line) dc_->DrawGeometry(line.Get(), R.Brush(argb), 1.5f, R.round_stroke_.Get());
        const auto last = pt(n - 1);
        dc_->FillEllipse(D2D1::Ellipse(last, 2.6f, 2.6f), R.Brush(argb));
    }
    static D2D1_COLOR_F ColorOf(uint32_t argb) {
        return D2D1::ColorF(((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f, ((argb >> 24) & 0xFF) / 255.0f);
    }
    void FillVerticalGradient(const D2D1_RECT_F& r, float rad, uint32_t top, uint32_t bottom, float op) {
        D2D1_GRADIENT_STOP stops[2] = {{0.0f, ColorOf(top)}, {1.0f, ColorOf(bottom)}};
        ComPtr<ID2D1GradientStopCollection> col;
        ComPtr<ID2D1LinearGradientBrush> br;
        if (FAILED(dc_->CreateGradientStopCollection(stops, 2, &col)) ||
            FAILED(dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, r.top), D2D1::Point2F(0, r.bottom)),
                                                  col.Get(), &br)))
            return;
        br->SetOpacity(op);
        dc_->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br.Get());
    }
    // A centred message for the empty, loading and error states.
    void EmptyState(const IconDef* icon, std::wstring_view title, std::wstring_view hint) {
        const bool tiny = cols_ == 1;
        const bool room_for_hint = !hint.empty() && h_ >= 150.0f;
        const float icon_s = tiny ? 22.0f : 26.0f;
        float block = (icon ? icon_s + 8.0f : 0.0f) + (title.empty() ? 0.0f : 17.0f) + (room_for_hint ? 34.0f : 0.0f);
        float y = rc_.top + (h_ - block) * 0.5f;
        const float cx = (rc_.left + rc_.right) * 0.5f;
        if (icon) {
            Icon(*icon, Box(cx, y + icon_s * 0.5f, icon_s), ink_.sec);
            y += icon_s + 8.0f;
        }
        if (!title.empty()) {
            Body(title, cx, y + 13.0f, w_ - 20.0f, Align::Center);
            y += 17.0f;
        }
        if (room_for_hint) {
            // Up to two caption lines.
            IDWriteTextFormat* fmt = R.FaceFormat(11.5f, DWRITE_FONT_WEIGHT_NORMAL, false);
            if (fmt) {
                ComPtr<IDWriteTextLayout> layout;
                const float lw = std::min(w_ - 28.0f, 300.0f);
                if (SUCCEEDED(R.dwrite_factory_->CreateTextLayout(hint.data(), static_cast<UINT32>(hint.size()), fmt, lw, 34.0f, &layout))) {
                    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    dc_->DrawTextLayout(D2D1::Point2F(cx - lw * 0.5f, y + 4.0f), layout.Get(), R.Brush(ink_.sec));
                }
            }
        }
    }
    uint32_t LoadColor(float pct) const {
        if (!set_.widgets.system_warn) return ink_.pri;
        return pct >= 90.0f ? kAlarm : pct >= 80.0f ? kWarn : ink_.pri;
    }

    // ------------------------------------------------------------------ the analog dial
    // face: 0 white, 1 dark (over a dark card), 2 night (dark face on a light card).
    void Dial(float cx, float cy, float rad, const SYSTEMTIME& t, int face, bool numbers, bool seconds) {
        const bool lightface = face == 0;
        const uint32_t face_c = face == 0 ? 0xFFFFFFFFu : face == 2 ? 0xFF3B3B3Du : 0x57000000u;
        const uint32_t hand = lightface ? 0xFF1D1D1Fu : 0xFFFFFFFFu;
        if (face != 1) dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + 1.0f), rad, rad), R.Brush(0x24000000u));
        dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), R.Brush(face_c));
        if (lightface) dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad - 0.5f, rad - 0.5f), R.Brush(0x10000000u), 1.0f);
        for (int i = 0; i < 60; ++i) {
            const bool hour = i % 5 == 0;
            if (numbers && hour) continue;
            const float a = static_cast<float>(i) * kPi / 30.0f;
            const float r1 = rad * (hour ? 0.86f : 0.925f), r2 = rad * 0.965f;
            dc_->DrawLine(D2D1::Point2F(cx + r1 * std::sin(a), cy - r1 * std::cos(a)), D2D1::Point2F(cx + r2 * std::sin(a), cy - r2 * std::cos(a)),
                          R.Brush(hand, hour ? 0.8f : 0.32f), hour ? std::max(1.4f, rad * 0.028f) : std::max(0.8f, rad * 0.012f), R.round_stroke_.Get());
        }
        if (numbers) {
            const float px = std::max(7.0f, rad * 0.22f);
            for (int n = 1; n <= 12; ++n) {
                const float a = static_cast<float>(n) * kPi / 6.0f, rr = rad * 0.76f;
                wchar_t b[4]{};
                std::swprintf(b, 4, L"%d", n);
                const float x = cx + rr * std::sin(a), y = cy - rr * std::cos(a);
                Text(b, px, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, x, y + px * 0.36f, px * 2.0f, hand, Align::Center);
            }
        }
        const float sec = static_cast<float>(t.wSecond);
        const float mins = static_cast<float>(t.wMinute) + sec / 60.0f;
        const float hrs = static_cast<float>(t.wHour % 12) + mins / 60.0f;
        auto hand_at = [&](float deg, float len, float width) {
            D2D1_MATRIX_3X2_F old{};
            dc_->GetTransform(&old);
            dc_->SetTransform(D2D1::Matrix3x2F::Rotation(deg, D2D1::Point2F(cx, cy)) * old);
            const float stub = std::max(1.2f, rad * 0.028f);
            dc_->FillRectangle(D2D1::RectF(cx - stub * 0.5f, cy - rad * 0.14f, cx + stub * 0.5f, cy), R.Brush(hand));
            FillRR(D2D1::RectF(cx - width * 0.5f, cy - len, cx + width * 0.5f, cy - rad * 0.12f), width * 0.5f, hand);
            dc_->SetTransform(old);
        };
        hand_at(hrs * 30.0f, rad * 0.5f, rad * 0.075f);
        hand_at(mins * 6.0f, rad * 0.84f, rad * 0.075f);
        dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad * 0.06f, rad * 0.06f), R.Brush(hand));
        if (seconds && set_.widgets.clock_seconds) {
            const float a = sec * 6.0f * kPi / 180.0f;
            const float sw = std::max(1.0f, rad * 0.018f);
            dc_->DrawLine(D2D1::Point2F(cx - rad * 0.16f * std::sin(a), cy + rad * 0.16f * std::cos(a)),
                          D2D1::Point2F(cx + rad * 0.9f * std::sin(a), cy - rad * 0.9f * std::cos(a)), R.Brush(kOrange), sw, R.round_stroke_.Get());
            dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad * 0.045f, rad * 0.045f), R.Brush(kOrange));
            dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad * 0.018f, rad * 0.018f), R.Brush(lightface ? 0xFFFFFFFFu : 0xFF3B3B3Du));
        }
    }
    int FaceFor(const SYSTEMTIME& t) const {
        if (light_) return 1;
        return set_.widgets.clock_night_face && IsNightHour(t.wHour) ? 2 : 0;
    }

    // ------------------------------------------------------------------ 时钟
    void Clock() {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        const float cx = (rc_.left + rc_.right) * 0.5f, cy = (rc_.top + rc_.bottom) * 0.5f;
        if (cols_ == 1) {
            const float rad = std::min(w_, h_) * 0.5f - 7.0f;
            Dial(cx, cy, rad, st, FaceFor(st), false, true);
            return;
        }
        std::wstring suffix;
        const std::wstring time = FormatTime(st, set_.widgets.clock_24h, &suffix);
        const std::wstring lunar = set_.widgets.lunar ? LunarShort(st) : L"";
        wchar_t date[48]{};
        std::swprintf(date, 48, L"%u月%u日%ls%ls", st.wMonth, st.wDay, lunar.empty() ? L"" : L" · ", lunar.c_str());
        if (cols_ < 4) {
            if (rows_ == 1) {
                float x = rc_.left + pad_;
                if (tile_.analog_clock) {
                    const float rad = (h_ - 20.0f) * 0.5f;
                    Dial(x + rad, cy, rad, st, FaceFor(st), false, true);
                    x += rad * 2.0f + 12.0f;
                }
                const float avail = rc_.right - pad_ - x;
                Label(WeekdayShort(st.wDayOfWeek), x, cy - 20.0f, avail, Align::Left, Today());
                const float tw = Text(time, tile_.analog_clock ? 32.0f : 40.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, x, cy + 13.0f, avail, ink_.pri);
                if (!suffix.empty()) Label(suffix, x + tw + 3.0f, cy + 13.0f, 40.0f);
                Caption(date, x, cy + 30.0f, avail);
                return;
            }
            if (tile_.analog_clock) {
                const float rad = std::min(w_, h_) * 0.5f - 12.0f;
                Dial(cx, cy, rad, st, FaceFor(st), true, true);
                return;
            }
            const float x = rc_.left + pad_, avail = w_ - pad_ * 2.0f;
            Label(WeekdayShort(st.wDayOfWeek), x, cy - 34.0f, avail, Align::Left, Today());
            const float tw = Text(time, std::min(54.0f, avail * 0.3f + 8.0f), DWRITE_FONT_WEIGHT_SEMI_BOLD, true, x, cy + 18.0f, avail, ink_.pri);
            if (!suffix.empty()) Label(suffix, x + tw + 3.0f, cy + 18.0f, 40.0f);
            Caption(date, x, cy + 38.0f, avail);
            return;
        }
        const auto cities = QueryWorldClocks(set_.widgets.world_cities);
        const size_t n = std::min<size_t>(cities.size(), 4);
        if (n == 0) return;
        if (rows_ == 1) {
            const float colw = (w_ - pad_ * 2.0f) / static_cast<float>(n);
            for (size_t i = 0; i < n; ++i) {
                const auto& c = cities[i];
                const float x = rc_.left + pad_ + colw * static_cast<float>(i) + (i ? 10.0f : 0.0f);
                if (i) dc_->FillRectangle(D2D1::RectF(x - 10.0f, rc_.top + 20.0f, x - 9.0f, rc_.bottom - 20.0f), R.Brush(ink_.hair));
                Body(c.label, x, cy - 15.0f, colw - 12.0f);
                Text(FormatTime(c.local_time, set_.widgets.clock_24h), 24.0f, DWRITE_FONT_WEIGHT_NORMAL, true, x, cy + 12.0f, colw - 12.0f, ink_.pri);
                Caption(c.day_label + L" " + c.offset_label, x, cy + 28.0f, colw - 12.0f);
            }
            return;
        }
        // 4×2: a row of dials. Taller: today's clock above, the world below.
        float top = rc_.top, bottom = rc_.bottom;
        if (rows_ >= 3) {
            const float big = std::min(h_ * 0.5f - 24.0f, w_ * 0.25f);
            Dial(rc_.left + pad_ + big, rc_.top + pad_ + big, big, st, FaceFor(st), true, true);
            const float x = rc_.left + pad_ + big * 2.0f + 22.0f, avail = rc_.right - pad_ - x;
            const float mid = rc_.top + pad_ + big;
            Label(WeekdayShort(st.wDayOfWeek), x, mid - 30.0f, avail, Align::Left, Today());
            const float tw = Text(time, 46.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, x, mid + 16.0f, avail, ink_.pri);
            if (!suffix.empty()) Label(suffix, x + tw + 3.0f, mid + 16.0f, 40.0f);
            Caption(date, x, mid + 36.0f, avail);
            top = rc_.top + pad_ + big * 2.0f + 8.0f;
            dc_->FillRectangle(D2D1::RectF(rc_.left + pad_, top, rc_.right - pad_, top + 1.0f), R.Brush(ink_.hair));
        }
        const float colw = (w_ - pad_ * 2.0f) / static_cast<float>(n);
        const float rad = std::min(colw * 0.5f - 6.0f, (bottom - top - 2.0f * pad_ - 36.0f) * 0.5f);
        const float block = rad * 2.0f + 8.0f + 30.0f;
        const float dy = top + (bottom - top - block) * 0.5f + rad;
        for (size_t i = 0; i < n; ++i) {
            const auto& c = cities[i];
            const float x = rc_.left + pad_ + colw * (static_cast<float>(i) + 0.5f);
            Dial(x, dy, rad, c.local_time, FaceFor(c.local_time), false, false);
            Body(c.label, x, dy + rad + 21.0f, colw, Align::Center);
            Caption(c.day_label + L" " + c.offset_label, x, dy + rad + 36.0f, colw, Align::Center);
        }
    }

    // ------------------------------------------------------------------ 天气
    int Temp(int c) const { return set_.widgets.fahrenheit ? static_cast<int>(std::lround(c * 9.0 / 5.0 + 32.0)) : c; }
    static const wchar_t* Condition(int code) {
        if (code == 0) return L"晴";
        if (code == 1) return L"晴间多云";
        if (code == 2) return L"多云";
        if (code == 3) return L"阴";
        if (code == 45 || code == 48) return L"雾";
        if (code >= 51 && code <= 57) return L"毛毛雨";
        if (code == 61 || code == 80) return L"小雨";
        if (code == 63 || code == 81) return L"中雨";
        if (code == 65 || code == 82) return L"大雨";
        if ((code >= 71 && code <= 77) || code == 85 || code == 86) return L"雪";
        if (code >= 95) return L"雷阵雨";
        return L"多云";
    }
    void WeatherIcon(int code, bool day, const D2D1_RECT_F& b) {
        const float k = (b.right - b.left) / 24.0f;
        auto sun = [&](float cx, float cy, float r) {
            const float X = b.left + cx * k, Y = b.top + cy * k, rr = r * k;
            dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(X, Y), rr, rr), R.Brush(kSun));
            for (int i = 0; i < 8; ++i) {
                const float a = static_cast<float>(i) * kPi / 4.0f;
                dc_->DrawLine(D2D1::Point2F(X + rr * 1.55f * std::cos(a), Y + rr * 1.55f * std::sin(a)),
                              D2D1::Point2F(X + rr * 2.05f * std::cos(a), Y + rr * 2.05f * std::sin(a)), R.Brush(kSun), rr * 0.42f, R.round_stroke_.Get());
            }
        };
        auto cloud = [&](float tx, float ty, float sc, uint32_t c) { DrawSvgPath(dc_, kCloudPath, Sub(b, tx, ty, sc), 24.0f, R.Brush(c), true); };
        auto moon = [&](float tx, float ty, float sc) { DrawSvgPath(dc_, kMoonPath, Sub(b, tx, ty, sc), 24.0f, R.Brush(0xFFF2F4FAu), true); };
        auto drops = [&](uint32_t c) {
            for (const float x : {8.5f, 12.5f, 16.5f})
                dc_->DrawLine(D2D1::Point2F(b.left + x * k, b.top + 18.2f * k), D2D1::Point2F(b.left + (x - 1.0f) * k, b.top + 20.8f * k),
                              R.Brush(c), 1.8f * k, R.round_stroke_.Get());
        };
        // Light glyph parts stay white; on a light card they keep a faint outline so they read.
        const uint32_t cloud_c = light_ ? 0xF5FFFFFFu : 0xFFFFFFFFu;
        if (code == 0) {
            if (day) sun(12, 12, 4.4f); else moon(0, 0, 1.0f);
        } else if (code <= 2) {
            if (day) sun(8.6f, 8.2f, 3.1f); else moon(-1.5f, -1.8f, 0.62f);
            cloud(2.2f, 1.6f, 0.9f, cloud_c);
        } else if (code == 3) {
            cloud(-1.0f, -1.2f, 1.04f, light_ ? 0x9EFFFFFFu : 0xFFB8C0CCu);
            cloud(1.6f, 1.4f, 0.88f, light_ ? cloud_c : 0xFFE6EAF0u);
        } else if (code == 45 || code == 48) {
            cloud(0, -3.5f, 1.0f, light_ ? cloud_c : 0xFFE6EAF0u);
            const uint32_t c = light_ ? cloud_c : 0xFFB8C0CCu;
            dc_->DrawLine(D2D1::Point2F(b.left + 4 * k, b.top + 19 * k), D2D1::Point2F(b.left + 20 * k, b.top + 19 * k), R.Brush(c), 1.6f * k, R.round_stroke_.Get());
            dc_->DrawLine(D2D1::Point2F(b.left + 6 * k, b.top + 22 * k), D2D1::Point2F(b.left + 18 * k, b.top + 22 * k), R.Brush(c), 1.6f * k, R.round_stroke_.Get());
        } else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
            cloud(0, -3.8f, 1.0f, light_ ? cloud_c : 0xFFD9DEE6u);
            drops(kDrop);
        } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
            cloud(0, -3.8f, 1.0f, light_ ? cloud_c : 0xFFD9DEE6u);
            for (const auto& p : {D2D1::Point2F(8, 19.4f), D2D1::Point2F(12, 20.6f), D2D1::Point2F(16, 19.4f)})
                dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(b.left + p.x * k, b.top + p.y * k), 1.15f * k, 1.15f * k),
                                 R.Brush(light_ ? 0xFFFFFFFFu : 0xFF9AA5B4u));
        } else {
            cloud(0, -3.8f, 1.0f, light_ ? cloud_c : 0xFFD9DEE6u);
            DrawSvgPath(dc_, kBoltSmall, b, 24.0f, R.Brush(kSun), true);
        }
    }
    static void SkyColors(int code, bool day, uint32_t& top, uint32_t& bottom) {
        if (day) {
            if (code == 0) { top = 0xFF2966BDu; bottom = 0xFF5C99E0u; }
            else if (code <= 2) { top = 0xFF3F73B8u; bottom = 0xFF7AA4D6u; }
            else if (code == 3 || code == 45 || code == 48) { top = 0xFF667D99u; bottom = 0xFF94A8BFu; }
            else { top = 0xFF4A596Eu; bottom = 0xFF738296u; }
        } else {
            if (code <= 2) { top = 0xFF080F29u; bottom = 0xFF1F3059u; }
            else { top = 0xFF1F2330u; bottom = 0xFF3D4554u; }
        }
    }
    void Weather() {
        const auto& W = live_.weather;
        if (!W.valid) {
            const bool no_city = set_.weather_city.empty();
            if (no_city) EmptyState(&kIcoLoc, cols_ == 1 ? L"设置城市" : L"还没设置城市", L"在插件设置 › 小组件 › 天气里填城市名");
            else if (W.status.empty()) EmptyState(nullptr, cols_ == 1 ? L"" : L"正在获取天气…", L"");
            else {
                // "找不到「…」" is the city's spelling; anything else is the connection, which retries by itself.
                const bool not_found = W.status.rfind(L"找不到", 0) == 0;
                EmptyState(&kIcoCloudOff, cols_ == 1 ? L"无法更新" : W.status,
                           h_ < 150.0f ? L"" : not_found ? L"换个写法再试，例如“上海”或“Shanghai”" : L"联网后会自动重试，每 5 分钟一次");
            }
            return;
        }
        if (set_.widgets.sky) {
            uint32_t t = 0, b = 0;
            SkyColors(W.code, W.is_day, t, b);
            FillVerticalGradient(rc_, in_.radius, t, b, 0.92f);
            SetInk(true);
        }
        wchar_t temp[16]{};
        std::swprintf(temp, 16, L"%d°", Temp(W.temp_c));
        wchar_t hilo[48]{};
        std::swprintf(hilo, 48, L"最高 %d° 最低 %d°", Temp(W.hi_c), Temp(W.lo_c));
        const float L0 = rc_.left + pad_, Rr = rc_.right - pad_, T0 = rc_.top + pad_;
        auto city = [&](float x, float base, float px, float max_w) {
            const float cw = Text(W.city, px, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, x, base, max_w - px, ink_.pri);
            Icon(kIcoLoc, Box(x + cw + px * 0.45f, base - px * 0.36f, px * 0.72f), ink_.pri);
        };
        if (cols_ == 1) {
            Caption(W.city, L0, T0 + 10.0f, w_ - pad_ * 2.0f, Align::Left, ink_.pri);
            Hero(temp, 34.0f, L0, rc_.bottom - pad_ - 2.0f, w_ - pad_ * 2.0f);
            WeatherIcon(W.code, W.is_day, Box(Rr - 10.0f, rc_.bottom - pad_ - 12.0f, 20.0f));
            return;
        }
        if (rows_ == 1) {
            const float cy = (rc_.top + rc_.bottom) * 0.5f;
            WeatherIcon(W.code, W.is_day, Box(L0 + 20.0f, cy, 40.0f));
            const float tw = Hero(temp, 44.0f, L0 + 52.0f, cy + 16.0f, 120.0f);
            const float x = L0 + 52.0f + tw + 10.0f;
            if (cols_ >= 4) {
                // 4×1: the next hours to the right.
                const float hx0 = std::max(x + 110.0f, rc_.left + w_ * 0.46f);
                city(x, cy - 6.0f, 13.0f, hx0 - x - 8.0f);
                Caption(std::wstring(Condition(W.code)) + L" · " + std::to_wstring(Temp(W.hi_c)) + L"° / " + std::to_wstring(Temp(W.lo_c)) + L"°",
                        x, cy + 12.0f, hx0 - x - 8.0f);
                const size_t nh = std::min<size_t>(W.hours.size(), 5);
                const float cw = (Rr - hx0) / static_cast<float>(std::max<size_t>(nh, 1));
                for (size_t i = 0; i < nh; ++i) {
                    const float hx = hx0 + cw * (static_cast<float>(i) + 0.5f);
                    Caption(W.hours[i].label, hx, cy - 16.0f, cw, Align::Center);
                    WeatherIcon(W.hours[i].code, W.hours[i].is_day, Box(hx, cy + 1.0f, 18.0f));
                    Body(std::to_wstring(Temp(W.hours[i].temp_c)) + L"°", hx, cy + 26.0f, cw, Align::Center);
                }
                return;
            }
            const float avail = Rr - x;
            city(Rr - std::min(avail, Measure(W.city, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false) + 14.0f), cy - 12.0f, 13.0f, avail);
            Caption(Condition(W.code), Rr, cy + 6.0f, avail, Align::Right);
            Caption(std::to_wstring(Temp(W.hi_c)) + L"° / " + std::to_wstring(Temp(W.lo_c)) + L"°", Rr, cy + 21.0f, avail, Align::Right);
            return;
        }
        // Medium and up: city, big temperature; condition and range.
        city(L0, T0 + 14.0f, 15.0f, w_ * 0.55f);
        Hero(temp, 46.0f, L0, T0 + 62.0f, w_ * 0.6f);
        if (cols_ < 4) {
            const float base = rc_.bottom - pad_;
            if (rows_ >= 3) {
                // 2×3: the next four hours between the temperature and the condition.
                const size_t nh = std::min<size_t>(W.hours.size(), 4);
                const float colw = (Rr - L0) / static_cast<float>(std::max<size_t>(nh, 1));
                const float top = T0 + 84.0f;
                dc_->FillRectangle(D2D1::RectF(L0, top, Rr, top + 1.0f), R.Brush(ink_.hair));
                for (size_t i = 0; i < nh; ++i) {
                    const float hx = L0 + colw * (static_cast<float>(i) + 0.5f);
                    Caption(W.hours[i].label, hx, top + 22.0f, colw, Align::Center);
                    WeatherIcon(W.hours[i].code, W.hours[i].is_day, Box(hx, top + 40.0f, 20.0f));
                    Body(std::to_wstring(Temp(W.hours[i].temp_c)) + L"°", hx, top + 70.0f, colw, Align::Center);
                }
            }
            WeatherIcon(W.code, W.is_day, Box(L0 + 11.0f, base - 51.0f, 22.0f));
            Body(Condition(W.code), L0, base - 20.0f, w_ - pad_ * 2.0f);
            Body(hilo, L0, base - 3.0f, w_ - pad_ * 2.0f);
            return;
        }
        WeatherIcon(W.code, W.is_day, Box(Rr - 11.0f, T0 + 11.0f, 22.0f));
        Body(Condition(W.code), Rr, T0 + 42.0f, w_ * 0.4f, Align::Right);
        Body(hilo, Rr, T0 + 59.0f, w_ * 0.4f, Align::Right);
        float y = T0 + 78.0f;
        dc_->FillRectangle(D2D1::RectF(L0, y, Rr, y + 1.0f), R.Brush(ink_.hair));
        const size_t nh = std::min<size_t>(W.hours.size(), 6);
        const float colw = (Rr - L0) / static_cast<float>(std::max<size_t>(nh, 1));
        const float hours_top = rows_ >= 3 ? y + 10.0f : y + (rc_.bottom - pad_ - y - 66.0f) * 0.5f + 4.0f;
        for (size_t i = 0; i < nh; ++i) {
            const float hx = L0 + colw * (static_cast<float>(i) + 0.5f);
            Caption(W.hours[i].label, hx, hours_top + 12.0f, colw, Align::Center);
            WeatherIcon(W.hours[i].code, W.hours[i].is_day, Box(hx, hours_top + 30.0f, 20.0f));
            Body(std::to_wstring(Temp(W.hours[i].temp_c)) + L"°", hx, hours_top + 60.0f, colw, Align::Center);
        }
        if (rows_ < 3) return;
        y = hours_top + 72.0f;
        dc_->FillRectangle(D2D1::RectF(L0, y, Rr, y + 1.0f), R.Brush(ink_.hair));
        y += 6.0f;
        int lo = 1000, hi = -1000;
        for (const auto& d : W.days) { lo = std::min(lo, d.lo_c); hi = std::max(hi, d.hi_c); }
        const float span = static_cast<float>(std::max(1, hi - lo));
        const float row_h = 30.0f;
        const size_t nd = std::min<size_t>(W.days.size(), static_cast<size_t>(std::max(0.0f, (rc_.bottom - pad_ - 18.0f - y) / row_h)));
        for (size_t i = 0; i < nd; ++i) {
            const auto& d = W.days[i];
            const float cyr = y + row_h * (static_cast<float>(i) + 0.5f);
            Body(d.label, L0, cyr + 5.0f, 44.0f);
            WeatherIcon(d.code, true, Box(L0 + 60.0f, cyr, 20.0f));
            Body(std::to_wstring(Temp(d.lo_c)) + L"°", L0 + 110.0f, cyr + 5.0f, 34.0f, Align::Right, ink_.sec);
            const float bx0 = L0 + 120.0f, bx1 = Rr - 40.0f;
            FillRR(D2D1::RectF(bx0, cyr - 2.5f, bx1, cyr + 2.5f), 2.5f, ink_.track);
            const float a = static_cast<float>(d.lo_c - lo) / span, z = static_cast<float>(d.hi_c - lo) / span;
            const D2D1_RECT_F seg = D2D1::RectF(bx0 + (bx1 - bx0) * a, cyr - 2.5f, bx0 + (bx1 - bx0) * std::max(z, a + 0.06f), cyr + 2.5f);
            D2D1_GRADIENT_STOP stops[2] = {{0.0f, ColorOf(0xFF8FD0FFu)}, {1.0f, ColorOf(d.hi_c >= 20 ? 0xFFFF9F0Au : 0xFFFFD60Au)}};
            ComPtr<ID2D1GradientStopCollection> col;
            ComPtr<ID2D1LinearGradientBrush> br;
            if (SUCCEEDED(dc_->CreateGradientStopCollection(stops, 2, &col)) &&
                SUCCEEDED(dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(bx0, 0), D2D1::Point2F(bx1, 0)), col.Get(), &br)))
                dc_->FillRoundedRectangle(D2D1::RoundedRect(seg, 2.5f, 2.5f), br.Get());
            Body(std::to_wstring(Temp(d.hi_c)) + L"°", Rr, cyr + 5.0f, 34.0f, Align::Right);
        }
        wchar_t extra[96]{};
        std::swprintf(extra, 96, L"体感 %d° · 湿度 %d%% · 风 %d km/h", Temp(W.feels_c), W.humidity_pct, W.wind_kmh);
        Caption(extra, L0, rc_.bottom - pad_ + 1.0f, Rr - L0);
    }

    // ------------------------------------------------------------------ 日历
    void MonthGrid(const SYSTEMTIME& st, const D2D1_RECT_F& area, float cell_h, bool with_lunar, float fs) {
        static constexpr const wchar_t* kHead[7] = {L"日", L"一", L"二", L"三", L"四", L"五", L"六"};
        const int ws = set_.widgets.week_start == 0 ? 0 : 1;
        const float colw = (area.right - area.left) / 7.0f;
        float y = area.top;
        for (int i = 0; i < 7; ++i) {
            const int wd = (i + ws) % 7;
            Text(kHead[wd], 10.0f, DWRITE_FONT_WEIGHT_BOLD, false, area.left + colw * (static_cast<float>(i) + 0.5f), y + 10.0f, colw,
                 wd == 0 || wd == 6 ? ink_.ter : ink_.sec, Align::Center);
        }
        y += 16.0f;
        const int days = DaysInMonth(st.wYear, st.wMonth);
        const int lead = (DayOfWeek(st.wYear, st.wMonth, 1) - ws + 7) % 7;
        for (int d = 1; d <= days; ++d) {
            const int slot = lead + d - 1, row = slot / 7, col = slot % 7;
            const float x = area.left + colw * (static_cast<float>(col) + 0.5f);
            const float top = y + cell_h * static_cast<float>(row);
            if (top + cell_h > area.bottom + 2.0f) break;
            const bool today = d == st.wDay;
            const int wd = (col + ws) % 7;
            const float num_cy = with_lunar ? top + cell_h * 0.38f : top + cell_h * 0.5f;
            if (today) {
                const float rr = fs * 0.95f;
                dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, num_cy), rr, rr), R.Brush(tinted_ ? 0xFFFFFFFFu : kRed));
            }
            Text(std::to_wstring(d), fs, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, x, num_cy + fs * 0.36f, colw,
                 today ? (tinted_ ? 0xFF1D1D1Fu : 0xFFFFFFFFu) : (wd == 0 || wd == 6) ? ink_.sec : ink_.pri, Align::Center);
            if (with_lunar) {
                bool fest = false;
                const std::wstring cell = overlays::LunarCellLabel(st.wYear, st.wMonth, d, &fest);
                Text(cell, 9.0f, DWRITE_FONT_WEIGHT_MEDIUM, false, x, top + cell_h * 0.84f, colw, fest ? Today() : ink_.ter, Align::Center);
            }
        }
    }
    void Calendar() {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        const std::wstring lunar = set_.widgets.lunar ? LunarShort(st) : L"";
        std::wstring sub = lunar.empty() ? std::to_wstring(st.wYear) + L"年" : L"农历" + lunar;
        if (set_.widgets.week_number) sub += L" · 第 " + std::to_wstring(IsoWeek(st)) + L" 周";
        const std::wstring day = std::to_wstring(st.wDay);
        const float L0 = rc_.left + pad_, Rr = rc_.right - pad_, T0 = rc_.top + pad_;
        const float cx = (rc_.left + rc_.right) * 0.5f, cy = (rc_.top + rc_.bottom) * 0.5f;
        if (cols_ == 1) {
            Text(WeekdayShort(st.wDayOfWeek), 12.0f, DWRITE_FONT_WEIGHT_BOLD, false, cx, cy - 18.0f, w_, Today(), Align::Center);
            Hero(day, 44.0f, cx, cy + 22.0f, w_, Align::Center);
            return;
        }
        if (rows_ == 1) {
            const float colx = L0 + 30.0f;
            Text(WeekdayShort(st.wDayOfWeek), 12.0f, DWRITE_FONT_WEIGHT_BOLD, false, colx, cy - 18.0f, 70.0f, Today(), Align::Center);
            Hero(day, 44.0f, colx, cy + 22.0f, 70.0f, Align::Center);
            const float x = colx + 44.0f;
            Body(MonthName(st.wMonth), x, cy - 3.0f, Rr - x);
            Caption(sub, x, cy + 14.0f, Rr - x);
            if (cols_ >= 4) {
                // 4×1: the next open to-dos.
                float ty = cy - 12.0f;
                const float tx = rc_.left + w_ * 0.5f;
                dc_->FillRectangle(D2D1::RectF(tx - 10.0f, rc_.top + 18.0f, tx - 9.0f, rc_.bottom - 18.0f), R.Brush(ink_.hair));
                int shown = 0;
                for (const auto& td : in_.state->todos) {
                    if (td.done || td.text.empty()) continue;
                    Caption(L"· " + td.text, tx, ty + 4.0f, Rr - tx, Align::Left, ink_.pri);
                    ty += 18.0f;
                    if (++shown == 2) break;
                }
                if (!shown) Caption(L"今天没有待办", tx, cy + 4.0f, Rr - tx);
            }
            return;
        }
        const bool month_style = rows_ >= 3 || (cols_ < 4 && rows_ == 2 && !tile_.analog_clock);
        // (2×2 shows the big date; its 月历 style is the tile's 表盘/数字 switch, which calendars do not otherwise use.)
        if (cols_ < 4 && !month_style) {
            Text(WeekdayShort(st.wDayOfWeek), 13.0f, DWRITE_FONT_WEIGHT_BOLD, false, L0, T0 + 12.0f, w_, Today());
            Hero(day, 76.0f, L0 - 2.0f, T0 + 88.0f, w_);
            Body(MonthName(st.wMonth), L0, rc_.bottom - pad_ - 17.0f, w_ - pad_ * 2.0f);
            Caption(sub, L0, rc_.bottom - pad_ - 1.0f, w_ - pad_ * 2.0f);
            return;
        }
        if (cols_ < 4) {
            Text(MonthName(st.wMonth), 12.0f, DWRITE_FONT_WEIGHT_BOLD, false, L0, T0 + 10.0f, w_, Today());
            if (rows_ >= 3) Caption(std::to_wstring(st.wYear), Rr, T0 + 10.0f, 60.0f, Align::Right);
            const bool lunar_cells = rows_ >= 3 && set_.widgets.lunar;
            const float grid_top = T0 + (rows_ >= 3 ? 24.0f : 18.0f);
            const float cell = (rc_.bottom - pad_ - grid_top - 16.0f) / 6.0f;
            MonthGrid(st, D2D1::RectF(L0 - 4.0f, grid_top, Rr + 4.0f, rc_.bottom - pad_), cell, lunar_cells, rows_ >= 3 ? 12.0f : 11.0f);
            return;
        }
        if (rows_ == 2) {
            const float lw = 132.0f;
            Text(WeekdayShort(st.wDayOfWeek), 13.0f, DWRITE_FONT_WEIGHT_BOLD, false, L0, T0 + 12.0f, lw, Today());
            Hero(day, 68.0f, L0 - 2.0f, T0 + 82.0f, lw);
            Body(MonthName(st.wMonth), L0, rc_.bottom - pad_ - 17.0f, lw);
            Caption(sub, L0, rc_.bottom - pad_ - 1.0f, lw);
            const float gx = L0 + lw + 12.0f;
            const float cell = (rc_.bottom - pad_ - T0 - 16.0f) / 6.0f;
            MonthGrid(st, D2D1::RectF(gx, T0, Rr, rc_.bottom - pad_), cell, false, 11.0f);
            return;
        }
        // 4×3 / 4×4: the month with lunar days; today's open to-dos under it when there is room.
        Text(MonthName(st.wMonth), 15.0f, DWRITE_FONT_WEIGHT_BOLD, false, L0, T0 + 13.0f, w_, Today());
        Caption(std::to_wstring(st.wYear), Rr, T0 + 13.0f, 60.0f, Align::Right);
        const float todo_room = rows_ >= 4 ? 96.0f : 0.0f;
        const float grid_bottom = rc_.bottom - pad_ - todo_room;
        const float cell = (grid_bottom - T0 - 22.0f - 16.0f) / 6.0f;
        MonthGrid(st, D2D1::RectF(L0, T0 + 22.0f, Rr, grid_bottom), cell, set_.widgets.lunar, 14.0f);
        if (todo_room > 0.0f) {
            float y = grid_bottom + 8.0f;
            dc_->FillRectangle(D2D1::RectF(L0, y, Rr, y + 1.0f), R.Brush(ink_.hair));
            Label(L"今天 · " + sub, L0, y + 22.0f, Rr - L0);
            y += 30.0f;
            int shown = 0;
            for (const auto& td : in_.state->todos) {
                if (td.done || td.text.empty()) continue;
                dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(L0 + 3.0f, y + 9.0f), 3.0f, 3.0f), R.Brush(Today()));
                Body(td.text, L0 + 14.0f, y + 14.0f, Rr - L0 - 14.0f);
                y += 21.0f;
                if (++shown == 3) break;
            }
            if (!shown) Caption(L"今天没有待办", L0, y + 12.0f, Rr - L0);
        }
    }

    // ------------------------------------------------------------------ 系统
    void System() {
        const auto& S = live_.system;
        const float L0 = rc_.left + pad_, Rr = rc_.right - pad_, T0 = rc_.top + pad_;
        if (!S.valid) {
            for (int i = 0; i < (cols_ == 1 ? 2 : 4); ++i)
                FillRR(D2D1::RectF(L0, T0 + 6.0f + 26.0f * static_cast<float>(i), L0 + (w_ - pad_ * 2.0f) * (0.5f + 0.1f * static_cast<float>(i % 3)),
                                   T0 + 18.0f + 26.0f * static_cast<float>(i)), 5.0f, ink_.fill);
            return;
        }
        const float cpu = S.cpu_pct_exact > 0.0f ? S.cpu_pct_exact : static_cast<float>(S.cpu_pct);
        const float mem = S.mem_pct_exact;
        auto gb = [](uint64_t b) { return static_cast<double>(b) / (1024.0 * 1024.0 * 1024.0); };
        wchar_t buf[64]{};
        struct Row { std::wstring label, value; float pct = -1.0f; bool net = false; };
        std::vector<Row> rows;
        const uint8_t items = set_.widgets.system_items ? set_.widgets.system_items : 0x1F;
        if (items & 1) { std::swprintf(buf, 64, L"%d%%", static_cast<int>(std::lround(cpu))); rows.push_back({L"CPU", buf, cpu}); }
        if (items & 2) { std::swprintf(buf, 64, L"%.1f / %.1f GB", gb(S.mem_used), gb(S.mem_total)); rows.push_back({L"内存", buf, mem}); }
        if ((items & 4) && S.has_vram) { std::swprintf(buf, 64, L"%.1f / %.0f GB", S.vram_used_gb, S.vram_total_gb); rows.push_back({L"显存", buf, S.vram_pct}); }
        if (items & 8) { std::swprintf(buf, 64, L"剩 %.0f GB", gb(S.disk_free)); rows.push_back({L"C 盘", buf, static_cast<float>(S.disk_used_pct)}); }
        if (items & 16) rows.push_back({L"网络", L"↓ " + FormatSpeedPerSec(S.net_down_bps), -1.0f, true});
        auto row_at = [&](const Row& r, float x, float y, float width) {
            Label(r.label, x, y + 12.0f, width * 0.5f);
            Text(r.value, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, x + width, y + 12.0f, width * 0.6f,
                 r.pct >= 0.0f ? LoadColor(r.pct) : ink_.pri, Align::Right);
            if (r.net) {
                const size_t n = S.net_history.size(), take = std::min<size_t>(n, 24);
                float mx = 1.0f;
                for (size_t i = n - take; i < n; ++i) mx = std::max(mx, S.net_history[i]);
                Sparkline(std::span<const float>(S.net_history).subspan(n - take), D2D1::RectF(x, y + 17.0f, x + width, y + 31.0f), mx * 1.1f, 0.0f, ink_.sec, false);
            } else {
                Bar(x, y + 19.0f, width, 4.0f, r.pct / 100.0f, LoadColor(r.pct));
            }
        };
        if (cols_ == 1) {
            Label(L"CPU", L0, T0 + 11.0f, w_);
            const float tw = Hero(std::to_wstring(static_cast<int>(std::lround(cpu))), 34.0f, L0, T0 + 45.0f, w_, Align::Left, LoadColor(cpu));
            Text(L"%", 16.0f, DWRITE_FONT_WEIGHT_NORMAL, true, L0 + tw + 1.0f, T0 + 45.0f, 20.0f, LoadColor(cpu));
            const size_t n = S.cpu_history.size(), take = std::min<size_t>(n, 20);
            Sparkline(std::span<const float>(S.cpu_history).subspan(n - take), D2D1::RectF(L0, rc_.bottom - pad_ - 22.0f, Rr, rc_.bottom - pad_), 100.0f, 0.0f, ink_.sec, false);
            return;
        }
        if (rows_ == 1) {
            const size_t n = std::min<size_t>(rows.size(), cols_ >= 4 ? 4 : 2);
            const float colw = (Rr - L0 - 16.0f * static_cast<float>(n - 1)) / static_cast<float>(std::max<size_t>(n, 1));
            const float cy = (rc_.top + rc_.bottom) * 0.5f;
            for (size_t i = 0; i < n; ++i) {
                const auto& r = rows[i];
                const float x = L0 + (colw + 16.0f) * static_cast<float>(i);
                Label(r.label, x, cy - 16.0f, colw);
                if (r.net) {
                    Text(FormatSpeedCompact(S.net_down_bps), 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, x, cy + 12.0f, colw, ink_.pri);
                } else {
                    std::swprintf(buf, 64, L"%d%%", static_cast<int>(std::lround(r.pct)));
                    Text(buf, 28.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, x, cy + 13.0f, colw, LoadColor(r.pct));
                    Bar(x, cy + 21.0f, colw, 4.0f, r.pct / 100.0f, LoadColor(r.pct));
                }
            }
            return;
        }
        if (cols_ < 4) {
            const size_t cap = static_cast<size_t>(std::max(1.0f, (h_ - pad_ * 2.0f + 6.0f) / 40.0f));
            const float step = (h_ - pad_ * 2.0f) / static_cast<float>(std::min(cap, rows.size()));
            for (size_t i = 0; i < rows.size() && i < cap; ++i) row_at(rows[i], L0, T0 + step * static_cast<float>(i) + (step - 32.0f) * 0.5f, Rr - L0);
            return;
        }
        // 4×2: CPU with its history on the left, the rest on the right.
        const float lw = std::min(176.0f, w_ * 0.42f);
        Label(L"CPU", L0, T0 + 11.0f, lw);
        const float tw = Hero(std::to_wstring(static_cast<int>(std::lround(cpu))), 44.0f, L0, T0 + 56.0f, lw, Align::Left, LoadColor(cpu));
        Text(L"%", 20.0f, DWRITE_FONT_WEIGHT_NORMAL, true, L0 + tw + 1.0f, T0 + 56.0f, 24.0f, LoadColor(cpu));
        if (S.physical_cores) {
            std::swprintf(buf, 64, L"%u 核 · %u 线程", S.physical_cores, S.logical_cores);
            Caption(buf, L0, T0 + 74.0f, lw);
        }
        Sparkline(S.cpu_history, D2D1::RectF(L0, std::max(T0 + 84.0f, rc_.bottom - pad_ - 64.0f), L0 + lw, rc_.bottom - pad_), 100.0f, 0.0f, ink_.pri, true);
        const float sx = L0 + lw + 18.0f;
        dc_->FillRectangle(D2D1::RectF(sx - 9.0f, T0, sx - 8.0f, rc_.bottom - pad_), R.Brush(ink_.hair));
        std::vector<Row> rest;
        for (const auto& r : rows) if (r.label != L"CPU") rest.push_back(r);
        const size_t cap = static_cast<size_t>(std::max(1.0f, (h_ - pad_ * 2.0f + 6.0f) / 40.0f));
        const size_t n = std::min(cap, rest.size());
        const float step = n ? (h_ - pad_ * 2.0f) / static_cast<float>(n) : 0.0f;
        for (size_t i = 0; i < n; ++i) row_at(rest[i], sx, T0 + step * static_cast<float>(i) + (step - 32.0f) * 0.5f, Rr - sx);
    }

    // ------------------------------------------------------------------ 电池
    struct Device { const IconDef* icon; std::wstring name; int pct; bool charging; bool ac; };
    std::vector<Device> Devices() {
        const auto& B = live_.battery;
        std::vector<Device> out;
        if (B.has_battery) out.push_back({&kIcoLaptop, L"这台电脑", B.battery_pct, B.charging, false});
        else out.push_back({&kIcoPlug, L"交流电源", -1, false, true});
        if (set_.widgets.battery_bluetooth) {
            for (const auto& d : B.bt_devices) {
                const IconDef* ic = d.kind == 1 ? &kIcoPhones : d.kind == 2 ? &kIcoMouse : d.kind == 3 ? &kIcoKeyboard : &kIcoBluetooth;
                out.push_back({ic, d.name, d.percent, false, false});
            }
        }
        return out;
    }
    uint32_t BatteryColor(const Device& d) const {
        if (d.charging) return kGreen;
        if (d.pct >= 0 && d.pct <= set_.widgets.battery_low_pct) return kAlarm;
        return ink_.pri;
    }
    void Battery() {
        const auto& B = live_.battery;
        if (!B.valid) { EmptyState(nullptr, cols_ == 1 ? L"" : L"正在读取电量…", L""); return; }
        const auto devs = Devices();
        const float L0 = rc_.left + pad_, Rr = rc_.right - pad_, T0 = rc_.top + pad_;
        const float cx = (rc_.left + rc_.right) * 0.5f, cy = (rc_.top + rc_.bottom) * 0.5f;
        auto ring_dev = [&](const Device& d, float x, float y, float rad) {
            Ring(x, y, rad, 5.5f, d.ac ? 0.0f : d.pct / 100.0f, BatteryColor(d), ink_.track);
            Icon(*d.icon, Box(x, y, rad * 0.75f), ink_.pri);
        };
        auto pct_text = [&](const Device& d) {
            if (d.ac) return std::wstring(L"电源");
            return std::to_wstring(std::max(0, d.pct)) + L"%";
        };
        if (cols_ == 1) {
            const auto& d = devs.front();
            if (d.ac) {
                Icon(kIcoPlug, Box(L0 + 13.0f, T0 + 13.0f, 26.0f), ink_.pri);
                Body(L"交流电源", L0, rc_.bottom - pad_ - 15.0f, w_ - pad_ * 2.0f);
                Caption(B.power_plan.empty() ? L"已接通" : B.power_plan, L0, rc_.bottom - pad_, w_ - pad_ * 2.0f);
                return;
            }
            ring_dev(d, cx, cy - 8.0f, 30.0f);
            float x = cx;
            const std::wstring t = pct_text(d);
            if (d.charging) {
                const float tw = Measure(t, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
                Icon(kIcoBolt, Box(cx - tw * 0.5f - 6.0f, rc_.bottom - pad_ - 5.0f, 11.0f), kGreen);
                x += 6.0f;
            }
            Body(t, x, rc_.bottom - pad_, w_, Align::Center);
            return;
        }
        if (rows_ == 1) {
            const auto& d = devs.front();
            ring_dev(d, L0 + 32.0f, cy, 30.0f);
            const float x = L0 + 76.0f;
            if (d.ac) {
                Body(L"交流电源", x, cy - 2.0f, Rr - x);
                Caption(B.power_plan, x, cy + 14.0f, Rr - x);
            } else {
                Hero(pct_text(d), 32.0f, x, cy + 6.0f, Rr - x);
                std::wstring sub = d.charging ? L"正在充电" : L"";
                if (!d.charging && B.remaining_min > 0) {
                    wchar_t b[48]{};
                    std::swprintf(b, 48, L"剩余 %d 小时 %d 分", B.remaining_min / 60, B.remaining_min % 60);
                    sub = b;
                }
                Caption(sub, x, cy + 22.0f, Rr - x);
            }
            if (cols_ >= 4 && devs.size() > 1) {
                const float rx0 = rc_.left + w_ * 0.5f;
                const size_t n = std::min<size_t>(devs.size() - 1, 3);
                const float colw = (Rr - rx0) / static_cast<float>(n);
                for (size_t i = 0; i < n; ++i) {
                    const auto& e = devs[i + 1];
                    const float ex = rx0 + colw * (static_cast<float>(i) + 0.5f);
                    ring_dev(e, ex, cy - 9.0f, 21.0f);
                    Caption(pct_text(e), ex, cy + 32.0f, colw, Align::Center, ink_.pri);
                }
            }
            return;
        }
        if (cols_ < 4) {
            // Up to four rings, two by two.
            const float colw = (w_ - pad_ * 2.0f) * 0.5f;
            const float rowh = (h_ - pad_ * 2.0f) * 0.5f;
            const float rad = std::min(32.0f, std::min(colw, rowh - 18.0f) * 0.5f - 2.0f);
            for (size_t i = 0; i < 4; ++i) {
                const float x = L0 + colw * (static_cast<float>(i % 2) + 0.5f);
                const float y = T0 + rowh * static_cast<float>(i / 2) + (rowh - rad * 2.0f - 18.0f) * 0.5f + rad;
                if (i < devs.size()) {
                    ring_dev(devs[i], x, y, rad);
                    std::wstring t = pct_text(devs[i]);
                    if (devs[i].charging) t = L"⚡" + t;
                    Caption(t, x, y + rad + 16.0f, colw, Align::Center, ink_.pri);
                } else {
                    Ring(x, y, rad, 5.5f, 0.0f, 0, ink_.track);
                }
            }
            return;
        }
        // 4×2 and up: a list.
        const float row_h = 36.0f;
        float y = T0;
        for (const auto& d : devs) {
            if (y + row_h > rc_.bottom - pad_ - 18.0f) break;
            const float ry = y + row_h * 0.5f;
            Icon(*d.icon, Box(L0 + 10.0f, ry, 20.0f), ink_.pri);
            Body(d.name, L0 + 32.0f, ry + 5.0f, 90.0f);
            if (d.ac) {
                Caption(B.power_plan, L0 + 130.0f, ry + 4.0f, Rr - L0 - 130.0f);
            } else {
                Bar(L0 + 130.0f, ry - 3.0f, Rr - L0 - 130.0f - 52.0f, 6.0f, d.pct / 100.0f, BatteryColor(d));
                Body(pct_text(d), Rr, ry + 5.0f, 50.0f, Align::Right);
            }
            y += row_h;
        }
        std::wstring foot;
        if (B.has_battery) foot = B.charging ? L"正在充电 · " : L"";
        if (!B.power_plan.empty()) foot += L"电源计划 " + B.power_plan;
        if (B.uptime_sec > 0) {
            wchar_t b[64]{};
            const uint64_t days = B.uptime_sec / 86400, hrs = (B.uptime_sec % 86400) / 3600;
            if (days) std::swprintf(b, 64, L" · 已开机 %llu 天 %llu 小时", static_cast<unsigned long long>(days), static_cast<unsigned long long>(hrs));
            else std::swprintf(b, 64, L" · 已开机 %llu 小时", static_cast<unsigned long long>(hrs));
            foot += b;
        }
        Caption(foot, L0, rc_.bottom - pad_ + 1.0f, Rr - L0);
    }

    // ------------------------------------------------------------------ 正在播放
    void Cover(ID2D1Bitmap* bmp, const D2D1_RECT_F& r, float rad) {
        FillRR(D2D1::RectF(r.left, r.top + 3.0f, r.right, r.bottom + 3.0f), rad, 0x30000000u);
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        if (FAILED(R.d2d_factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, rad, rad), &clip))) return;
        dc_->PushLayer(D2D1::LayerParameters1(r, clip.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.0f, nullptr,
                                              D2D1_LAYER_OPTIONS1_NONE), nullptr);
        if (bmp) {
            const D2D1_SIZE_F bs = bmp->GetSize();
            const float tw = r.right - r.left, th = r.bottom - r.top;
            const float sc = bs.width > 0 && bs.height > 0 ? std::max(tw / bs.width, th / bs.height) : 1.0f;
            const float dw = bs.width * sc, dh = bs.height * sc;
            dc_->DrawBitmap(bmp, D2D1::RectF(r.left + (tw - dw) * 0.5f, r.top + (th - dh) * 0.5f, r.left + (tw + dw) * 0.5f, r.top + (th + dh) * 0.5f),
                            1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            FillVerticalGradient(r, 0.0f, 0xFF3A4A66u, 0xFF1E2638u, 1.0f);
            Icon(kIcoNote, Box((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f, (r.right - r.left) * 0.42f), 0xCCFFFFFFu);
        }
        dc_->PopLayer();
    }
    void TransportButton(int id, const D2D1_RECT_F& box, bool primary) {
        const float cx = (box.left + box.right) * 0.5f, cy = (box.top + box.bottom) * 0.5f, s = box.right - box.left;
        if (primary) dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), s * 0.5f, s * 0.5f), R.Brush(ink_.fill2));
        const IconDef& ic = id == 0 ? kIcoPrev : id == 2 ? kIcoNext : (live_.media.playing ? kIcoPause : kIcoPlay);
        Icon(ic, Box(cx + (id == 1 && !live_.media.playing ? 1.0f : 0.0f), cy, s * (primary ? 0.5f : 0.62f)), ink_.pri);
    }
    void Media() {
        const auto& M = live_.media;
        if (!M.active) {
            EmptyState(&kIcoNote, cols_ == 1 ? L"" : L"没有在播放", L"用任意播放器放音乐或视频，这里就能看到并控制");
            return;
        }
        ID2D1Bitmap* cover = R.EnsureMediaCoverBitmap(M);
        if (set_.widgets.cover_background && cover) {
            // The cover, blurred and darkened, as the card's colour.
            ComPtr<ID2D1Effect> blur;
            ComPtr<ID2D1RoundedRectangleGeometry> clip;
            if (SUCCEEDED(dc_->CreateEffect(CLSID_D2D1GaussianBlur, &blur)) &&
                SUCCEEDED(R.d2d_factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rc_, in_.radius, in_.radius), &clip))) {
                dc_->PushLayer(D2D1::LayerParameters1(rc_, clip.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 0.62f, nullptr,
                                                      D2D1_LAYER_OPTIONS1_NONE), nullptr);
                const D2D1_SIZE_F bs = cover->GetSize();
                const float sc = bs.width > 0 && bs.height > 0 ? std::max(w_ / bs.width, h_ / bs.height) * 1.5f : 1.0f;
                blur->SetInput(0, cover);
                blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 18.0f / std::max(0.1f, sc));
                blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
                D2D1_MATRIX_3X2_F old{};
                dc_->GetTransform(&old);
                const float ox = rc_.left + (w_ - bs.width * sc) * 0.5f, oy = rc_.top + (h_ - bs.height * sc) * 0.5f;
                dc_->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc) * D2D1::Matrix3x2F::Translation(ox, oy) * old);
                dc_->DrawImage(blur.Get());
                dc_->SetTransform(old);
                dc_->FillRectangle(rc_, R.Brush(0x38000000u));
                dc_->PopLayer();
                SetInk(true);
            }
        }
        const float L0 = rc_.left + pad_, Rr = rc_.right - pad_, T0 = rc_.top + pad_;
        auto buttons = [&]() {
            for (int id = 0; id <= 2; ++id) {
                const D2D1_RECT_F b = MediaButtonBox(id);
                if (b.right > b.left) TransportButton(id, b, id == 1);
            }
        };
        if (cols_ == 1) {
            const float s = std::min(w_, h_) - 30.0f;
            Cover(cover, Box((rc_.left + rc_.right) * 0.5f, (rc_.top + rc_.bottom) * 0.5f, s), 10.0f);
            const D2D1_RECT_F b = MediaButtonBox(1);
            if (b.right > b.left) {
                const float bcx = (b.left + b.right) * 0.5f, bcy = (b.top + b.bottom) * 0.5f;
                dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bcx, bcy), 14.0f, 14.0f), R.Brush(0x73000000u));
                Icon(M.playing ? kIcoPause : kIcoPlay, Box(bcx, bcy, 14.0f), 0xFFFFFFFFu);
            }
            return;
        }
        if (rows_ == 1) {
            const float s = h_ - 28.0f;
            Cover(cover, D2D1::RectF(L0, T0, L0 + s, T0 + s), 10.0f);
            const float x = L0 + s + 11.0f;
            const float tr = cols_ >= 4 ? rc_.right - 140.0f : Rr;
            Body(M.title, x, T0 + 14.0f, tr - x);
            Caption(M.artist, x, T0 + 30.0f, tr - x);
            if (cols_ >= 4 && set_.widgets.show_source) Caption(M.source_app, x, T0 + 46.0f, tr - x, Align::Left, ink_.ter);
            buttons();
            return;
        }
        if (cols_ < 4) {
            Cover(cover, D2D1::RectF(L0, T0, L0 + 76.0f, T0 + 76.0f), 10.0f);
            if (M.playing) {
                // A quiet level meter in the corner while it plays.
                const float bx = Rr - 16.0f, by = T0 + 16.0f;
                const float hs[4] = {8, 13, 6, 11};
                for (int i = 0; i < 4; ++i) FillRR(D2D1::RectF(bx + 4.0f * i, by - hs[i], bx + 4.0f * i + 3.0f, by), 1.5f, ink_.sec);
            }
            Body(M.title, L0, rc_.bottom - pad_ - 52.0f, Rr - L0);
            Caption(M.artist, L0, rc_.bottom - pad_ - 37.0f, Rr - L0);
            if (set_.widgets.show_source) Caption(M.source_app, L0, rc_.bottom - pad_ - 8.0f, (Rr - L0) * 0.42f);
            buttons();
            return;
        }
        const float s = std::min(h_ - pad_ * 2.0f, 180.0f);
        Cover(cover, D2D1::RectF(L0, T0, L0 + s, T0 + s), 12.0f);
        const float x = L0 + s + 16.0f, aw = Rr - x;
        float y = T0;
        if (set_.widgets.show_source) { Caption(M.source_app, x, y + 11.0f, aw); y += 16.0f; }
        Text(M.title, 17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, x, y + 22.0f, aw, ink_.pri);
        Text(M.artist, 13.0f, DWRITE_FONT_WEIGHT_MEDIUM, false, x, y + 41.0f, aw, ink_.sec);
        Caption(M.album, x, y + 57.0f, aw, Align::Left, ink_.ter);
        buttons();
    }
    D2D1_RECT_F MediaButtonBox(int id) const { return DesktopRenderer::MediaButtonRect(*in_.tile_in, rc_, id); }

    // ------------------------------------------------------------------ 番茄钟
    void Pomodoro() {
        const auto& P = live_.pomodoro;
        const int left = std::max(0, P.remaining_sec);
        wchar_t t[16]{};
        std::swprintf(t, 16, L"%02d:%02d", left / 60, left % 60);
        const float pct = P.total_sec > 0 ? 1.0f - static_cast<float>(left) / static_cast<float>(P.total_sec) : 0.0f;
        const bool focus = P.phase == live::PomodoroPhase::Focus;
        const uint32_t pc = focus ? kFocus : kGreen;
        const std::wstring phase = focus ? L"专注" : P.phase == live::PomodoroPhase::LongBreak ? L"长休息" : L"休息";
        const float L0 = rc_.left + pad_, Rr = rc_.right - pad_, T0 = rc_.top + pad_;
        const float cx = (rc_.left + rc_.right) * 0.5f, cy = (rc_.top + rc_.bottom) * 0.5f;
        auto play_button = [&](int id, bool pill) {
            const D2D1_RECT_F b = DesktopRenderer::PomodoroButtonRect(*in_.tile_in, rc_, id);
            if (b.right <= b.left) return;
            const float bh = b.bottom - b.top;
            if (id == 1) {
                dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F((b.left + b.right) * 0.5f, (b.top + b.bottom) * 0.5f), bh * 0.5f, bh * 0.5f), R.Brush(ink_.fill));
                Icon(kIcoReset, Box((b.left + b.right) * 0.5f, (b.top + b.bottom) * 0.5f, bh * 0.5f), ink_.pri);
                return;
            }
            FillRR(b, bh * 0.5f, ink_.fill2);
            if (pill) {
                const std::wstring lbl = P.running ? L"暂停" : (left == P.total_sec ? L"开始" : L"继续");
                const float tw = Measure(lbl, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
                const float x0 = (b.left + b.right) * 0.5f - (tw + 19.0f) * 0.5f;
                Icon(P.running ? kIcoPause : kIcoPlay, Box(x0 + 6.5f, (b.top + b.bottom) * 0.5f, 13.0f), ink_.pri);
                Text(lbl, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, x0 + 19.0f, (b.top + b.bottom) * 0.5f + 4.5f, tw + 4.0f, ink_.pri);
            } else {
                Icon(P.running ? kIcoPause : kIcoPlay, Box((b.left + b.right) * 0.5f, (b.top + b.bottom) * 0.5f, bh * 0.5f), ink_.pri);
            }
        };
        if (cols_ == 1) {
            const float rad = std::min(w_, h_) * 0.5f - 12.0f;
            Ring(cx, cy, rad, 5.0f, pct, pc, ink_.track);
            Text(t, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, cx, cy + 3.0f, w_, ink_.pri, Align::Center);
            Caption(P.running ? phase : std::wstring(L"已暂停"), cx, cy + 17.0f, w_, Align::Center);
            return;
        }
        if (rows_ == 1) {
            const float rr = cols_ >= 4 ? 29.0f : 21.0f;
            Ring(L0 + rr + 3.0f, cy, rr, 5.0f, pct, pc, ink_.track);
            dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(L0 + rr + 3.0f, cy), 4.0f, 4.0f), R.Brush(pc));
            const float x = L0 + rr * 2.0f + 16.0f;
            const float room = Rr - x - (cols_ >= 4 ? 80.0f : 42.0f);
            Hero(t, 32.0f, x, cy + 6.0f, room);
            Caption(P.running ? phase + L"中" : std::wstring(L"已暂停"), x, cy + 22.0f, room);
            play_button(0, false);
            play_button(1, false);
            return;
        }
        // Phase (click to switch) and today's count on top, the time, progress, then the controls.
        const float lw = cols_ >= 4 ? w_ * 0.5f : w_;
        const float R0 = rc_.left + lw - pad_;
        dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(L0 + 4.0f, T0 + 7.0f), 4.0f, 4.0f), R.Brush(pc));
        Body(phase, L0 + 14.0f, T0 + 12.0f, 80.0f);
        Caption(L"今天 " + std::to_wstring(P.completed_today) + L" 个", R0, T0 + 12.0f, 80.0f, Align::Right);
        const float base = rc_.bottom - pad_ - 52.0f;
        Hero(t, cols_ >= 4 ? 64.0f : 52.0f, L0 - 2.0f, base, R0 - L0);
        Bar(L0, base + 10.0f, R0 - L0, 4.0f, pct, pc);
        play_button(0, true);
        play_button(1, false);
        wchar_t mins[16]{};
        std::swprintf(mins, 16, L"%d 分钟", P.total_sec / 60);
        if (cols_ < 4) Caption(mins, R0, rc_.bottom - pad_ - 10.0f, 70.0f, Align::Right);
        if (cols_ >= 4) {
            // Presets on the right: 25 / 45 minutes of focus, 5 minutes of rest.
            const float sx = rc_.left + lw + 4.0f;
            dc_->FillRectangle(D2D1::RectF(sx - 10.0f, T0, sx - 9.0f, rc_.bottom - pad_), R.Brush(ink_.hair));
            Label(L"时长", sx, T0 + 12.0f, Rr - sx);
            const std::wstring kPre[3] = {L"专注 25 分钟", L"专注 45 分钟", L"休息 " + std::to_wstring(P.short_min) + L" 分钟"};
            for (int i = 0; i < 3; ++i) {
                const D2D1_RECT_F b = DesktopRenderer::PomodoroButtonRect(*in_.tile_in, rc_, 3 + i);
                if (b.right <= b.left) continue;
                const bool on = (i == 0 && focus && P.focus_minutes == 25) || (i == 1 && focus && P.focus_minutes == 45) || (i == 2 && !focus);
                FillRR(b, 8.0f, on ? ink_.fill2 : ink_.fill);
                Body(kPre[i], b.left + 12.0f, (b.top + b.bottom) * 0.5f + 4.5f, b.right - b.left - 20.0f, Align::Left, on ? ink_.pri : ink_.sec);
            }
        }
    }

    // ------------------------------------------------------------------ 倒数日
    void Countdown() {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        const auto C = live::ComputeCountdown(tile_.title, tile_.countdown_date, st);
        // Without a date it counts to the next 元旦 ("2027 元旦"); the year is in the date line already.
        std::wstring title = C.title;
        if (tile_.countdown_date.empty() && title.size() > 3 && title.ends_with(L" 元旦")) title = L"元旦";
        int y = 0, m = 0, d = 0;
        live::ParseIsoDate(C.target_iso, y, m, d);
        const float L0 = rc_.left + pad_, Rr = rc_.right - pad_, T0 = rc_.top + pad_;
        const std::wstring days = std::to_wstring(std::abs(C.days_delta));
        auto big = [&](float px, float x, float base, float max_w) {
            const float tw = Hero(days, px, x, base, max_w);
            Body(L"天", x + tw + 4.0f, base, 30.0f);
        };
        const std::wstring lead = C.is_today ? L"就是今天" : C.is_past ? L"已经过去" : L"还有";
        if (cols_ == 1) {
            Label(title, L0, T0 + 11.0f, w_ - pad_ * 2.0f);
            if (C.is_today) Body(L"今天", L0, rc_.bottom - pad_ - 2.0f, w_);
            else big(38.0f, L0, rc_.bottom - pad_ - 2.0f, w_ - pad_ * 2.0f - 18.0f);
            return;
        }
        if (rows_ == 1) {
            const float cy = (rc_.top + rc_.bottom) * 0.5f;
            Body(title, L0, cy - 2.0f, w_ * 0.5f);
            wchar_t b[48]{};
            std::swprintf(b, 48, L"%d月%d日 · %ls", m, d, lead.c_str());
            Caption(b, L0, cy + 14.0f, w_ * 0.5f);
            if (C.is_today) { Hero(L"今天", 32.0f, Rr, cy + 12.0f, w_ * 0.45f, Align::Right); return; }
            const float tw = Measure(days, 44.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
            big(44.0f, Rr - tw - 18.0f, cy + 16.0f, tw + 2.0f);
            return;
        }
        const float lw = cols_ >= 4 ? std::min(170.0f, w_ * 0.42f) : w_ - pad_ * 2.0f;
        Label(C.is_past ? L"已经过去" : L"距离", L0, T0 + 11.0f, lw);
        Text(title, 17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, L0, T0 + 31.0f, lw, ink_.pri);
        if (C.is_today) Hero(L"就是今天", 34.0f, L0, rc_.bottom - pad_ - 22.0f, lw);
        else big(64.0f, L0 - 2.0f, rc_.bottom - pad_ - 22.0f, lw - 24.0f);
        if (y > 0) {
            wchar_t b[48]{};
            std::swprintf(b, 48, L"%d年%d月%d日 %ls", y, m, d, WeekdayShort(DayOfWeek(y, m, d)).c_str());
            Caption(b, L0, rc_.bottom - pad_ - 1.0f, lw);
        }
        if (cols_ < 4) return;
        const float sx = L0 + lw + 18.0f;
        dc_->FillRectangle(D2D1::RectF(sx - 9.0f, T0, sx - 8.0f, rc_.bottom - pad_), R.Brush(ink_.hair));
        const std::pair<const wchar_t*, float> bars[4] = {{L"今天", C.day_pct}, {L"本周", C.week_pct}, {L"本月", C.month_pct}, {L"今年", C.year_pct}};
        const float step = (h_ - pad_ * 2.0f) / 4.0f;
        for (int i = 0; i < 4; ++i) {
            const float by = T0 + step * static_cast<float>(i) + (step - 26.0f) * 0.5f;
            Label(bars[i].first, sx, by + 12.0f, Rr - sx);
            Text(std::to_wstring(static_cast<int>(bars[i].second)) + L"%", 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, Rr, by + 12.0f, 60.0f, ink_.pri, Align::Right);
            Bar(sx, by + 19.0f, Rr - sx, 4.0f, bars[i].second / 100.0f, ink_.pri);
        }
    }

    // ------------------------------------------------------------------ 待办
    void Todo() {
        const auto& todos = in_.state->todos;
        size_t open = 0;
        for (const auto& t : todos) open += t.done ? 0 : 1;
        const float L0 = rc_.left + pad_, Rr = rc_.right - pad_, T0 = rc_.top + pad_;
        if (cols_ == 1) {
            Label(L"待办", L0, T0 + 11.0f, w_);
            Hero(std::to_wstring(open), 40.0f, L0, rc_.bottom - pad_ - 16.0f, w_);
            Caption(open ? L"项没做完" : L"都做完了", L0, rc_.bottom - pad_, w_);
            return;
        }
        if (rows_ == 1) {
            const float cy = (rc_.top + rc_.bottom) * 0.5f;
            Hero(std::to_wstring(open), 36.0f, L0, cy + 13.0f, 60.0f);
            const float x = L0 + Measure(std::to_wstring(open), 36.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true) + 12.0f;
            const std::wstring* first = nullptr;
            for (const auto& t : todos) if (!t.done && !t.text.empty()) { first = &t.text; break; }
            Body(L"待办", x, cy - 3.0f, Rr - x);
            Caption(first ? *first : std::wstring(L"都做完了"), x, cy + 13.0f, Rr - x);
            return;
        }
        Body(L"待办", L0, T0 + 12.0f, w_ * 0.5f);
        wchar_t cnt[24]{};
        std::swprintf(cnt, 24, L"%zu/%zu", todos.size() - open, todos.size());
        if (!todos.empty()) Caption(cnt, Rr, T0 + 12.0f, 60.0f, Align::Right, ink_.ter);
        if (todos.empty()) {
            EmptyState(&kIcoCheck, L"都做完了", L"点“添加待办”写下一件事");
        }
        const auto order = DesktopRenderer::TodoDisplayOrder(todos);
        const size_t cap = DesktopRenderer::TodoRowCapacity(*in_.tile_in, rc_);
        const size_t shown = std::min(cap, order.size());
        const int hovered = in_.interaction ? in_.interaction->hovered_todo_index : -1;
        for (size_t i = 0; i < shown; ++i) {
            const auto& td = todos[order[i]];
            const D2D1_RECT_F row = DesktopRenderer::TodoRowRect(*in_.tile_in, rc_, i);
            const float ry = (row.top + row.bottom) * 0.5f;
            const D2D1_POINT_2F c = D2D1::Point2F(row.left + 8.5f, ry);
            if (td.done) {
                dc_->FillEllipse(D2D1::Ellipse(c, 8.5f, 8.5f), R.Brush(in_.accent));
                Icon(kIcoCheck, Box(c.x, c.y, 11.0f), 0xFFFFFFFFu);
            } else {
                dc_->DrawEllipse(D2D1::Ellipse(c, 7.75f, 7.75f), R.Brush(ink_.ter), 1.5f);
            }
            const bool hot = in_.hovered && hovered == static_cast<int>(order[i]);
            const float tx = row.left + 26.0f, tmax = row.right - tx - (hot ? 28.0f : 0.0f);
            const float tw = Text(td.text, 13.0f, DWRITE_FONT_WEIGHT_MEDIUM, false, tx, ry + 4.5f, tmax, td.done ? ink_.ter : ink_.pri);
            if (td.done) dc_->FillRectangle(D2D1::RectF(tx, ry - 0.5f, tx + tw, ry + 0.5f), R.Brush(ink_.ter));
            if (hot) {
                const D2D1_RECT_F x = DesktopRenderer::TodoDeleteRect(*in_.tile_in, rc_, i);
                const float xc = (x.left + x.right) * 0.5f, yc = (x.top + x.bottom) * 0.5f;
                dc_->DrawLine(D2D1::Point2F(xc - 4, yc - 4), D2D1::Point2F(xc + 4, yc + 4), R.Brush(ink_.sec), 1.6f, R.round_stroke_.Get());
                dc_->DrawLine(D2D1::Point2F(xc + 4, yc - 4), D2D1::Point2F(xc - 4, yc + 4), R.Brush(ink_.sec), 1.6f, R.round_stroke_.Get());
            }
        }
        const D2D1_RECT_F add = DesktopRenderer::TodoAddRect(*in_.tile_in, rc_, shown);
        if (add.bottom > add.top) {
            const float ay = (add.top + add.bottom) * 0.5f;
            Icon(kIcoPlus, Box(add.left + 8.5f, ay, 14.0f), ink_.ter);
            Text(L"添加待办", 13.0f, DWRITE_FONT_WEIGHT_MEDIUM, false, add.left + 26.0f, ay + 4.5f, add.right - add.left - 26.0f, ink_.ter);
        }
    }

    // ------------------------------------------------------------------ 便笺
    void Note() {
        static constexpr uint32_t kPaper[6] = {0xFFF7E8A4u, 0xFFD4EAC9u, 0xFFCFE1F2u, 0xFFF4D5DEu, 0xFFE0D6F2u, 0xFFE6E6E2u};
        const int ci = std::clamp(in_.tile_in->note_color, 0, 5);
        // Paper in both themes; a touch dimmer on a dark desktop so it does not glare. The text stays dark.
        FillRR(rc_, in_.radius, kPaper[ci], in_.light_ink ? 0.88f : 0.95f);
        light_ = false;
        ink_ = Ink{0xFF3A3320u, 0x993A3320u, 0x613A3320u, 0x1A3A3320u, 0x1F3A3320u, 0x123A3320u, 0x1F3A3320u};
        shadow_ = false;  // dark text on paper needs no lift
        std::wstring body = tile_.note_body;
        const float L0 = rc_.left + pad_, Rr = rc_.right - pad_, T0 = rc_.top + pad_;
        if (body.empty()) {
            Text(L"双击写点什么", 14.0f, DWRITE_FONT_WEIGHT_MEDIUM, false, L0, T0 + 15.0f, Rr - L0, ink_.ter);
            return;
        }
        // The first line reads as the note's title.
        const size_t nl = body.find(L'\n');
        const std::wstring first = body.substr(0, nl);
        const std::wstring rest = nl == std::wstring::npos ? L"" : body.substr(nl + 1);
        Text(first, 14.0f, DWRITE_FONT_WEIGHT_BOLD, false, L0, T0 + 15.0f, Rr - L0, ink_.pri);
        if (!rest.empty()) {
            IDWriteTextFormat* fmt = R.FaceFormat(14.0f, DWRITE_FONT_WEIGHT_MEDIUM, false);
            ComPtr<IDWriteTextLayout> layout;
            const float top = T0 + 22.0f;
            if (fmt && SUCCEEDED(R.dwrite_factory_->CreateTextLayout(rest.c_str(), static_cast<UINT32>(rest.size()), fmt, Rr - L0,
                                                                     std::max(1.0f, rc_.bottom - pad_ - top), &layout))) {
                layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 22.0f, 17.0f);
                DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                ComPtr<IDWriteInlineObject> ell;
                if (SUCCEEDED(R.dwrite_factory_->CreateEllipsisTrimmingSign(fmt, &ell))) layout->SetTrimming(&trim, ell.Get());
                D2D1_RECT_F clip = D2D1::RectF(L0, top, Rr, rc_.bottom - pad_ + 2.0f);
                dc_->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
                dc_->DrawTextLayout(D2D1::Point2F(L0, top), layout.Get(), R.Brush(ink_.pri));
                dc_->PopAxisAlignedClip();
            }
        }
    }

    // ------------------------------------------------------------------ 本地磁盘
    static std::wstring Gb(uint64_t bytes) {
        wchar_t b[32]{};
        const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
        if (gb >= 1000.0) std::swprintf(b, 32, L"%.1f TB", gb / 1024.0);
        else std::swprintf(b, 32, L"%.0f GB", gb);
        return b;
    }
    void Disk() {
        std::vector<mount::DriveSpaceStats> drives = live_.drives;
        if (drives.empty()) drives.push_back(mount::QueryDriveSpaceStats(tile_.target_path.empty() ? L"C:\\" : tile_.target_path));
        // A tile pinned to one drive shows that one first.
        if (!tile_.target_path.empty())
            std::stable_partition(drives.begin(), drives.end(), [&](const auto& d) { return _wcsnicmp(d.root_path.c_str(), tile_.target_path.c_str(), 2) == 0; });
        auto pct_of = [](const mount::DriveSpaceStats& d) {
            return d.total_bytes ? 100.0f * static_cast<float>(static_cast<double>(d.total_bytes - std::min(d.free_bytes, d.total_bytes)) / static_cast<double>(d.total_bytes)) : 0.0f;
        };
        auto letter = [](const mount::DriveSpaceStats& d) { return std::wstring(1, d.root_path.empty() ? L'C' : static_cast<wchar_t>(std::towupper(d.root_path[0]))) + L":"; };
        const auto& d0 = drives.front();
        const float p0 = pct_of(d0);
        const float L0 = rc_.left + pad_, Rr = rc_.right - pad_, T0 = rc_.top + pad_;
        const float cx = (rc_.left + rc_.right) * 0.5f, cy = (rc_.top + rc_.bottom) * 0.5f;
        if (cols_ == 1) {
            Ring(cx, cy - 9.0f, 27.0f, 5.5f, p0 / 100.0f, LoadColor(p0), ink_.track);
            Body(letter(d0), cx, cy - 4.5f, w_, Align::Center);
            Caption(L"剩 " + Gb(d0.free_bytes), cx, rc_.bottom - pad_, w_, Align::Center);
            return;
        }
        if (rows_ == 1) {
            Ring(L0 + 31.0f, cy, 28.0f, 6.0f, p0 / 100.0f, LoadColor(p0), ink_.track);
            Text(std::to_wstring(static_cast<int>(std::lround(p0))) + L"%", 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, L0 + 31.0f, cy + 4.0f, 56.0f, ink_.pri, Align::Center);
            const float x = L0 + 74.0f;
            Body(L"本地磁盘 (" + letter(d0) + L")", x, cy - 3.0f, Rr - x);
            Caption(L"已用 " + Gb(d0.total_bytes - std::min(d0.free_bytes, d0.total_bytes)) + L"，共 " + Gb(d0.total_bytes), x, cy + 14.0f, Rr - x);
            return;
        }
        Body(L"磁盘", L0, T0 + 12.0f, w_);
        const float row_h = 40.0f;
        float y = T0 + 26.0f;
        for (const auto& d : drives) {
            if (y + 30.0f > rc_.bottom - pad_ + 2.0f) break;
            const float p = pct_of(d);
            Label(letter(d), L0, y + 12.0f, 40.0f, Align::Left, ink_.pri);
            Text(L"剩 " + Gb(d.free_bytes) + L"，共 " + Gb(d.total_bytes), 12.0f, DWRITE_FONT_WEIGHT_MEDIUM, false, Rr, y + 12.0f, Rr - L0 - 40.0f, ink_.sec, Align::Right);
            Bar(L0, y + 19.0f, Rr - L0, 5.0f, p / 100.0f, LoadColor(p));
            y += row_h;
        }
    }

    DesktopRenderer& R;
    const DesktopRenderer::WidgetFaceInput& in_;
    ID2D1DeviceContext* dc_;
    const model::BeautifySettings& set_;
    const LiveTileRuntimeData& live_;
    const model::DesktopTile& tile_;
    D2D1_RECT_F rc_;
    float w_, h_;
    int cols_, rows_;
    bool light_;
    Ink ink_;
    float pad_ = 14.0f;
    bool shadow_ = false;
    bool tinted_ = false;
    uint32_t Today() const { return tinted_ ? ink_.pri : kRed; }
};

// =====================================================================================================
bool DesktopRenderer::HasWidgetFace(model::TileKind kind) noexcept {
    switch (kind) {
    case model::TileKind::Clock: case model::TileKind::Weather: case model::TileKind::Calendar:
    case model::TileKind::System: case model::TileKind::Battery: case model::TileKind::MediaPlayer:
    case model::TileKind::Pomodoro: case model::TileKind::Countdown: case model::TileKind::Todo:
    case model::TileKind::StickyNote: case model::TileKind::DiskRing:
        return true;
    default:
        return false;
    }
}

bool DesktopRenderer::DrawWidgetFace(const WidgetFaceInput& in) {
    if (!in.tile || !in.tile_in || !in.state || !in.live || !d2d_dc_) return false;
    WidgetFacePainter painter(*this, in);
    return painter.Draw();
}

IDWriteTextFormat* DesktopRenderer::FaceFormat(float px, DWRITE_FONT_WEIGHT weight, bool display) {
    const uint32_t key = (static_cast<uint32_t>(std::lround(px * 4.0f)) << 12) | (static_cast<uint32_t>(weight) << 1) | (display ? 1u : 0u);
    auto it = face_formats_.find(key);
    if (it != face_formats_.end()) return it->second.Get();
    if (!dwrite_factory_) return nullptr;
    static const wchar_t* families_display[] = {L"Segoe UI Variable Display", L"Segoe UI", L"Microsoft YaHei UI"};
    static const wchar_t* families_text[] = {L"Segoe UI Variable Text", L"Microsoft YaHei UI", L"Segoe UI"};
    ComPtr<IDWriteFontCollection> sys;
    dwrite_factory_->GetSystemFontCollection(&sys, FALSE);
    const wchar_t* family = L"Segoe UI";
    for (const wchar_t* f : display ? families_display : families_text) {
        UINT32 idx = 0;
        BOOL exists = FALSE;
        if (sys && SUCCEEDED(sys->FindFamilyName(f, &idx, &exists)) && exists) { family = f; break; }
    }
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwrite_factory_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"zh-CN", &fmt)))
        return nullptr;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    ComPtr<IDWriteInlineObject> ell;
    if (SUCCEEDED(dwrite_factory_->CreateEllipsisTrimmingSign(fmt.Get(), &ell)) && ell) {
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        fmt->SetTrimming(&trim, ell.Get());
    }
    auto* raw = fmt.Get();
    face_formats_.emplace(key, std::move(fmt));
    return raw;
}

// ---- 待办 rows, shared with the host's clicks ----
std::vector<size_t> DesktopRenderer::TodoDisplayOrder(const std::vector<model::TodoItem>& todos) {
    std::vector<size_t> order;
    order.reserve(todos.size());
    for (size_t i = 0; i < todos.size(); ++i) if (!todos[i].done) order.push_back(i);
    for (size_t i = 0; i < todos.size(); ++i) if (todos[i].done) order.push_back(i);
    return order;
}
namespace {
constexpr float kTodoTop = 36.0f;    // under the 待办 line
constexpr float kTodoRow = 26.0f;
constexpr float kTodoBottom = 10.0f;
}
size_t DesktopRenderer::TodoRowCapacity(const model::DesktopTile& tile, const D2D1_RECT_F& r) noexcept {
    const float avail = (r.bottom - r.top) - kTodoTop - kTodoBottom - kTodoRow;  // leave room for 添加待办
    const int per_col = std::max(0, static_cast<int>(avail / kTodoRow));
    return static_cast<size_t>(per_col) * (tile.size.cols >= 4 ? 2u : 1u);
}
D2D1_RECT_F DesktopRenderer::TodoRowRect(const model::DesktopTile& tile, const D2D1_RECT_F& r, size_t row) noexcept {
    const size_t cap = TodoRowCapacity(tile, r);
    if (row >= cap) return {};
    const bool two = tile.size.cols >= 4;
    const size_t per_col = two ? cap / 2 : cap;
    const size_t col = per_col ? row / per_col : 0, idx = per_col ? row % per_col : row;
    const float inner_l = r.left + 14.0f, inner_r = r.right - 14.0f;
    const float colw = two ? (inner_r - inner_l - 18.0f) * 0.5f : inner_r - inner_l;
    const float left = inner_l + static_cast<float>(col) * (colw + 18.0f);
    const float top = r.top + kTodoTop + kTodoRow * static_cast<float>(idx);
    return D2D1::RectF(left, top, left + colw, top + kTodoRow);
}
D2D1_RECT_F DesktopRenderer::TodoAddRect(const model::DesktopTile& tile, const D2D1_RECT_F& r, size_t shown) noexcept {
    const size_t cap = TodoRowCapacity(tile, r);
    const bool two = tile.size.cols >= 4;
    const size_t per_col = two ? cap / 2 : cap;
    const size_t below = two ? std::min(shown, per_col) : shown;
    const float top = r.top + kTodoTop + kTodoRow * static_cast<float>(below);
    if (top + kTodoRow > r.bottom - kTodoBottom + 0.5f) return {};
    return D2D1::RectF(r.left + 14.0f, top, two ? (r.left + r.right) * 0.5f - 9.0f : r.right - 14.0f, top + kTodoRow);
}

} // namespace pulse::desktop::render
