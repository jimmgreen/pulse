#pragma once
#include "../common/picker_places_snapshot.h"

namespace pulse::app {
// The caller supplies the active tab and its normal in-memory row-to-path resolver.
// Kept independent of AppState so navigation and selection edge cases can be tested in isolation.
template<class Tab, class ResolvePath>
void CapturePickerSelection(const Tab* tab, picker_places::Snapshot& snapshot, ResolvePath resolve_path) {
    snapshot.selected_items.clear();
    snapshot.selection_available = true;
    if (!tab || !tab->snapshot) return;
    // Navigation can retain the old listing. Content results can read backing storage.
    if (tab->snapshot_path != tab->current_path || tab->content_results ||
        tab->SelectedCount() > static_cast<int>(picker_places::kMaxSelectedItems)) {
        snapshot.selection_available = false;
        return;
    }
    for (int index : tab->SelectedIndices()) {
        const auto path = resolve_path(*tab, index);
        if (!picker_places::detail::DiskPath(path)) {
            snapshot.selection_available = false;
            snapshot.selected_items.clear();
            return;
        }
        const auto& entry = tab->EntryAt(static_cast<size_t>(index));
        snapshot.selected_items.push_back({path, entry.is_dir, entry.size});
    }
}
}
