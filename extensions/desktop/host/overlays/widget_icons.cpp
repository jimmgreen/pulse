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

struct Base { uint32_t top, bottom; };

Base BaseFor(model::TileKind kind) {
    switch (kind) {
    case model::TileKind::Photos:      return {0xFFFFB86Bu, 0xFFE8556Fu};  // sunset sky
    case model::TileKind::Weather:     return {0xFF6CC6FFu, 0xFF2A7DE1u};  // day sky
    case model::TileKind::Calendar:    return {0xFFFFFFFFu, 0xFFE9EDF2u};  // paper
    case model::TileKind::Todo:        return {0xFF5BD98Au, 0xFF1E9E5Au};
    case model::TileKind::Clock:       return {0xFF3A3F4Bu, 0xFF1C1F27u};  // night dial
    case model::TileKind::StickyNote:  return {0xFFFFE787u, 0xFFF4C443u};  // note paper
    case model::TileKind::Apps:        return {0xFF7F8CFFu, 0xFF4A4FD8u};
    case model::TileKind::Recent:      return {0xFFA88BFFu, 0xFF6B4FD8u};
    case model::TileKind::Downloads:   return {0xFF5BB4FFu, 0xFF1F6FE0u};
    case model::TileKind::System:      return {0xFF4B5670u, 0xFF262C3Bu};  // instrument panel
    case model::TileKind::StagingTray: return {0xFFC39BFFu, 0xFF8754E0u};
    case model::TileKind::DiskRing:    return {0xFFFFC75Au, 0xFFE0892Bu};
    default:                           return {0xFF8A94A6u, 0xFF5B6475u};
    }
}

void DrawScene(Painter& p, model::TileKind kind, IDWriteTextFormat* number_format) {
    auto white = [&](float a = 1.0f) { return p.Solid(0xFFFFFFFFu, a); };
    switch (kind) {
    case model::TileKind::Photos: {
        auto sun = p.Linear({p.X(20), p.Y(6)}, {p.X(28), p.Y(14)}, {{0.0f, Col(0xFFFFF4C2u)}, {1.0f, Col(0xFFFFD36Bu)}});
        p.Circle(24.5f, 10.5f, 4.6f, sun.Get());
        auto far_hills = p.Vertical(14, 34, 0xFF6E5A9Eu, 0xFF4B3F7Au);
        p.Path({{-1, 34}, {9, 17}, {15, 25}, {21, 19}, {35, 34}}, true, far_hills.Get());
        auto near_hills = p.Vertical(20, 34, 0xFF2F6F73u, 0xFF1E4B52u);
        p.Path({{-1, 34}, {-1, 29}, {7, 23}, {16, 31}, {24, 24}, {35, 31}, {35, 34}}, true, near_hills.Get());
        break;
    }
    case model::TileKind::Weather: {
        auto glow = p.Solid(0xFFFFE27Au, 0.35f);
        p.Circle(13.0f, 13.0f, 9.0f, glow);
        auto sun = p.Linear({p.X(7), p.Y(7)}, {p.X(19), p.Y(19)}, {{0.0f, Col(0xFFFFF09Au)}, {1.0f, Col(0xFFFFB627u)}});
        p.Circle(13.0f, 13.0f, 6.2f, sun.Get());
        auto shade = p.Solid(0xFF1B4F9Cu, 0.28f);  // the cloud's soft shadow on the sky
        p.Circle(17.0f, 23.8f, 5.6f, shade); p.Circle(23.0f, 21.3f, 6.6f, shade); p.Box(11.0f, 22.3f, 30.0f, 29.8f, 3.8f, shade);
        auto cloud = p.Vertical(14, 29, 0xFFFFFFFFu, 0xFFDDEBFAu);
        p.Circle(16.5f, 22.5f, 5.4f, cloud.Get()); p.Circle(22.5f, 20.0f, 6.4f, cloud.Get());
        p.Box(10.5f, 21.0f, 29.5f, 28.5f, 3.7f, cloud.Get());
        break;
    }
    case model::TileKind::Calendar: {
        auto band = p.Vertical(0, 11, 0xFFFF6B6Bu, 0xFFE5484Du);
        p.Box(0, 0, 34, 12, 0, band.Get());
        p.Circle(10.0f, 5.8f, 1.4f, white(0.9f)); p.Circle(24.0f, 5.8f, 1.4f, white(0.9f));
        if (number_format) {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            wchar_t day[4]{};
            std::swprintf(day, 4, L"%u", static_cast<unsigned>(now.wDay));
            ComPtr<IDWriteFactory> dw;
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                              reinterpret_cast<IUnknown**>(dw.GetAddressOf()))) &&
                SUCCEEDED(dw->CreateTextLayout(day, static_cast<UINT32>(std::wcslen(day)), number_format,
                                               p.S(34), p.S(22), &layout))) {
                const DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(std::wcslen(day))};
                layout->SetFontSize(p.S(16.5f), all);
                layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, all);
                layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                p.Dc()->DrawTextLayout(D2D1::Point2F(p.X(0), p.Y(11.5f)), layout.Get(), p.Solid(0xFF1F2430u));
            }
        }
        break;
    }
    case model::TileKind::Todo: {
        p.Box(6, 6.5f, 28, 27.5f, 4.5f, white(0.95f));
        const float rows[3] = {11.5f, 17.0f, 22.5f};
        for (int i = 0; i < 3; ++i) {
            const float y = rows[i];
            if (i < 2) {
                p.Circle(10.8f, y, 2.4f, p.Solid(0xFF27B467u));
                p.Path({{9.7f, y}, {10.5f, y + 0.9f}, {12.0f, y - 0.9f}}, false, nullptr, white(), 0.9f);
                p.Line(15.0f, y, 23.5f - i * 3.0f, y, 1.6f, p.Solid(0xFF27B467u, 0.35f));
            } else {
                p.Ring(10.8f, y, 2.0f, 0.9f, p.Solid(0xFF27B467u, 0.8f));
                p.Line(15.0f, y, 24.0f, y, 1.6f, p.Solid(0xFF1F2430u, 0.45f));
            }
        }
        break;
    }
    case model::TileKind::Clock: {
        auto face = p.Vertical(5, 29, 0xFFFFFFFFu, 0xFFE6E9EFu);
        p.Circle(17.0f, 17.5f, 12.4f, p.Solid(0xFF000000u, 0.35f));
        p.Circle(17.0f, 17.0f, 12.0f, face.Get());
        for (int i = 0; i < 12; ++i) {
            const float a = static_cast<float>(i) * 30.0f * 3.14159265f / 180.0f;
            const float inner = (i % 3 == 0) ? 8.6f : 9.8f;
            p.Line(17.0f + std::sin(a) * inner, 17.0f - std::cos(a) * inner, 17.0f + std::sin(a) * 10.6f,
                   17.0f - std::cos(a) * 10.6f, i % 3 == 0 ? 1.2f : 0.6f, p.Solid(0xFF2B2F3Au, i % 3 == 0 ? 0.9f : 0.45f));
        }
        p.Line(17.0f, 17.0f, 12.5f, 12.8f, 1.8f, p.Solid(0xFF1F2430u));   // ≈10:10, the watchmaker's smile
        p.Line(17.0f, 17.0f, 23.4f, 11.0f, 1.3f, p.Solid(0xFF1F2430u));
        p.Line(17.0f, 19.5f, 17.0f, 8.0f, 0.6f, p.Solid(0xFFFF8A2Bu));
        p.Circle(17.0f, 17.0f, 1.5f, p.Solid(0xFFFF8A2Bu));
        break;
    }
    case model::TileKind::StickyNote: {
        auto fold = p.Linear({p.X(25), p.Y(25)}, {p.X(31), p.Y(31)}, {{0.0f, Col(0xFFFFF4C4u)}, {1.0f, Col(0xFFE0A92Au)}});
        p.Path({{23, 34}, {23, 26}, {34, 23}}, true, p.Solid(0xFF9A6B00u, 0.25f));
        p.Path({{23, 34}, {23.5f, 24.5f}, {34, 23}}, true, fold.Get());
        for (int i = 0; i < 3; ++i)
            p.Line(7.0f, 10.0f + i * 5.5f, i == 2 ? 17.0f : 26.0f, 10.0f + i * 5.5f, 1.4f, p.Solid(0xFF8A5A00u, 0.55f));
        break;
    }
    case model::TileKind::Apps: {
        const uint32_t tiles[4][2] = {{0xFFFF8FB1u, 0xFFF0507Au}, {0xFFFFD36Bu, 0xFFF5A524u},
                                      {0xFF6EE7C8u, 0xFF1FB58Fu}, {0xFFFFFFFFu, 0xFFDDE2FFu}};
        for (int i = 0; i < 4; ++i) {
            const float l = 7.0f + static_cast<float>(i % 2) * 10.5f, t = 7.0f + static_cast<float>(i / 2) * 10.5f;
            p.Box(l, t + 0.8f, l + 9.0f, t + 9.8f, 2.6f, p.Solid(0xFF1E1F66u, 0.30f));
            auto fill = p.Vertical(t, t + 9, tiles[i][0], tiles[i][1]);
            p.Box(l, t, l + 9.0f, t + 9.0f, 2.6f, fill.Get());
        }
        break;
    }
    case model::TileKind::Recent: {
        p.Box(11, 6, 26, 25, 2.5f, white(0.45f));
        p.Box(8, 9, 23, 28, 2.5f, p.Solid(0xFF2E1A7Au, 0.25f));
        p.Box(8, 8, 23, 27, 2.5f, white(0.96f));
        for (int i = 0; i < 3; ++i) p.Line(11.0f, 13.0f + i * 4.0f, i == 2 ? 16.5f : 20.0f, 13.0f + i * 4.0f, 1.3f, p.Solid(0xFF6B4FD8u, 0.55f));
        p.Circle(24.5f, 24.5f, 6.0f, p.Solid(0xFF4A2FB8u));
        p.Ring(24.5f, 24.5f, 6.0f, 1.2f, white());
        p.Path({{24.5f, 21.2f}, {24.5f, 24.7f}, {26.8f, 26.0f}}, false, nullptr, white(), 1.2f);
        break;
    }
    case model::TileKind::Downloads: {
        p.Path({{8.5f, 20.5f}, {8.5f, 25.5f}, {25.5f, 25.5f}, {25.5f, 20.5f}}, false, nullptr, p.Solid(0xFF0D3F99u, 0.35f), 3.2f);
        p.Path({{8.5f, 20}, {8.5f, 25}, {25.5f, 25}, {25.5f, 20}}, false, nullptr, white(), 2.6f);
        p.Line(17.0f, 7.5f, 17.0f, 19.5f, 2.6f, white());
        p.Path({{12.5f, 15.5f}, {17.0f, 20.0f}, {21.5f, 15.5f}}, false, nullptr, white(), 2.6f);
        break;
    }
    case model::TileKind::System: {
        p.Arc(17.0f, 19.5f, 10.0f, -120.0f, 240.0f, 3.2f, white(0.18f));
        auto meter = p.Linear({p.X(7), p.Y(19)}, {p.X(27), p.Y(19)},
                              {{0.0f, Col(0xFF4ADE80u)}, {0.6f, Col(0xFFFACC15u)}, {1.0f, Col(0xFFFB7185u)}});
        p.Arc(17.0f, 19.5f, 10.0f, -120.0f, 165.0f, 3.2f, meter.Get());
        p.Line(17.0f, 19.5f, 23.0f, 13.8f, 1.6f, white());
        p.Circle(17.0f, 19.5f, 2.2f, white());
        p.Line(12.0f, 27.5f, 22.0f, 27.5f, 1.4f, white(0.4f));
        break;
    }
    case model::TileKind::StagingTray: {
        p.Box(8, 13.5f, 26, 27.5f, 2.5f, p.Solid(0xFF3B1C8Au, 0.28f));
        p.Box(8, 12.5f, 26, 26.5f, 2.5f, white(0.94f));
        p.Box(6.5f, 8, 27.5f, 13.5f, 2.0f, white());
        p.Line(14.0f, 17.5f, 20.0f, 17.5f, 1.8f, p.Solid(0xFF8754E0u, 0.75f));
        break;
    }
    case model::TileKind::DiskRing: {
        p.Ring(17.0f, 17.0f, 9.5f, 4.2f, white(0.30f));
        p.Arc(17.0f, 17.0f, 9.5f, 0.0f, 250.0f, 4.2f, white());
        p.Circle(17.0f, 17.0f, 3.0f, p.Solid(0xFF7A3E00u, 0.35f));
        break;
    }
    default:
        p.Box(9, 9, 25, 25, 4, white(0.9f));
        break;
    }
}
} // namespace

void DrawWidgetIcon(ID2D1DeviceContext* dc, model::TileKind kind, const D2D1_RECT_F& rect, float radius,
                    IDWriteTextFormat* number_format) {
    if (!dc) return;
    Painter p(dc, rect);
    ID2D1Factory* factory = p.Factory();
    ComPtr<ID2D1RoundedRectangleGeometry> shape;
    if (!factory || FAILED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, radius, radius), &shape))) return;

    // Lift: a soft contact shadow under the badge.
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.left + 1, rect.top + 2, rect.right - 1, rect.bottom + 2), radius, radius),
                             p.Solid(0xFF000000u, 0.22f));

    dc->PushLayer(D2D1::LayerParameters1(rect, shape.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
    const Base base = BaseFor(kind);
    auto fill = p.Linear({p.X(4), p.Y(0)}, {p.X(30), p.Y(34)}, {{0.0f, Col(base.top)}, {1.0f, Col(base.bottom)}});
    dc->FillRectangle(rect, fill.Get());
    DrawScene(p, kind, number_format);
    // Glass: light from above, the top half a touch brighter.
    auto sheen = p.Linear({p.X(17), p.Y(0)}, {p.X(17), p.Y(18)}, {{0.0f, D2D1::ColorF(1, 1, 1, 0.22f)}, {1.0f, D2D1::ColorF(1, 1, 1, 0.0f)}});
    dc->FillRectangle(rect, sheen.Get());
    dc->PopLayer();

    // Rim: bright along the top edge, fading down the sides.
    auto rim = p.Linear({p.X(17), p.Y(0)}, {p.X(17), p.Y(34)}, {{0.0f, D2D1::ColorF(1, 1, 1, 0.55f)}, {0.5f, D2D1::ColorF(1, 1, 1, 0.10f)},
                                                               {1.0f, D2D1::ColorF(0, 0, 0, 0.18f)}});
    const D2D1_RECT_F inner = D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f, rect.right - 0.5f, rect.bottom - 0.5f);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(inner, radius - 0.5f, radius - 0.5f), rim.Get(), 1.0f);
}

} // namespace pulse::desktop::overlays
