#include "column_strip_layout.h"

#include <algorithm>

namespace pulse::ui {

float ColumnStripWidthDip(const std::vector<float>& widths_dip, size_t slot) noexcept {
    const float fallback = slot == 0 ? kColumnStripChildDefaultDip : kColumnStripDefaultDip;
    if (slot >= widths_dip.size() || widths_dip[slot] <= 0.0f) return fallback;
    return std::clamp(widths_dip[slot], kColumnStripMinDip, kColumnStripMaxDip);
}

size_t ColumnStripWidthSlot(int column_id, size_t ancestor_count) noexcept {
    if (column_id == kColumnStripBodyId) return kColumnStripBodySlot;
    if (column_id == kColumnStripChildId || column_id < 0) return 0;
    const size_t index = static_cast<size_t>(column_id);
    return index < ancestor_count ? ancestor_count - index : 0;
}

ColumnStripLayout LayoutColumnStrip(const D2D1_RECT_F& pane_bounds, float top,
                                    size_t ancestor_count, bool child_slot,
                                    const std::vector<float>& widths_dip,
                                    float scale, float scroll_from_right_dip,
                                    std::optional<float> ancestor_viewport_limit_dip) noexcept {
    ColumnStripLayout out;
    out.body = pane_bounds;
    const float unit = std::max(0.5f, scale);
    const float width = std::max(0.0f, pane_bounds.right - pane_bounds.left);
    const float min_w = kColumnStripMinDip * unit;
    if (width < min_w) return out;

    const float body_w = std::min(width, ColumnStripWidthDip(widths_dip, kColumnStripBodySlot) * unit);
    float content = 0.0f;
    for (size_t k = 1; k <= ancestor_count; ++k) content += ColumnStripWidthDip(widths_dip, k) * unit;

    // The selected folder's contents come before the ancestors; the column
    // narrows to fit. A selected file adds nothing: its properties live in
    // the details panel.
    float trail_w = 0.0f;
    if (child_slot) {
        const float w = ColumnStripWidthDip(widths_dip, 0) * unit;
        if (body_w + w <= width) trail_w = w;
        else if (width - body_w >= min_w) trail_w = width - body_w;
    }
    float viewport_w = std::max(0.0f, std::min(content, width - body_w - trail_w));
    if (ancestor_viewport_limit_dip) {
        // Keep a usable strip for ancestors that were clipped at the left edge.
        const float limit = std::max(kColumnStripMinAnchorViewportDip, *ancestor_viewport_limit_dip);
        viewport_w = std::min(viewport_w, limit * unit);
    }

    out.active = true;
    const float bottom = std::max(top, pane_bounds.bottom);
    out.content_px = content;
    out.max_scroll_px = std::max(0.0f, content - viewport_w);
    out.scroll_px = std::clamp(out.max_scroll_px - std::max(0.0f, scroll_from_right_dip) * unit,
                               0.0f, out.max_scroll_px);
    out.body.left = pane_bounds.left + viewport_w;
    out.body.right = out.body.left + body_w;
    if (viewport_w > 0.0f) {
        out.viewport = D2D1::RectF(pane_bounds.left, top, out.body.left, bottom);
        float x = pane_bounds.left - out.scroll_px;
        out.columns.reserve(ancestor_count + 1);
        for (size_t i = 0; i < ancestor_count; ++i) {
            const float w = ColumnStripWidthDip(widths_dip, ancestor_count - i) * unit;
            if (x + w > out.viewport.left && x < out.viewport.right) {
                out.columns.push_back({static_cast<int>(i), D2D1::RectF(x, top, x + w, bottom)});
            }
            x += w;
        }
    }
    if (trail_w > 0.0f) {
        out.columns.push_back({kColumnStripChildId,
                               D2D1::RectF(out.body.right, top, out.body.right + trail_w, bottom)});
    }
    return out;
}

} // namespace pulse::ui
