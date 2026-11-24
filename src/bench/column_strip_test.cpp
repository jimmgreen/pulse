#include "../ui/column_strip_layout.h"
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
            const auto anchored = LayoutColumnStrip(bounds, 80 * scale, 12, true, {}, scale, 0.0f, 80.0f);
            check(!layout.active || std::abs(anchored.body.left - std::fmin(layout.body.left, bounds.left + 80.0f * scale)) < .01f,
                  "clicked ancestor anchor keeps the body at the old coordinate when there is room");
            const auto clipped = LayoutColumnStrip(bounds, 80 * scale, 12, true, {}, scale, 0.0f, 0.0f);
            check(!layout.active || std::abs(clipped.body.left - std::fmin(layout.body.left,
                      bounds.left + kColumnStripMinAnchorViewportDip * scale)) < .01f,
                  "clipped anchor keeps a usable ancestor viewport without taking room from the body or child");
            const auto root = LayoutColumnStrip(bounds, 80 * scale, 0, false, {}, scale);
            check(root.active && root.body.left == bounds.left && root.columns.empty(),
                  "a root folder alone is one column at the left");
        }
    }
    return failures ? 1 : 0;
}
