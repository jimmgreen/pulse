#pragma once
#include <algorithm>
#include <vector>
namespace pulse::ui {
// Allocate the actual budget before hiding labels; short titles release room
// to longer ones. A bounded search avoids per-frame iterative redistribution.
inline bool FitTabWidths(std::vector<float>& widths, const std::vector<size_t>& visible,
                         float budget, float minimum) {
    if (visible.empty()) return true;
    float minimum_total = 0;
    for (auto i : visible) minimum_total += (std::min)(widths[i], minimum);
    if (budget < minimum_total) return false;
    float low = minimum, high = minimum;
    for (auto i : visible) high = (std::max)(high, widths[i]);
    for (int pass = 0; pass < 20; ++pass) {
        const float cap = (low + high) * .5f;
        float used = 0;
        for (auto i : visible) used += (std::min)(widths[i], cap);
        if (used > budget) high = cap; else low = cap;
    }
    for (auto i : visible) widths[i] = (std::min)(widths[i], low);
    return true;
}
// Below the label threshold, preserve the active title where possible while
// continuing to allocate the entire budget, rather than snapping every other
// tab to a fixed icon slot and leaving a large unused part of the title bar.
inline bool FitCompactTabWidths(std::vector<float>& widths, const std::vector<size_t>& visible,
                                float budget, float minimum, size_t active, float active_limit) {
    std::vector<size_t> others;
    float others_minimum = 0;
    for (auto i : visible) {
        if (i == active) continue;
        others.push_back(i);
        others_minimum += (std::min)(widths[i], minimum);
    }
    if (budget < others_minimum + (std::min)(widths[active], minimum)) return false;
    const float active_width = (std::min)(widths[active],
        (std::min)(active_limit, budget - others_minimum));
    if (!FitTabWidths(widths, others, budget - active_width, minimum)) return false;
    widths[active] = active_width;
    return true;
}
inline bool TabStripMotionSupported(bool explicit_rects, int layout) noexcept {
    return !explicit_rects || layout == 0;
}
}
