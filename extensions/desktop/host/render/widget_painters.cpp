#include "host/render/widget_painters.h"
#include "host/model/world_cities.h"

#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>

namespace pulse::desktop::render {
namespace {

using Microsoft::WRL::ComPtr;

constexpr float kPi = 3.14159265358979323846f;

inline D2D1_COLOR_F ArgbToColorF(uint32_t argb, float opacity_mul = 1.0f) noexcept {
    const float a = (static_cast<float>((argb >> 24) & 0xFFu) / 255.0f) * opacity_mul;
    const float r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    const float g = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>(argb & 0xFFu) / 255.0f;
    return D2D1::ColorF(r, g, b, std::clamp(a, 0.0f, 1.0f));
}

enum class WxIconKind { Sun, Moon, PartlyDay, PartlyNight, Cloud, Fog, Drizzle, Rain, Snow, Thunder };

WxIconKind WxIconFromWmo(int code, bool is_day) noexcept {
    if (code == 0) return is_day ? WxIconKind::Sun : WxIconKind::Moon;
    if (code <= 2) return is_day ? WxIconKind::PartlyDay : WxIconKind::PartlyNight;
    if (code == 3) return WxIconKind::Cloud;
    if (code == 45 || code == 48) return WxIconKind::Fog;
    if (code >= 51 && code <= 57) return WxIconKind::Drizzle;
    if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return WxIconKind::Rain;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WxIconKind::Snow;
    if (code >= 95) return WxIconKind::Thunder;
    return WxIconKind::Cloud;
}

ComPtr<ID2D1Geometry> CombineGeometries(ID2D1Factory* f,
                                        ID2D1Geometry* a,
                                        ID2D1Geometry* b,
                                        D2D1_COMBINE_MODE mode) {
    ComPtr<ID2D1PathGeometry> out;
    if (!f || !a || !b || FAILED(f->CreatePathGeometry(&out)) || !out) return {};
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(out->Open(&sink)) || !sink) return {};
    a->CombineWithGeometry(b, mode, nullptr, sink.Get());
    sink->Close();
    return out;
}

ComPtr<ID2D1Geometry> MakeCircleGeometry(ID2D1Factory* f, float cx, float cy, float r) {
    ComPtr<ID2D1EllipseGeometry> g;
    if (f) f->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), &g);
    return g;
}

ComPtr<ID2D1Geometry> MakeCloudGeometry(ID2D1Factory* f, float cx, float cy, float s) {
    ComPtr<ID2D1RoundedRectangleGeometry> base;
    if (FAILED(f->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(
                D2D1::RectF(cx - 0.40f * s, cy + 0.02f * s, cx + 0.44f * s, cy + 0.25f * s),
                0.115f * s, 0.115f * s),
            &base)) || !base) {
        return {};
    }
    auto c1 = MakeCircleGeometry(f, cx - 0.19f * s, cy + 0.05f * s, 0.20f * s);
    auto c2 = MakeCircleGeometry(f, cx + 0.04f * s, cy - 0.06f * s, 0.27f * s);
    auto c3 = MakeCircleGeometry(f, cx + 0.26f * s, cy + 0.08f * s, 0.17f * s);
    auto g = CombineGeometries(f, c1.Get(), c2.Get(), D2D1_COMBINE_MODE_UNION);
    g = CombineGeometries(f, g.Get(), c3.Get(), D2D1_COMBINE_MODE_UNION);
    return CombineGeometries(f, g.Get(), base.Get(), D2D1_COMBINE_MODE_UNION);
}

ComPtr<ID2D1Geometry> MakeMoonGeometry(ID2D1Factory* f, float cx, float cy, float s) {
    auto outer = MakeCircleGeometry(f, cx, cy, 0.32f * s);
    auto cutout = MakeCircleGeometry(f, cx + 0.19f * s, cy - 0.13f * s, 0.27f * s);
    return CombineGeometries(f, outer.Get(), cutout.Get(), D2D1_COMBINE_MODE_EXCLUDE);
}

void DrawSunGlyphInternal(ID2D1DeviceContext* dc,
                          ID2D1SolidColorBrush* br,
                          ID2D1StrokeStyle* st,
                          float cx,
                          float cy,
                          float s,
                          D2D1_COLOR_F col) {
    // Soft luminous solar halo
    br->SetColor(D2D1::ColorF(col.r, col.g, col.b, 0.24f));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 0.29f * s, 0.29f * s), br);
    br->SetColor(col);
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 0.20f * s, 0.20f * s), br);
    const float ray_w = std::max(1.1f, 0.075f * s);
    for (int i = 0; i < 8; ++i) {
        const float a = static_cast<float>(i) * (kPi * 0.25f);
        const float dx = std::sin(a);
        const float dy = -std::cos(a);
        dc->DrawLine(D2D1::Point2F(cx + dx * 0.31f * s, cy + dy * 0.31f * s),
                     D2D1::Point2F(cx + dx * 0.44f * s, cy + dy * 0.44f * s),
                     br, ray_w, st);
    }
}

struct WorldCitySpec {
    const wchar_t* label;
    const wchar_t* tz_key;
};

bool LookupTimeInZone(const std::wstring& tz_key, const SYSTEMTIME& utc, SYSTEMTIME& out) {
    static std::mutex mu;
    static std::map<std::wstring, DYNAMIC_TIME_ZONE_INFORMATION> cache;
    DYNAMIC_TIME_ZONE_INFORMATION info{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mu);
        auto it = cache.find(tz_key);
        if (it == cache.end()) {
            DYNAMIC_TIME_ZONE_INFORMATION d{};
            for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &d) == ERROR_SUCCESS; ++i) {
                if (_wcsicmp(d.TimeZoneKeyName, tz_key.c_str()) == 0) {
                    it = cache.emplace(tz_key, d).first;
                    break;
                }
            }
        }
        if (it != cache.end()) {
            info = it->second;
            found = true;
        }
    }
    if (!found) return false;
    return SystemTimeToTzSpecificLocalTimeEx(&info, &utc, &out) != FALSE;
}

int64_t SystemTimeToTotalMinutes(const SYSTEMTIME& t) noexcept {
    FILETIME ft{};
    if (!SystemTimeToFileTime(&t, &ft)) return 0;
    const uint64_t ticks = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<int64_t>(ticks / 600000000ULL);
}

} // namespace

void DrawTahoeWeatherIcon(ID2D1Factory* factory,
                          ID2D1DeviceContext* dc,
                          ID2D1SolidColorBrush* brush,
                          ID2D1StrokeStyle* round_stroke,
                          const D2D1_RECT_F& rect,
                          int wmo_code,
                          bool is_day,
                          bool dark_ink_light) {
    if (!factory || !dc || !brush) return;
    const float w = rect.right - rect.left;
    const float h = rect.bottom - rect.top;
    const float s = std::min(w, h);
    if (s <= 2.0f) return;
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;

    const D2D1_COLOR_F sun_col = dark_ink_light
        ? D2D1::ColorF(1.00f, 0.84f, 0.14f, 0.98f)
        : D2D1::ColorF(0.95f, 0.64f, 0.06f, 0.98f);
    const D2D1_COLOR_F moon_col = dark_ink_light
        ? D2D1::ColorF(0.95f, 0.96f, 1.00f, 0.96f)
        : D2D1::ColorF(0.32f, 0.42f, 0.62f, 0.96f);
    const D2D1_COLOR_F cloud_col = dark_ink_light
        ? D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.96f)
        : D2D1::ColorF(0.54f, 0.62f, 0.72f, 0.96f);
    const D2D1_COLOR_F storm_cloud = dark_ink_light
        ? D2D1::ColorF(0.88f, 0.92f, 0.98f, 0.96f)
        : D2D1::ColorF(0.42f, 0.50f, 0.61f, 0.96f);
    const D2D1_COLOR_F drop_col = dark_ink_light
        ? D2D1::ColorF(0.42f, 0.82f, 1.00f, 0.98f)
        : D2D1::ColorF(0.12f, 0.54f, 0.92f, 0.98f);
    const D2D1_COLOR_F snow_col = dark_ink_light
        ? D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.96f)
        : D2D1::ColorF(0.45f, 0.65f, 0.88f, 0.96f);

    auto fill_geo = [&](const ComPtr<ID2D1Geometry>& g, D2D1_COLOR_F c) {
        if (!g) return;
        brush->SetColor(c);
        dc->FillGeometry(g.Get(), brush);
    };

    const WxIconKind ic = WxIconFromWmo(wmo_code, is_day);
    const float ccy = cy - 0.12f * s;
    switch (ic) {
    case WxIconKind::Sun:
        DrawSunGlyphInternal(dc, brush, round_stroke, cx, cy, s, sun_col);
        break;
    case WxIconKind::Moon:
        fill_geo(MakeMoonGeometry(factory, cx, cy, s), moon_col);
        break;
    case WxIconKind::PartlyDay:
        DrawSunGlyphInternal(dc, brush, round_stroke, cx - 0.14f * s, cy - 0.14f * s, 0.78f * s, sun_col);
        fill_geo(MakeCloudGeometry(factory, cx + 0.08f * s, cy + 0.06f * s, 0.82f * s), cloud_col);
        break;
    case WxIconKind::PartlyNight:
        fill_geo(MakeMoonGeometry(factory, cx - 0.14f * s, cy - 0.16f * s, 0.72f * s), moon_col);
        fill_geo(MakeCloudGeometry(factory, cx + 0.08f * s, cy + 0.06f * s, 0.82f * s), cloud_col);
        break;
    case WxIconKind::Cloud:
        fill_geo(MakeCloudGeometry(factory, cx, cy, s), cloud_col);
        break;
    case WxIconKind::Fog: {
        fill_geo(MakeCloudGeometry(factory, cx, cy - 0.10f * s, 0.88f * s), cloud_col);
        brush->SetColor(dark_ink_light ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.82f)
                                       : D2D1::ColorF(0.45f, 0.53f, 0.64f, 0.88f));
        const float lw = std::max(1.1f, 0.072f * s);
        dc->DrawLine(D2D1::Point2F(cx - 0.36f * s, cy + 0.28f * s),
                     D2D1::Point2F(cx + 0.36f * s, cy + 0.28f * s), brush, lw, round_stroke);
        dc->DrawLine(D2D1::Point2F(cx - 0.26f * s, cy + 0.40f * s),
                     D2D1::Point2F(cx + 0.30f * s, cy + 0.40f * s), brush, lw, round_stroke);
        break;
    }
    case WxIconKind::Drizzle:
    case WxIconKind::Rain:
    case WxIconKind::Snow:
    case WxIconKind::Thunder: {
        fill_geo(MakeCloudGeometry(factory, cx, ccy, 0.92f * s),
                 ic == WxIconKind::Thunder ? storm_cloud : cloud_col);
        if (ic == WxIconKind::Thunder) {
            ComPtr<ID2D1PathGeometry> bolt;
            ComPtr<ID2D1GeometrySink> sk;
            if (SUCCEEDED(factory->CreatePathGeometry(&bolt)) && bolt && SUCCEEDED(bolt->Open(&sk)) && sk) {
                const D2D1_POINT_2F pts[] = {
                    { cx + 0.02f * s, cy + 0.12f * s }, { cx - 0.12f * s, cy + 0.33f * s },
                    { cx - 0.01f * s, cy + 0.33f * s }, { cx - 0.06f * s, cy + 0.50f * s },
                    { cx + 0.12f * s, cy + 0.26f * s }, { cx + 0.01f * s, cy + 0.26f * s },
                };
                sk->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
                sk->AddLines(pts + 1, 5);
                sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                sk->Close();
                brush->SetColor(sun_col);
                dc->FillGeometry(bolt.Get(), brush);
            }
            break;
        }
        brush->SetColor(ic == WxIconKind::Snow ? snow_col : drop_col);
        const float drop_w = std::max(1.1f, 0.075f * s);
        for (int i = -1; i <= 1; ++i) {
            const float x = cx + static_cast<float>(i) * 0.19f * s;
            const float y = cy + 0.22f * s;
            if (ic == WxIconKind::Rain) {
                dc->DrawLine(D2D1::Point2F(x + 0.04f * s, y),
                             D2D1::Point2F(x - 0.04f * s, y + 0.20f * s),
                             brush, drop_w, round_stroke);
            } else if (ic == WxIconKind::Drizzle) {
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y + 0.06f * s), 0.045f * s, 0.045f * s), brush);
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x - 0.04f * s, y + 0.20f * s), 0.045f * s, 0.045f * s), brush);
            } else {
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y + 0.12f * s + (i == 0 ? 0.06f * s : 0.0f)),
                                              0.06f * s, 0.06f * s), brush);
            }
        }
        break;
    }
    }
}

void DrawTahoeLocationArrow(ID2D1Factory* factory,
                            ID2D1DeviceContext* dc,
                            ID2D1SolidColorBrush* brush,
                            float x,
                            float y,
                            uint32_t argb,
                            float opacity) {
    if (!factory || !dc || !brush) return;
    ComPtr<ID2D1PathGeometry> p;
    ComPtr<ID2D1GeometrySink> sk;
    if (FAILED(factory->CreatePathGeometry(&p)) || !p || FAILED(p->Open(&sk)) || !sk) return;
    const D2D1_POINT_2F pts[] = {
        { x + 8.5f, y - 4.5f },
        { x + 4.5f, y + 4.5f },
        { x + 3.5f, y + 1.0f },
        { x, y },
    };
    sk->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
    sk->AddLines(pts + 1, 3);
    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
    sk->Close();
    brush->SetColor(ArgbToColorF(argb, opacity));
    dc->FillGeometry(p.Get(), brush);
}

void DrawTahoeGlassCard(ID2D1DeviceContext* dc,
                        ID2D1SolidColorBrush* brush,
                        const D2D1_RECT_F& rect,
                        float radius,
                        bool dark_ink_light) {
    if (!dc || !brush || rect.right <= rect.left || rect.bottom <= rect.top) return;
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect, radius, radius);
    const D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, dark_ink_light ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.135f)
                              : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.48f)},
        {1.0f, dark_ink_light ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.055f)
                              : D2D1::ColorF(0.08f, 0.11f, 0.16f, 0.055f)},
    };
    ComPtr<ID2D1GradientStopCollection> sc;
    ComPtr<ID2D1LinearGradientBrush> lg;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &sc)) && sc &&
        SUCCEEDED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.left, rect.bottom)),
            sc.Get(), &lg)) && lg) {
        dc->FillRoundedRectangle(rr, lg.Get());
    } else {
        brush->SetColor(dark_ink_light ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f)
                                       : D2D1::ColorF(0.08f, 0.11f, 0.16f, 0.065f));
        dc->FillRoundedRectangle(rr, brush);
    }
    // Delicate top-edge specular gleam
    if (rect.right - rect.left > radius * 2.0f + 4.0f) {
        brush->SetColor(dark_ink_light ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.24f)
                                       : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.65f));
        dc->DrawLine(D2D1::Point2F(rect.left + radius, rect.top + 0.75f),
                     D2D1::Point2F(rect.right - radius, rect.top + 0.75f), brush, 0.8f);
    }
    brush->SetColor(dark_ink_light ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f)
                                   : D2D1::ColorF(0.08f, 0.11f, 0.16f, 0.14f));
    dc->DrawRoundedRectangle(rr, brush, 0.75f);
}

void DrawTahoeWeatherSkyWash(ID2D1Factory* factory,
                             ID2D1DeviceContext* dc,
                             const D2D1_RECT_F& rect,
                             float corner_radius,
                             int wmo_code,
                             bool is_day,
                             bool dark_ink_light) {
    if (!factory || !dc || rect.right <= rect.left || rect.bottom <= rect.top) return;
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    if (FAILED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, corner_radius, corner_radius), &clip)) || !clip) {
        return;
    }

    const WxIconKind kind = WxIconFromWmo(wmo_code, is_day);
    D2D1_COLOR_F top_col{}, mid_col{}, bot_col{};
    const float gain = dark_ink_light ? 1.0f : 0.72f;
    if (!is_day) {
        // Clear/cloudy night: rich Apple Weather midnight indigo -> deep slate violet
        top_col = D2D1::ColorF(0.07f, 0.14f, 0.38f, 0.42f * gain);
        mid_col = D2D1::ColorF(0.11f, 0.17f, 0.36f, 0.24f * gain);
        bot_col = D2D1::ColorF(0.06f, 0.08f, 0.18f, 0.12f * gain);
    } else if (kind == WxIconKind::Sun) {
        // Clear sunny day: crisp cerulean sky blue -> warm sunlit mist
        top_col = D2D1::ColorF(0.10f, 0.48f, 0.92f, 0.36f * gain);
        mid_col = D2D1::ColorF(0.24f, 0.62f, 0.96f, 0.20f * gain);
        bot_col = D2D1::ColorF(0.42f, 0.74f, 0.98f, 0.08f * gain);
    } else if (kind == WxIconKind::PartlyDay) {
        // Partly cloudy: bright sky blue + soft sunlit haze
        top_col = D2D1::ColorF(0.16f, 0.46f, 0.82f, 0.32f * gain);
        mid_col = D2D1::ColorF(0.30f, 0.56f, 0.82f, 0.18f * gain);
        bot_col = D2D1::ColorF(0.50f, 0.64f, 0.78f, 0.06f * gain);
    } else if (kind == WxIconKind::Rain || kind == WxIconKind::Drizzle || kind == WxIconKind::Thunder) {
        // Rain/storm: moody deep storm blue-slate
        top_col = D2D1::ColorF(0.14f, 0.24f, 0.42f, 0.38f * gain);
        mid_col = D2D1::ColorF(0.18f, 0.28f, 0.44f, 0.22f * gain);
        bot_col = D2D1::ColorF(0.10f, 0.15f, 0.26f, 0.10f * gain);
    } else {
        // Fog / Overcast / Snow: luminous silver-ice slate
        top_col = D2D1::ColorF(0.36f, 0.48f, 0.64f, 0.28f * gain);
        mid_col = D2D1::ColorF(0.46f, 0.56f, 0.68f, 0.16f * gain);
        bot_col = D2D1::ColorF(0.32f, 0.40f, 0.52f, 0.06f * gain);
    }

    dc->PushLayer(D2D1::LayerParameters1(rect, clip.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                         D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                  nullptr);
    const D2D1_GRADIENT_STOP stops[3] = {{0.0f, top_col}, {0.55f, mid_col}, {1.0f, bot_col}};
    ComPtr<ID2D1GradientStopCollection> sc;
    ComPtr<ID2D1LinearGradientBrush> lg;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 3, &sc)) && sc &&
        SUCCEEDED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.right, rect.bottom)),
            sc.Get(), &lg)) && lg) {
        dc->FillRectangle(rect, lg.Get());
    }
    // Subtle sunlit radial glow in upper corner on sunny/partly-cloudy days
    if (is_day && (kind == WxIconKind::Sun || kind == WxIconKind::PartlyDay)) {
        const float gx = rect.right - (rect.right - rect.left) * 0.18f;
        const float gy = rect.top + (rect.bottom - rect.top) * 0.18f;
        const float gr = std::max(56.0f, std::min(rect.right - rect.left, rect.bottom - rect.top) * 0.62f);
        const D2D1_GRADIENT_STOP rs[2] = {
            {0.0f, D2D1::ColorF(1.0f, 0.88f, 0.48f, dark_ink_light ? 0.22f : 0.18f)},
            {1.0f, D2D1::ColorF(1.0f, 0.82f, 0.36f, 0.0f)},
        };
        ComPtr<ID2D1GradientStopCollection> rsc;
        ComPtr<ID2D1RadialGradientBrush> rgb;
        if (SUCCEEDED(dc->CreateGradientStopCollection(rs, 2, &rsc)) && rsc &&
            SUCCEEDED(dc->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(D2D1::Point2F(gx, gy), D2D1::Point2F(0, 0), gr, gr),
                rsc.Get(), &rgb)) && rgb) {
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(gx, gy), gr, gr), rgb.Get());
        }
    }
    dc->PopLayer();
}

void DrawTahoeHeaderBadge(ID2D1DeviceContext* dc,
                          ID2D1SolidColorBrush* brush,
                          const D2D1_RECT_F& rect,
                          uint32_t accent_argb,
                          bool dark_ink_light) {
    if (!dc || !brush || rect.right <= rect.left || rect.bottom <= rect.top) return;
    const float w = rect.right - rect.left;
    const float rad = std::clamp(w * 0.30f, 4.5f, 8.0f);
    const D2D1_COLOR_F c = ArgbToColorF(accent_argb, 1.0f);
    const D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, D2D1::ColorF(std::min(1.0f, c.r * 1.18f + 0.08f), std::min(1.0f, c.g * 1.18f + 0.08f),
                            std::min(1.0f, c.b * 1.18f + 0.08f), dark_ink_light ? 0.34f : 0.22f)},
        {1.0f, D2D1::ColorF(c.r * 0.88f, c.g * 0.88f, c.b * 0.88f, dark_ink_light ? 0.18f : 0.12f)},
    };
    ComPtr<ID2D1GradientStopCollection> sc;
    ComPtr<ID2D1LinearGradientBrush> lg;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &sc)) && sc &&
        SUCCEEDED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.left, rect.bottom)),
            sc.Get(), &lg)) && lg) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, rad, rad), lg.Get());
    } else {
        brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, 0.24f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, rad, rad), brush);
    }
    brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, dark_ink_light ? 0.44f : 0.32f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, rad, rad), brush, 0.75f);
}

void DrawTahoeIconAura(ID2D1DeviceContext* dc,
                       float cx,
                       float cy,
                       float radius,
                       uint32_t accent_argb,
                       bool dark_ink_light) {
    if (!dc || radius <= 2.0f) return;
    const D2D1_COLOR_F c = ArgbToColorF(accent_argb, 1.0f);
    const D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, D2D1::ColorF(c.r, c.g, c.b, dark_ink_light ? 0.28f : 0.16f)},
        {1.0f, D2D1::ColorF(c.r, c.g, c.b, 0.0f)},
    };
    ComPtr<ID2D1GradientStopCollection> sc;
    ComPtr<ID2D1RadialGradientBrush> rgb;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &sc)) && sc &&
        SUCCEEDED(dc->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), radius, radius),
            sc.Get(), &rgb)) && rgb) {
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), rgb.Get());
    }
}

void DrawTahoeGlassRing(ID2D1Factory* factory,
                        ID2D1DeviceContext* dc,
                        ID2D1SolidColorBrush* brush,
                        ID2D1StrokeStyle* round_stroke,
                        float cx,
                        float cy,
                        float radius,
                        float stroke_w,
                        float percent,
                        uint32_t accent_argb,
                        bool dark_ink_light) {
    if (!dc || !brush || radius <= 1.0f) return;

    // Translucent glass track + subtle inner depth rim
    brush->SetColor(dark_ink_light ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f)
                                   : D2D1::ColorF(0.08f, 0.11f, 0.16f, 0.11f));
    dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush, stroke_w);
    brush->SetColor(dark_ink_light ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f)
                                   : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.35f));
    dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius + stroke_w * 0.45f, radius + stroke_w * 0.45f), brush, 0.65f);

    const float clamped = std::clamp(percent, 0.0f, 100.0f);
    const float frac = clamped / 100.0f;
    if (frac <= 0.004f) return;

    const D2D1_COLOR_F col = ArgbToColorF(accent_argb, 1.0f);
    D2D1_COLOR_F glow = col;
    glow.a = dark_ink_light ? 0.30f : 0.20f;

    if (frac >= 0.998f) {
        brush->SetColor(glow);
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush, stroke_w * 2.15f);
        brush->SetColor(col);
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush, stroke_w);
        return;
    }

    if (!factory) return;
    ComPtr<ID2D1PathGeometry> arc;
    ComPtr<ID2D1GeometrySink> sk;
    if (FAILED(factory->CreatePathGeometry(&arc)) || !arc || FAILED(arc->Open(&sk)) || !sk) return;

    const float ang = frac * 2.0f * kPi;
    const D2D1_POINT_2F start_pt{ cx, cy - radius };
    const D2D1_POINT_2F end_pt{ cx + radius * std::sin(ang), cy - radius * std::cos(ang) };
    sk->BeginFigure(start_pt, D2D1_FIGURE_BEGIN_HOLLOW);
    sk->AddArc(D2D1::ArcSegment(
        end_pt, D2D1::SizeF(radius, radius), 0.0f,
        D2D1_SWEEP_DIRECTION_CLOCKWISE,
        frac > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sk->EndFigure(D2D1_FIGURE_END_OPEN);
    sk->Close();

    brush->SetColor(glow);
    dc->DrawGeometry(arc.Get(), brush, stroke_w * 2.15f, round_stroke);
    brush->SetColor(col);
    dc->DrawGeometry(arc.Get(), brush, stroke_w, round_stroke);

    // Subtle luminous end-cap specular highlight (Apple Activity ring polish)
    if (stroke_w >= 3.4f) {
        brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, dark_ink_light ? 0.52f : 0.42f));
        dc->FillEllipse(D2D1::Ellipse(end_pt, stroke_w * 0.24f, stroke_w * 0.24f), brush);
    }
}

SparklineRange ComputeMemoryZoomRange(std::span<const float> history, float total_gb) {
    if (history.empty()) return {0.0f, 100.0f};
    float lo = history[0];
    float hi = history[0];
    for (float v : history) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    lo = std::clamp(lo, 0.0f, 100.0f);
    hi = std::clamp(hi, 0.0f, 100.0f);

    // Keep at least ~123 MB of vertical range on large-RAM hosts (clamped to [0.15%, 2.0%])
    // so genuine 15-60 MB shifts read clearly without amplifying tiny noise into jitter.
    const float min_span = (total_gb > 0.5f)
        ? std::clamp((0.12f / total_gb) * 100.0f, 0.15f, 2.0f)
        : 2.0f;
    const float eff_span = std::min(100.0f, std::max(min_span, (hi - lo) * 1.32f));
    const float mid = (lo + hi) * 0.5f;
    float y_min = mid - eff_span * 0.5f;
    float y_max = mid + eff_span * 0.5f;
    if (y_min < 0.0f) {
        y_min = 0.0f;
        y_max = std::min(100.0f, eff_span);
    }
    if (y_max > 100.0f) {
        y_max = 100.0f;
        y_min = std::max(0.0f, 100.0f - eff_span);
    }
    if (y_max - y_min < 0.05f) {
        y_max = std::min(100.0f, y_min + 0.05f);
        y_min = std::max(0.0f, y_max - 0.05f);
    }
    return {y_min, y_max};
}

std::wstring FormatMemoryZoomLabel(const SparklineRange& range) {
    const float span = std::max(0.0f, range.max_val - range.min_val);
    wchar_t buf[32]{};
    if (span >= 10.0f) {
        std::swprintf(buf, ARRAYSIZE(buf), L"%ld–%ld%%",
                      std::lround(range.min_val), std::lround(range.max_val));
    } else if (std::lround(range.min_val * 10.0f) != std::lround(range.max_val * 10.0f)) {
        std::swprintf(buf, ARRAYSIZE(buf), L"%.1f–%.1f%%", range.min_val, range.max_val);
    } else {
        std::swprintf(buf, ARRAYSIZE(buf), L"%.2f–%.2f%%", range.min_val, range.max_val);
    }
    return buf;
}

void DrawTahoeSparkline(ID2D1Factory* factory,
                        ID2D1DeviceContext* dc,
                        ID2D1SolidColorBrush* brush,
                        ID2D1StrokeStyle* round_stroke,
                        std::span<const float> history,
                        float max_val,
                        const D2D1_RECT_F& plot,
                        uint32_t accent_argb,
                        bool fill_area,
                        bool dark_ink_light,
                        size_t max_slots,
                        float min_val) {
    if (!factory || !dc || !brush) return;
    const float pw = plot.right - plot.left;
    const float ph = plot.bottom - plot.top;
    if (pw < 4.0f || ph < 4.0f) return;

    const D2D1_COLOR_F accent = ArgbToColorF(accent_argb, 1.0f);
    if (history.empty()) {
        brush->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b, 0.35f));
        dc->DrawLine(D2D1::Point2F(plot.left, plot.bottom - 1.0f),
                     D2D1::Point2F(plot.right, plot.bottom - 1.0f), brush, 1.2f, round_stroke);
        return;
    }

    const float safe_span = (min_val == 0.0f)
        ? std::max(1.0f, max_val)
        : std::max(0.05f, max_val - min_val);
    const size_t n = history.size();
    const size_t slots = std::max<size_t>(2, std::max(max_slots, n));

    auto pt_at = [&](size_t idx) -> D2D1_POINT_2F {
        const size_t slot = (slots > n) ? (slots - n + idx) : idx;
        const float u = static_cast<float>(slot) / static_cast<float>(slots - 1);
        const float v = std::clamp((history[idx] - min_val) / safe_span, 0.0f, 1.0f);
        return D2D1::Point2F(plot.left + u * pw, plot.bottom - v * ph);
    };

    if (n == 1) {
        const D2D1_POINT_2F p = pt_at(0);
        brush->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b, 0.32f));
        dc->FillEllipse(D2D1::Ellipse(p, 4.4f, 4.4f), brush);
        brush->SetColor(accent);
        dc->FillEllipse(D2D1::Ellipse(p, 2.4f, 2.4f), brush);
        return;
    }

    if (fill_area) {
        ComPtr<ID2D1PathGeometry> area_geo;
        ComPtr<ID2D1GeometrySink> sk;
        if (SUCCEEDED(factory->CreatePathGeometry(&area_geo)) && area_geo && SUCCEEDED(area_geo->Open(&sk)) && sk) {
            const D2D1_POINT_2F first = pt_at(0);
            const D2D1_POINT_2F last = pt_at(n - 1);
            sk->BeginFigure(D2D1::Point2F(first.x, plot.bottom), D2D1_FIGURE_BEGIN_FILLED);
            sk->AddLine(first);
            for (size_t i = 1; i < n; ++i) sk->AddLine(pt_at(i));
            sk->AddLine(D2D1::Point2F(last.x, plot.bottom));
            sk->EndFigure(D2D1_FIGURE_END_CLOSED);
            sk->Close();

            const D2D1_GRADIENT_STOP stops[2] = {
                { 0.0f, D2D1::ColorF(accent.r, accent.g, accent.b, dark_ink_light ? 0.36f : 0.28f) },
                { 1.0f, D2D1::ColorF(accent.r, accent.g, accent.b, 0.02f) },
            };
            ComPtr<ID2D1GradientStopCollection> sc;
            ComPtr<ID2D1LinearGradientBrush> lg;
            if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &sc)) && sc &&
                SUCCEEDED(dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, plot.top),
                                                        D2D1::Point2F(0.0f, plot.bottom)),
                    sc.Get(), &lg)) && lg) {
                dc->FillGeometry(area_geo.Get(), lg.Get());
            }
        }
    }

    ComPtr<ID2D1PathGeometry> line_geo;
    ComPtr<ID2D1GeometrySink> sk;
    if (SUCCEEDED(factory->CreatePathGeometry(&line_geo)) && line_geo && SUCCEEDED(line_geo->Open(&sk)) && sk) {
        sk->BeginFigure(pt_at(0), D2D1_FIGURE_BEGIN_HOLLOW);
        for (size_t i = 1; i < n; ++i) sk->AddLine(pt_at(i));
        sk->EndFigure(D2D1_FIGURE_END_OPEN);
        sk->Close();

        brush->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b, dark_ink_light ? 0.28f : 0.20f));
        dc->DrawGeometry(line_geo.Get(), brush, 3.6f, round_stroke);
        brush->SetColor(accent);
        dc->DrawGeometry(line_geo.Get(), brush, 1.75f, round_stroke);
    }

    const D2D1_POINT_2F tip = pt_at(n - 1);
    brush->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b, 0.36f));
    dc->FillEllipse(D2D1::Ellipse(tip, 4.4f, 4.4f), brush);
    brush->SetColor(dark_ink_light ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f) : accent);
    dc->FillEllipse(D2D1::Ellipse(tip, 2.1f, 2.1f), brush);
}

void DrawTahoeAnalogDial(ID2D1DeviceContext* dc,
                         ID2D1SolidColorBrush* brush,
                         ID2D1StrokeStyle* round_stroke,
                         float cx,
                         float cy,
                         float radius,
                         const SYSTEMTIME& t,
                         bool dark_ink_light,
                         bool night_shade,
                         bool show_ticks,
                         bool show_numbers,
                         const WidgetTextDrawFn& draw_numeral) {
    if (!dc || !brush || radius <= 6.0f) return;

    // Multi-layered Swiss/Apple Watch crystal dial face: distinct obsidian night face vs luminous pearl day face
    const bool world_day_pearl = (!night_shade && !show_ticks);
    const bool face_light_ink = night_shade ? true : (world_day_pearl ? false : dark_ink_light);
    {
        D2D1_GRADIENT_STOP stops[2]{};
        if (night_shade) {
            stops[0] = {0.0f, D2D1::ColorF(0.11f, 0.14f, 0.22f, dark_ink_light ? 0.62f : 0.88f)};
            stops[1] = {1.0f, D2D1::ColorF(0.03f, 0.04f, 0.08f, dark_ink_light ? 0.72f : 0.94f)};
        } else if (world_day_pearl) {
            stops[0] = {0.0f, D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.94f)};
            stops[1] = {1.0f, D2D1::ColorF(0.88f, 0.91f, 0.96f, 0.88f)};
        } else if (dark_ink_light) {
            stops[0] = {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f)};
            stops[1] = {1.0f, D2D1::ColorF(0.75f, 0.82f, 0.96f, 0.08f)};
        } else {
            stops[0] = {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.84f)};
            stops[1] = {1.0f, D2D1::ColorF(0.90f, 0.93f, 0.97f, 0.68f)};
        }
        ComPtr<ID2D1GradientStopCollection> sc;
        ComPtr<ID2D1RadialGradientBrush> rgb;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &sc)) && sc &&
            SUCCEEDED(dc->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx - radius * 0.20f, cy - radius * 0.25f),
                                                    D2D1::Point2F(0, 0), radius * 1.15f, radius * 1.15f),
                sc.Get(), &rgb)) && rgb) {
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), rgb.Get());
        } else {
            brush->SetColor(stops[0].color);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush);
        }
    }

    // Precision double metallic bezel ring (outer rim + delicate inner chamfer)
    brush->SetColor(face_light_ink ? D2D1::ColorF(1.0f, 1.0f, 1.0f, night_shade ? 0.28f : 0.38f)
                                   : D2D1::ColorF(0.10f, 0.13f, 0.18f, 0.20f));
    dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius - 0.5f, radius - 0.5f), brush, 1.05f);
    if (radius >= 20.0f) {
        brush->SetColor(face_light_ink ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f)
                                       : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f));
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius - 1.8f, radius - 1.8f), brush, 0.7f);
    }

    if (show_ticks || radius >= 15.0f) {
        const int tick_count = (show_ticks && radius >= 28.0f) ? 60 : 12;
        for (int i = 0; i < tick_count; ++i) {
            const bool major = (tick_count == 60) ? (i % 5 == 0) : (i % 3 == 0);
            const float a = static_cast<float>(i) * (2.0f * kPi / static_cast<float>(tick_count));
            const float dx = std::sin(a);
            const float dy = -std::cos(a);
            const float r0 = radius * (major ? (show_ticks ? 0.84f : 0.80f) : (show_ticks ? 0.91f : 0.87f));
            const float r1 = radius * 0.95f;
            if (face_light_ink) {
                brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, major ? 0.84f : 0.34f));
            } else {
                brush->SetColor(D2D1::ColorF(0.11f, 0.13f, 0.18f, major ? 0.78f : 0.30f));
            }
            dc->DrawLine(D2D1::Point2F(cx + dx * r0, cy + dy * r0),
                         D2D1::Point2F(cx + dx * r1, cy + dy * r1),
                         brush, major ? std::max(1.25f, radius * 0.028f) : 0.85f, round_stroke);
        }
    }

    if (show_numbers && draw_numeral && radius >= 24.0f) {
        const float num_r = radius * (show_ticks ? 0.70f : 0.73f);
        const float box_r = std::max(8.5f, radius * 0.22f);
        const uint32_t num_argb = face_light_ink ? 0xF5FFFFFFu : 0xF21B2028u;
        const bool cardinal_only = (radius < 42.0f);
        for (int i = 1; i <= 12; ++i) {
            if (cardinal_only && (i % 3 != 0)) continue;
            const float a = static_cast<float>(i) * (kPi / 6.0f);
            const float nx = cx + std::sin(a) * num_r;
            const float ny = cy - std::cos(a) * num_r;
            draw_numeral(std::to_wstring(i),
                         D2D1::RectF(nx - box_r, ny - box_r * 0.75f, nx + box_r, ny + box_r * 0.75f),
                         num_argb, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    const float sec = static_cast<float>(t.wSecond) + static_cast<float>(t.wMilliseconds) / 1000.0f;
    const float min = static_cast<float>(t.wMinute) + sec / 60.0f;
    const float hr = static_cast<float>(t.wHour % 12) + min / 60.0f;

    const D2D1_COLOR_F hand_col = face_light_ink
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f)
        : D2D1::ColorF(0.11f, 0.13f, 0.18f, 0.95f);
    const D2D1_COLOR_F orange = D2D1::ColorF(1.0f, 0.584f, 0.0f, 1.0f);

    auto draw_hand = [&](float frac, float from, float to, float width, D2D1_COLOR_F col) {
        const float a = frac * 2.0f * kPi;
        const float dx = std::sin(a);
        const float dy = -std::cos(a);
        brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, face_light_ink ? 0.24f : 0.14f));
        dc->DrawLine(D2D1::Point2F(cx + dx * from, cy + dy * from + 1.0f),
                     D2D1::Point2F(cx + dx * to, cy + dy * to + 1.0f),
                     brush, width, round_stroke);
        brush->SetColor(col);
        dc->DrawLine(D2D1::Point2F(cx + dx * from, cy + dy * from),
                     D2D1::Point2F(cx + dx * to, cy + dy * to),
                     brush, width, round_stroke);
    };

    // Two-stage tapered Apple Tahoe hour & minute hands
    draw_hand(hr / 12.0f, 0.0f, radius * 0.14f, std::max(1.2f, radius * 0.032f), hand_col);
    draw_hand(hr / 12.0f, radius * 0.14f, radius * 0.50f, std::max(2.2f, radius * 0.072f), hand_col);
    draw_hand(min / 60.0f, 0.0f, radius * 0.14f, std::max(1.2f, radius * 0.032f), hand_col);
    draw_hand(min / 60.0f, radius * 0.14f, radius * 0.78f, std::max(1.9f, radius * 0.062f), hand_col);

    // Center cap layer 1
    brush->SetColor(hand_col);
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), std::max(2.2f, radius * 0.062f), std::max(2.2f, radius * 0.062f)), brush);

    // Vivid Apple Orange sweep second hand with counterweight tail
    draw_hand(sec / 60.0f, -radius * 0.16f, radius * 0.86f, std::max(1.0f, radius * 0.022f), orange);
    brush->SetColor(orange);
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), std::max(1.6f, radius * 0.044f), std::max(1.6f, radius * 0.044f)), brush);
    brush->SetColor(D2D1::ColorF(0.12f, 0.12f, 0.15f, 0.92f));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), std::max(0.8f, radius * 0.018f), std::max(0.8f, radius * 0.018f)), brush);
}

void DrawTahoeSolarArc(ID2D1Factory* factory,
                       ID2D1DeviceContext* dc,
                       ID2D1SolidColorBrush* brush,
                       ID2D1StrokeStyle* round_stroke,
                       const D2D1_RECT_F& rect,
                       int sunrise_min,
                       int sunset_min,
                       int now_min,
                       bool dark_ink_light) {
    if (!factory || !dc || !brush) return;
    const float w = rect.right - rect.left;
    const float h = rect.bottom - rect.top;
    if (w < 20.0f || h < 14.0f) return;

    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float amp = std::max(4.0f, h * 0.36f);
    auto y_of = [&](float u) { return cy + amp * std::cos(2.0f * kPi * u); };

    const int safe_sr = std::clamp(sunrise_min, 60, 700);
    const int safe_ss = std::clamp(sunset_min, 740, 1380);
    const float sr_u = static_cast<float>(safe_sr) / 1440.0f;
    const float horizon_y = y_of(sr_u);
    (void)safe_ss;

    brush->SetColor(dark_ink_light ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f)
                                   : D2D1::ColorF(0.10f, 0.14f, 0.20f, 0.22f));
    dc->DrawLine(D2D1::Point2F(rect.left, horizon_y), D2D1::Point2F(rect.right, horizon_y), brush, 0.85f);

    ComPtr<ID2D1PathGeometry> curve;
    ComPtr<ID2D1GeometrySink> sk;
    if (SUCCEEDED(factory->CreatePathGeometry(&curve)) && curve && SUCCEEDED(curve->Open(&sk)) && sk) {
        sk->BeginFigure(D2D1::Point2F(rect.left, y_of(0.0f)), D2D1_FIGURE_BEGIN_HOLLOW);
        for (int i = 1; i <= 64; ++i) {
            const float u = static_cast<float>(i) / 64.0f;
            sk->AddLine(D2D1::Point2F(rect.left + w * u, y_of(u)));
        }
        sk->EndFigure(D2D1_FIGURE_END_OPEN);
        sk->Close();

        dc->PushAxisAlignedClip(D2D1::RectF(rect.left, horizon_y, rect.right, rect.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        brush->SetColor(dark_ink_light ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.26f)
                                       : D2D1::ColorF(0.10f, 0.14f, 0.20f, 0.22f));
        dc->DrawGeometry(curve.Get(), brush, 1.8f, round_stroke);
        dc->PopAxisAlignedClip();

        dc->PushAxisAlignedClip(D2D1::RectF(rect.left, rect.top, rect.right, horizon_y), D2D1_ANTIALIAS_MODE_ALIASED);
        // Warm golden sunlit horizon fill beneath the daytime solar arc
        ComPtr<ID2D1PathGeometry> day_fill;
        ComPtr<ID2D1GeometrySink> fsk;
        if (SUCCEEDED(factory->CreatePathGeometry(&day_fill)) && day_fill && SUCCEEDED(day_fill->Open(&fsk)) && fsk) {
            fsk->BeginFigure(D2D1::Point2F(rect.left, horizon_y), D2D1_FIGURE_BEGIN_FILLED);
            for (int i = 0; i <= 64; ++i) {
                const float u = static_cast<float>(i) / 64.0f;
                fsk->AddLine(D2D1::Point2F(rect.left + w * u, y_of(u)));
            }
            fsk->AddLine(D2D1::Point2F(rect.right, horizon_y));
            fsk->EndFigure(D2D1_FIGURE_END_CLOSED);
            fsk->Close();
            const D2D1_GRADIENT_STOP fstops[2] = {
                {0.0f, D2D1::ColorF(1.0f, 0.82f, 0.28f, dark_ink_light ? 0.30f : 0.24f)},
                {1.0f, D2D1::ColorF(1.0f, 0.74f, 0.20f, 0.02f)},
            };
            ComPtr<ID2D1GradientStopCollection> fsc;
            ComPtr<ID2D1LinearGradientBrush> flg;
            if (SUCCEEDED(dc->CreateGradientStopCollection(fstops, 2, &fsc)) && fsc &&
                SUCCEEDED(dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, rect.top), D2D1::Point2F(0.0f, horizon_y)),
                    fsc.Get(), &flg)) && flg) {
                dc->FillGeometry(day_fill.Get(), flg.Get());
            }
        }
        brush->SetColor(dark_ink_light ? D2D1::ColorF(1.0f, 0.88f, 0.48f, 0.96f)
                                       : D2D1::ColorF(0.92f, 0.58f, 0.06f, 0.96f));
        dc->DrawGeometry(curve.Get(), brush, 2.2f, round_stroke);
        dc->PopAxisAlignedClip();
    }

    const float u_now = static_cast<float>(std::clamp(now_min, 0, 1439)) / 1440.0f;
    const float sx = rect.left + w * u_now;
    const float sy = y_of(u_now);
    brush->SetColor(D2D1::ColorF(1.0f, 0.84f, 0.24f, 0.36f));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), 6.2f, 6.2f), brush);
    brush->SetColor(dark_ink_light ? D2D1::ColorF(1.0f, 0.96f, 0.82f, 1.0f)
                                   : D2D1::ColorF(0.96f, 0.62f, 0.05f, 1.0f));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), 3.1f, 3.1f), brush);
}

std::vector<WorldClockCityEntry> QueryWorldClocks(std::span<const std::wstring> labels) {
    std::vector<WorldCitySpec> cities;
    if (labels.empty()) {
        for (const auto l : model::kDefaultWorldCities)
            if (const auto* c = model::FindWorldCity(l)) cities.push_back({c->label.data(), c->tz_key.data()});
    } else {
        for (const auto& l : labels)
            if (const auto* c = model::FindWorldCity(l)) cities.push_back({c->label.data(), c->tz_key.data()});
    }
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    GetSystemTime(&utc);
    GetLocalTime(&local);
    const int64_t local_mins = SystemTimeToTotalMinutes(local);
    const int key_local = local.wYear * 10000 + local.wMonth * 100 + local.wDay;

    std::vector<WorldClockCityEntry> out;
    out.reserve(4);
    for (const auto& c : cities) {
        SYSTEMTIME t{};
        if (!LookupTimeInZone(c.tz_key, utc, t)) continue;
        t.wMilliseconds = local.wMilliseconds;
        WorldClockCityEntry e;
        e.label = c.label;
        e.local_time = t;
        e.is_night = (t.wHour < 6 || t.wHour >= 18);
        const int key_target = t.wYear * 10000 + t.wMonth * 100 + t.wDay;
        e.day_label = (key_target > key_local) ? L"明天" : (key_target < key_local) ? L"昨天" : L"今天";
        const int64_t diff_min = SystemTimeToTotalMinutes(t) - local_mins;
        const int64_t abs_min = diff_min < 0 ? -diff_min : diff_min;
        wchar_t off_buf[32]{};
        if (abs_min == 0) {
            std::swprintf(off_buf, ARRAYSIZE(off_buf), L"+0 小时");
        } else if (abs_min % 60 != 0) {
            std::swprintf(off_buf, ARRAYSIZE(off_buf), L"%ls%lld:%02lld",
                          diff_min < 0 ? L"−" : L"+", abs_min / 60, abs_min % 60);
        } else {
            std::swprintf(off_buf, ARRAYSIZE(off_buf), L"%ls%lld 小时",
                          diff_min < 0 ? L"−" : L"+", abs_min / 60);
        }
        e.offset_label = off_buf;
        out.push_back(std::move(e));
    }
    return out;
}

std::wstring FormatSpeedPerSec(uint64_t bytes_per_sec) {
    const double bps = static_cast<double>(bytes_per_sec);
    wchar_t buf[32]{};
    if (bps >= 1024.0 * 1024.0 * 1024.0) {
        std::swprintf(buf, ARRAYSIZE(buf), L"%.2f GB/s", bps / (1024.0 * 1024.0 * 1024.0));
    } else if (bps >= 1024.0 * 1024.0) {
        std::swprintf(buf, ARRAYSIZE(buf), L"%.1f MB/s", bps / (1024.0 * 1024.0));
    } else if (bps >= 1024.0) {
        std::swprintf(buf, ARRAYSIZE(buf), L"%.0f KB/s", bps / 1024.0);
    } else {
        std::swprintf(buf, ARRAYSIZE(buf), L"%.0f B/s", std::max(0.0, bps));
    }
    return buf;
}

std::wstring FormatSpeedCompact(uint64_t bytes_per_sec) {
    const double bps = static_cast<double>(bytes_per_sec);
    wchar_t buf[24]{};
    if (bps >= 1024.0 * 1024.0 * 1024.0) {
        std::swprintf(buf, ARRAYSIZE(buf), L"%.1fG/s", bps / (1024.0 * 1024.0 * 1024.0));
    } else if (bps >= 1024.0 * 1024.0) {
        const double mb = bps / (1024.0 * 1024.0);
        std::swprintf(buf, ARRAYSIZE(buf), mb >= 100.0 ? L"%.0fM/s" : L"%.1fM/s", mb);
    } else if (bps >= 1024.0) {
        std::swprintf(buf, ARRAYSIZE(buf), L"%.0fK/s", bps / 1024.0);
    } else {
        std::swprintf(buf, ARRAYSIZE(buf), L"%.0fB/s", std::max(0.0, bps));
    }
    return buf;
}

void ComputeAstronomicalSunMinutes(double lat_deg,
                                   double lon_deg,
                                   int utc_offset_s,
                                   int& sunrise_min,
                                   int& sunset_min) noexcept {
    constexpr double kDpi = 3.14159265358979323846;
    const std::time_t now_sec = std::time(nullptr);
    std::tm gmt{};
    gmtime_s(&gmt, &now_sec);
    const int doy = gmt.tm_yday + 1;
    const double lat_rad = std::clamp(lat_deg, -65.0, 65.0) * kDpi / 180.0;
    const double decl = 23.44 * kDpi / 180.0 * std::sin(2.0 * kDpi * (284 + doy) / 365.0);
    const double cos_h = std::clamp(
        (std::sin(-0.833 * kDpi / 180.0) - std::sin(lat_rad) * std::sin(decl)) /
            std::max(1e-6, std::cos(lat_rad) * std::cos(decl)),
        -1.0, 1.0);
    const double half_day_h = std::acos(cos_h) * 180.0 / kDpi / 15.0;
    const double solar_noon_h = 12.0 + (static_cast<double>(utc_offset_s) / 3600.0) - (lon_deg / 15.0);
    sunrise_min = std::clamp(static_cast<int>(std::lround((solar_noon_h - half_day_h) * 60.0)), 60, 700);
    sunset_min = std::clamp(static_cast<int>(std::lround((solar_noon_h + half_day_h) * 60.0)), 740, 1380);
}

} // namespace pulse::desktop::render
