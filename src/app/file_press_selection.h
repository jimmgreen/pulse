#pragma once
#include "list_selection_gesture.h"
#include "listing_selection_restore.h"

namespace pulse::app {
enum class FilePressMatch { Valid, ContextChanged, SourceMissing, SelectionChanged };

inline void CaptureFilePressIdentity(ListSelectionGesture& gesture, const Tab& tab, int row) {
    gesture.file_snapshot = tab.snapshot;
    gesture.file_index = row;
    gesture.file_path = tab.current_path;
    gesture.file_key = ListingSelectionKey(tab.EntryAt(static_cast<size_t>(row)));
    for (int index : tab.SelectedIndices())
        gesture.file_selection.push_back(ListingSelectionKey(tab.EntryAt(static_cast<size_t>(index))));
    std::sort(gesture.file_selection.begin(), gesture.file_selection.end());
}

inline FilePressMatch MatchFilePress(ListSelectionGesture& gesture, const void* pane, const Tab* tab) {
    if (!tab || !gesture.PendingFile()) return FilePressMatch::ContextChanged;
    const MarqueeContext next{pane, tab, tab->snapshot.get(), tab->TreeEntries(), tab->view_generation};
    // Selection indices may move on refresh; only the same selected identities
    // may continue a file press. Marquees still require the exact old snapshot.
    int pressed = -1;
    std::vector<std::wstring> selected;
    const bool same_context = gesture.context.pane == next.pane && gesture.context.tab == next.tab &&
        gesture.context.generation == next.generation && gesture.file_path == tab->current_path;
    const bool refreshed = gesture.context.snapshot != next.snapshot || gesture.context.tree != next.tree;
    if (same_context && refreshed) {
        const auto is_pressed = [&](int index) {
            return tab->EntryVisible(index) &&
                ListingSelectionKey(tab->EntryAt(static_cast<size_t>(index))) == gesture.file_key;
        };
        // Size-only updates keep indices stable, which is the common case in
        // a downloading directory. Search the listing only after a reorder.
        if (is_pressed(gesture.file_index)) pressed = gesture.file_index;
        else {
            for (size_t i = 0; i < tab->EntryCount(); ++i) {
                if (is_pressed(static_cast<int>(i))) { pressed = static_cast<int>(i); break; }
            }
        }
        if (pressed >= 0) {
            for (int index : tab->SelectedIndices()) {
                if (tab->EntryVisible(index))
                    selected.push_back(ListingSelectionKey(tab->EntryAt(static_cast<size_t>(index))));
            }
        }
    }
    if (!same_context) return FilePressMatch::ContextChanged;
    if (!refreshed && (!gesture.revision_tracked || gesture.revision == tab->selection_revision))
        return FilePressMatch::Valid;
    if (refreshed && pressed < 0) return FilePressMatch::SourceMissing;
    if (!gesture.RebaseFile(next, tab->current_path, pressed, std::move(selected)))
        return FilePressMatch::SelectionChanged;
    gesture.file_snapshot = tab->snapshot;
    gesture.revision = tab->selection_revision;
    return FilePressMatch::Valid;
}
} // namespace pulse::app
