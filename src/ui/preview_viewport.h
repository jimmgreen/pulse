#pragma once
#include <algorithm>
#include <cmath>
#include <d2d1.h>

namespace pulse::ui {

// Screen-pixel geometry shared by drawing, wheel zoom and grab panning.
struct PreviewViewport {
    D2D1_RECT_F bounds{};
    float width = 0, height = 0;
    float zoom = 1, x = 0, y = 0;
    bool image = false, fit = true;

    float ViewWidth() const { return std::max(1.0f, bounds.right - bounds.left); }
    float ViewHeight() const { return std::max(1.0f, bounds.bottom - bounds.top); }
    float FitZoom() const {
        return std::min({1.0f, ViewWidth() / std::max(1.0f, width),
                         ViewHeight() / std::max(1.0f, height)});
    }
    float MaxX() const { return std::max(0.0f, width * zoom - ViewWidth()); }
    float MaxY() const { return std::max(0.0f, height * zoom - ViewHeight()); }
    void Clamp() { x = std::clamp(x, 0.0f, MaxX()); y = std::clamp(y, 0.0f, MaxY()); }
    void SetContent(D2D1_RECT_F rect, float w, float h, bool bitmap) {
        bounds = rect; width = w; height = h; image = bitmap;
        if (image && fit) zoom = FitZoom();
        if (!image) zoom = 1;
        Clamp();
    }
    D2D1_RECT_F ContentRect() const {
        const float left = bounds.left + (image ? std::max(0.0f, ViewWidth() - width * zoom) * .5f : 0) - x;
        const float top = bounds.top + (image ? std::max(0.0f, ViewHeight() - height * zoom) * .5f : 0) - y;
        return D2D1::RectF(left, top, left + width * zoom, top + height * zoom);
    }
    void Pan(float dx, float dy) { x -= dx; y -= dy; Clamp(); }
    void ZoomAt(float next, float px, float py) {
        if (!image || width <= 0 || height <= 0) return;
        const auto old = ContentRect();
        const float u = (px - old.left) / zoom, v = (py - old.top) / zoom;
        zoom = std::clamp(next, std::min(.01f, FitZoom()), 16.0f);
        fit = false;
        x = bounds.left + std::max(0.0f, ViewWidth() - width * zoom) * .5f + u * zoom - px;
        y = bounds.top + std::max(0.0f, ViewHeight() - height * zoom) * .5f + v * zoom - py;
        Clamp();
    }
    void ToggleFit(float px, float py) {
        if (!image) return;
        if (fit) ZoomAt(1.0f, px, py);
        else { fit = true; zoom = FitZoom(); x = y = 0; }
    }
};
} // namespace pulse::ui
