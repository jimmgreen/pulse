#include "../ui/column_strip_layout.h"
#include "../app/column_drop_target.h"
#include "../app/column_interaction_rules.h"
#include <memory>
#include <vector>
#include <cmath>
#include <cstdio>

int main() {
    using namespace pulse::ui;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    check(ColumnStripWidthSlot(2, 3) == 1 && ColumnStripWidthSlot(0, 3) == 3 &&
          ColumnStripWidthSlot(kColumnStripChildId, 3) == 0 &&
          ColumnStripWidthSlot(kColumnStripBodyId, 3) == kColumnStripBodySlot,
          "widths follow ancestor depth, child and current-folder identity");
    check(ColumnStripWidthDip({1.0f, 900.0f}, 0) == kColumnStripMinDip &&
          ColumnStripWidthDip({1.0f, 900.0f}, 1) == kColumnStripMaxDip, "stored widths respect limits");
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        for (float width : {200.0f, 360.0f, 640.0f, 1200.0f}) {
            const auto bounds = D2D1::RectF(10, 20, 10 + width * scale, 600 * scale);
            const auto layout = LayoutColumnStrip(bounds, 80 * scale, 12, true, {}, scale);
            check(layout.body.left >= bounds.left && layout.body.right <= bounds.right &&
                  layout.body.right >= layout.body.left, "current pane remains inside bounds across DPI and narrow widths");
            check(!layout.active || layout.body.right - layout.body.left >=
                      std::fmin(kColumnStripDefaultDip, width) * scale - .01f,
                  "ancestors and child preserve the current folder's column width");
            check(std::abs(layout.scroll_px - layout.max_scroll_px) < .01f,
                  "initial ancestor viewport stays beside immediate parent");
            const auto scrolled = LayoutColumnStrip(bounds, 80 * scale, 12, true, {}, scale, 100000);
            check(scrolled.scroll_px == 0, "horizontal overscroll clamps at oldest ancestor");
            const auto root = LayoutColumnStrip(bounds, 80 * scale, 0, false, {}, scale);
            check(root.active && root.body.left == bounds.left && root.columns.empty(),
                  "a root folder alone is one column at the left");
        }
    }
    // Column drop routing uses the row-to-snapshot mapping of the hit column,
    // never the active tab's index. Test both directions and invalid targets.
    {
        using pulse::app::ColumnDropDestination;
        pulse::app::ColumnStripState state;
        state.for_path = L"D:\\Project\\Current";
        state.ancestors.resize(1);
        auto& ancestor = state.ancestors[0];
        ancestor.path = L"D:\\Project";
        auto listing = std::make_shared<std::vector<pulse::fs::DirEntry>>();
        listing->resize(3);
        (*listing)[0].name = L"readme.txt";
        (*listing)[1].name = L"Assets";
        (*listing)[1].is_dir = true;
        (*listing)[2].name = L"removed";
        (*listing)[2].is_dir = true;
        (*listing)[2].change_record_only = true;
        ancestor.snapshot = listing;
        ancestor.rows_snapshot = listing;
        ancestor.rows = std::make_shared<const std::vector<int>>(std::vector<int>{0, 2, 1});
        state.child.path = L"D:\\Project\\Current\\Selected";
        check(ColumnDropDestination(state, L"D:\\Project\\Current", 0, -1, false) ==
                  L"D:\\Project", "ancestor column blank targets the ancestor folder");
        check(ColumnDropDestination(state, L"D:\\Project\\Current", 0, 2, true) ==
                  L"D:\\Project\\Assets", "ancestor folder row uses mapped snapshot index");
        check(ColumnDropDestination(state, L"D:\\Project\\Current", 0, 0, true).empty(),
              "ordinary-file row is not silently treated as current folder");
        check(ColumnDropDestination(state, L"D:\\Project\\Current", 0, 1, true).empty(),
              "historical or deleted row is not a drop target");
        check(ColumnDropDestination(state, L"D:\\Project\\Current", 0, 3, true).empty(),
              "out-of-range row cannot redirect a drop");
        check(ColumnDropDestination(state, L"D:\\Project\\Current", kColumnStripChildId, -1, false) ==
                  state.child.path, "child column blank targets the child folder");
        check(ColumnDropDestination(state, L"D:\\Project\\Current", -1, -1, false).empty() &&
                  ColumnDropDestination(state, L"D:\\Project\\Current", kColumnStripBodyId, -1, false).empty(),
              "unowned blank and current-body pseudo-column are rejected");
        check(ColumnDropDestination(state, L"D:\\Elsewhere", 0, -1, false).empty(),
              "stale navigation state cannot supply a destination");
        check(pulse::app::ColumnStripRowItemPath(state, L"D:\\Project\\Current", 0, 0) ==
                  L"D:\\Project\\readme.txt", "file row starts a drag without navigation");
        check(pulse::app::ColumnStripRowItemPath(state, L"D:\\Project\\Current", 0, 2) ==
                  L"D:\\Project\\Assets", "folder drag source follows row index mapping");
        check(pulse::app::ColumnStripRowItemPath(state, L"D:\\Project\\Current", 0, 1).empty(),
              "history row cannot start a drag");
        ancestor.snapshot = std::make_shared<const std::vector<pulse::fs::DirEntry>>();
        check(ColumnDropDestination(state, L"D:\\Project\\Current", 0, 2, true).empty(),
              "stale row snapshot is rejected during refresh");
        check(ColumnDropDestination(state, L"D:\\Project\\Current", 0, -1, false) ==
                  L"D:\\Project", "column background remains targetable while row list refreshes");
    }
    check(!pulse::app::ColumnGestureExceededSlop(30, 40, 33, 43, 4, 4) &&
           pulse::app::ColumnGestureExceededSlop(30, 40, 34, 43, 4, 4),
          "press becomes a drag only above the Windows drag threshold");
    check(ColumnStripVisualScroll(120, 40, true, true) == 40 &&
          ColumnStripVisualScroll(120, 40, false, true) == 120 &&
          ColumnStripVisualScroll(120, 40, true, false) == 120,
          "paint and hit test use the same in-flight column scroll offset");
    return failures ? 1 : 0;
}
