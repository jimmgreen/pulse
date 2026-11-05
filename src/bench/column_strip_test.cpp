#include "../ui/column_strip_layout.h"
#include "../app/column_drop_target.h"
#include "../app/column_interaction_rules.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

int main() {
    using namespace pulse::ui;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    const auto eq = [](float a, float b) { return std::fabs(a - b) < 0.02f; };

    check(ColumnStripWidthSlot(2, 3) == 1 && ColumnStripWidthSlot(0, 3) == 3 &&
          ColumnStripWidthSlot(kColumnStripChildId, 3) == 0 &&
          ColumnStripWidthSlot(kColumnStripBodyId, 3) == kColumnStripBodySlot,
          "column width slot identities remain unchanged");
    check(ColumnStripWidthDip({}, 0) == kColumnStripDefaultDip &&
          ColumnStripWidthDip({}, kColumnStripBodySlot) == kColumnStripDefaultDip,
          "all unconfigured directory roles use a uniform 240 DIP width");
    check(ColumnStripWidthDip({1.0f, 900.0f}, 0) == kColumnStripMinDip &&
          ColumnStripWidthDip({1.0f, 900.0f}, 1) == kColumnStripMaxDip,
          "column width limits remain unchanged");

    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        for (float width : {200.0f, 360.0f, 640.0f, 1200.0f}) {
            const auto bounds = D2D1::RectF(10, 20, 10 + width * scale, 600 * scale);
            const auto layout = LayoutColumnStrip(bounds, 80 * scale, 12, true, {}, scale);
            check(layout.active && eq(layout.viewport.left, bounds.left) &&
                  eq(layout.viewport.right, bounds.right), "body and child share the horizontal viewport");
            check(eq(layout.body.right - layout.body.left, kColumnStripDefaultDip * scale),
                  "regular body width stays stable when offscreen");
            check(eq(layout.scroll_px, layout.max_scroll_px), "new navigation reveals the newest column");
            const auto left = LayoutColumnStrip(bounds, 80 * scale, 12, true, {}, scale, 0);
            check(eq(left.scroll_px, 0) &&
                  eq(left.body.left, bounds.left + 12 * kColumnStripDefaultDip * scale),
                  "scrolling left reveals the oldest ancestor");
            const auto root = LayoutColumnStrip(bounds, 80 * scale, 0, false, {}, scale);
            check(root.active && eq(root.body.left, bounds.left) && root.columns.empty(),
                  "single root starts at the pane's leading edge");
            const auto narrow_bounds = D2D1::RectF(0, 0, 180 * scale, 600 * scale);
            const auto narrow_root = LayoutColumnStrip(narrow_bounds, 80 * scale, 0, false, {}, scale);
            check(narrow_root.active && eq(narrow_root.scroll_px, 0.0f) && eq(narrow_root.body.left, 0.0f),
                  "narrow root remains left-anchored before user scrolling");
            const auto root_scrolled = LayoutColumnStrip(narrow_bounds, 80 * scale, 0, false, {}, scale, 20.0f);
            check(eq(root_scrolled.scroll_px, 20.0f * scale) &&
                  eq(root_scrolled.body.left, -20.0f * scale) &&
                  eq(root_scrolled.max_scroll_px, 60.0f * scale),
                  "narrow root honors explicit horizontal scrolling");
            const auto root_clamped = LayoutColumnStrip(narrow_bounds, 80 * scale, 0, false, {}, scale, 100.0f);
            check(eq(root_clamped.scroll_px, 60.0f * scale) && eq(root_clamped.max_scroll_px, 60.0f * scale),
                  "root scroll clamps to actual overflow after resize");

            const D2D1_RECT_F row_column = D2D1::RectF(20, 0, 260 * scale, 600 * scale);
            const auto folder_row = LayoutColumnStripRow(row_column, 100 * scale, 132 * scale, scale, true);
            const auto file_row = LayoutColumnStripRow(row_column, 100 * scale, 132 * scale, scale, false);
            check(eq(folder_row.cell.left, file_row.cell.left) &&
                  eq(folder_row.icon.left, file_row.icon.left) &&
                  eq(folder_row.text.left, file_row.text.left),
                  "folder and file names share the same text origin");
            check(eq(folder_row.text.right, folder_row.chevron.left - 4.0f * scale) &&
                  eq(file_row.text.right, file_row.cell.right - 4.0f * scale),
                  "text and chevron retain a four-DIP gap at every DPI");
        }
    }

    const auto bounds = D2D1::RectF(0, 0, 1440, 800);
    const auto before = LayoutColumnStrip(bounds, 42, 5, true, {}, 1.0f);
    float clicked_x = -1;
    for (const auto& slot : before.columns) if (slot.id == 4) clicked_x = slot.rect.left;
    const auto after = LayoutColumnStrip(bounds, 42, 4, true, {}, 1.0f, before.scroll_px);
    check(eq(before.scroll_px, 240) && eq(after.scroll_px, 240) && eq(clicked_x, after.body.left),
          "clicking an earlier directory does not move its source column");
    check(eq(after.max_scroll_px, 240) && after.content_px > 1440,
          "trailing whitespace prevents horizontal snap-back");
    const auto left = LayoutColumnStrip(bounds, 42, 4, true, {}, 1.0f, 0);
    check(eq(left.scroll_px, 0) && eq(left.max_scroll_px, 0),
          "manual scroll to the beginning releases extra whitespace");
    const auto narrow = LayoutColumnStrip(D2D1::RectF(0, 0, 180, 600), 42, 0, false, {}, 1.0f);
    check(eq(narrow.body.right - narrow.body.left, 240.0f) && narrow.max_scroll_px > 0.0f,
          "narrow pane scrolls instead of narrowing the stored body width");
    const auto custom = LayoutColumnStrip(bounds, 42, 4, true, {300.0f, 220.0f, 180.0f, 260.0f, 280.0f}, 1.0f, 200.0f);
    check(eq(custom.body.left, 280 + 260 + 180 + 220 - 200), "resized columns share the same coordinates");

    check(ColumnStripVisualScroll(120, 40, true, true) == 40 &&
          ColumnStripVisualScroll(120, 40, false, true) == 120 &&
          ColumnStripVisualScroll(120, 40, true, false) == 120,
          "paint and hit testing share the in-flight visual scroll offset");
    check(!pulse::app::ColumnGestureExceededSlop(30, 40, 33, 43, 4, 4) &&
          pulse::app::ColumnGestureExceededSlop(30, 40, 34, 43, 4, 4),
          "a press becomes a drag only after the Windows drag threshold");

    {
        using pulse::app::ColumnDropDestination;
        using pulse::app::ColumnStripRowItemPath;
        pulse::app::ColumnStripState state;
        state.for_path = L"D:\\Project\\Current";
        state.ancestors.resize(1);
        auto& ancestor = state.ancestors[0];
        ancestor.path = L"D:\\Project";
        auto listing = std::make_shared<std::vector<pulse::fs::DirEntry>>(4);
        (*listing)[0].name = L"readme.txt";
        (*listing)[1].name = L"removed";
        (*listing)[1].is_dir = true;
        (*listing)[1].change_record_only = true;
        (*listing)[2].name = L"Assets";
        (*listing)[2].is_dir = true;
        (*listing)[3].name = L"LinkDir";
        (*listing)[3].link_target_is_dir = true;
        (*listing)[3].full_path = L"D:\\Project\\LinkDir";
        ancestor.snapshot = listing;
        ancestor.rows_snapshot = listing;
        ancestor.rows = std::make_shared<const std::vector<int>>(std::vector<int>{0, 1, 2, 3});
        state.child.path = L"D:\\Project\\Current\\Selected";

        check(ColumnDropDestination(state, L"D:\\Project\\Current", 0, -1, false) == L"D:\\Project",
              "ancestor column background targets its owning folder");
        check(ColumnDropDestination(state, L"D:\\Project\\Current", 0, 2, true) == L"D:\\Project\\Assets",
              "folder row uses the displayed snapshot mapping");
        check(ColumnDropDestination(state, L"D:\\Project\\Current", 0, 3, true) == L"D:\\Project\\LinkDir",
              "directory symlink row is a valid folder target");
        check(ColumnDropDestination(state, L"D:\\Project\\Current", 0, 0, true).empty() &&
              ColumnDropDestination(state, L"D:\\Project\\Current", 0, 1, true).empty(),
              "file and historical rows are rejected as folder targets");
        check(ColumnDropDestination(state, L"D:\\Project\\Current", 0, 4, true).empty(),
              "out-of-range row cannot redirect a drop");
        check(ColumnDropDestination(state, L"D:\\Project\\Current", kColumnStripChildId, -1, false) ==
              state.child.path, "child column background targets its own folder");
        check(ColumnDropDestination(state, L"D:\\Project\\Current", -1, -1, false).empty() &&
              ColumnDropDestination(state, L"D:\\Project\\Current", kColumnStripBodyId, -1, false).empty(),
              "unowned blank and body pseudo-column are rejected");
        check(ColumnDropDestination(state, L"D:\\Elsewhere", 0, -1, false).empty(),
              "stale navigation state cannot supply a destination");
        check(ColumnStripRowItemPath(state, L"D:\\Project\\Current", 0, 0) == L"D:\\Project\\readme.txt" &&
              ColumnStripRowItemPath(state, L"D:\\Project\\Current", 0, 2) == L"D:\\Project\\Assets" &&
              ColumnStripRowItemPath(state, L"D:\\Project\\Current", 0, 3) == L"D:\\Project\\LinkDir",
              "drag source paths come from the displayed column and row mapping");
        check(ColumnStripRowItemPath(state, L"D:\\Project\\Current", 0, 1).empty(),
              "historical rows cannot start a drag");
        ancestor.snapshot = std::make_shared<const std::vector<pulse::fs::DirEntry>>();
        check(ColumnDropDestination(state, L"D:\\Project\\Current", 0, 2, true).empty() &&
              ColumnDropDestination(state, L"D:\\Project\\Current", 0, -1, false) == L"D:\\Project",
              "stale row snapshots reject row targets but preserve safe background targets");
    }
    return failures ? 1 : 0;
}
