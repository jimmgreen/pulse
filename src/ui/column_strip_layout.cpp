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
                                    float scale, float scroll_left_dip) noexcept {
    ColumnStripLayout out;
    out.body = pane_bounds;
    const float unit = std::max(0.5f, scale);
    const float width = std::max(0.0f, pane_bounds.right - pane_bounds.left);
    const float min_w = kColumnStripMinDip * unit;
    if (width < min_w) return out;

    // The body scrolls like its neighbors: clipping it in a narrow viewport
    // must not silently change the stored width.
    const float body_w = ColumnStripWidthDip(widths_dip, kColumnStripBodySlot) * unit;
    const float child_w = child_slot ? ColumnStripWidthDip(widths_dip, 0) * unit : 0.0f;
    float ancestor_w = 0.0f;
    for (size_t k = 1; k <= ancestor_count; ++k)
        ancestor_w += ColumnStripWidthDip(widths_dip, k) * unit;
    const float actual_w = ancestor_w + body_w + child_w;
    const float natural_max = std::max(0.0f, actual_w - width);

    // One continuous content axis. Preserve a requested left scroll when the
    // branch gets shorter instead of pulling the clicked folder to the right.
    // A lone root has no earlier columns to reveal. Always anchor it at the
    // pane's leading edge, even when its fixed width exceeds the viewport.
    const float requested = (ancestor_count == 0 && !child_slot) ? 0.0f
        : scroll_left_dip < 0.0f
            ? natural_max : std::max(0.0f, scroll_left_dip) * unit;
    out.active = true;
    out.scroll_px = requested;
    out.max_scroll_px = std::max(natural_max, requested);
    out.content_px = std::max(actual_w, width + out.max_scroll_px);
    const float bottom = std::max(top, pane_bounds.bottom);
    out.viewport = D2D1::RectF(pane_bounds.left, top, pane_bounds.right, bottom);
    float x = pane_bounds.left - out.scroll_px;
    out.columns.reserve(ancestor_count + (child_slot ? 1 : 0));
    for (size_t i = 0; i < ancestor_count; ++i) {
        const float w = ColumnStripWidthDip(widths_dip, ancestor_count - i) * unit;
        if (x + w > out.viewport.left && x < out.viewport.right)
            out.columns.push_back({static_cast<int>(i), D2D1::RectF(x, top, x + w, bottom)});
        x += w;
    }
    out.body = D2D1::RectF(x, pane_bounds.top, x + body_w, pane_bounds.bottom);
    x += body_w;
    if (child_slot && x + child_w > out.viewport.left && x < out.viewport.right)
        out.columns.push_back({kColumnStripChildId, D2D1::RectF(x, top, x + child_w, bottom)});
    return out;
}

} // namespace pulse::ui
