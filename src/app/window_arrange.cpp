#include "window_arrange.h"
#include <algorithm>
#include <cmath>
namespace pulse::app {
std::vector<RECT> CalculateWindowArrangement(WindowArrangeLayout layout, RECT w, size_t n, int gap, int mw, int mh,
                                             int cascade_step) {
    std::vector<RECT> out;
    if (!n || n > 4096 || w.right <= w.left || w.bottom <= w.top)
        return out;
    gap = std::clamp(gap, 0, 1000);
    mw = (std::max)(1, mw);
    mh = (std::max)(1, mh);
    const int width = w.right - w.left, height = w.bottom - w.top;
    if (layout == WindowArrangeLayout::SideBySide || layout == WindowArrangeLayout::Stacked)
        n = (std::min)(n, size_t{2});
    if (layout == WindowArrangeLayout::Columns)
        n = (std::min)(n, size_t{3});
    auto grid = [&](RECT area, size_t count, int cols) {
        const int rows = (static_cast<int>(count) + cols - 1) / cols;
        const int aw = area.right - area.left - gap * (cols - 1), ah = area.bottom - area.top - gap * (rows - 1);
        if (aw / cols < mw || ah / rows < mh)
            return false;
        for (size_t i = 0; i < count; ++i) {
            const int c = static_cast<int>(i) % cols, r = static_cast<int>(i) / cols;
            out.push_back({area.left + aw * c / cols + gap * c, area.top + ah * r / rows + gap * r,
                           area.left + aw * (c + 1) / cols + gap * c, area.top + ah * (r + 1) / rows + gap * r});
        }
        return true;
    };
    if (layout == WindowArrangeLayout::Cascade) {
        if (width < mw || height < mh)
            return {};
        const int step = (std::min)((std::max)(0, cascade_step), (std::min)((width - mw) / static_cast<int>(n),
                                                                            (height - mh) / static_cast<int>(n)));
        const int offset = step * static_cast<int>(n - 1);
        for (size_t i = 0; i < n; ++i) {
            const int d = step * static_cast<int>(i);
            out.push_back({w.left + d, w.top + d, w.right - offset + d, w.bottom - offset + d});
        }
    } else if (layout == WindowArrangeLayout::Main && n > 1) {
        const int main_width = (width - gap) * 3 / 5;
        if (main_width < mw || height < mh)
            return {};
        out.push_back({w.left, w.top, w.left + main_width, w.bottom});
        if (!grid({w.left + main_width + gap, w.top, w.right, w.bottom}, n - 1, 1))
            return {};
    } else if (layout == WindowArrangeLayout::Tile || layout == WindowArrangeLayout::Main) {
        const int preferred = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
        bool found = false;
        for (int c = preferred; c >= 1; --c)
            if (grid(w, n, c)) {
                found = true;
                break;
            }
        if (!found)
            for (int c = preferred + 1; c <= static_cast<int>(n); ++c)
                if (grid(w, n, c)) {
                    found = true;
                    break;
                }
        if (!found)
            return {};
    } else if (!grid(w, n, layout == WindowArrangeLayout::Stacked ? 1 : static_cast<int>(n)))
        return {};
    return out;
}
} // namespace pulse::app
