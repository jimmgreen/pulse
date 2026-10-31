#include "../app/window_arrange.h"
#include <cstdio>
int main() {
    using namespace pulse::app;
    int failures = 0;
    auto check = [&](bool ok, const char *name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok)
            ++failures;
    };
    const RECT work{-1920, -100, 0, 980};
    for (auto layout : {WindowArrangeLayout::SideBySide, WindowArrangeLayout::Stacked, WindowArrangeLayout::Columns,
                        WindowArrangeLayout::Tile, WindowArrangeLayout::Main, WindowArrangeLayout::Cascade}) {
        auto r = CalculateWindowArrangement(layout, work, 3, 6, 320, 420);
        check(r.size() >= 2, "layout produces windows");
        for (auto a : r)
            check(a.left >= work.left && a.top >= work.top && a.right <= work.right && a.bottom <= work.bottom &&
                      a.right - a.left >= 320 && a.bottom - a.top >= 420,
                  "bounds and minimum size");
    }
    check(CalculateWindowArrangement(WindowArrangeLayout::SideBySide, work, 8, 0, 320, 420).size() == 2,
          "side-by-side cap");
    check(CalculateWindowArrangement(WindowArrangeLayout::Columns, work, 8, 0, 320, 420).size() == 3, "columns cap");
    check(CalculateWindowArrangement(WindowArrangeLayout::Tile, work, 0, 0, 320, 420).empty(), "empty input");
    check(CalculateWindowArrangement(WindowArrangeLayout::Tile, {0, 0, 600, 400}, 2, 6, 320, 420).empty(),
          "small screen rejects");
    check(CalculateWindowArrangement(WindowArrangeLayout::Tile, {0, 0, 3840, 2160}, 4, 12, 640, 840).size() == 4,
          "200 percent DPI");
    const auto ordered = CalculateWindowArrangement(WindowArrangeLayout::SideBySide, work, 2, 6, 320, 420);
    check(ordered.size() == 2 && ordered[0].left < ordered[1].left && ordered[1].left - ordered[0].right == 6,
          "stable order and gap");
    check(CalculateWindowArrangement(WindowArrangeLayout::Cascade, {0, 0, 200, 200}, 2, 0, 320, 420).empty(),
          "cascade refuses undersized work area");
    const auto cascade =
        CalculateWindowArrangement(WindowArrangeLayout::Cascade, {0, 0, 3840, 2160}, 3, 12, 640, 840, 64);
    check(cascade.size() == 3 && cascade[1].left - cascade[0].left == 64, "cascade 32 DIP at 200 percent DPI");
    return failures ? 1 : 0;
}
