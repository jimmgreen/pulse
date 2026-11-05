// column_strip_layout.h — Pure horizontal geometry of the column (Miller) view.
#pragma once

#include <d2d1.h>
#include <cstddef>
#include <vector>

namespace pulse::ui {

// Finder-style columns: every column, the current folder's list included,
// has the same default width, so going one level deeper only adds a column
// on the right and the ones already shown stay where they are.
inline constexpr float kColumnStripDefaultDip = 240.0f;
inline constexpr float kColumnStripChildDefaultDip = 240.0f;
inline constexpr float kColumnStripMinDip = 120.0f;
inline constexpr float kColumnStripMaxDip = 560.0f;
// Column id of the trailing "contents of the selected folder" column.
// Ancestor columns use their index into the root-to-parent chain.
inline constexpr int kColumnStripChildId = 10000;
// Divider id of the current folder's list (its right edge).
inline constexpr int kColumnStripBodyId = 10001;
// Width slot of the current folder's list (past every ancestor slot).
inline constexpr size_t kColumnStripBodySlot = 16;

// Width slot 0 is the child column, slot k (1 <= k < kColumnStripBodySlot)
// the k-th ancestor counted from the current folder (1 = parent) and
// kColumnStripBodySlot the current folder. Missing/zero entries use defaults.
float ColumnStripWidthDip(const std::vector<float>& widths_dip, size_t slot) noexcept;
size_t ColumnStripWidthSlot(int column_id, size_t ancestor_count) noexcept;

struct ColumnStripSlot {
    int id = -1;
    D2D1_RECT_F rect{};
};

// One source of truth for the row geometry shared by ancestor columns and
// the current-folder list: icon, filename, and optional folder chevron.
struct ColumnStripRowLayout {
    D2D1_RECT_F cell{};
    D2D1_RECT_F icon{};
    D2D1_RECT_F text{};
    D2D1_RECT_F chevron{};
};

ColumnStripRowLayout LayoutColumnStripRow(const D2D1_RECT_F& column,
                                          float row_top, float row_bottom,
                                          float scale, bool has_chevron) noexcept;

struct ColumnStripLayout {
    bool active = false;
    // Replaces the pane bounds for everything the regular list draws:
    // same top/bottom as the pane, narrowed horizontally.
    D2D1_RECT_F body{};
    // Full-width viewport shared by ancestor, regular body and child columns.
    D2D1_RECT_F viewport{};
    float scroll_px = 0.0f;      // from left of the complete directory chain
    float max_scroll_px = 0.0f;
    float content_px = 0.0f;     // may include trailing slack on a shortened branch
    // Visible ancestor/child slots; the regular list has its own body rectangle.
    std::vector<ColumnStripSlot> columns;
};

// Hit tests follow the currently painted smooth-scroll position. A path change
// or an uninitialized motion channel uses the target until the first frame.
inline float ColumnStripVisualScroll(float target, float shown, bool initialized,
                                     bool same_path) noexcept {
    return initialized && same_path ? shown : target;
}

// pane_bounds is the full pane rect; column rects span [top, pane bottom).
// All columns share a horizontal coordinate system. Negative scroll_left_dip
// shows the newest columns initially; a nonnegative offset holds their positions
// on a branch change even if there is empty space at the far right.
ColumnStripLayout LayoutColumnStrip(const D2D1_RECT_F& pane_bounds, float top,
                                    size_t ancestor_count, bool child_slot,
                                    const std::vector<float>& widths_dip,
                                    float scale, float scroll_left_dip = -1.0f) noexcept;

} // namespace pulse::ui
