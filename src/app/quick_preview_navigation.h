#pragma once
#include <algorithm>

namespace pulse::app {
// -1 means no selection change, so callers must not restart the preview.
inline int QuickPreviewNavigationTarget(int current, int count, int direction) {
    if (count <= 0) return -1;
    const int start = std::clamp(current, 0, count - 1);
    const int target = direction < 0 ? (std::max)(start - 1, 0)
                                     : (std::min)(start + 1, count - 1);
    // A missing selection still uses the old first-row fallback.
    return target == current ? -1 : target;
}
}
