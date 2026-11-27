#pragma once

namespace pulse::app {
inline bool ShouldPulseSearchForeground(bool double_ctrl, bool direct_accepted, bool current_generation,
                                       bool foreground_is_search, bool foreground_unchanged, bool input_released) {
    return double_ctrl && !direct_accepted && current_generation && !foreground_is_search &&
        foreground_unchanged && input_released;
}
}
