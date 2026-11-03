// Pure column input rules: no theme, layout, file operations or global state.
#pragma once
#include <algorithm>
#include <cstdlib>
namespace pulse::app {
inline bool ColumnGestureExceededSlop(int x0, int y0, int x, int y,
                                      int slop_x, int slop_y) noexcept {
    return std::abs(x - x0) >= std::max(1, slop_x) ||
           std::abs(y - y0) >= std::max(1, slop_y);
}
} // namespace pulse::app
