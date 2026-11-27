#pragma once

#include "folder_picker_model.h"

namespace pulse::ui {

// DragOver must stay in memory. Existence, access, links and caller vetoes are
// resolved later by the normal asynchronous validation / FileOk path.
inline bool PickerCanUseIncomingFiles(const std::vector<std::wstring>& paths,
    PickerMode mode, const PickerOptions& options, bool multiselect) {
    if ((mode != PickerMode::File && mode != PickerMode::Image) || paths.empty() ||
        paths.size() > 64 || (!multiselect && paths.size() != 1)) return false;
    for (const auto& path : paths)
        if (!IsPickerDiskPath(path) || !PickerMatchesFilter(path.substr(path.find_last_of(L"\\/") + 1), mode, options)) return false;
    return true;
}

// Using an existing file never transfers ownership to the target. In particular,
// do not answer MOVE to a source that could then delete its original file.
inline DWORD PickerUseFilesEffect(DWORD allowed) {
    return allowed & DROPEFFECT_LINK ? DROPEFFECT_LINK : allowed & DROPEFFECT_COPY ? DROPEFFECT_COPY : DROPEFFECT_NONE;
}

} // namespace pulse::ui
