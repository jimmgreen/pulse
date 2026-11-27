#pragma once
#include <d2d1.h>
#include <algorithm>
namespace pulse::ui {
struct PaneEmptyLayout {
    D2D1_RECT_F art{};
    D2D1_RECT_F title{};
    D2D1_RECT_F message{};
    D2D1_RECT_F action{};
    bool show_message = false;
    bool show_action = false;
};

inline PaneEmptyLayout MakePaneEmptyLayout(const D2D1_RECT_F& bounds, float scale,
                                    bool can_create, float art_aspect = 512.0f / 360.0f) {
    PaneEmptyLayout out;
    const float width = std::max(0.0f, bounds.right - bounds.left);
    const float height = std::max(0.0f, bounds.bottom - bounds.top);
    out.show_message = height >= 220.0f * scale;
    out.show_action = can_create && height >= 280.0f * scale && width >= 180.0f * scale;

    const float titleH = std::min(24.0f * scale, height);
    const float messageH = out.show_message ? 20.0f * scale : 0.0f;
    const float actionH = out.show_action ? 34.0f * scale : 0.0f;
    const float textGap = out.show_message ? 2.0f * scale : 0.0f;
    const float actionGap = out.show_action ? 14.0f * scale : 0.0f;
    const float fixedH = titleH + textGap + messageH + actionGap + actionH;
    const float aspect = art_aspect > 0.05f ? art_aspect : (512.0f / 360.0f);
    const float maxArtW = std::max(0.0f, std::min(200.0f * scale, width - 32.0f * scale));
    // A short picker pane must keep its title visible before reserving artwork.
    const float maxArtH = std::min(std::max(60.0f * scale, height - fixedH - 44.0f * scale),
        std::max(0.0f, height - fixedH - 18.0f * scale));
    const float artW = std::min(maxArtW, maxArtH * aspect);
    const float artH = artW / aspect;
    // The SVG viewBox already has bottom breathing room; keep only a small
    // layout gap so the illustration and copy read as one centered group.
    const float artGap = artH > 0.0f ? 2.0f * scale : 0.0f;
    const float totalH = artH + artGap + fixedH;
    float y = bounds.top + std::max(0.0f, (height - totalH) * 0.5f);
    const float cx = (bounds.left + bounds.right) * 0.5f;
    out.art = D2D1::RectF(cx - artW * 0.5f, y, cx + artW * 0.5f, y + artH);
    y = out.art.bottom + artGap;
    const float text_pad = std::min(12.0f * scale, width * 0.5f);
    out.title = D2D1::RectF(bounds.left + text_pad, y,
                            bounds.right - text_pad, y + titleH);
    y = out.title.bottom + textGap;
    out.message = D2D1::RectF(bounds.left + text_pad, y,
                              bounds.right - text_pad, y + messageH);
    y = out.message.bottom + actionGap;
    const float actionW = std::max(0.0f, std::min(142.0f * scale, width - 32.0f * scale));
    out.action = D2D1::RectF(cx - actionW * 0.5f, y,
                             cx + actionW * 0.5f, y + actionH);
    return out;
}

// Empty search results: optional illustration, a title and up to three
// stacked suggestion buttons. Shared by drawing and hit testing.
struct SearchEmptyLayout {
    D2D1_RECT_F art{};
    D2D1_RECT_F title{};
    D2D1_RECT_F buttons[3]{};
    bool show_art = false;
};

// One-time teaching bubble: bottom-right card above the status bar. Fixed
// geometry so hit testing needs no text measurement.
struct TeachBubbleLayout {
    D2D1_RECT_F card{}, close{}, icon{}, title{}, body{}, never{}, primary{};
};

inline TeachBubbleLayout MakeTeachBubbleLayout(const D2D1_RECT_F& rect, float status_h, float scale) {
    TeachBubbleLayout t;
    const float w = std::max(0.0f, std::min(340.0f * scale, rect.right - rect.left - 32.0f * scale));
    const float h = 150.0f * scale;
    const float right = rect.right - 16.0f * scale;
    const float bottom = rect.bottom - status_h - 12.0f * scale;
    t.card = D2D1::RectF(right - w, bottom - h, right, bottom);
    t.close = D2D1::RectF(t.card.right - 34.0f * scale, t.card.top + 8.0f * scale,
                          t.card.right - 8.0f * scale, t.card.top + 34.0f * scale);
    t.icon = D2D1::RectF(t.card.left + 12.0f * scale, t.card.top + 12.0f * scale,
                         t.card.left + 36.0f * scale, t.card.top + 36.0f * scale);
    t.title = D2D1::RectF(t.icon.right + 4.0f * scale, t.card.top + 12.0f * scale,
                          t.close.left - 4.0f * scale, t.card.top + 36.0f * scale);
    t.body = D2D1::RectF(t.card.left + 16.0f * scale, t.title.bottom + 4.0f * scale,
                         t.card.right - 16.0f * scale, t.title.bottom + 60.0f * scale);
    t.primary = D2D1::RectF(t.card.right - 16.0f * scale - 116.0f * scale, t.card.bottom - 44.0f * scale,
                            t.card.right - 16.0f * scale, t.card.bottom - 14.0f * scale);
    t.never = D2D1::RectF(t.card.left + 8.0f * scale, t.primary.top,
                          std::max(t.card.left + 8.0f * scale, t.primary.left - 8.0f * scale), t.primary.bottom);
    return t;
}

inline SearchEmptyLayout MakeSearchEmptyLayout(const D2D1_RECT_F& bounds, float scale, int count) {
    SearchEmptyLayout out;
    count = std::clamp(count, 0, 3);
    const float width = std::max(0.0f, bounds.right - bounds.left);
    const float height = std::max(0.0f, bounds.bottom - bounds.top);
    const float titleH = 24.0f * scale;
    const float buttonH = 32.0f * scale;
    const float buttonGap = 6.0f * scale;
    const float buttonsH = count > 0
        ? 10.0f * scale + count * buttonH + (count - 1) * buttonGap : 0.0f;
    const float fixedH = titleH + buttonsH;
    const float aspect = 320.0f / 240.0f;
    float artH = std::min(120.0f * scale, height - fixedH - 44.0f * scale);
    artH = std::min(artH, (width - 32.0f * scale) / aspect);
    out.show_art = artH >= 48.0f * scale;
    if (!out.show_art) artH = 0.0f;
    const float artW = artH * aspect;
    const float artGap = out.show_art ? 4.0f * scale : 0.0f;
    const float totalH = artH + artGap + fixedH;
    float y = bounds.top + std::max(8.0f * scale, (height - totalH) * 0.5f);
    const float cx = (bounds.left + bounds.right) * 0.5f;
    out.art = D2D1::RectF(cx - artW * 0.5f, y, cx + artW * 0.5f, y + artH);
    y += artH + artGap;
    out.title = D2D1::RectF(bounds.left + 12.0f * scale, y, bounds.right - 12.0f * scale, y + titleH);
    y = out.title.bottom + 10.0f * scale;
    const float buttonW = std::max(0.0f, std::min(220.0f * scale, width - 32.0f * scale));
    for (int i = 0; i < count; ++i) {
        out.buttons[i] = D2D1::RectF(cx - buttonW * 0.5f, y, cx + buttonW * 0.5f, y + buttonH);
        y += buttonH + buttonGap;
    }
    return out;
}

}
