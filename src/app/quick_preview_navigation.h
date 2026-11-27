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
// Absolute jumps (the photo viewer's filmstrip and filtered steps): -1 when
// the row is outside the listing or already selected.
inline int QuickPreviewJumpTarget(int current, int count, int target) {
    if (count <= 0 || target < 0 || target >= count || target == current) return -1;
    return target;
}
}
