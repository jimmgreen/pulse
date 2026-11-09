#include "window_arrange.h"
#include <algorithm>
#include <cmath>
namespace pulse::app {
std::vector<RECT> CalculateWindowArrangement(WindowArrangeLayout layout, RECT w, size_t n, int gap, int mw, int mh,
                                             int cascade_step, const std::vector<SIZE>& original_sizes) {
    std::vector<RECT> out;
    if (!n || n > 4096 || w.right <= w.left || w.bottom <= w.top)
        return out;
    gap = std::clamp(gap, 0, 1000);
    mw = (std::max)(1, mw);
    mh = (std::max)(1, mh);
    const int width = w.right - w.left, height = w.bottom - w.top;
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
        if (!original_sizes.empty()) {
            if (original_sizes.size() != n) return {};
            int widest = 0, tallest = 0;
            for (const auto size : original_sizes) {
                if (size.cx <= 0 || size.cy <= 0) return {};
                widest = (std::max)(widest, (std::min)(width, static_cast<int>(size.cx)));
                tallest = (std::max)(tallest, (std::min)(height, static_cast<int>(size.cy)));
            }
            const int intervals = static_cast<int>((std::max)(size_t{1}, n - 1));
            const int step = (std::min)((std::max)(0, cascade_step),
                (std::min)((width - widest) / intervals, (height - tallest) / intervals));
            for (size_t i = 0; i < n; ++i) {
                const int d = step * static_cast<int>(i);
                out.push_back({w.left + d, w.top + d,
                    w.left + d + (std::min)(width, static_cast<int>(original_sizes[i].cx)),
                    w.top + d + (std::min)(height, static_cast<int>(original_sizes[i].cy))});
            }
        } else {
            const int step = (std::min)((std::max)(0, cascade_step), (std::min)((width - mw) / static_cast<int>(n),
                                                                                (height - mh) / static_cast<int>(n)));
            const int offset = step * static_cast<int>(n - 1);
            for (size_t i = 0; i < n; ++i) {
                const int d = step * static_cast<int>(i);
                out.push_back({w.left + d, w.top + d, w.right - offset + d, w.bottom - offset + d});
            }
        }
    } else if (layout == WindowArrangeLayout::MainRight && n > 1) {
        const int main_width = (width - gap) * 3 / 5;
        const int side_width = width - gap - main_width;
        if (main_width < mw || height < mh) return {};
        out.push_back({w.left + side_width + gap, w.top, w.right, w.bottom});
        if (!grid({w.left, w.top, w.left + side_width, w.bottom}, n - 1, 1)) return {};
    } else if (layout == WindowArrangeLayout::MainTop && n > 1) {
        const int main_height = (height - gap) * 3 / 5;
        if (width < mw || main_height < mh) return {};
        out.push_back({w.left, w.top, w.right, w.top + main_height});
        if (!grid({w.left, w.top + main_height + gap, w.right, w.bottom}, n - 1,
                  static_cast<int>(n - 1))) return {};
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
    } else if (!grid(w, n, layout == WindowArrangeLayout::Stacked ? 1
                          : layout == WindowArrangeLayout::Columns ? (std::min)(3, static_cast<int>(n))
                                                                   : static_cast<int>(n)))
        return {};
    return out;
}
} // namespace pulse::app
