// quick_preview_follow.h — Decides when the floating quick preview should
// start showing a different entry because the focused listing row moved
// (#91). Pure logic, no AppState and no IO, so the rules stay unit testable.
#pragma once

#include <cstdint>
#include <string>

#include "../ui/quick_preview_window.h"

namespace pulse::app {

enum class QuickPreviewFollow {
    Stay,    // keep the shown entry (not previewable, or same entry unchanged)
    Switch,  // reload the preview from the followed entry
};

// followed_* mirror the QuickPreviewItem fields that decide whether the
// already displayed entry is still current; an empty followed_path means the
// focused row has no previewable entry (recycle bin / This PC / virtual
// location), which must leave the window untouched instead of blanking it.
inline QuickPreviewFollow DecideQuickPreviewFollow(const ui::QuickPreviewItem& shown,
                                                   const std::wstring& followed_path,
                                                   uint64_t followed_modified,
                                                   uint64_t followed_size,
                                                   DWORD followed_attrs) {
    if (followed_path.empty()) return QuickPreviewFollow::Stay;
    if (followed_path != shown.path) return QuickPreviewFollow::Switch;
    // Same path: only a content change on disk warrants a reload, so holding
    // an arrow key over one row does not restart playback every frame.
    if (followed_modified != shown.modified || followed_size != shown.size ||
        followed_attrs != shown.attrs)
        return QuickPreviewFollow::Switch;
    return QuickPreviewFollow::Stay;
}

} // namespace pulse::app
