#include "../ui/ui_motion.h"
#include <cstdio>
#include <cmath>
using namespace pulse::ui::motion;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; };
    const auto a = D2D1::RectF(0, 0, 20, 20);
    const auto b = D2D1::RectF(100, 100, 120, 120);
    const auto c = D2D1::RectF(200, 200, 220, 220);
    for (SpringSpec spec : {spring::Plate, spring::Smooth, SpringSpec{400, 40}, SpringSpec{400, 60}}) {
        SpringRectMotion coarse, fine;
        coarse.Update(1, 0, a, 1, 1000, spec);
        fine.Update(1, 0, a, 1, 1000, spec);
        coarse.Update(1, 1, b, 2, 1000, spec);
        fine.Update(1, 1, b, 2, 1000, spec);
        D2D1_RECT_F f{};
        for (uint64_t i = 1; i <= 10; ++i) f = fine.Update(1, 1, b, 2 + i, 1000 + i * 10, spec);
        const auto slow = coarse.Update(1, 1, b, 3, 1100, spec);
        check(std::fabs(slow.left - f.left) < 0.001f, "100ms frame matches ten 10ms frames");
        const auto redirected = coarse.Update(1, 2, c, 4, 1150, spec);
        const auto prior = fine.Update(1, 1, b, 13, 1150, spec);
        fine.Update(1, 2, c, 14, 1150, spec);
        check(std::fabs(redirected.left - prior.left) < 0.001f, "retarget samples previous trajectory before changing destination");
        const auto cf = coarse.Update(1, 2, c, 5, 1190, spec);
        const auto ff = fine.Update(1, 2, c, 15, 1190, spec);
        check(std::fabs(cf.left - ff.left) < 0.001f, "retarget preserves velocity");
        coarse.SetAnimationsEnabled(false);
        check(coarse.Update(1, 3, a, 6, 1200, spec).left == 0 && !coarse.Active(1200), "disabled animations snap");
        fine.Hide();
        check(fine.Update(1, 3, a, 16, 1200, spec).left == 0 && !fine.Active(1200), "hidden surface resets stale motion");
    }
    return failures ? 1 : 0;
}
