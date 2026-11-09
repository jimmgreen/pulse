#include "../ui/anchored_popup_position.h"
#include <cstdio>

int main() {
    using pulse::ui::AnchoredPopupPosition;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    const RECT work{0, 0, 1920, 1040};
    for (int dpi : {100, 150, 200}) {
        const int w = 280 * dpi / 100, h = 300 * dpi / 100, gap = 4 * dpi / 100;
        const RECT icon{920, 950, 952, 982};
        const auto r = AnchoredPopupPosition(icon, work, w, h, 20, gap, true);
        check(r.bottom - 20 == icon.top - gap, "card sits above icon with scaled gap");
        check((r.left + r.right) / 2 == (icon.left + icon.right) / 2, "card is centered on icon, independent of pointer");
    }
    const auto right = AnchoredPopupPosition({1890, 950, 1918, 980}, work, 280, 300, 20, 4, true);
    check(right.right == work.right && right.bottom - 20 == 946, "right edge clamps horizontally while staying above");
    const auto left = AnchoredPopupPosition({2, 950, 30, 980}, work, 280, 300, 20, 4, true);
    check(left.left == work.left, "left edge stays inside work area");
    const auto top = AnchoredPopupPosition({500, 0, 532, 32}, work, 280, 300, 20, 4, true);
    check(top.top + 20 == 36, "top-edge taskbar flips below icon when above cannot fit");
    const auto secondary = AnchoredPopupPosition({-1100, -100, -1068, -68}, {-1920, -1080, 0, 0}, 280, 300, 20, 4, true);
    check(secondary.bottom - 20 == -104 && (secondary.left + secondary.right) / 2 == -1084,
        "negative-coordinate monitor retains icon anchor");
    const auto normal = AnchoredPopupPosition({500, 100, 700, 132}, work, 280, 300, 20, 4, false);
    check(normal.left == 480 && normal.top == 116, "ordinary dropdown keeps existing below-left placement");
    const auto flipped = AnchoredPopupPosition({500, 950, 700, 982}, work, 280, 300, 20, 4, false);
    check(flipped.left == 480 && flipped.bottom - 20 == 946, "ordinary dropdown still flips above near bottom");
    return failures ? 1 : 0;
}
