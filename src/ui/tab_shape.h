// tab_shape.h — Chromium-style tab silhouette (convex top, inverse bottom).
#pragma once
#include "ui_compositor.h"

namespace pulse::ui {

struct ChromeTabShape {
    float top_radius = 8.0f;
    float bottom_radius = 8.0f;
    bool connect_bottom = false;
};

// `bounds` is the content box (icon + title). Inverse shoulders extend
// `bottom_radius` past left/right along the bottom edge.
void FillChromeTab(ID2D1DeviceContext* dc, ID2D1Brush* brush,
                   const D2D1_RECT_F& bounds, const ChromeTabShape& shape);

// Straight accent strip whose ends are clipped by the tab's rounded path.
void FillChromeTabAccent(ID2D1DeviceContext* dc, ID2D1Brush* brush,
                         const D2D1_RECT_F& bounds, const ChromeTabShape& shape,
                         float thickness);

enum class AccentEdge { Top, Left };

// Straight strip clipped to a rounded rectangle (file selection, list rows).
void FillRoundedAccent(ID2D1DeviceContext* dc, ID2D1Brush* brush,
                       const D2D1_RECT_F& bounds, float radius,
                       float thickness, AccentEdge edge);

} // namespace pulse::ui
