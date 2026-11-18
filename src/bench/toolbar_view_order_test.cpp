#include "../app/toolbar_view_order.h"
#include "../ui/toolbar_layout.h"
#include <cstdio>
#include <cmath>

int main() {
    using namespace pulse;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) { std::printf("[FAIL] %s\n", label); ++failures; }
    };
    check(app::NormalizeToolbarViewOrder({}) == std::vector<std::wstring>{L"sort", L"filter", L"group"},
        "older preferences keep default order");
    check(app::NormalizeToolbarViewOrder({L"group", L"invalid", L"group", L"filter"}) ==
        std::vector<std::wstring>{L"group", L"filter", L"sort"}, "unknown duplicates and missing ids normalize");
    check(app::ReorderVisibleToolbarViews({L"sort", L"filter", L"group"}, {L"filter", L"group"}) ==
        std::vector<std::wstring>{L"sort", L"filter", L"group"}, "hiding a control preserves its saved position");
    check(app::ReorderVisibleToolbarViews({L"sort", L"filter", L"group"}, {L"group", L"filter"}) ==
        std::vector<std::wstring>{L"sort", L"group", L"filter"}, "visible drag reorders around hidden saved slot");
    std::array<int, 3> permutation{0, 1, 2};
    size_t cases = 0;
    do {
        std::vector<std::wstring> ids;
        for (int i : permutation) ids.emplace_back(app::kToolbarViewIds[i]);
        check(app::ToolbarViewLayoutOrder(ids) == permutation, "saved ids preserve requested order");
        for (float scale : {1.0f, 1.5f, 2.0f}) for (float width : {480.0f, 900.0f, 1400.0f})
        for (int mask = 0; mask < 8; ++mask) for (float expanded : {0.0f, 1.0f}) {
            ui::ToolbarPinOptions pins;
            pins.view_order = app::ToolbarViewLayoutOrder(ids);
            const auto layout = ui::MakeToolbarLayout(width*scale, scale, 0, 12*scale, 80*scale, 0,
                expanded, 0, mask & 4 ? 90*scale : 0, mask & 1 ? 90*scale : -1,
                mask & 2 ? 90*scale : -1, 0, ui::SelectionChipSpan(scale, 80*scale, 4), pins);
            const std::array<D2D1_RECT_F, 3> rects{layout.sort, layout.filter, layout.group};
            float previous_right = -1;
            for (int i : permutation) {
                const auto& rect = rects[i];
                check(std::isfinite(rect.left) && rect.right >= rect.left, "finite nonnegative rectangles");
                if (rect.right <= rect.left) continue;
                check(previous_right < 0 || rect.left >= previous_right + 5.9f*scale, "visible controls honor saved order without overlap");
                previous_right = rect.right;
            }
            check((layout.sort.right > layout.sort.left) == !!(mask & 1), "hidden sort has no target");
            check((layout.filter.right > layout.filter.left) == (!!(mask & 2) || expanded > 0), "active filter remains available");
            check((layout.group.right > layout.group.left) == !!(mask & 4), "hidden group has no target");
            const auto chip = ui::MakeToolbarSelectionChip(layout, scale, 80*scale, 4);
            if (chip.chip.right > chip.chip.left)
                check(chip.chip.right <= std::min({layout.sort.left, layout.filter.left, layout.group.left}) - 9.9f*scale,
                    "selection chip stops before first reordered control");
            ++cases;
        }
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    std::printf("[%s] toolbar order: %zu layout cases, failures=%d\n", failures ? "FAIL" : "PASS", cases, failures);
    return failures ? 1 : 0;
}
