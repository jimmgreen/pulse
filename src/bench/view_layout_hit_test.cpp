#include "../ui/view_layout.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <vector>

int main() {
    using namespace pulse::ui;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        failures += !ok;
    };
    {
        // Rows are centred: both margins around an extra-large row are blank.
        const ViewLayout xl(ViewMode::ExtraLargeIcons, {0, 0, 900, 900}, 20, 0, 0, 1);
        const auto first = xl.ItemRect(0), last = xl.ItemRect(xl.Metrics().columns - 1);
        check(first.left > 0 && last.right < 900 && xl.HitTest(first.left - 1, 20) == -1 &&
              xl.HitTest(last.right + 1, 20) == -1, "extra-large row margins are blank");
    }
    {
        bool label_fits = true, size_slot = true, bottom_hit = true;
        for (float dpi : {1.0f, 1.5f, 2.0f}) for (float font : {1.0f, 1.12f, 1.25f})
        for (auto mode : {ViewMode::MediumIcons, ViewMode::LargeIcons, ViewMode::ExtraLargeIcons}) {
            const float line = kIconGridNameLineDip * font;
            const ViewLayout layout(mode, {0, 0, 900 * dpi, 700 * dpi}, 30, 0, 0, dpi,
                ViewLayoutParams(IconGridNameBlockDip(4), 0, line));
            const auto label = layout.NameRect(0);
            const auto cell = layout.ItemRect(0);
            const auto second_row = layout.ItemRect(layout.Metrics().columns);
            const auto size = layout.FolderSizeRect(0);
            label_fits &= layout.Metrics().name_lines == 4 && label.bottom <= cell.bottom &&
                cell.bottom < second_row.top &&
                std::abs((label.bottom - label.top) - (4 * line + 2) * dpi) < 0.01f;
            size_slot &= std::abs(size.top - (label.top + 3 * line * dpi)) < 0.01f &&
                size.bottom == label.bottom;
            bottom_hit &= layout.HitTest((label.left + label.right) * 0.5f, label.bottom - 1) == 0;
        }
        check(label_fits, "scaled four-line name block fits without clipping into the next row");
        check(size_slot, "folder size reserves a complete scaled name line");
        check(bottom_hit, "last visible name line belongs to the same item");
    }
    check(ViewLayout(ViewMode::List, {0, 0, 800, 250}, 30, 0, 0, 1).HitTest(30, 245) == -1,
          "column-major bottom remainder is blank");
    for (int mode = 0; mode < 8; ++mode) {
        bool valid = true;
        for (float scale : {1.0f, 1.5f, 2.0f}) for (float width : {241.0f, 541.0f, 900.0f})
        for (float height : {101.0f, 250.0f, 701.0f}) for (size_t count : {size_t{0}, size_t{1}, size_t{17}, size_t{120}})
        for (float scroll : {0.0f, 27.0f, 123.0f}) {
            const D2D1_RECT_F viewport{13, 19, 13 + width * scale, 19 + height * scale};
            ViewLayout layout(ViewModeFromIndex(mode), viewport, count, scroll, scroll, scale);
            auto contains = [](D2D1_RECT_F r, float x, float y) {
                return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
            };
            for (float y = viewport.top; y <= viewport.bottom + 1; y += 7.3f)
                for (float x = viewport.left; x <= viewport.right + 1; x += 11.7f) {
                    const int hit = layout.HitTest(x, y);
                    if (hit >= 0 && (!contains(viewport, x, y) || !contains(layout.ItemRect(hit), x, y))) valid = false;
                }
            // Also verify interiors remain reachable; rejecting every point is not a fix.
            for (size_t i = 0; i < count; ++i) {
                const auto r = layout.ItemRect(static_cast<int>(i));
                const float x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
                if (contains(viewport, x, y) && layout.HitTest(x, y) != static_cast<int>(i)) valid = false;
            }
        }
        std::cout << "[MODE] " << mode << '\n';
        check(valid, "real layout hit implies rendered rectangle containment and visible interiors remain reachable");
    }
    ListGroups groups{{0, 3}, {3, 2}}; groups[1].collapsed = true;
    ViewLayout grouped(ViewMode::Details, {0, 0, 700, 400}, 5, 0, 0, 1, 0, &groups);
    const auto header = grouped.HeaderRect(0);
    check(grouped.HitTest(30, (header.top + header.bottom) / 2) == -1, "group header keeps priority over item hits");
    const auto collapsed = grouped.ItemRect(3);
    check(collapsed.top == collapsed.bottom, "collapsed group has no hittable item area");

    // Group headers in icon grids (C5b): every group starts a new row under its
    // header; hit testing, ranges and keyboard moves follow that geometry.
    {
        bool valid = true, ordered = true, ranges = true, rows_start = true;
        const ViewMode grid_modes[] = {ViewMode::ExtraLargeIcons, ViewMode::LargeIcons, ViewMode::MediumIcons,
                                       ViewMode::SmallIcons, ViewMode::Tiles, ViewMode::Details, ViewMode::Content};
        for (ViewMode mode : grid_modes)
        for (float scale : {1.0f, 1.5f}) for (float width : {241.0f, 700.0f, 1300.0f})
        for (int collapse = 0; collapse < 3; ++collapse) for (float scroll : {0.0f, 97.0f, 400.0f}) {
            ListGroups gs{{0, 7}, {7, 1}, {8, 13}, {21, 4}};
            if (collapse == 1) gs[1].collapsed = true;
            if (collapse == 2) gs[2].collapsed = true;
            const size_t count = 25;
            const D2D1_RECT_F viewport{13, 19, 13 + width * scale, 19 + 520 * scale};
            ViewLayout layout(mode, viewport, count, 0, scroll, scale, 0, &gs);
            if (!layout.Grouped()) { valid = false; continue; }
            auto contains = [](D2D1_RECT_F r, float x, float y) {
                return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
            };
            for (float y = viewport.top; y <= viewport.bottom + 1; y += 6.1f)
                for (float x = viewport.left; x <= viewport.right + 1; x += 9.7f) {
                    const int hit = layout.HitTest(x, y);
                    if (hit >= 0 && (!contains(viewport, x, y) || !contains(layout.ItemRect(hit), x, y))) valid = false;
                    if (hit >= 0 && layout.HeaderHitTest(x, y) >= 0) valid = false;
                }
            const int cols = std::max(1, layout.Metrics().columns);
            for (int g = 0; g < static_cast<int>(gs.size()); ++g) {
                const auto head_rect = layout.HeaderRect(g);
                const auto next = g + 1 < static_cast<int>(gs.size()) ? layout.HeaderRect(g + 1).top : 1e9f;
                for (int i = gs[g].first; i < gs[g].first + gs[g].count; ++i) {
                    const auto r = layout.ItemRect(i);
                    if (gs[g].collapsed) { if (r.bottom != r.top) valid = false; continue; }
                    if (r.top < head_rect.bottom - 0.01f || r.bottom > next + 0.01f) ordered = false;
                    if (layout.GroupOfIndex(i) != g) ordered = false;
                    const float x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
                    if (contains(viewport, x, y) && layout.HitTest(x, y) != i) valid = false;
                    // The group's first item opens a row at the left edge.
                    if (i == gs[g].first && cols > 1 && std::abs(r.left - layout.ItemRect(0).left) > 0.01f) rows_start = false;
                }
            }
            const auto range = layout.VisibleRange();
            for (int i = 0; i < static_cast<int>(count); ++i) {
                const auto r = layout.ItemRect(i);
                const bool visible = r.bottom > r.top && r.bottom > viewport.top && r.top < viewport.bottom;
                if (visible && (i < range.first || i > range.second)) ranges = false;
                const auto span = layout.RangeForSpan(viewport.top, viewport.bottom);
                if (visible && (i < span.first || i > span.second)) ranges = false;
            }
            if (layout.ContentHeight() < layout.HeaderRect(3).bottom + scroll - viewport.top - 0.01f) ordered = false;
        }
        check(valid, "grouped icon grids: hits stay inside cells, never on headers, interiors reachable");
        check(ordered, "grouped icon grids: each group's cells lie between its header and the next");
        check(rows_start, "grouped icon grids: every group starts a new row");
        check(ranges, "grouped icon grids: visible and span ranges cover every visible cell");
    }
    {
        // Up / down across groups keeps the column; left / right walk the order.
        // 500 px medium icons: 5 columns. Group 0 = 0..7 (rows 0..4, 5..7),
        // group 1 = 8..9 (one row), group 2 = 10..19 (rows 10..14, 15..19).
        ListGroups gs{{0, 8}, {8, 2}, {10, 10}};
        const ViewLayout g(ViewMode::MediumIcons, {0, 0, 500, 600}, 20, 0, 0, 1, 0, &gs);
        check(g.Metrics().columns == 5, "medium icons at 500 px: 5 columns");
        check(g.MoveIndex(1, 0, 1) == 6, "down within a group: next row, same column");
        check(g.MoveIndex(6, 0, 1) == 9, "down out of a group: same column of the next group's first row");
        check(g.MoveIndex(7, 0, 1) == 9, "down into a short group: its last cell when the column is missing");
        check(g.MoveIndex(9, 0, 1) == 11, "down from a one-row group into the next");
        check(g.MoveIndex(10, 0, -1) == 8, "up into the previous group's last row");
        check(g.MoveIndex(8, 0, -1) == 5, "up from a group's first row lands in the previous group's last row");
        check(g.MoveIndex(7, 1, 0) == 8 && g.MoveIndex(8, -1, 0) == 7, "left / right cross group edges");
        check(g.MoveIndex(0, 0, -1) == 0 && g.MoveIndex(19, 0, 1) == 19, "edges stay put");
        check(g.MoveIndex(1, 0, 3) == 11, "three rows down crosses two groups");
        ListGroups folded = gs; folded[1].collapsed = true;
        const ViewLayout f(ViewMode::MediumIcons, {0, 0, 500, 600}, 20, 0, 0, 1, 0, &folded);
        check(f.MoveIndex(6, 0, 1) == 11, "down skips a collapsed group");
    }
    {
        // Single-column views keep their old geometry exactly.
        ListGroups gs{{0, 3}, {3, 4}};
        const ViewLayout d(ViewMode::Details, {0, 0, 700, 400}, 7, 0, 0, 1, 0, &gs);
        const float row = d.ItemRect(1).top - d.ItemRect(0).top;
        check(std::abs(d.ItemRect(0).top - kGroupHeaderDip) < 0.01f &&
              std::abs(d.ItemRect(3).top - (2 * kGroupHeaderDip + 3 * row)) < 0.01f,
              "details: rows flush under headers as before");
        check(d.MoveIndex(2, 0, 1) == 3 && d.MoveIndex(3, 0, -1) == 2, "details: up / down walk rows across groups");
        // List (column-major) and photo walls do not group.
        check(!ViewLayout(ViewMode::List, {0, 0, 700, 400}, 7, 0, 0, 1, 0, &gs).Grouped(), "list view is not grouped");
        check(!ShowsGroups(ViewMode::List) && ShowsGroups(ViewMode::LargeIcons) && ShowsGroups(ViewMode::Tiles),
              "ShowsGroups: all views but List");
        std::vector<float> aspects(7, 1.5f);
        const WallRows wall = BuildWallRows(aspects, 700, 120, 1);
        ViewLayoutParams wp(0, 0);
        wp.wall = &wall;
        check(!ViewLayout(ViewMode::LargeIcons, {0, 0, 700, 400}, 7, 0, 0, 1, wp, &gs).Grouped(), "photo wall is not grouped");
    }
    return failures ? 1 : 0;
}
