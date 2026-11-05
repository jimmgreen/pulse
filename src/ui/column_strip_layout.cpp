#include "column_strip_layout.h"

#include <algorithm>
#include <cmath>

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

ColumnStripRowLayout LayoutColumnStripRow(const D2D1_RECT_F& column,
                                          float row_top, float row_bottom,
                                          float scale, bool has_chevron) noexcept {
    ColumnStripRowLayout out;
    const float unit = std::max(0.5f, scale);
    const float pad = 6.0f * unit;
    out.cell = D2D1::RectF(column.left + pad, row_top + unit,
                           std::max(column.left + pad, column.right - pad),
                           std::max(row_top + unit, row_bottom - unit));

    const float icon_side = 16.0f * unit;
    const float icon_left = out.cell.left + 8.0f * unit;
    const float icon_top = static_cast<float>(std::round(
        row_top + ((row_bottom - row_top) - icon_side) * 0.5f));
    out.icon = D2D1::RectF(icon_left, icon_top, icon_left + icon_side, icon_top + icon_side);

    const float chevron_width = has_chevron ? 18.0f * unit : 0.0f;
    const float text_left = out.icon.right + 8.0f * unit;
    const float text_right = std::max(text_left, out.cell.right - chevron_width - (has_chevron ? 6.0f : 4.0f) * unit);
    out.text = D2D1::RectF(text_left, row_top, text_right, row_bottom);
    if (has_chevron) {
        const float chevron_left = out.cell.right - chevron_width - 2.0f * unit;
        out.chevron = D2D1::RectF(chevron_left, row_top, chevron_left + chevron_width, row_bottom);
    }
    return out;
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
    // A lone root starts at the leading edge when no offset has been chosen,
    // but a narrow pane must still honor a user's explicit horizontal scroll.
    const bool single_root = ancestor_count == 0 && !child_slot;
    const bool root_initial = single_root && scroll_left_dip < 0.0f;
    const float requested = root_initial ? 0.0f
        : scroll_left_dip < 0.0f ? natural_max
        : std::max(0.0f, scroll_left_dip) * unit;
    out.active = true;
    out.max_scroll_px = single_root ? natural_max : std::max(natural_max, requested);
    out.scroll_px = std::clamp(requested, 0.0f, out.max_scroll_px);
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
