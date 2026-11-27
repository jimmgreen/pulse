#pragma once
#include "app_model.h"

namespace pulse::app {
// A save is not a navigation/selection command. Bind the empty-selection intent
// before waiting for disk IO, then discard it after an intervening user action.
struct PickerSavedSelection {
    uint64_t view_generation = 0;
    uint64_t selection_revision = 0;
    fs::SnapshotPtr snapshot;
    bool armed = false;
};
inline PickerSavedSelection ArmPickerSavedSelection(const Tab& tab, bool interacting) {
    return {tab.view_generation, tab.selection_revision, tab.snapshot,
        !interacting && tab.SelectedCount() == 0 && tab.pending_selected_name.empty() &&
        tab.pending_selected_names.empty()};
}
inline bool CanApplyPickerSavedSelection(const Tab& tab, const PickerSavedSelection& intent,
                                         bool interacting) {
    if (!intent.armed || interacting || tab.view_generation != intent.view_generation ||
        tab.SelectedCount() != 0 || !tab.pending_selected_name.empty() ||
        !tab.pending_selected_names.empty()) return false;
    if (tab.selection_revision == intent.selection_revision) return true;
    // The production initial/refresh listing focuses (not selects) its first
    // row. This single, explicitly marked revision is not a user's selection.
    return tab.snapshot != intent.snapshot && tab.selection_revision == intent.selection_revision + 1 &&
        tab.HasFocusHint();
}
inline int ApplyPickerSavedSelection(Tab& tab, PickerSavedSelection& intent,
                                     const std::wstring& name, bool interacting) {
    if (!CanApplyPickerSavedSelection(tab, intent, interacting)) { intent.armed = false; return -1; }
    if (tab.loading || !tab.snapshot) return -1;
    for (int i = 0; i < static_cast<int>(tab.EntryCount()); ++i) {
        if (_wcsicmp(tab.EntryAt(static_cast<size_t>(i)).name.c_str(), name.c_str()) || !tab.EntryVisible(i)) continue;
        tab.SelectOnly(i);
        intent.armed = false;
        return i;
    }
    return -1;
}
}
