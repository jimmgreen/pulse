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
        if (layout == WindowArrangeLayout::Stacked) {
            check(r.empty(), "stacked rejects insufficient height for all three windows");
            r = CalculateWindowArrangement(layout, {-1920, -100, 0, 1400}, 3, 6, 320, 420);
            check(r.size() == 3, "stacked covers all three windows when height permits");
            for (auto a : r)
                check(a.left >= -1920 && a.top >= -100 && a.right <= 0 && a.bottom <= 1400 &&
                          a.right - a.left >= 320 && a.bottom - a.top >= 420,
                      "stacked bounds and minimum size");
            continue;
        }
        check(r.size() == 3, "layout covers all three windows");
        for (auto a : r)
            check(a.left >= work.left && a.top >= work.top && a.right <= work.right && a.bottom <= work.bottom &&
                      a.right - a.left >= 320 && a.bottom - a.top >= 420,
                  "bounds and minimum size");
    }
    check(CalculateWindowArrangement(WindowArrangeLayout::SideBySide, work, 4, 0, 320, 420).size() == 4,
          "side-by-side covers all windows");
    check(CalculateWindowArrangement(WindowArrangeLayout::Columns, work, 6, 0, 320, 420).size() == 6, "columns covers all windows");
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
    const auto columns = CalculateWindowArrangement(WindowArrangeLayout::Columns, work, 4, 6, 320, 420);
    check(columns.size() == 4 && columns[3].left == columns[0].left && columns[3].top > columns[0].top,
          "columns wraps after three windows");
    check(CalculateWindowArrangement(WindowArrangeLayout::SideBySide, work, 8, 0, 320, 420).empty(),
          "side-by-side rejects insufficient width without omitting windows");
    const std::vector<SIZE> sizes{{800, 600}, {1100, 700}, {640, 480}};
    const auto preserved = CalculateWindowArrangement(WindowArrangeLayout::Cascade, work, sizes.size(), 0, 320, 420, 32, sizes);
    bool unchanged = preserved.size() == sizes.size();
    for (size_t i = 0; unchanged && i < sizes.size(); ++i)
        unchanged = preserved[i].right-preserved[i].left == sizes[i].cx &&
            preserved[i].bottom-preserved[i].top == sizes[i].cy;
    check(unchanged && preserved[1].left-preserved[0].left == 32, "cascade preserves each original size and offsets positions");
    const auto tight = CalculateWindowArrangement(WindowArrangeLayout::Cascade, {0,0,1200,800}, 2, 0, 320,420,32, {{1190,790},{800,600}});
    check(tight.size()==2 && tight[0].right-tight[0].left==1190 && tight[1].left==10,
        "cascade reduces spacing instead of resizing windows that fit");
    const auto large = CalculateWindowArrangement(WindowArrangeLayout::Cascade, {0,0,1200,800}, 2,0,320,420,32,{{1600,1000},{800,600}});
    check(large.size()==2 && large[0].right-large[0].left==1200 && large[0].bottom-large[0].top==800 &&
        large[1].right-large[1].left==800, "cascade only shrinks oversized windows");
    for (auto layout : {WindowArrangeLayout::MainRight, WindowArrangeLayout::MainTop}) {
        const RECT area{-2000, -200, 0, 1400};
        const auto r = CalculateWindowArrangement(layout, area, 3, 8, 320, 300);
        check(r.size() == 3, "new main layouts include every window");
        if (r.size() == 3) {
            check(layout == WindowArrangeLayout::MainRight ? r[0].left > r[1].right : r[0].bottom < r[1].top,
                  "main window uses requested side");
            for (auto a : r) check(a.left >= area.left && a.top >= area.top && a.right <= area.right &&
                a.bottom <= area.bottom && a.right-a.left >= 320 && a.bottom-a.top >= 300,
                "new main layout respects bounds and minimum sizes");
        }
        check(CalculateWindowArrangement(layout, {0,0,500,400}, 3, 8, 320, 300).empty(),
              "new main layouts reject insufficient space");
    }
    return failures ? 1 : 0;
}
