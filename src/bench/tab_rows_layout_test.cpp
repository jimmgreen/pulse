#include "../ui/tab_rows_layout.h"
#include <cassert>
#include <cstdio>
int main() {
    using namespace pulse::ui;
    std::vector<TabRowItem> items(24, {140.0f, false, false});
    for (int max_rows = 2; max_rows <= 4; ++max_rows) {
        auto a = ComputeTabRows(items, 600, 60, 400, false, max_rows, 999, 0, true);
        assert(a.visible_rows == max_rows);
        assert(a.VisibleRow(0) == 0);
        assert(a.first_row == a.rows - a.visible_rows);
        auto b = ComputeTabRows(items, 600, 60, 400, false, max_rows, 0, 23, true);
        assert(b.VisibleRow(b.slots[23].row) > 0);
        auto c = ComputeTabRows(items, 600, 60, 400, false, max_rows, b.first_row, 2, true);
        assert(c.VisibleRow(c.slots[2].row) >= 0);
    }
    items[1].hidden = true;
    auto hidden = ComputeTabRows(items, 600, 60, 400, true, 3, 0, 0, false);
    assert(hidden.slots[1].row == -1);
    for (bool aligned : {false, true}) {
        for (float width : {1.0f, 20.0f, 100.0f, 500.0f}) {
            auto narrow = ComputeTabRows(items, width, 200, 100, aligned, 3, 0, 0, false);
            for (const auto& slot : narrow.slots) {
                if (slot.row < 0) continue;
                assert(slot.x >= 0 && slot.width > 0 && slot.x + slot.width <= width + 0.01f);
            }
        }
    }
    for(float width : {768.0f, 800.0f, 1168.0f}) {
        const auto aligned=ComputeTabRows(items,width,72,420,true,3,0,0,false);
        TabRowSlot previous{};previous.row=-1;
        for(const auto& slot:aligned.slots) {
            if(slot.row<0) continue;
            if(slot.row==previous.row) assert(std::fabs(slot.x-previous.x-previous.width)<0.1f);
            previous=slot;
        }
    }
    puts("[PASS] tab rows: fixed first row, paging, reveal, hidden, narrow, row limits");
}
