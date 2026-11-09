#include "../ui/drag_layout.h"
#include <cstdio>
int main() {
    int failures = 0;
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f, 3.0f}) {
        for (float cursor : {0.0f, 8.0f, 39.0f, 40.0f, 300.0f, 590.0f, 600.0f}) {
            float height = 24 * scale;
            float y = pulse::ui::drag_layout::BadgeTop(cursor * scale, height, scale,
                                                       0, 600 * scale, 8 * scale);
            float stack_top = (cursor + pulse::ui::drag_layout::offset_y -
                pulse::ui::drag_layout::card_max_width * 0.225f / 2 - 12) * scale;
            float stack_bottom = (cursor + pulse::ui::drag_layout::stack_bottom) * scale;
            bool separate = y + height <= stack_top || y >= stack_bottom;
            bool contained = y >= 8 * scale && y + height <= 592 * scale;
            if (!separate || !contained) {
                ++failures;
                printf("[FAIL] scale %.2f cursor %.0f\n", scale, cursor);
            }
        }
    }
    printf("[%s] drag badge geometry: 35 edge/DPI cases\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
