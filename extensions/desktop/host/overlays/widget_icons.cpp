#include "host/overlays/widget_icons.h"

#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <initializer_list>

namespace pulse::desktop::overlays {
namespace {
using Microsoft::WRL::ComPtr;

D2D1_COLOR_F Col(uint32_t argb, float alpha_mul = 1.0f) {
    return D2D1::ColorF(static_cast<float>((argb >> 16) & 255) / 255.0f, static_cast<float>((argb >> 8) & 255) / 255.0f,
                        static_cast<float>(argb & 255) / 255.0f, static_cast<float>((argb >> 24) & 255) / 255.0f * alpha_mul);
}

class Painter {
public:
    Painter(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) : dc_(dc), r_(r) { dc->GetFactory(&factory_); }

    // Points in a 34×34 design box, mapped onto the badge.
    float X(float v) const { return r_.left + v * (r_.right - r_.left) / 34.0f; }
    float Y(float v) const { return r_.top + v * (r_.bottom - r_.top) / 34.0f; }
    float S(float v) const { return v * (r_.right - r_.left) / 34.0f; }
    D2D1_RECT_F R(float l, float t, float rt, float b) const { return D2D1::RectF(X(l), Y(t), X(rt), Y(b)); }

    ID2D1SolidColorBrush* Solid(uint32_t argb, float alpha = 1.0f) {
        solid_.Reset();
        dc_->CreateSolidColorBrush(Col(argb, alpha), &solid_);
        return solid_.Get();
    }
    ComPtr<ID2D1LinearGradientBrush> Linear(D2D1_POINT_2F a, D2D1_POINT_2F b, std::initializer_list<D2D1_GRADIENT_STOP> stops) {
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> brush;
        if (SUCCEEDED(dc_->CreateGradientStopCollection(stops.begin(), static_cast<UINT32>(stops.size()), &collection)))
            dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), collection.Get(), &brush);
        return brush;
    }
    ComPtr<ID2D1LinearGradientBrush> Vertical(float top, float bottom, uint32_t from, uint32_t to) {
        return Linear({X(17), Y(top)}, {X(17), Y(bottom)}, {{0.0f, Col(from)}, {1.0f, Col(to)}});
    }
    void Circle(float cx, float cy, float rad, ID2D1Brush* brush) {
        if (brush) dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(X(cx), Y(cy)), S(rad), S(rad)), brush);
    }
    void Ring(float cx, float cy, float rad, float width, ID2D1Brush* brush) {
        if (brush) dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(X(cx), Y(cy)), S(rad), S(rad)), brush, S(width));
    }
    void Box(float l, float t, float rt, float b, float rad, ID2D1Brush* brush) {
        if (brush) dc_->FillRoundedRectangle(D2D1::RoundedRect(R(l, t, rt, b), S(rad), S(rad)), brush);
    }
    void Line(float x0, float y0, float x1, float y1, float width, ID2D1Brush* brush) {
        if (brush) dc_->DrawLine(D2D1::Point2F(X(x0), Y(y0)), D2D1::Point2F(X(x1), Y(y1)), brush, S(width), Round());
    }
    // Polygon / polyline through design points.
    void Path(std::initializer_list<D2D1_POINT_2F> pts, bool closed, ID2D1Brush* fill, ID2D1Brush* stroke = nullptr, float width = 0) {
        ComPtr<ID2D1PathGeometry> geo;
        ComPtr<ID2D1GeometrySink> sink;
        if (!factory_ || FAILED(factory_->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) return;
        auto it = pts.begin();
        sink->BeginFigure(D2D1::Point2F(X(it->x), Y(it->y)), fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (++it; it != pts.end(); ++it) sink->AddLine(D2D1::Point2F(X(it->x), Y(it->y)));
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        sink->Close();
        if (fill) dc_->FillGeometry(geo.Get(), fill);
        if (stroke) dc_->DrawGeometry(geo.Get(), stroke, S(width), Round());
    }
    // Arc from `start` sweeping `sweep` degrees clockwise (0° = 12 o'clock).
    void Arc(float cx, float cy, float rad, float start, float sweep, float width, ID2D1Brush* brush) {
        if (!brush || sweep <= 0.1f) return;
        ComPtr<ID2D1PathGeometry> geo;
        ComPtr<ID2D1GeometrySink> sink;
        if (!factory_ || FAILED(factory_->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) return;
        const auto at = [&](float deg) {
            const float a = (deg - 90.0f) * 3.14159265f / 180.0f;
            return D2D1::Point2F(X(cx) + S(rad) * std::cos(a), Y(cy) + S(rad) * std::sin(a));
        };
        sink->BeginFigure(at(start), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(at(start + sweep), D2D1::SizeF(S(rad), S(rad)), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      sweep > 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        dc_->DrawGeometry(geo.Get(), brush, S(width), Round());
    }
    ID2D1StrokeStyle* Round() {
        if (!round_ && factory_) {
            factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                    D2D1_LINE_JOIN_ROUND), nullptr, 0, &round_);
        }
        return round_.Get();
    }
    ID2D1Factory* Factory() const { return factory_.Get(); }
    ID2D1DeviceContext* Dc() const { return dc_; }

private:
    ID2D1DeviceContext* dc_;
    D2D1_RECT_F r_;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<ID2D1SolidColorBrush> solid_;
    ComPtr<ID2D1StrokeStyle> round_;
};

struct Base { uint32_t top, bottom; bool paper; };

// Body colours, lit from above (macOS icon grid: a vertical gradient, never a diagonal one). Paper icons
// (Calendar, Reminders, Notes, Photos) are white or tinted paper with a hairline rim.
Base BaseFor(model::TileKind kind) {
    switch (kind) {
    case model::TileKind::Photos:      return {0xFFFFFFFFu, 0xFFEEEFF3u, true};
    case model::TileKind::Weather:     return {0xFF5FB8FFu, 0xFF1D6BDBu, false};
    case model::TileKind::Calendar:    return {0xFFFFFFFFu, 0xFFEEEFF3u, true};
    case model::TileKind::Todo:        return {0xFFFFFFFFu, 0xFFEEEFF3u, true};
    case model::TileKind::Clock:       return {0xFF3A3D45u, 0xFF141519u, false};
    case model::TileKind::StickyNote:  return {0xFFFFE98Cu, 0xFFF6C944u, true};
    case model::TileKind::Apps:        return {0xFF5F6675u, 0xFF2E333Du, false};
    case model::TileKind::Recent:      return {0xFFB08CFFu, 0xFF6C46DFu, false};
    case model::TileKind::Downloads:   return {0xFF5CB2FFu, 0xFF1A6ADFu, false};
    case model::TileKind::System:      return {0xFF39404Du, 0xFF12161Du, false};
    case model::TileKind::StagingTray: return {0xFF5DD3CBu, 0xFF158B86u, false};
    case model::TileKind::DiskRing:    return {0xFFF6F7F9u, 0xFFD4D8DFu, true};
    case model::TileKind::Pomodoro:    return {0xFFFF7D6Eu, 0xFFE0352Bu, false};
    case model::TileKind::MediaPlayer: return {0xFFFF6E8Au, 0xFFEE2A52u, false};
    case model::TileKind::Countdown:   return {0xFFFFB648u, 0xFFF0760Cu, false};
    case model::TileKind::Battery:     return {0xFF5BE07Au, 0xFF1EA249u, false};
    default:                           return {0xFF9AA2B1u, 0xFF5E6676u, false};
    }
}

// Text drawn with the caller's format family at a design size (the calendar's weekday and date).
void DrawLabel(Painter& p, IDWriteTextFormat* format, const wchar_t* s, float top, float height, float size,
               DWRITE_FONT_WEIGHT weight, uint32_t argb) {
    if (!format || !s || !*s) return;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IDWriteTextLayout> layout;
    const UINT32 n = static_cast<UINT32>(std::wcslen(s));
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dw.GetAddressOf()))) ||
        FAILED(dw->CreateTextLayout(s, n, format, p.S(34), p.S(height), &layout)))
        return;
    const DWRITE_TEXT_RANGE all{0, n};
    layout->SetFontSize(p.S(size), all);
    layout->SetFontWeight(weight, all);
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    p.Dc()->DrawTextLayout(D2D1::Point2F(p.X(0), p.Y(top)), layout.Get(), p.Solid(argb));
}

void DrawScene(Painter& p, model::TileKind kind, IDWriteTextFormat* number_format) {
    auto white = [&](float a = 1.0f) { return p.Solid(0xFFFFFFFFu, a); };
    switch (kind) {
    case model::TileKind::Photos: {
        // A landscape print: sky, sun, two hills.
        auto sky = p.Vertical(8, 26, 0xFF86CCFFu, 0xFF3C8AE8u);
        p.Box(6.5f, 8.0f, 27.5f, 26.0f, 3.5f, sky.Get());
        p.Circle(22.0f, 12.8f, 2.6f, p.Solid(0xFFFFD54Au));
        auto hills = p.Vertical(15, 26, 0xFF6FD68Bu, 0xFF2FA65Bu);
        p.Path({{6.5f, 26.0f}, {6.5f, 21.0f}, {12.5f, 15.5f}, {19.5f, 22.0f}, {23.0f, 19.0f}, {27.5f, 23.0f}, {27.5f, 26.0f}}, true, hills.Get());
        break;
    }
    case model::TileKind::Weather: {
        auto sun = p.Linear({p.X(8), p.Y(7)}, {p.X(18), p.Y(17)}, {{0.0f, Col(0xFFFFF08Au)}, {1.0f, Col(0xFFFFB81Fu)}});
        p.Circle(13.0f, 12.5f, 5.4f, sun.Get());
        auto cloud = p.Vertical(15, 28, 0xFFFFFFFFu, 0xFFE3EEFBu);
        p.Circle(16.0f, 22.0f, 4.9f, cloud.Get()); p.Circle(22.0f, 19.6f, 6.0f, cloud.Get());
        p.Box(10.5f, 20.5f, 28.5f, 27.0f, 3.3f, cloud.Get());
        break;
    }
    case model::TileKind::Calendar: {
        // The weekday in red over the date, as on a desk calendar.
        SYSTEMTIME now{};
        GetLocalTime(&now);
        static constexpr const wchar_t* kWd[7] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};
        DrawLabel(p, number_format, kWd[now.wDayOfWeek % 7], 3.0f, 8.0f, 6.6f, DWRITE_FONT_WEIGHT_BOLD, 0xFFFF3B30u);
        wchar_t day[4]{};
        std::swprintf(day, 4, L"%u", static_cast<unsigned>(now.wDay));
        DrawLabel(p, number_format, day, 9.5f, 22.0f, 17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0xFF1D1D1Fu);
        break;
    }
    case model::TileKind::Todo: {
        // Three reminders: coloured rings and grey lines.
        const uint32_t dots[3] = {0xFF0A84FFu, 0xFFFF9F0Au, 0xFFFF453Au};
        const float rows[3] = {10.5f, 17.0f, 23.5f};
        for (int i = 0; i < 3; ++i) {
            p.Ring(9.5f, rows[i], 2.4f, 1.6f, p.Solid(dots[i]));
            if (i == 0) p.Circle(9.5f, rows[i], 1.2f, p.Solid(dots[i]));
            p.Box(14.0f, rows[i] - 1.0f, i == 1 ? 24.0f : 27.0f, rows[i] + 1.0f, 1.0f, p.Solid(0xFFB9BDC6u));
        }
        break;
    }
    case model::TileKind::Clock: {
        // A white dial: hour ticks, black hands, the orange sweep hand.
        p.Circle(17.0f, 17.0f, 12.6f, white());
        for (int h = 0; h < 12; ++h) {
            const float a = static_cast<float>(h) * 3.14159265f / 6.0f;
            const float r1 = h % 3 == 0 ? 9.4f : 10.4f, r2 = 11.4f;
            p.Line(17.0f + r1 * std::sin(a), 17.0f - r1 * std::cos(a), 17.0f + r2 * std::sin(a), 17.0f - r2 * std::cos(a),
                   h % 3 == 0 ? 1.2f : 0.7f, p.Solid(0xFF1D1D1Fu, h % 3 == 0 ? 0.9f : 0.45f));
        }
        p.Line(17.0f, 17.0f, 12.2f, 12.0f, 1.9f, p.Solid(0xFF1D1D1Fu));   // hour, ~10:10
        p.Line(17.0f, 17.0f, 24.5f, 12.4f, 1.6f, p.Solid(0xFF1D1D1Fu));   // minute
        p.Line(17.0f, 19.5f, 17.0f, 7.5f, 0.8f, p.Solid(0xFFFF9500u));     // sweep
        p.Circle(17.0f, 17.0f, 1.4f, p.Solid(0xFFFF9500u));
        break;
    }
    case model::TileKind::StickyNote: {
        p.Box(0, 0, 34, 7.0f, 0, p.Solid(0xFFE0A91Fu, 0.35f));            // the gummed strip
        for (const float y : {13.0f, 18.5f, 24.0f})
            p.Box(7.0f, y - 0.7f, y == 24.0f ? 20.0f : 27.0f, y + 0.7f, 0.7f, p.Solid(0xFF8A6A12u, 0.45f));
        break;
    }
    case model::TileKind::Apps: {
        const uint32_t c[4] = {0xFF0A84FFu, 0xFF30D158u, 0xFFFF9F0Au, 0xFFFF375Fu};
        const float x[2] = {8.0f, 18.0f}, y[2] = {8.0f, 18.0f};
        for (int i = 0; i < 4; ++i) p.Box(x[i % 2], y[i / 2], x[i % 2] + 8.0f, y[i / 2] + 8.0f, 2.4f, p.Solid(c[i]));
        break;
    }
    case model::TileKind::Recent: {
        // A page with a clock on its corner.
        p.Box(8.5f, 6.5f, 22.5f, 25.5f, 2.6f, white(0.96f));
        for (const float y : {11.0f, 15.0f, 19.0f}) p.Box(11.0f, y - 0.6f, 18.5f, y + 0.6f, 0.6f, p.Solid(0xFF6C46DFu, 0.35f));
        p.Circle(23.0f, 22.5f, 6.4f, p.Solid(0xFF6C46DFu));
        p.Circle(23.0f, 22.5f, 5.2f, white());
        p.Line(23.0f, 22.5f, 23.0f, 19.4f, 1.2f, p.Solid(0xFF6C46DFu));
        p.Line(23.0f, 22.5f, 25.4f, 23.8f, 1.2f, p.Solid(0xFF6C46DFu));
        break;
    }
    case model::TileKind::Downloads: {
        p.Line(17.0f, 7.5f, 17.0f, 20.5f, 2.8f, white());
        p.Path({{11.0f, 15.5f}, {17.0f, 21.8f}, {23.0f, 15.5f}}, false, nullptr, white(), 2.8f);
        p.Line(9.0f, 26.0f, 25.0f, 26.0f, 2.8f, white());
        break;
    }
    case model::TileKind::System: {
        // An instrument screen with a green trace.
        p.Box(6.0f, 8.0f, 28.0f, 26.0f, 3.0f, p.Solid(0xFF000000u, 0.45f));
        for (const float y : {14.0f, 20.0f}) p.Line(7.5f, y, 26.5f, y, 0.5f, p.Solid(0xFF33D17Au, 0.22f));
        p.Path({{7.5f, 21.0f}, {11.0f, 21.0f}, {13.0f, 13.5f}, {15.5f, 23.5f}, {18.0f, 16.0f}, {20.5f, 19.5f}, {26.5f, 19.5f}},
               false, nullptr, p.Solid(0xFF3BE07Cu), 1.6f);
        break;
    }
    case model::TileKind::StagingTray: {
        // An open tray.
        p.Path({{8.0f, 18.5f}, {11.0f, 9.5f}, {23.0f, 9.5f}, {26.0f, 18.5f}}, false, nullptr, white(), 2.2f);
        p.Path({{8.0f, 18.5f}, {13.0f, 18.5f}, {14.5f, 21.0f}, {19.5f, 21.0f}, {21.0f, 18.5f}, {26.0f, 18.5f}, {26.0f, 25.0f}, {8.0f, 25.0f}},
               true, white(0.95f));
        break;
    }
    case model::TileKind::DiskRing: {
        // A drive: a dark slab, a status light, a grille.
        auto slab = p.Vertical(11, 24, 0xFF8E96A3u, 0xFF4B5361u);
        p.Box(6.0f, 11.0f, 28.0f, 24.0f, 3.2f, slab.Get());
        p.Circle(23.5f, 17.5f, 1.4f, p.Solid(0xFF3BE07Cu));
        for (const float x : {9.5f, 12.0f, 14.5f}) p.Line(x, 14.5f, x, 20.5f, 1.1f, p.Solid(0xFFFFFFFFu, 0.45f));
        break;
    }
    case model::TileKind::Pomodoro: {
        // A timer: the remaining arc and a knob on top.
        p.Ring(17.0f, 18.0f, 9.0f, 2.6f, white(0.32f));
        p.Arc(17.0f, 18.0f, 9.0f, 0.0f, 250.0f, 2.6f, white());
        p.Box(15.0f, 5.0f, 19.0f, 7.4f, 1.0f, white());
        p.Circle(17.0f, 18.0f, 1.8f, white());
        break;
    }
    case model::TileKind::MediaPlayer: {
        // A beamed pair of notes.
        p.Path({{13.0f, 23.0f}, {13.0f, 10.5f}, {25.0f, 8.0f}, {25.0f, 20.5f}}, false, nullptr, white(), 2.0f);
        p.Line(13.0f, 13.6f, 25.0f, 11.1f, 2.6f, white());
        p.Circle(10.6f, 23.5f, 3.3f, white()); p.Circle(22.6f, 21.0f, 3.3f, white());
        break;
    }
    case model::TileKind::Countdown: {
        // An hourglass.
        p.Line(10.0f, 7.5f, 24.0f, 7.5f, 2.2f, white());
        p.Line(10.0f, 26.5f, 24.0f, 26.5f, 2.2f, white());
        p.Path({{11.5f, 8.5f}, {22.5f, 8.5f}, {17.0f, 17.0f}}, true, white(0.35f));
        p.Path({{11.5f, 25.5f}, {22.5f, 25.5f}, {17.0f, 17.0f}}, true, white());
        p.Path({{11.5f, 8.5f}, {17.0f, 17.0f}, {11.5f, 25.5f}}, false, nullptr, white(), 1.5f);
        p.Path({{22.5f, 8.5f}, {17.0f, 17.0f}, {22.5f, 25.5f}}, false, nullptr, white(), 1.5f);
        break;
    }
    case model::TileKind::Battery: {
        p.Box(6.5f, 11.5f, 25.0f, 22.5f, 3.2f, white(0.32f));
        p.Box(8.3f, 13.3f, 19.5f, 20.7f, 1.8f, white());
        p.Box(25.8f, 15.0f, 27.8f, 19.0f, 1.0f, white(0.85f));
        break;
    }
    default:
        p.Box(10, 10, 24, 24, 3.5f, white(0.9f));
        break;
    }
}

// macOS's continuous-corner icon shape: a superellipse |x|^n + |y|^n = 1 (n = 5) through the badge.
ComPtr<ID2D1PathGeometry> Squircle(ID2D1Factory* factory, const D2D1_RECT_F& r) {
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (!factory || FAILED(factory->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) return nullptr;
    const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f;
    const float ax = (r.right - r.left) * 0.5f, ay = (r.bottom - r.top) * 0.5f;
    constexpr int kSteps = 96;
    constexpr float kN = 5.0f;
    for (int i = 0; i < kSteps; ++i) {
        const float t = static_cast<float>(i) * 2.0f * 3.14159265f / kSteps;
        const float c = std::cos(t), s = std::sin(t);
        const float x = cx + ax * std::copysign(std::pow(std::abs(c), 2.0f / kN), c);
        const float y = cy + ay * std::copysign(std::pow(std::abs(s), 2.0f / kN), s);
        if (i == 0) sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED);
        else sink->AddLine(D2D1::Point2F(x, y));
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return geo;
}
} // namespace

void DrawWidgetIcon(ID2D1DeviceContext* dc, model::TileKind kind, const D2D1_RECT_F& rect, float /*radius*/,
                    IDWriteTextFormat* number_format) {
    if (!dc) return;
    Painter p(dc, rect);
    ID2D1Factory* factory = p.Factory();
    auto shape = Squircle(factory, rect);
    if (!shape) return;
    const Base base = BaseFor(kind);

    // A soft shadow under the icon, offset down (light from above).
    {
        D2D1_MATRIX_3X2_F old{};
        dc->GetTransform(&old);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, p.S(1.2f)) * old);
        dc->FillGeometry(shape.Get(), p.Solid(0xFF000000u, 0.16f));
        dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, p.S(0.5f)) * old);
        dc->FillGeometry(shape.Get(), p.Solid(0xFF000000u, 0.12f));
        dc->SetTransform(old);
    }

    dc->PushLayer(D2D1::LayerParameters1(rect, shape.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
    auto fill = p.Vertical(0, 34, base.top, base.bottom);
    dc->FillRectangle(rect, fill.Get());
    DrawScene(p, kind, number_format);
    // The upper half catches a little more light.
    auto sheen = p.Linear({p.X(17), p.Y(0)}, {p.X(17), p.Y(17)},
                          {{0.0f, D2D1::ColorF(1, 1, 1, base.paper ? 0.0f : 0.16f)}, {1.0f, D2D1::ColorF(1, 1, 1, 0.0f)}});
    dc->FillRectangle(rect, sheen.Get());
    dc->PopLayer();

    // Rim: a light hairline on top for coloured icons, a faint grey edge so paper icons hold on light walls.
    if (base.paper) {
        dc->DrawGeometry(shape.Get(), p.Solid(0xFF000000u, 0.12f), 0.75f);
    } else {
        auto rim = p.Linear({p.X(17), p.Y(0)}, {p.X(17), p.Y(34)},
                            {{0.0f, D2D1::ColorF(1, 1, 1, 0.42f)}, {0.45f, D2D1::ColorF(1, 1, 1, 0.06f)}, {1.0f, D2D1::ColorF(0, 0, 0, 0.10f)}});
        dc->DrawGeometry(shape.Get(), rim.Get(), 0.75f);
    }
}

} // namespace pulse::desktop::overlays
