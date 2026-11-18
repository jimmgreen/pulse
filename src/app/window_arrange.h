#pragma once
#include <vector>
#include <windows.h>
namespace pulse::app {
enum class WindowArrangeLayout {
    SideBySide,
    Stacked,
    Columns,
    Tile,
    Main,
    Cascade,
    MainRight,
    MainTop
};
std::vector<RECT> CalculateWindowArrangement(WindowArrangeLayout layout, RECT work, size_t count, int gap,
                                             int min_width, int min_height, int cascade_step = 32,
                                             const std::vector<SIZE>& original_sizes = {});
} // namespace pulse::app
