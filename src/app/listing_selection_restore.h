#pragma once
#include "app_model.h"
#include "../fs/fs_enum.h"
#include <unordered_set>
#include <unordered_map>
#include <utility>

namespace pulse::app {
inline std::wstring ListingSelectionKey(const fs::DirEntry& entry) {
    if (!entry.recycle_path.empty()) return L"recycle:" + entry.recycle_path;
    if (!entry.full_path.empty()) return L"path:" + entry.full_path;
    return L"name:" + entry.name;
}

inline void CapturePendingListingSelection(Tab& tab) {
    tab.pending_selection_revision = tab.selection_revision;
    tab.pending_selection_key_revision = tab.selection_revision;
    tab.pending_selection_keys.clear();
    tab.pending_selection_key_names.clear();
    tab.pending_selection_focus_key.clear();
    tab.pending_selection_key_focus_name.clear();
    tab.pending_selected_names.clear();
    tab.pending_selected_name.clear();
    tab.pending_ensure_selection_visible = false;
    if (!tab.snapshot || tab.SelectedCount() <= 0) return;
    for (int index : tab.SelectedIndices()) {
        if (index >= 0 && index < static_cast<int>(tab.EntryCount())) {
            const auto& entry = tab.EntryAt(static_cast<size_t>(index));
            tab.pending_selected_names.push_back(entry.name);
            if (fs::IsVirtualPath(tab.current_path))
                tab.pending_selection_keys.push_back(ListingSelectionKey(entry));
        }
    }
    if (tab.selected_index >= 0 && tab.selected_index < static_cast<int>(tab.EntryCount()))
        tab.pending_selected_name = tab.EntryAt(static_cast<size_t>(tab.selected_index)).name;
    if (!tab.pending_selection_keys.empty()) {
        tab.pending_selection_key_names = tab.pending_selected_names;
        tab.pending_selection_key_focus_name = tab.pending_selected_name;
        if (tab.selected_index >= 0 && tab.selected_index < static_cast<int>(tab.EntryCount()))
            tab.pending_selection_focus_key = ListingSelectionKey(tab.EntryAt(static_cast<size_t>(tab.selected_index)));
    }
}

struct ListingSelectionRestore {
    std::vector<std::wstring> names;
    std::wstring focus;
    bool ensure_visible = false;
    bool user_changed = false;
    fs::SnapshotPtr previous_snapshot;
    int previous_focus = -1;
    std::vector<std::wstring> keys;
    std::wstring focus_key;
};

// Called after request-generation validation and before replacing the old snapshot.
inline ListingSelectionRestore TakeListingSelection(Tab& tab, const std::wstring& result_path) {
    const bool changed = tab.pending_selection_revision != UINT64_MAX &&
        tab.pending_selection_revision != tab.selection_revision && tab.snapshot &&
        tab.snapshot_path == result_path;
    if (changed) CapturePendingListingSelection(tab);
    const bool captured_identity = tab.pending_selection_revision != UINT64_MAX &&
        tab.pending_selection_key_revision == tab.pending_selection_revision &&
        tab.snapshot_path == result_path &&
        tab.pending_selection_key_names == tab.pending_selected_names &&
        tab.pending_selection_key_focus_name == tab.pending_selected_name;
    ListingSelectionRestore restore{
        std::exchange(tab.pending_selected_names, {}),
        std::exchange(tab.pending_selected_name, {}),
        std::exchange(tab.pending_ensure_selection_visible, false), changed};
    if (!changed && !tab.content_results && tab.snapshot && tab.snapshot_path == result_path &&
        !fs::IsVirtualPath(result_path) && tab.selected_index >= 0 &&
        static_cast<size_t>(tab.selected_index) < tab.snapshot->size() &&
        (*tab.snapshot)[static_cast<size_t>(tab.selected_index)].name == restore.focus) {
        restore.previous_snapshot = tab.snapshot;
        restore.previous_focus = tab.selected_index;
    }
    if (captured_identity) {
        restore.keys = std::move(tab.pending_selection_keys);
        restore.focus_key = std::move(tab.pending_selection_focus_key);
    }
    tab.pending_selection_keys.clear();
    tab.pending_selection_key_names.clear();
    tab.pending_selection_focus_key.clear();
    tab.pending_selection_key_focus_name.clear();
    tab.pending_selection_key_revision = UINT64_MAX;
    tab.pending_selection_revision = UINT64_MAX;
    return restore;
}

inline void RestoreListingSelection(Tab& tab, const ListingSelectionRestore& restore,
                                    const Pane* pane = nullptr, const PlacesCatalog* places = nullptr) {
    if (!restore.keys.empty()) {
        const std::unordered_set<std::wstring> wanted(restore.keys.begin(), restore.keys.end());
        std::vector<int> indices;
        int focus = -1;
        for (int i = 0; i < static_cast<int>(tab.EntryCount()); ++i) {
            if (!tab.EntryVisible(i)) continue;
            const auto key = ListingSelectionKey(tab.EntryAt(static_cast<size_t>(i)));
            if (!wanted.contains(key)) continue;
            indices.push_back(i);
            if (key == restore.focus_key) focus = i;
        }
        tab.SelectIndices(indices);
        if (focus >= 0) tab.selected_index = tab.selection_anchor = focus;
        return;
    }
    const auto& names = restore.names;
    const auto& focus = restore.focus;
    if (restore.user_changed) {
        const std::unordered_set<std::wstring> wanted(names.begin(), names.end());
        bool survives = false;
        for (size_t i = 0; i < tab.EntryCount(); ++i) {
            if (!tab.EntryVisible(static_cast<int>(i))) continue;
            const auto name = tab.EntryAt(i).name;
            if (wanted.contains(name)) {
                survives = true;
                break;
            }
        }
        if (!survives) { tab.ClearSelection(); return; }
    }
    if (!names.empty()) {
        tab.RemapSelection(names, focus, false);
        if (tab.SelectedCount() > 0) return;
        if (restore.previous_snapshot && restore.previous_focus >= 0 && tab.snapshot) {
            // Keep the nearest surviving identity in the old displayed order,
            // rather than an index shifted by a multi-file deletion.
            std::vector<int> visible;
            if (pane) {
                ui::PaneViewModel view;
                FillPaneViewModel(view, *pane, places);
                const auto* groups = view.Groups();
                size_t group = 0;
                for (int row = 0; row < static_cast<int>(view.EntryCount()); ++row) {
                    if (groups) {
                        while (group < groups->size() && row >= (*groups)[group].first + (*groups)[group].count) ++group;
                        if (group < groups->size() && (*groups)[group].collapsed) continue;
                    }
                    visible.push_back(view.SourceIndex(row));
                }
            } else {
                CollectFilterMatches(tab, places, visible);
            }
            std::unordered_map<std::wstring_view, int> surviving;
            surviving.reserve(visible.size());
            for (int index : visible) {
                if (tab.EntryVisible(index) && static_cast<size_t>(index) < tab.snapshot->size())
                    surviving.emplace((*tab.snapshot)[static_cast<size_t>(index)].name, index);
            }
            const auto select = [&](int old_index) {
                const auto found = surviving.find((*restore.previous_snapshot)[static_cast<size_t>(old_index)].name);
                if (found == surviving.end()) return false;
                tab.SelectOnly(found->second);
                return true;
            };
            // previous_focus can name a tree child row, which sits past the end
            // of the snapshot; only snapshot rows carry an old identity.
            const int previous_size = static_cast<int>(restore.previous_snapshot->size());
            const int start = (std::min)(restore.previous_focus, previous_size);
            for (int i = start + 1; i < previous_size; ++i)
                if (select(i)) return;
            for (int i = start - 1; i >= 0; --i)
                if (select(i)) return;
            if (!visible.empty()) tab.SelectOnly(visible.front());
            return;
        }
        if (tab.EntryCount() != 0) tab.FocusFirstEntry();
    } else if (tab.snapshot && tab.EntryCount() != 0) tab.FocusFirstEntry();
    else tab.ClearSelection();
}
} // namespace pulse::app
