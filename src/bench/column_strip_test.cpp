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
    const auto approximatelyEqual = [](float a, float b) { return std::fabs(a - b) < 0.02f; };
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
            check(layout.active && approximatelyEqual(layout.viewport.left, bounds.left) &&
                  approximatelyEqual(layout.viewport.right, bounds.right),
                  "body and child participate in full horizontal viewport");
            check(approximatelyEqual(layout.body.right - layout.body.left,
                       kColumnStripDefaultDip * scale),
                  "regular body width stays stable when offscreen");
            check(approximatelyEqual(layout.scroll_px, layout.max_scroll_px),
                  "new navigation initially reveals newest column");
            const auto left = LayoutColumnStrip(bounds, 80 * scale, 12, true, {}, scale, 0);
            check(approximatelyEqual(left.scroll_px, 0) &&
                  approximatelyEqual(left.body.left, bounds.left + 12 * kColumnStripDefaultDip * scale),
                  "scroll left can reveal oldest ancestor and move body out of view");
            const auto root = LayoutColumnStrip(bounds, 80 * scale, 0, false, {}, scale);
            check(root.active && approximatelyEqual(root.body.left, bounds.left) && root.columns.empty(),
                  "single root starts on pane left");
        }
    }
    const auto bounds = D2D1::RectF(0, 0, 1440, 800);
    const auto before = LayoutColumnStrip(bounds, 42, 5, true, {}, 1.0f);
    float clicked_x = -1;
    for (const auto& slot : before.columns) if (slot.id == 4) clicked_x = slot.rect.left;
    const auto after = LayoutColumnStrip(bounds, 42, 4, true, {}, 1.0f, before.scroll_px);
    check(approximatelyEqual(before.scroll_px, 240) && approximatelyEqual(after.scroll_px, 240) &&
          approximatelyEqual(clicked_x, after.body.left),
          "clicking earlier directory does not move source column on shorter branch");
    check(approximatelyEqual(after.max_scroll_px, 240) && after.content_px > 1440,
          "trailing whitespace prevents horizontal snap-back");
    const auto left = LayoutColumnStrip(bounds, 42, 4, true, {}, 1.0f, 0);
    check(approximatelyEqual(left.scroll_px, 0) && approximatelyEqual(left.max_scroll_px, 0),
          "manual scroll to beginning releases the extra whitespace");
    const auto narrow = LayoutColumnStrip(D2D1::RectF(0, 0, 180, 600),
                                          42, 0, false, {}, 1.0f);
    check(approximatelyEqual(narrow.body.right - narrow.body.left, 240.0f) &&
          narrow.max_scroll_px > 0.0f,
          "narrow pane scrolls rather than narrowing the stored body width");
    const auto custom = LayoutColumnStrip(bounds, 42, 4, true,
        {300.0f, 220.0f, 180.0f, 260.0f, 280.0f}, 1.0f, 200.0f);
    check(approximatelyEqual(custom.body.left, 280 + 260 + 180 + 220 - 200),
          "resized columns share the same coordinates");
    return failures ? 1 : 0;
}
