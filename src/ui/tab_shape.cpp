// tab_shape.cpp — Chromium HorizontalTabStyleViews::GetPath, in D2D.
// Active tabs: convex top corners + concave bottom shoulders so the tab
// grows out of the toolbar. Inactive tabs stay a detached rounded rect.
#include "tab_shape.h"

#include <algorithm>

namespace pulse::ui {
namespace {

void AddQuarterArc(ID2D1GeometrySink* sink, D2D1_POINT_2F end, float radius,
                   D2D1_SWEEP_DIRECTION sweep) {
    if (radius < 0.5f) {
        sink->AddLine(end);
        return;
    }
    sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(radius, radius), 0.0f, sweep,
                                  D2D1_ARC_SIZE_SMALL));
}

HRESULT CreateChromeTabGeometry(ID2D1Factory* factory, const D2D1_RECT_F& bounds,
                                const ChromeTabShape& shape, ID2D1Geometry** out) {
    if (!factory || !out) return E_POINTER;
    *out = nullptr;

    const float w = bounds.right - bounds.left;
    const float h = bounds.bottom - bounds.top;
    if (w < 2.0f || h < 2.0f) return E_INVALIDARG;

    const float top_r = (std::min)({ shape.top_radius, w * 0.5f, h * 0.5f });
    if (!shape.connect_bottom) {
        ComPtr<ID2D1RoundedRectangleGeometry> rounded;
        const HRESULT hr = factory->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(bounds, top_r, top_r), &rounded);
        if (FAILED(hr)) return hr;
        *out = rounded.get();
        if (*out) (*out)->AddRef();
        return S_OK;
    }

    const float bot_r = (std::min)({ shape.bottom_radius, w * 0.5f, h * 0.5f });
    ComPtr<ID2D1PathGeometry> geometry;
    HRESULT hr = factory->CreatePathGeometry(&geometry);
    if (FAILED(hr)) return hr;
    ComPtr<ID2D1GeometrySink> sink;
    hr = geometry->Open(&sink);
    if (FAILED(hr)) return hr;

    const float left = bounds.left;
    const float right = bounds.right;
    const float top = bounds.top;
    const float bottom = bounds.bottom;
    sink->BeginFigure(D2D1::Point2F(left - bot_r, bottom), D2D1_FIGURE_BEGIN_FILLED);
    AddQuarterArc(sink.get(), D2D1::Point2F(left, bottom - bot_r), bot_r,
                  D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE);
    sink->AddLine(D2D1::Point2F(left, top + top_r));
    AddQuarterArc(sink.get(), D2D1::Point2F(left + top_r, top), top_r,
                  D2D1_SWEEP_DIRECTION_CLOCKWISE);
    sink->AddLine(D2D1::Point2F(right - top_r, top));
    AddQuarterArc(sink.get(), D2D1::Point2F(right, top + top_r), top_r,
                  D2D1_SWEEP_DIRECTION_CLOCKWISE);
    sink->AddLine(D2D1::Point2F(right, bottom - bot_r));
    AddQuarterArc(sink.get(), D2D1::Point2F(right + bot_r, bottom), bot_r,
                  D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    hr = sink->Close();
    if (FAILED(hr)) return hr;

    *out = geometry.get();
    if (*out) (*out)->AddRef();
    return S_OK;
}

} // namespace

void FillChromeTab(ID2D1DeviceContext* dc, ID2D1Brush* brush,
                   const D2D1_RECT_F& bounds, const ChromeTabShape& shape) {
    if (!dc || !brush) return;
    ID2D1Factory* factory = nullptr;
    dc->GetFactory(&factory);
    if (!factory) return;

    ComPtr<ID2D1Geometry> geometry;
    if (FAILED(CreateChromeTabGeometry(factory, bounds, shape, &geometry)) ||
        !geometry.get()) {
        return;
    }
    dc->FillGeometry(geometry.get(), brush);
}

void FillChromeTabAccent(ID2D1DeviceContext* dc, ID2D1Brush* brush,
                         const D2D1_RECT_F& bounds, const ChromeTabShape& shape,
                         float thickness) {
    if (!dc || !brush || thickness < 0.5f) return;
    ID2D1Factory* factory = nullptr;
    dc->GetFactory(&factory);
    if (!factory) return;

    ComPtr<ID2D1Geometry> tab;
    if (FAILED(CreateChromeTabGeometry(factory, bounds, shape, &tab)) || !tab.get())
        return;

    const D2D1_RECT_F strip = D2D1::RectF(bounds.left - shape.bottom_radius,
                                          bounds.top,
                                          bounds.right + shape.bottom_radius,
                                          bounds.top + thickness);
    ComPtr<ID2D1RectangleGeometry> strip_geo;
    if (FAILED(factory->CreateRectangleGeometry(strip, &strip_geo))) return;

    ComPtr<ID2D1PathGeometry> clipped;
    if (FAILED(factory->CreatePathGeometry(&clipped))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(clipped->Open(&sink))) return;
    if (FAILED(tab->CombineWithGeometry(strip_geo.get(), D2D1_COMBINE_MODE_INTERSECT,
                                        nullptr, sink.get()))) {
        return;
    }
    if (FAILED(sink->Close())) return;
    dc->FillGeometry(clipped.get(), brush);
}

void FillRoundedAccent(ID2D1DeviceContext* dc, ID2D1Brush* brush,
                       const D2D1_RECT_F& bounds, float radius,
                       float thickness, AccentEdge edge) {
    if (!dc || !brush || thickness < 0.5f) return;
    const float w = bounds.right - bounds.left;
    const float h = bounds.bottom - bounds.top;
    if (w < 2.0f || h < 2.0f) return;

    ID2D1Factory* factory = nullptr;
    dc->GetFactory(&factory);
    if (!factory) return;

    const float r = (std::min)({ radius, w * 0.5f, h * 0.5f });
    ComPtr<ID2D1RoundedRectangleGeometry> mask;
    if (FAILED(factory->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(bounds, r, r), &mask))) {
        return;
    }

    D2D1_RECT_F strip = bounds;
    if (edge == AccentEdge::Left) {
        strip.right = bounds.left + thickness;
    } else {
        strip.bottom = bounds.top + thickness;
    }
    ComPtr<ID2D1RectangleGeometry> strip_geo;
    if (FAILED(factory->CreateRectangleGeometry(strip, &strip_geo))) return;

    ComPtr<ID2D1PathGeometry> clipped;
    if (FAILED(factory->CreatePathGeometry(&clipped))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(clipped->Open(&sink))) return;
    if (FAILED(mask->CombineWithGeometry(strip_geo.get(), D2D1_COMBINE_MODE_INTERSECT,
                                         nullptr, sink.get()))) {
        return;
    }
    if (FAILED(sink->Close())) return;
    dc->FillGeometry(clipped.get(), brush);
}

} // namespace pulse::ui
