#include "name_pattern.h"
// app_model.cpp
#include "app_model.h"
#include "git_status.h"
#include "cloud_roots.h"
#include "../fs/device_source.h"
#include "folder_alias.h"
#include "selection_caps.h"
#include "split_tree_shape.h"
#include "layout_pane_selection.h"
#include "network_sidebar.h"
#include "pane_banner.h"
#include "../common/config_json.h"
#include "search_query.h"
#include "entry_group.h"
#include "tag_query.h"
#include "../common/json_utils.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include "../common/display_path.h"
#include "../common/text_format.h"
#include "../fs/drive_label.h"
#include "../fs/drive_kind.h"
#include "../ui/drive_capacity.h"
#include "../ops/clipboard.h"
#include "../ops/source_listing.h"
#include <commctrl.h>
#include <prsht.h>
#include <shlobj.h>
// GetUserNameExW (account display name for the OneDrive rows) lives in secur32.
#define SECURITY_WIN32 1
#include <security.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <unordered_set>
#include <utility>

namespace pulse::app {

std::wstring NavigationReturnChildName(const std::wstring& from_path,
                                       const std::wstring& destination_path) {
    if (from_path.empty() || destination_path.empty() ||
        fs::IsVirtualPath(from_path) || fs::IsVirtualPath(destination_path)) {
        return {};
    }

    const std::wstring from = fs::NormalizePath(from_path);
    const std::wstring destination = fs::NormalizePath(destination_path);
    if (_wcsicmp(from.c_str(), destination.c_str()) == 0) return {};

    std::wstring child = from;
    for (;;) {
        const std::wstring parent = fs::ParentPath(child);
        if (_wcsicmp(parent.c_str(), child.c_str()) == 0) return {};
        if (_wcsicmp(parent.c_str(), destination.c_str()) == 0) {
            const size_t separator = child.find_last_of(L"\\/");
            return separator == std::wstring::npos ? child : child.substr(separator + 1);
        }
        child = parent;
    }
}

// ---------------------------------------------------------------------------
// Tab / Pane / SplitContainer
// ---------------------------------------------------------------------------
namespace {
bool IsSearchPath(const std::wstring& path) {
    std::wstring kind;
    return ParsePulsePath(path, &kind, nullptr) && kind == L"search";
}

std::optional<std::wstring> CurrentSearchOrigin(const Tab& tab) {
    return IsSearchPath(tab.current_path) && tab.search_origin_valid
        ? std::optional<std::wstring>(tab.search_origin_path) : std::nullopt;
}

void AlignHistoryOrigins(const std::stack<std::wstring>& paths,
                         std::stack<std::optional<std::wstring>>& origins) {
    if (origins.size() == paths.size()) return;
    // Restored/older history can have paths without source metadata.
    origins = {};
    for (size_t i = 0; i < paths.size(); ++i) origins.push(std::nullopt);
}

void RestoreHistoryOrigin(Tab& tab, const std::optional<std::wstring>& origin) {
    tab.search_origin_valid = IsSearchPath(tab.current_path) && origin.has_value();
    tab.search_origin_path = tab.search_origin_valid ? *origin : std::wstring{};
}
}

void Tab::NavigateTo(const std::wstring& path) {
    ++view_generation;
    // This PC is the empty path, yet still a place to come back to; only
    // reopening This PC from This PC adds no history entry.
    if (!current_path.empty() || !path.empty()) {
        AlignHistoryOrigins(back_stack, back_search_origins);
        back_stack.push(current_path);
        back_search_origins.push(CurrentSearchOrigin(*this));
    }
    forward_stack = {};
    forward_search_origins = {};
    if (IsSearchPath(path) && !IsSearchPath(current_path)) {
        search_origin_path = current_path;
        search_origin_valid = true;
    } else if (!IsSearchPath(path)) {
        search_origin_path.clear();
        search_origin_valid = false;
    }
    current_path = path;
    ClearSelection();
    scroll_y = 0.0f;
    scroll_x = 0.0f;
    filter_text.clear();
    virtual_title.clear();
    virtual_header_note.clear();
    banner_title.clear();
    banner_message.clear();
    net_readonly = false;
    git_root.clear();
    cache_unix = 0;
    loading = true;
}

fs::DirEntry Tab::EntryAt(size_t index) const {
    if(content_results) {
        if(const auto it=content_action_rows.find(index); it!=content_action_rows.end()) return it->second.entry;
        index::ContentResultStore::Row row;
        if(content_results->Get(index,row)) return row.entry;
        fs::DirEntry pending; pending.change_record_only=true; return pending;
    }
    if (const auto* combined = TreeEntries()) return index < combined->size() ? (*combined)[index] : fs::DirEntry{};
    return snapshot && index<snapshot->size() ? (*snapshot)[index] : fs::DirEntry{};
}

void Tab::StashTreeSelection() {
    const auto* combined = TreeEntries();
    if (!combined || all_selected) return;
    DetailsTree& t = *tree;
    const auto child = [&](int index) {
        return index >= 0 && static_cast<size_t>(index) >= t.base_size &&
               static_cast<size_t>(index) < combined->size();
    };
    bool moved = false;
    for (auto it = selected.begin(); it != selected.end();) {
        if (!child(*it)) { ++it; continue; }
        if (!t.IsPlaceholder(static_cast<size_t>(*it)))
            t.stash_selected.push_back((*combined)[static_cast<size_t>(*it)].full_path);
        it = selected.erase(it);
        moved = true;
    }
    if (child(selected_index)) {
        if (!t.IsPlaceholder(static_cast<size_t>(selected_index)))
            t.stash_focus = (*combined)[static_cast<size_t>(selected_index)].full_path;
        selected_index = selected.empty() ? -1 : *selected.begin();
        moved = true;
    }
    if (child(selection_anchor)) selection_anchor = selected_index;
    if (moved) ++selection_revision;
}

void Tab::SetSnapshot(fs::SnapshotPtr value) {
    // Child rows sit past the end of the old snapshot; keep them by path.
    if (value != snapshot) StashTreeSelection();
    snapshot = std::move(value);
    view_cache_snapshot.reset();
    view_cache_places = nullptr;
    view_cache_places_revision = 0;
    view_cache_filter_text.clear();
    view_filter_map.reset();
    view_tag_dots.reset();
    view_row_cache.reset();
    view_groups.reset();
    view_groups_snapshot.reset();
    view_groups_filter.reset();
    view_compare_base.reset();
    view_compare_marks.reset();
    view_compare_map.reset();
    snapshot_path = snapshot ? current_path : L"";
    if(content_results) { directory_count=0; file_count=content_results->Count(); return; }
    if (all_selected && !ShowsEveryEntry()) MaterializeSelection();
    std::erase_if(selected, [&](int i) { return !EntryVisible(i); });
    if (selected_index >= 0 && !EntryVisible(selected_index)) {
        selected_index = selected.empty() ? -1 : *selected.begin();
        selection_anchor = selected_index;
    }
    directory_count = 0;
    file_count = 0;
    if (!snapshot) return;

    for (const auto& entry : *snapshot) {
        if (!AllowsAttributes(entry.attrs)) continue;
        if (entry.is_dir) ++directory_count;
        else ++file_count;
    }
}

bool Tab::EntryVisible(int index) const {
    if(index < 0 || static_cast<size_t>(index) >= EntryCount()) return false;
    if(content_results) return true;
    if (const auto* combined = TreeEntries()) {
        // Only the "more" row takes focus (Enter shows more); loading,
        // empty and failed rows are skipped.
        if (tree->IsPlaceholder(static_cast<size_t>(index))) return tree->IsMoreRow(static_cast<size_t>(index));
        return AllowsAttributes((*combined)[static_cast<size_t>(index)].attrs);
    }
    return AllowsAttributes((*snapshot)[static_cast<size_t>(index)].attrs);
}

bool Tab::AllowsAttributes(DWORD attrs) const {
    // Recycle Bin lists payloads whose hidden attributes belong to Windows.
    if (current_path.starts_with(L"pulse:recycle")) return true;
    if ((attrs & FILE_ATTRIBUTE_HIDDEN) == 0) return true; // system-only entries stay visible
    if (!show_hidden_files) return false;
    if ((attrs & FILE_ATTRIBUTE_SYSTEM) != 0) return show_protected_os_files;
    return true;
}

void Tab::SetShowHiddenFiles(bool show) {
    if (show_hidden_files == show) return;
    show_hidden_files = show;
    ClearSelection();
    SetSnapshot(snapshot);
    scroll_y = 0.0f;
    ++view_generation;
}

void Tab::SetShowProtectedOsFiles(bool show) {
    if (show_protected_os_files == show) return;
    show_protected_os_files = show;
    ClearSelection();
    SetSnapshot(snapshot);
    scroll_y = 0.0f;
    ++view_generation;
}

void Tab::RememberContentSelection() {
    if (!content_results) return;
    content_selected_paths.clear();
    // Keep selection capture bounded by the display cache; unresolved selections
    // are cancelled on a later re-order rather than naming different files.
    if (SelectedCount() > index::ContentResultStore::kCachePages * index::ContentResultStore::kPageSize) return;
    const auto order = content_results->OrderRevision();
    if (order != content_order_revision) return;
    for (int selected_row : SelectedIndices()) {
        index::ContentResultStore::Row row;
        if (!content_results->Get(static_cast<size_t>(selected_row), row)) { content_selected_paths.clear(); return; }
        content_selected_paths.push_back(row.entry.full_path);
        if (selected_row == selected_index) content_focus_path = row.entry.full_path;
    }
    if (order != content_results->OrderRevision()) content_selected_paths.clear();
}
namespace {
struct RememberSelection {
    Tab& tab;
    ~RememberSelection() { tab.RememberContentSelection(); }
};
}

void Tab::ClearSelection() {
    RememberSelection remember{*this};
    ++selection_revision;
    selected_index = -1;
    selection_anchor = -1;
    all_selected = false;
    selected.clear();
}

int Tab::CountBound() const {
    if (search_retaining_results) return 0;
    return static_cast<int>(std::min(EntryCount(),static_cast<size_t>(INT_MAX)));
}

void Tab::MaterializeSelection() {
    if (!all_selected) return;
    all_selected = false;
    selected.clear();
    const int n = CountBound();
    selected.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) if (EntryVisible(i)) selected.insert(i);
}

void Tab::FocusFirstEntry() {
    ClearSelection();
    focus_hint_revision = selection_revision;
}

void Tab::SelectOnly(int index) {
    RememberSelection remember{*this};
    ++selection_revision;
    selected.clear();
    all_selected = false;
    const int n = CountBound();
    if (index < 0 || index >= n || !EntryVisible(index)) {
        selected_index = -1;
        selection_anchor = -1;
        return;
    }
    selected.insert(index);
    selected_index = index;
    selection_anchor = index;
}

void Tab::ToggleSelect(int index) {
    RememberSelection remember{*this};
    ++selection_revision;
    const int n = CountBound();
    if (index < 0 || index >= n || !EntryVisible(index)) return;
    MaterializeSelection();
    if (selected.contains(index)) {
        selected.erase(index);
        if (selected_index == index)
            selected_index = selected.empty() ? -1 : *selected.begin();
    } else {
        selected.insert(index);
        selected_index = index;
    }
    // As in File Explorer, the toggled item becomes the anchor for a later
    // Shift-click and keeps keyboard focus even when it was deselected.
    selection_anchor = index;
    SetCaret(index);
}

void Tab::SelectRange(int from, int to, const ui::PaneViewModel* view) {
    RememberSelection remember{*this};
    ++selection_revision;
    const int n = CountBound();
    if (n <= 0) {
        ClearSelection();
        return;
    }
    from = std::clamp(from, 0, n - 1);
    to = std::clamp(to, 0, n - 1);
    all_selected = false;
    selected.clear();
    int first = view ? view->ViewIndex(from) : from;
    const int last = view ? view->ViewIndex(to) : to;
    if (last < 0 || (view && static_cast<size_t>(last) >= view->EntryCount())) {
        ClearSelection();
        return;
    }
    // A filter or collapsed group can remove the old anchor. Start a new
    // range at the target instead of widening it across invisible entries.
    const auto* groups = view ? view->Groups() : nullptr;
    const auto collapsed = [&](int row) {
        if (groups) for (const auto& group : *groups) {
            if (row >= group.first && row < group.first + group.count) return group.collapsed;
        }
        return false;
    };
    if (collapsed(last)) {
        ClearSelection();
        return;
    }
    if (first < 0 || collapsed(first)) {
        first = last;
        from = to;
        selection_anchor = from;
    }
    const int lo = std::min(first, last);
    const int hi = std::max(first, last);
    selected.reserve(static_cast<size_t>(hi - lo + 1));
    size_t group_index = 0;
    for (int row = lo; row <= hi; ++row) {
        if (groups) {
            while (group_index < groups->size() &&
                   row >= (*groups)[group_index].first + (*groups)[group_index].count) ++group_index;
            if (group_index < groups->size() && (*groups)[group_index].collapsed) continue;
        }
        const int source = view ? view->SourceIndex(row) : row;
        if (EntryVisible(source)) selected.insert(source);
    }
    selected_index = selected.contains(to) ? to : -1;
    if (selection_anchor < 0) selection_anchor = from;
}

void Tab::SelectAll() {
    RememberSelection remember{*this};
    ++selection_revision;
    // Tree placeholders never count as selected, so list the real rows.
    if (!content_results && (!ShowsEveryEntry() || TreeEntries())) {
        std::vector<int> visible;
        for (int i = 0; i < CountBound(); ++i)
            if (EntryVisible(i) && !(TreeEntries() && tree->IsPlaceholder(static_cast<size_t>(i)))) visible.push_back(i);
        SelectIndices(visible);
        return;
    }
    const int n = CountBound();
    if (n <= 0) {
        ClearSelection();
        return;
    }
    selected.clear();
    all_selected = true;
    if (selected_index < 0 || selected_index >= n) selected_index = 0;
    if (selection_anchor < 0) selection_anchor = selected_index;
}

void Tab::SelectIndices(const std::vector<int>& indices) {
    RememberSelection remember{*this};
    ++selection_revision;
    const int n = CountBound();
    if (n <= 0 || indices.empty()) {
        ClearSelection();
        return;
    }
    selected.clear();
    all_selected = false;
    selected.reserve(indices.size());
    int focus = -1;
    for (int index : indices) {
        if (index < 0 || index >= n || !EntryVisible(index)) continue;
        // Details tree "more" rows take focus alone, never in a group.
        if (TreeEntries() && tree->IsPlaceholder(static_cast<size_t>(index))) continue;
        if (selected.insert(index).second && focus < 0) focus = index;
    }
    if (selected.empty()) {
        selected_index = -1;
        selection_anchor = -1;
        return;
    }
    if (static_cast<int>(selected.size()) == n) {
        all_selected = true;
        selected.clear();
    }
    selected_index = focus;
    selection_anchor = focus;
}

void Tab::InvertIndices(const std::vector<int>& universe) {
    RememberSelection remember{*this};
    ++selection_revision;
    const int n = CountBound();
    if (n <= 0) {
        ClearSelection();
        return;
    }
    if (universe.empty()) return;
    MaterializeSelection();
    std::unordered_set<int> uni;
    uni.reserve(universe.size());
    for (int index : universe) {
        if (index >= 0 && index < n && EntryVisible(index) &&
            !(TreeEntries() && tree->IsPlaceholder(static_cast<size_t>(index)))) uni.insert(index);
    }
    if (uni.empty()) return;
    std::unordered_set<int> next;
    next.reserve(selected.size() + uni.size());
    for (int index : selected) {
        if (!uni.contains(index)) next.insert(index);
    }
    for (int index : uni) {
        if (!selected.contains(index)) next.insert(index);
    }
    selected = std::move(next);
    all_selected = static_cast<int>(selected.size()) == n;
    if (all_selected) selected.clear();
    if (all_selected) {
        if (selected_index < 0 || selected_index >= n) selected_index = 0;
        if (selection_anchor < 0) selection_anchor = selected_index;
        return;
    }
    if (selected.empty()) {
        selected_index = -1;
        selection_anchor = -1;
        return;
    }
    if (selected_index < 0 || !IsSelected(selected_index)) {
        selected_index = *selected.begin();
        selection_anchor = selected_index;
    }
}

void Tab::AddRange(int from, int to, const ui::PaneViewModel* view) {
    RememberSelection remember{*this};
    const int n = CountBound();
    if (n <= 0) {
        ClearSelection();
        return;
    }
    MaterializeSelection();
    std::unordered_set<int> base = std::move(selected);
    SelectRange(from, to, view);
    for (int index : base) selected.insert(index);
    if (static_cast<int>(selected.size()) == n) {
        all_selected = true;
        selected.clear();
    }
    to = std::clamp(to, 0, n - 1);
    if (IsSelected(to)) selected_index = to;
    else if (selected_index < 0 && !selected.empty()) selected_index = *selected.begin();
}

void Tab::MoveFocus(int index, bool extend, const ui::PaneViewModel* view) {
    const int n = CountBound();
    if (n <= 0) {
        ClearSelection();
        return;
    }
    index = std::clamp(index, 0, n - 1);
    if (extend) {
        if (selection_anchor < 0)
            selection_anchor = selected_index >= 0 ? selected_index : index;
        SelectRange(selection_anchor, index, view);
    } else {
        SelectOnly(index);
    }
}

bool Tab::IsSelected(int index) const {
    if (index < 0 || index >= CountBound() || !EntryVisible(index)) return false;
    if (all_selected) return true;
    return selected.contains(index);
}

int Tab::SelectedCount() const {
    if (all_selected) return CountBound();
    return static_cast<int>(selected.size());
}

std::vector<int> Tab::SelectedIndices() const {
    const int n = CountBound();
    std::vector<int> out;
    if (all_selected) {
        out.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) if (EntryVisible(i)) out.push_back(i);
        return out;
    }
    out.assign(selected.begin(), selected.end());
    std::sort(out.begin(), out.end());
    return out;
}

void Tab::SelectionSizeSummary(uint64_t* bytes, int* files, int* folders) const {
    auto& c = selection_size_cache;
    const int count = SelectedCount();
    const size_t entries = EntryCount();
    const auto* tree_entries = TreeEntries();
    const std::vector<fs::DirEntry>* list = tree_entries ? tree_entries : snapshot.get();
    if (c.revision != selection_revision || c.snapshot != list ||
        c.entry_count != entries || c.selected_count != count) {
        c.revision = selection_revision;
        c.snapshot = list;
        c.entry_count = entries;
        c.selected_count = count;
        c.bytes = 0;
        c.files = 0;
        c.folders = 0;
        if (list && !content_results) {
            const auto add = [&](int index) {
                if (index < 0 || static_cast<size_t>(index) >= list->size()) return;
                const fs::DirEntry& entry = (*list)[static_cast<size_t>(index)];
                if (entry.change_record_only) return;
                if (entry.is_dir) ++c.folders;
                else { ++c.files; c.bytes += entry.size; }
            };
            if (all_selected) {
                for (int i = 0; i < CountBound(); ++i) if (EntryVisible(i)) add(i);
            } else {
                for (int index : selected) add(index);
            }
        }
    }
    if (bytes) *bytes = c.bytes;
    if (files) *files = c.files;
    if (folders) *folders = c.folders;
}

void Tab::RemapSelection(const std::vector<std::wstring>& names, const std::wstring& focus_name,
                         bool select_first_if_missing) {
    ClearSelection();
    const int n = CountBound();
    if (n <= 0 || names.empty()) {
        if (n > 0 && select_first_if_missing) SelectOnly(0);
        return;
    }
    std::unordered_set<std::wstring> want(names.begin(), names.end());
    for (int i = 0; i < n; ++i) {
        if (!EntryVisible(i) || !want.contains(EntryAt(static_cast<size_t>(i)).name)) continue;
        selected.insert(i);
        if (EntryAt(static_cast<size_t>(i)).name == focus_name) selected_index = i;
    }
    if (selected.empty()) {
        if (select_first_if_missing) SelectOnly(0);
        return;
    }
    if (selected_index < 0) selected_index = *selected.begin();
    selection_anchor = selected_index;
    if (static_cast<int>(selected.size()) == n) {
        all_selected = true;
        selected.clear();
    }
}

std::wstring Tab::GoBack() {
    if (back_stack.empty()) return current_path;
    AlignHistoryOrigins(back_stack, back_search_origins);
    AlignHistoryOrigins(forward_stack, forward_search_origins);
    forward_stack.push(current_path);
    forward_search_origins.push(CurrentSearchOrigin(*this));
    ++view_generation;
    current_path = back_stack.top();
    RestoreHistoryOrigin(*this, back_search_origins.top());
    back_stack.pop();
    back_search_origins.pop();
    ClearSelection();
    scroll_y = 0.0f;
    scroll_x = 0.0f;
    filter_text.clear();
    virtual_title.clear();
    virtual_header_note.clear();
    loading = true;
    return current_path;
}

std::wstring Tab::GoForward() {
    if (forward_stack.empty()) return current_path;
    AlignHistoryOrigins(forward_stack, forward_search_origins);
    AlignHistoryOrigins(back_stack, back_search_origins);
    back_stack.push(current_path);
    back_search_origins.push(CurrentSearchOrigin(*this));
    ++view_generation;
    current_path = forward_stack.top();
    RestoreHistoryOrigin(*this, forward_search_origins.top());
    forward_stack.pop();
    forward_search_origins.pop();
    ClearSelection();
    scroll_y = 0.0f;
    scroll_x = 0.0f;
    filter_text.clear();
    virtual_title.clear();
    virtual_header_note.clear();
    loading = true;
    return current_path;
}

std::wstring Tab::GoUp() {
    std::wstring parent = fs::ParentPath(current_path);
    if (parent == current_path) return current_path;
    NavigateTo(parent);
    return current_path;
}

void Pane::NewTab(const std::wstring& path) {
    const ui::ViewMode mode = view.view_mode;
    const auto columns = view.details_column_dividers;
    const auto search_columns = view.search_column_dividers;
    const auto media_columns = view.media_column_widths;
    const bool column_layout = view.column_layout;
    auto column_widths = std::move(view.column_widths_dip);
    view = Tab{};
    view.view_mode = mode;
    view.details_column_dividers = columns;
    view.search_column_dividers = search_columns;
    view.media_column_widths = media_columns;
    view.column_layout = column_layout;
    view.column_widths_dip = std::move(column_widths);
    view.current_path = fs::NormalizePath(path);
    view.loading = true;
}

Pane* LayoutTab::FocusedPane() {
    if (panes.empty()) return nullptr;
    size_t i = focused_index < 0 ? 0 : static_cast<size_t>(focused_index);
    if (i >= panes.size()) i = panes.size() - 1;
    return panes[i].get();
}

const Pane* LayoutTab::FocusedPane() const {
    if (panes.empty()) return nullptr;
    size_t i = focused_index < 0 ? 0 : static_cast<size_t>(focused_index);
    if (i >= panes.size()) i = panes.size() - 1;
    return panes[i].get();
}

Tab* LayoutTab::ActiveFolder() {
    Pane* pane = FocusedPane();
    return pane ? pane->ActiveTab() : nullptr;
}

const Tab* LayoutTab::ActiveFolder() const {
    const Pane* pane = FocusedPane();
    return pane ? pane->ActiveTab() : nullptr;
}

std::unique_ptr<LayoutTab> MakeSingleLayoutTab(const std::wstring& path, const Tab* source) {
    auto tab = std::make_unique<LayoutTab>();
    auto pane = std::make_unique<Pane>();
    pane->focused = true;
    if (source) {
        pane->view.view_mode = source->view_mode;
        pane->view.details_column_dividers = source->details_column_dividers;
        pane->view.media_column_widths = source->media_column_widths;
        pane->view.search_column_dividers = source->search_column_dividers;
        pane->view.column_layout = source->column_layout;
        pane->view.column_widths_dip = source->column_widths_dip;
    }
    pane->NewTab(path.empty() ? L"C:\\" : path);
    tab->panes.push_back(std::move(pane));
    tab->root = SplitContainer::CreateLeaf(tab->panes[0].get());
    tab->layout = LayoutPreset::Single;
    tab->focused_index = 0;
    return tab;
}

void RebuildLayoutRoot(LayoutTab& tab) {
    if (tab.layout == LayoutPreset::Custom) {
        // A free tree has no preset to be rebuilt from (M7k): it stays while
        // every leaf is a pane of the tab, else the tab takes a preset.
        std::vector<Pane*> leaves;
        if (tab.root) tab.root->CollectPanes(leaves);
        const bool owned = !leaves.empty() && std::all_of(leaves.begin(), leaves.end(), [&](const Pane* leaf) {
            return std::any_of(tab.panes.begin(), tab.panes.end(),
                               [&](const std::unique_ptr<Pane>& pane) { return pane.get() == leaf; });
        });
        if (owned) return;
        tab.layout = PresetForPaneCount(tab.panes.size());
    }
    const size_t n = LayoutPresetCount(tab.layout);
    std::vector<Pane*> visible;
    if (tab.root) tab.root->CollectPanes(visible);
    const auto used = SelectLayoutPanes(tab.panes, tab.FocusedPane(), n, visible);
    tab.root = used.empty() ? nullptr : MakePresetTree(tab.layout, used);
}

void WindowTabs::EnsureDefault() {
    if (!items.empty()) return;
    auto tab = std::make_unique<LayoutTab>();
    auto pane = std::make_unique<Pane>();
    pane->focused = true;
    tab->panes.push_back(std::move(pane));
    tab->root = SplitContainer::CreateLeaf(tab->panes[0].get());
    items.push_back(std::move(tab));
    active = 0;
}

LayoutTab& WindowTabs::NewTab(const std::wstring& path) {
    return NewTabAt(items.size(), path);
}

LayoutTab& WindowTabs::NewTabAt(size_t index, const std::wstring& path) {
    const Tab* source = Active() ? Active()->ActiveFolder() : nullptr;
    auto tab = MakeSingleLayoutTab(path, source);
    size_t first_unpinned = 0;
    while (first_unpinned < items.size() && items[first_unpinned]->pinned)
        ++first_unpinned;
    index = std::clamp(index, first_unpinned, items.size());
    auto it = items.insert(items.begin() + static_cast<ptrdiff_t>(index), std::move(tab));
    active = index;
    return **it;
}

void WindowTabs::CloseTab(size_t idx) {
    if (idx >= items.size() || items.size() <= 1) return;
    if (const auto& transaction = items[idx]->explorer_transaction) {
        transaction->handoff->Cancel();
        const auto phase = transaction->handoff->state.load();
        if (phase == HandoffState::Closing || phase == HandoffState::Closed) return;
    }
    if (items[idx]->pinned) return;
    items.erase(items.begin() + static_cast<ptrdiff_t>(idx));
    if (active >= items.size()) active = items.size() - 1;
    else if (idx < active) --active;
}

std::unique_ptr<LayoutTab> WindowTabs::TakeTab(size_t idx, bool allow_last) {
    if (idx >= items.size() || (items.size() <= 1 && !allow_last) || !items[idx]) return nullptr;
    if (const auto& transaction = items[idx]->explorer_transaction) {
        transaction->handoff->Cancel();
        const auto phase = transaction->handoff->state.load();
        if (phase == HandoffState::Closing || phase == HandoffState::Closed) return nullptr;
    }
    std::unique_ptr<LayoutTab> tab = std::move(items[idx]);
    items.erase(items.begin() + static_cast<ptrdiff_t>(idx));
    if (items.empty()) active = 0;
    else if (active >= items.size()) active = items.size() - 1;
    else if (idx < active) --active;
    const int group = tab->tab_group;
    tab->tab_group = 0;
    if (group && std::none_of(items.begin(), items.end(),
                              [group](const auto& other) { return other->tab_group == group; })) {
        tab_groups.erase(std::remove_if(tab_groups.begin(), tab_groups.end(),
                                        [group](const TabGroup& g) { return g.id == group; }),
                         tab_groups.end());
    }
    return tab;
}

size_t WindowTabs::InsertTab(std::unique_ptr<LayoutTab> tab, size_t index) {
    if (!tab) return active;
    tab->tab_group = 0;
    size_t first_unpinned = 0;
    while (first_unpinned < items.size() && items[first_unpinned]->pinned) ++first_unpinned;
    index = tab->pinned ? (std::min)(index, first_unpinned)
                        : std::clamp(index, first_unpinned, items.size());
    while (index > 0 && index < items.size() && items[index - 1]->tab_group != 0 &&
           items[index - 1]->tab_group == items[index]->tab_group)
        ++index;
    items.insert(items.begin() + static_cast<ptrdiff_t>(index), std::move(tab));
    active = index;
    return index;
}

void WindowTabs::SwitchTab(size_t idx) {
    if (idx < items.size()) active = idx;
}

void WindowTabs::MoveTab(size_t from, size_t to) {
    if (from >= items.size() || to >= items.size() || from == to) return;

    // TogglePin changes the moved tab's flag before moving it to its new region.
    // Count the other pins so that transition and ordinary dragging share a boundary.
    size_t other_pins = 0;
    for (size_t i = 0; i < items.size(); ++i)
        if (i != from && items[i]->pinned) ++other_pins;
    to = items[from]->pinned ? (std::min)(to, other_pins) : (std::max)(to, other_pins);
    if (from == to) return;

    auto moved = std::move(items[from]);
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(from));
    items.insert(items.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));

    if (active == from) {
        active = to;
    } else if (from < active && active <= to) {
        --active;
    } else if (to <= active && active < from) {
        ++active;
    }
}

std::unique_ptr<SplitContainer> SplitContainer::CreateLeaf(Pane* pane) {
    auto node = std::make_unique<SplitContainer>();
    node->is_leaf = true;
    node->pane = pane;
    return node;
}

std::unique_ptr<SplitContainer> SplitContainer::Join(SplitOrientation orient, float ratio,
                                                     std::unique_ptr<SplitContainer> first,
                                                     std::unique_ptr<SplitContainer> second) {
    auto parent = std::make_unique<SplitContainer>();
    parent->is_leaf = false;
    parent->orientation = orient;
    parent->ratio = ratio;
    parent->first = std::move(first);
    parent->second = std::move(second);
    return parent;
}

void SplitContainer::CollectPanes(std::vector<Pane*>& out) const {
    if (is_leaf) {
        if (pane) out.push_back(pane);
        return;
    }
    if (first) first->CollectPanes(out);
    if (second) second->CollectPanes(out);
}

size_t LayoutPresetCount(LayoutPreset preset) {
    switch (preset) {
    case LayoutPreset::Single: return 1;
    case LayoutPreset::TwoVertical:
    case LayoutPreset::TwoHorizontal: return 2;
    case LayoutPreset::Three: return 3;
    case LayoutPreset::FourGrid: return 4;
    case LayoutPreset::Custom: return 0;  // its tree's leaves
    }
    return 1;
}

std::unique_ptr<SplitContainer> MakePresetTree(LayoutPreset preset,
                                               const std::vector<Pane*>& panes) {
    auto leaf = [](Pane* p) { return SplitContainer::CreateLeaf(p); };
    const size_t n = LayoutPresetCount(preset);
    if (panes.size() < n || panes.empty()) return leaf(panes.empty() ? nullptr : panes[0]);
    switch (preset) {
    case LayoutPreset::TwoVertical:
        return SplitContainer::Join(SplitOrientation::Vertical, 0.5f,
                                    leaf(panes[0]), leaf(panes[1]));
    case LayoutPreset::TwoHorizontal:
        return SplitContainer::Join(SplitOrientation::Horizontal, 0.5f,
                                    leaf(panes[0]), leaf(panes[1]));
    case LayoutPreset::Three:
        return SplitContainer::Join(SplitOrientation::Vertical, 0.5f, leaf(panes[0]),
            SplitContainer::Join(SplitOrientation::Horizontal, 0.5f,
                                 leaf(panes[1]), leaf(panes[2])));
    case LayoutPreset::FourGrid:
        return SplitContainer::Join(SplitOrientation::Horizontal, 0.5f,
            SplitContainer::Join(SplitOrientation::Vertical, 0.5f, leaf(panes[0]), leaf(panes[1])),
            SplitContainer::Join(SplitOrientation::Vertical, 0.5f, leaf(panes[2]), leaf(panes[3])));
    case LayoutPreset::Single:
    default:
        return leaf(panes[0]);
    }
}

void ApplySplitRatio(SplitContainer& node, const D2D1_RECT_F& parent_bounds, float gap,
                     float pointer_x, float pointer_y) {
    const float g = std::max(0.0f, gap);
    const bool vertical = node.orientation == SplitOrientation::Vertical;
    const float span = vertical
        ? std::max(1.0f, parent_bounds.right - parent_bounds.left - g)
        : std::max(1.0f, parent_bounds.bottom - parent_bounds.top - g);
    const float origin = vertical ? parent_bounds.left : parent_bounds.top;
    const float pointer = vertical ? pointer_x : pointer_y;
    node.ratio = ClampSplitRatio((pointer - origin - g * 0.5f) / span,
                                 parent_bounds, node.orientation, gap);
}

void CollectSplitRatios(const SplitContainer& node, std::vector<float>& out) {
    if (node.is_leaf) return;
    out.push_back(node.ratio);
    if (node.first) CollectSplitRatios(*node.first, out);
    if (node.second) CollectSplitRatios(*node.second, out);
}

static void ApplySplitRatiosAt(SplitContainer& node, const std::vector<float>& ratios,
                               size_t& index) {
    if (node.is_leaf) return;
    if (index < ratios.size()) node.ratio = ratios[index++];
    if (node.first) ApplySplitRatiosAt(*node.first, ratios, index);
    if (node.second) ApplySplitRatiosAt(*node.second, ratios, index);
}

void ApplySplitRatios(SplitContainer& node, const std::vector<float>& ratios) {
    size_t index = 0;
    ApplySplitRatiosAt(node, ratios, index);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::wstring ParentDir(std::wstring path) {
    while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    const auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos || pos < 2) return {};
    return path.substr(0, pos);
}

static std::wstring FindGitRootImpl(std::wstring path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
    else if (path.starts_with(L"\\\\?\\")) path = path.substr(4);
    for (int i = 0; i < 12 && !path.empty(); ++i) {
        if (pulse::path::Exists(path + L"\\.git")) return path;
        std::wstring parent = ParentDir(path);
        if (parent.empty() || parent == path) break;
        path = std::move(parent);
    }
    return {};
}

std::wstring FindGitRoot(const std::wstring& path) {
    return FindGitRootImpl(path);
}

std::wstring TabTitle(const std::wstring& path) {
    if (path.empty()) return l10n::Get(l10n::StringId::ThisPc);
    std::wstring kind, rest;
    if (ParsePulsePath(path, &kind, &rest)) {
        if (kind == L"settings") return l10n::Get(l10n::StringId::Settings);
        if (kind == L"starred") return l10n::Get(l10n::StringId::StarredItems);
        if (kind == L"recent") return l10n::Get(l10n::StringId::Recent);
        if (kind == L"recycle") return l10n::Get(l10n::StringId::RecycleBin);
        if (kind == L"search") return l10n::Get(l10n::StringId::Search);
        if (kind == L"tag") return rest.empty() ? l10n::Get(l10n::StringId::Tag) : rest;
        if (kind == L"tags") return l10n::Get(l10n::StringId::Tag);
    }
    // 别名: tabs, the sidebar and window lists show the folder's alias.
    if (std::wstring alias = AliasStore::Instance().Shown(path); !alias.empty()) return alias;
    std::wstring_view v = path;
    if (v.size() > 1 && v.back() == L'\\') v.remove_suffix(1);
    auto pos = v.find_last_of(L"\\/");
    if (pos != std::wstring_view::npos && pos + 1 < v.size())
        return std::wstring(v.substr(pos + 1));
    return std::wstring(v);
}

static std::wstring DisplayPath(const std::wstring& path) {
    if (path.empty()) return l10n::Get(l10n::StringId::ThisPc);
    // Keep the UNC prefix. Dropping it turns \\server\share into a relative
    // path; breadcrumb clicks then resolve against the process CWD.
    return pulse::path::FriendlyPathText(path);
}

// ---------------------------------------------------------------------------
// StagingTray
// ---------------------------------------------------------------------------
void StagingTray::Collect(const std::vector<std::wstring>& paths, bool move_intent) {
    if (paths.empty()) return;
    TrayBatch batch;
    batch.id = next_id_++;
    batch.move_intent = move_intent;
    batch.total_size = 0;
    std::vector<std::wstring> normalized;
    normalized.reserve(paths.size());
    for (const auto& p : paths) normalized.push_back(fs::NormalizePath(p));
    // Thousands of items from one folder come from one listing of it.
    const ops::SourceListing listing(normalized);
    for (auto& path : normalized) {
        TrayItem it;
        it.id = next_id_++;
        it.path = std::move(path);
        // One probe covers existence, icon attrs and the rough size sum.
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        const ops::ListedSource* listed = listing.Find(it.path);
        if (listed) {
            fad.dwFileAttributes = listed->attributes;
            fad.nFileSizeLow = static_cast<DWORD>(listed->bytes);
            fad.nFileSizeHigh = static_cast<DWORD>(listed->bytes >> 32);
        }
        if (listed || GetFileAttributesExW(it.path.c_str(), GetFileExInfoStandard, &fad)) {
            it.exists = true;
            it.attrs = fad.dwFileAttributes;
            it.is_dir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!it.is_dir) {
                ULARGE_INTEGER sz;
                sz.LowPart = fad.nFileSizeLow;
                sz.HighPart = fad.nFileSizeHigh;
                it.size = sz.QuadPart;
                batch.total_size += it.size;
            }
        } else {
            it.exists = false;
        }
        batch.items.push_back(std::move(it));
    }
    batches_.push_back(std::move(batch));
}

void StagingTray::ReplacePath(const std::wstring& from, const std::wstring& to) {
    const std::wstring key = fs::NormalizePath(from);
    std::wstring target = fs::NormalizePath(to);
    if (key.empty() || target.empty()) return;
    for (auto& batch : batches_) {
        for (auto& item : batch.items) {
            if (item.inflight_task != 0) continue;
            if (_wcsicmp(item.path.c_str(), key.c_str()) == 0) {
                item.path = target;
                continue;
            }
            // Items staged from inside a renamed or moved folder follow it.
            const bool child = item.path.size() > key.size() &&
                _wcsnicmp(item.path.c_str(), key.c_str(), key.size()) == 0 &&
                (key.back() == L'\\' || item.path[key.size()] == L'\\');
            if (!child) continue;
            std::wstring rest = item.path.substr(key.size());
            if (rest.empty() || rest.front() != L'\\') rest.insert(rest.begin(), L'\\');
            std::wstring base = target;
            while (!base.empty() && base.back() == L'\\') base.pop_back();
            item.path = base + rest;
        }
    }
}

void StagingTray::MarkPendingMove(uint64_t task_id, const std::vector<std::wstring>& paths,
                                  std::optional<size_t> batch) {
    std::vector<uint64_t> ids;
    std::unordered_set<std::wstring> wanted;
    wanted.reserve(paths.size());
    for (const auto& path : paths) wanted.insert(pulse::path::FoldedKey(fs::NormalizePath(path)));
    for (size_t b = 0; b < batches_.size(); ++b) {
        if (batch && b != *batch) continue;
        for (const auto& item : batches_[b].items)
            if (wanted.contains(pulse::path::FoldedKey(item.path))) ids.push_back(item.id);
    }
    MarkInFlight(task_id, ids, true);
}
void StagingTray::CompleteMove(uint64_t task_id, const std::vector<std::wstring>& moved) {
    CompleteTask(task_id, moved);
}

void StagingTray::ApplyMetadata(const std::wstring& path, const WIN32_FILE_ATTRIBUTE_DATA& data) {
    for (auto& batch : batches_) {
        for (auto& item : batch.items) {
            if (item.path != path) continue;
            batch.total_size -= std::min(batch.total_size, item.size);
            item.exists = true;
            item.attrs = data.dwFileAttributes;
            item.is_dir = (item.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
            item.size = item.is_dir ? 0 :
                (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            batch.total_size += item.size;
        }
    }
}

bool StagingTray::RefreshExists() {
    bool changed = false;
    for (auto& batch : batches_) {
        for (auto& it : batch.items) {
            // Mapped shares and user-mode cloud mounts can stall just like UNC paths.
            const bool unc = it.path.starts_with(L"\\\\?\\UNC\\") ||
                (it.path.starts_with(L"\\\\") && !it.path.starts_with(L"\\\\?\\"));
            if (unc || fs::IsRemoteLikeDrive(it.path)) continue;
            const bool now = GetFileAttributesW(it.path.c_str()) != INVALID_FILE_ATTRIBUTES;
            if (now != it.exists) {
                it.exists = now;
                changed = true;
            }
        }
    }
    return changed;
}

size_t StagingTray::RemoveMissing() {
    size_t removed = 0;
    for (size_t b = batches_.size(); b-- > 0;) {
        auto& batch = batches_[b];
        for (size_t i = batch.items.size(); i-- > 0;) {
            if (batch.items[i].exists) continue;
            batch.total_size -= std::min(batch.total_size, batch.items[i].size);
            batch.items.erase(batch.items.begin() + static_cast<std::ptrdiff_t>(i));
            ++removed;
        }
        if (batch.items.empty()) batches_.erase(batches_.begin() + static_cast<std::ptrdiff_t>(b));
    }
    return removed;
}

void StagingTray::RemoveBatch(size_t idx) {
    if (idx < batches_.size()) batches_.erase(batches_.begin() + idx);
}

void StagingTray::SetMoveIntent(size_t idx, bool move_intent) {
    if (idx < batches_.size()) batches_[idx].move_intent = move_intent;
}

void StagingTray::RemoveItem(size_t batch_idx, size_t item_idx) {
    if (batch_idx >= batches_.size()) return;
    auto& b = batches_[batch_idx];
    if (item_idx < b.items.size()) {
        b.total_size -= std::min(b.total_size, b.items[item_idx].size);
        b.items.erase(b.items.begin() + item_idx);
    }
    if (b.items.empty()) batches_.erase(batches_.begin() + batch_idx);
}

void StagingTray::Clear() { batches_.clear(); }

void StagingTray::MarkInFlight(uint64_t task_id, const std::vector<uint64_t>& item_ids, bool move) {
    if (!task_id) return;
    const std::unordered_set<uint64_t> wanted(item_ids.begin(), item_ids.end());
    for (auto& batch : batches_)
        for (auto& item : batch.items)
            if (!item.inflight_task && wanted.contains(item.id)) {
                item.inflight_task = task_id;
                item.inflight_move = move;
                item.inflight_source = item.path;
                inflight_sources_[task_id].push_back(item.path);
                ++inflight_keys_[pulse::path::FoldedKey(item.path)];
            }
}

void StagingTray::CompleteTask(uint64_t task_id, const std::vector<std::wstring>& moved_sources) {
    if (!task_id) return;
    if (const auto task = inflight_sources_.find(task_id); task != inflight_sources_.end()) {
        for (const auto& source : task->second) {
            const auto key = inflight_keys_.find(pulse::path::FoldedKey(source));
            if (key != inflight_keys_.end() && !--key->second) inflight_keys_.erase(key);
        }
        inflight_sources_.erase(task);
    }
    std::unordered_set<std::wstring> moved;
    moved.reserve(moved_sources.size());
    for (const auto& source : moved_sources) moved.insert(pulse::path::FoldedKey(fs::NormalizePath(source)));
    for (size_t b = batches_.size(); b-- > 0;) {
        auto& batch = batches_[b];
        // Kept items close up in one pass; erasing each consumed one would
        // shift the rest every time.
        size_t kept = 0;
        for (size_t i = 0; i < batch.items.size(); ++i) {
            auto& item = batch.items[i];
            bool consume = false;
            if (item.inflight_task == task_id) {
                consume = item.inflight_move && moved.contains(pulse::path::FoldedKey(item.inflight_source));
                item.inflight_task = 0;
                item.inflight_move = false;
                item.inflight_source.clear();
                if (consume) batch.total_size -= std::min(batch.total_size, item.size);
            }
            if (consume) continue;
            if (kept != i) batch.items[kept] = std::move(item);
            ++kept;
        }
        batch.items.erase(batch.items.begin() + static_cast<std::ptrdiff_t>(kept), batch.items.end());
        if (batch.items.empty()) batches_.erase(batches_.begin() + static_cast<std::ptrdiff_t>(b));
    }
}

bool StagingTray::IsInFlight(const std::wstring& path) const {
    return !inflight_keys_.empty() && inflight_keys_.contains(pulse::path::FoldedKey(fs::NormalizePath(path)));
}

void StagingTray::RemoveDeleted(const std::vector<std::wstring>& paths) {
    if (batches_.empty()) return;
    std::unordered_set<std::wstring> deleted;
    deleted.reserve(paths.size());
    for (const auto& path : paths)
        if (!path.empty()) deleted.insert(pulse::path::FoldedKey(fs::NormalizePath(path)));
    if (deleted.empty()) return;
    // An item goes when it or a folder above it was deleted: look its own path
    // and each parent up (with and without the separator) instead of
    // comparing every item with every deleted path.
    const auto gone = [&](const std::wstring& path) {
        const std::wstring key = pulse::path::FoldedKey(path);
        if (deleted.contains(key)) return true;
        for (size_t i = key.size(); i-- > 0;) {
            if (key[i] != L'\\') continue;
            if (deleted.contains(key.substr(0, i + 1)) || (i && deleted.contains(key.substr(0, i)))) return true;
        }
        return false;
    };
    for (size_t b = batches_.size(); b-- > 0;) {
        auto& batch = batches_[b];
        size_t kept = 0;
        for (size_t i = 0; i < batch.items.size(); ++i) {
            if (gone(batch.items[i].path)) {
                batch.total_size -= std::min(batch.total_size, batch.items[i].size);
                continue;
            }
            if (kept != i) batch.items[kept] = std::move(batch.items[i]);
            ++kept;
        }
        if (kept == batch.items.size()) continue;
        batch.items.erase(batch.items.begin() + static_cast<std::ptrdiff_t>(kept), batch.items.end());
        if (batch.items.empty()) batches_.erase(batches_.begin() + static_cast<std::ptrdiff_t>(b));
    }
}

void StagingTray::ToJson(std::wstring& out) const {
    out = L"[\n";
    for (size_t i = 0; i < batches_.size(); ++i) {
        const auto& b = batches_[i];
        out += L"  {\"move\":";
        out += b.move_intent ? L"true" : L"false";
        out += L",\"items\":[";
        for (size_t j = 0; j < b.items.size(); ++j) {
            out += L"\"";
            pulse::json::Escape(b.items[j].path, out);
            out += L"\"";
            if (j + 1 < b.items.size()) out += L",";
        }
        out += L"]}";
        if (i + 1 < batches_.size()) out += L",";
        out += L"\n";
    }
    out += L"]";
}

bool StagingTray::FromJson(const std::wstring& in) {
    if (!pulse::json::ValidConfigArray(in)) return false;
    std::vector<TrayBatch> parsed;
    const size_t open = in.find(L'[');
    if (open == std::wstring::npos) return false;
    const size_t close = pulse::json::MatchingClose(in, open);
    if (close == std::wstring::npos) return false;
    const bool ok = pulse::json::ForEachElement(in.substr(open, close - open + 1), [&](const std::wstring& block) {
        if (block.empty() || block.front() != L'{') return;
        TrayBatch batch;
        batch.id = next_id_++;
        batch.move_intent = pulse::json::ExtractBool(block, L"move");
        for (const std::wstring& path : pulse::json::ExtractStringArray(block, L"items")) {
            TrayItem it;
            it.id = next_id_++;
            it.path = fs::NormalizePath(path);
            WIN32_FILE_ATTRIBUTE_DATA data{};
            it.exists = GetFileAttributesExW(it.path.c_str(), GetFileExInfoStandard, &data) != FALSE;
            it.attrs = it.exists ? data.dwFileAttributes : 0;
            it.is_dir = it.exists && (it.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (it.exists && !it.is_dir) {
                it.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                batch.total_size += it.size;
            }
            batch.items.push_back(std::move(it));
        }
        if (!batch.items.empty()) parsed.push_back(std::move(batch));
    });
    if (!ok) return false;
    batches_ = std::move(parsed);
    return true;
}

// ---------------------------------------------------------------------------
// Sidebar model
// ---------------------------------------------------------------------------
static SidebarEntry MakeKnownEntry(REFKNOWNFOLDERID fid, const wchar_t* glyph, const wchar_t* fallback,
                                   D2D1_COLOR_F color, const wchar_t* override_name = nullptr) {
    SidebarEntry e;
    e.glyph = glyph;
    e.fallback = fallback;
    e.color = color;
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(fid, 0, nullptr, &path)) && path) {
        e.path = fs::NormalizePath(path);
        e.label = override_name ? override_name : TabTitle(e.path);
        CoTaskMemFree(path);
    } else {
        e.label = fallback;
    }
    return e;
}

// ---------------------------------------------------------------------------
// OneDrive accounts
// ---------------------------------------------------------------------------
// Explorer lists one row per signed-in account. The client records them under
// HKCU\Software\Microsoft\OneDrive\Accounts ("Personal", "Business1", …) with
// the synced root in "UserFolder"; the shell namespace would be the other
// source, but it is not worth a COM round-trip for a folder path.
constexpr const wchar_t* kOneDriveAccountsKey = L"Software\\Microsoft\\OneDrive\\Accounts";

std::wstring RegStringValue(HKEY root, const std::wstring& subkey, const wchar_t* value) {
    // One buffer big enough for a path: asking RegGetValue for the size first
    // (pvData = nullptr) proved unreliable here.
    std::wstring out(1024, L'\0');
    DWORD bytes = static_cast<DWORD>(out.size() * sizeof(wchar_t));
    if (RegGetValueW(root, subkey.c_str(), value, RRF_RT_REG_SZ, nullptr, out.data(), &bytes)
            != ERROR_SUCCESS || bytes < sizeof(wchar_t))
        return {};
    out.resize(bytes / sizeof(wchar_t));
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

// Explorer names the rows "<account name> - <Personal|tenant>"; the prefix is
// the Windows account's display name.
std::wstring WindowsAccountDisplayName() {
    wchar_t name[256]{};
    ULONG length = ARRAYSIZE(name);
    if (GetUserNameExW(NameDisplay, name, &length) && name[0]) return name;
    length = ARRAYSIZE(name);
    if (GetUserNameW(name, &length) && name[0]) return name;
    return {};
}

std::vector<SidebarEntry> BuildOneDriveEntries() {
    std::vector<SidebarEntry> out;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kOneDriveAccountsKey, 0, KEY_READ, &root)
            != ERROR_SUCCESS)
        return out;
    const std::wstring prefix = WindowsAccountDisplayName();
    std::vector<std::wstring> keys{ L"Personal" };
    for (int i = 1; i <= 16; ++i) keys.push_back(L"Business" + std::to_wstring(i));
    for (const auto& key : keys) {
        // Relative to the already-opened Accounts key.
        const std::wstring folder = RegStringValue(root, key, L"UserFolder");
        if (folder.empty()) continue;
        const std::wstring path = fs::NormalizePath(folder);
        if (path.empty()) continue;
        const bool duplicate = std::any_of(out.begin(), out.end(),
            [&](const SidebarEntry& entry) {
                return SameCloudRootPath(entry.path, path);
            });
        if (duplicate) continue;
        std::wstring suffix = key == L"Personal"
            ? l10n::Get(l10n::StringId::OneDrivePersonal)
            : RegStringValue(root, key, L"DisplayName");
        if (suffix.empty()) suffix = RegStringValue(root, key, L"UserName");
        SidebarEntry entry;
        entry.glyph = L"\xE753";
        entry.fallback = L"1D";
        entry.color = ui::HexColor(0x2B88D8);
        entry.path = path;
        entry.label = prefix.empty() ? TabTitle(path)
            : (suffix.empty() ? prefix : prefix + L" - " + suffix);
        out.push_back(std::move(entry));
    }
    RegCloseKey(root);
    return out;
}

// Other cloud folders Explorer pins to its navigation pane (WPS网盘, 百度网盘
// 同步空间, Dropbox, ...) and Cloud Files sync roots, after the OneDrive rows.
// Runs on the sidebar refresh thread with the rest of the model; a folder that
// no longer exists is left out, the same as Explorer does.
void AppendOtherCloudEntries(std::vector<SidebarEntry>& cloud) {
    CloudRegistry registry = CloudRegistry::System();
    // Isolated test instances only: a private HKCU subtree stands in for the
    // machine's registration, so tests never touch Explorer's own keys.
    if (GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) > 0) {
        wchar_t prefix[512]{};
        const DWORD n = GetEnvironmentVariableW(L"PULSE_TEST_CLOUD_REG", prefix, ARRAYSIZE(prefix));
        if (n > 0 && n < ARRAYSIZE(prefix)) registry = CloudRegistry::Fixture(prefix, L"S-1-5-21-pulse-test");
    }
    std::vector<CloudRoot> roots;
    for (const auto& entry : cloud) roots.push_back({entry.path, entry.label, {}, CloudRootSource::Seed});
    const size_t seeded = roots.size();
    const auto is_directory = [](const std::wstring& path) {
        const DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    };
    auto sync_roots = ReadSyncRootCloudRoots(registry);
    std::vector<std::wstring> cloud_files_roots;
    for (const auto& root : sync_roots) cloud_files_roots.push_back(root.path);
    PublishCloudFilesRoots(std::move(cloud_files_roots));
    MergeCloudRoots(roots, ReadNavigationPaneCloudRoots(registry), is_directory);
    MergeCloudRoots(roots, std::move(sync_roots), is_directory);
    PublishCloudRootLabels(roots);  // 下载并预览 names the provider
    for (size_t i = seeded; i < roots.size(); ++i) {
        SidebarEntry entry;
        entry.glyph = L"\xE753";
        entry.fallback = L"Cloud";
        entry.color = ui::HexColor(CloudRootColor(roots[i]));
        entry.path = fs::NormalizePath(roots[i].path);
        entry.label = roots[i].label;
        // Explorer's navigation pane shows the program's own icon for its
        // CLSID (WPS网盘, 百度网盘); the tinted cloud stays as the fallback.
        if (roots[i].source == CloudRootSource::NavigationPane && !roots[i].id.empty())
            entry.icon_path = L"::" + roots[i].id;
        if (!entry.path.empty()) cloud.push_back(std::move(entry));
    }
}

std::vector<int> DefaultSidebarOrder() {
    // Mirrors Explorer's navigation pane: the OneDrive accounts lead, the starred
    // root follows next to quick access, and the drive list sits near the bottom
    // ("This PC") with network locations below it. Ids, not positions: the masks
    // and menus key off them.
    return { static_cast<int>(SidebarSectionId::Cloud),
             static_cast<int>(SidebarSectionId::Starred),
             static_cast<int>(SidebarSectionId::QuickAccess),
             static_cast<int>(SidebarSectionId::Workspaces),
             static_cast<int>(SidebarSectionId::SavedSearches),
             static_cast<int>(SidebarSectionId::Tags),
             static_cast<int>(SidebarSectionId::Drives),
             static_cast<int>(SidebarSectionId::Networks) };
}

int SidebarSectionIndex(const ui::WindowViewModel& vm, int section_id) {
    for (int i = 0; i < static_cast<int>(vm.sidebar.size()); ++i)
        if (vm.sidebar[static_cast<size_t>(i)].id == section_id) return i;
    return -1;
}

std::vector<int> NormalizeSidebarOrder(const std::vector<int>& order) {
    std::vector<int> out;
    out.reserve(static_cast<size_t>(kSidebarSectionCount));
    std::array<bool, static_cast<size_t>(kSidebarSectionCount)> seen{};
    for (int id : order) {
        if (id < 0 || id >= kSidebarSectionCount) continue;
        if (seen[static_cast<size_t>(id)]) continue;
        seen[static_cast<size_t>(id)] = true;
        out.push_back(id);
    }
    if (out.empty()) return DefaultSidebarOrder();
    for (int id = 0; id < kSidebarSectionCount; ++id) {
        if (!seen[static_cast<size_t>(id)]) out.push_back(id);
    }
    return out;
}

void RefreshSidebarDriveCapacity(std::vector<SidebarEntry>& drives) {
    DWORD old_mode = 0;
    const BOOL changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode);
    for (auto& entry : drives) {
        ULARGE_INTEGER free_bytes{}, total_bytes{};
        entry.detail.clear();
        entry.used_ratio = 0.0f;
        entry.free_bytes = ~uint64_t{0};
        entry.total_bytes = 0;
        entry.danger = false;
        if (!GetDiskFreeSpaceExW(entry.path.c_str(), &free_bytes, &total_bytes, nullptr))
            continue;
        const uint64_t total = total_bytes.QuadPart;
        const uint64_t free = std::min(free_bytes.QuadPart, total_bytes.QuadPart);
        entry.detail = ui::DriveSidebarDetail(free, total);
        if (total > 0) {
            entry.used_ratio = static_cast<float>(static_cast<double>(total - free) / static_cast<double>(total));
            entry.free_bytes = free;
            entry.total_bytes = total;
            entry.danger = ui::CapacityLevelFor(entry.used_ratio, free) == ui::CapacityLevel::Critical;
        }
    }
    if (changed) SetThreadErrorMode(old_mode, nullptr);
}

SidebarModel BuildSidebarModel(const fs::RecycleBinInfo* recycle) {
    SidebarModel m;
    // Starred items lead their own section, next to (not inside) quick access.
    SidebarEntry starred;
    starred.glyph = L"\xE735";
    starred.fallback = L"Starred";
    starred.color = ui::HexColor(0xFBBF24);
    starred.label = l10n::Get(l10n::StringId::StarredItems);
    starred.path = MakeStarredPath();
    starred.expandable = true;
    m.starred.push_back(std::move(starred));
    SidebarEntry recent;
    recent.glyph = L"\xE823";
    recent.fallback = L"Recent";
    recent.color = ui::HexColor(0x60A5FA);
    recent.label = l10n::Get(l10n::StringId::Recent);
    recent.path = MakeRecentPath();
    recent.builtin = static_cast<int>(BuiltinQuickAccess::Recent);
    m.quick_access.push_back(std::move(recent));
    SidebarEntry desktop = MakeKnownEntry(FOLDERID_Desktop, L"\xE7F4", L"Desktop",
        ui::HexColor(0x38BDF8), l10n::Get(l10n::StringId::Desktop).c_str());
    desktop.badge = l10n::Get(l10n::StringId::Desktop);
    desktop.badge_rgb = 0x0078D4;
    desktop.builtin = static_cast<int>(BuiltinQuickAccess::Desktop);
    m.quick_access.push_back(std::move(desktop));
    // Explorer shows the localized name ("下载"), not the folder name on disk.
    SidebarEntry downloads = MakeKnownEntry(FOLDERID_Downloads, L"\xE896", L"Downloads",
        ui::HexColor(0xC084FC), l10n::Get(l10n::StringId::Downloads).c_str());
    downloads.builtin = static_cast<int>(BuiltinQuickAccess::Downloads);
    m.quick_access.push_back(std::move(downloads));
    // The library folders Explorer lists next to them, in Explorer's order.
    // Documents and Music start hidden (kDefaultQuickAccessHiddenMask); a
    // folder the profile does not have is left out, section menu included.
    const struct {
        REFKNOWNFOLDERID id;
        const wchar_t* glyph;
        const wchar_t* fallback;
        uint32_t rgb;
        l10n::StringId label;
        BuiltinQuickAccess builtin;
    } libraries[] = {
        // Documents: \xE7C3 (Page), not the file glyph, so the sidebar gives it
        // its color document icon (FluentSvgIdForGlyph).
        {FOLDERID_Documents, L"\xE7C3", L"Docs", 0x60A5FA, l10n::StringId::PickerDocuments,
         BuiltinQuickAccess::Documents},
        {FOLDERID_Pictures, L"\xE91B", L"Pics", 0x34D399, l10n::StringId::PickerPictures,
         BuiltinQuickAccess::Pictures},
        {FOLDERID_Music, L"\xE8D6", L"Music", 0xF472B6, l10n::StringId::Music,
         BuiltinQuickAccess::Music},
        {FOLDERID_Videos, L"\xE714", L"Video", 0xF59E0B, l10n::StringId::Videos,
         BuiltinQuickAccess::Videos},
    };
    for (const auto& library : libraries) {
        SidebarEntry entry = MakeKnownEntry(library.id, library.glyph, library.fallback,
            ui::HexColor(library.rgb), l10n::Get(library.label).c_str());
        if (entry.path.empty()) continue;
        entry.builtin = static_cast<int>(library.builtin);
        m.quick_access.push_back(std::move(entry));
    }
    // OneDrive leads its own section: one row per signed-in account, the way
    // Explorer lists them. The known folder only resolves for the primary
    // account, so it stays as the fallback when the registry has nothing.
    m.cloud = BuildOneDriveEntries();
    if (m.cloud.empty()) {
        SidebarEntry onedrive = MakeKnownEntry(FOLDERID_OneDrive, L"\xE753", L"OneDrive",
            ui::HexColor(0x2B88D8), L"OneDrive");
        if (!onedrive.path.empty()) m.cloud.push_back(std::move(onedrive));
    }
    // Isolated test instances only: a fixture folder stands in for a OneDrive
    // account so the row menu (#80) can be exercised without a signed-in client.
    wchar_t fake[MAX_PATH]{};
    if (m.cloud.empty() && GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) > 0) {
        const DWORD n = GetEnvironmentVariableW(L"PULSE_TEST_ONEDRIVE", fake, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            SidebarEntry onedrive;
            onedrive.glyph = L"\xE753";
            onedrive.fallback = L"OneDrive";
            onedrive.color = ui::HexColor(0x2B88D8);
            onedrive.label = L"OneDrive";
            onedrive.path = fs::NormalizePath(fake);
            m.cloud.push_back(std::move(onedrive));
        }
    }
    AppendOtherCloudEntries(m.cloud);
    SidebarEntry recycle_bin;
    recycle_bin.glyph = L"\xE75C";
    recycle_bin.fallback = L"Bin";
    recycle_bin.color = ui::HexColor(0x94A3B8);
    recycle_bin.label = l10n::Get(l10n::StringId::RecycleBin);
    recycle_bin.path = MakeRecyclePath();
    recycle_bin.builtin = static_cast<int>(BuiltinQuickAccess::RecycleBin);
    if (recycle && recycle->valid) {
        wchar_t occupancy[96]{};
        swprintf_s(occupancy, l10n::Get(l10n::StringId::RecycleOccupancyFormat).c_str(),
                   std::to_wstring(recycle->items).c_str(),
                   pulse::format::ByteSize(recycle->bytes).c_str());
        recycle_bin.detail = occupancy;
    }
    m.quick_access.push_back(std::move(recycle_bin));

    static const uint32_t kDrivePalette[] = { 0x60A5FA, 0x34D399, 0xFBBF24, 0xC084FC };
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1 << i))) continue;
        wchar_t root[4] = { wchar_t(L'A' + i), L':', L'\\', L'\0' };
        wchar_t volName[MAX_PATH + 1] = {};
        DWORD sn = 0;
        GetVolumeInformationW(root, volName, MAX_PATH, &sn, nullptr, nullptr, nullptr, 0);
        SidebarEntry e;
        e.label = fs::DriveLabel(root[0], volName, l10n::Get(l10n::StringId::LocalDisk)) +
                  L" (" + root[0] + L":)";
        e.glyph = L"\xE7F1"; // HardDrive (Segoe Fluent Icons)
        e.fallback = L"Drive";
        e.path = fs::NormalizePath(std::wstring(root));
        e.is_drive = true;
        e.color = ui::HexColor(kDrivePalette[m.drives.size() % 4]);
        m.drives.push_back(std::move(e));
    }
    RefreshSidebarDriveCapacity(m.drives);
    return m;
}

// ---------------------------------------------------------------------------
// Build window view-model
// ---------------------------------------------------------------------------
static std::wstring PaneHeaderText(const Tab& tab) {
    std::wstring text = tab.virtual_title.empty() ? TabTitle(tab.current_path) : tab.virtual_title;
    // 网络位置: the network it lists devices for ("Home-5G · 专用网络").
    if (!tab.virtual_header_note.empty()) text += L"  \u00B7  " + tab.virtual_header_note;
    return text;
}

static std::wstring StatusText(const Tab& tab) {
    if (tab.search_content_active || tab.content_results) {
        wchar_t count[128]{};
        swprintf_s(count,l10n::Get(l10n::StringId::ContentStatusMatches).c_str(),tab.search_total);
        return count;
    }
    if (tab.loading || !tab.snapshot) return L"";
    if (tab.current_path == L"pulse:networks" && !tab.virtual_status.empty()) return tab.virtual_status;
    if (tab.current_path.empty()) {
        // This PC: what is there (2 个磁盘 · 1 个网络盘 · 1 个设备), not
        // folders and files: phones and program icons are neither.
        size_t counts[4]{};
        for (size_t i = 0; i < tab.EntryCount(); ++i) {
            const fs::DirEntry& e = tab.EntryAt(i);
            if (e.drive_type) ++counts[e.drive_remote_like ? 1 : 0];
            else if (e.pc_device & fs::kPcDevicePhone) ++counts[2];
            else if (e.pc_launcher) ++counts[3];
        }
        const l10n::StringId ids[4] = {l10n::StringId::ThisPcStatusDrives, l10n::StringId::ThisPcStatusMounted,
                                       l10n::StringId::ThisPcStatusDevices, l10n::StringId::ThisPcStatusLaunchers};
        std::wstring text;
        for (int k = 0; k < 4; ++k) {
            if (counts[k] == 0 && k != 0) continue;
            wchar_t part[96]{};
            swprintf_s(part, l10n::Get(ids[k]).c_str(), counts[k]);
            if (!text.empty()) text += L" \u00B7 ";
            text += part;
        }
        return text;
    }
    wchar_t buf[128];
    swprintf_s(buf, l10n::Get(l10n::StringId::StatusItemsFormat).c_str(),
        tab.directory_count + tab.file_count);
    return buf;
}

static std::wstring SelectionText(const Tab& tab) {
    const int count = tab.SelectedCount();
    if (count <= 0) return {};
    // Only the details tree's "more" row: it is not an item.
    if (count == 1 && tab.TreeEntries() && tab.selected_index >= 0 &&
        tab.tree->IsPlaceholder(static_cast<size_t>(tab.selected_index)))
        return {};
    wchar_t buf[64]{};
    swprintf_s(buf, l10n::Get(l10n::StringId::SelectedCountFormat).c_str(), count);
    return buf;
}

static ui::SidebarGroup ConvertGroup(const std::wstring& header, const std::vector<SidebarEntry>& src,
                                     bool collapsed) {
    ui::SidebarGroup g;
    g.header = header;
    g.collapsed = collapsed;
    for (const auto& e : src) {
        ui::SidebarItem it;
        it.label = e.label;
        it.detail = e.detail;
        it.icon_glyph = e.glyph;
        it.icon_path = e.icon_path;
        it.fallback_text = e.fallback;
        it.icon_color = e.color;
        it.danger = e.danger;
        it.path = e.path;
        it.is_drive = e.is_drive;
        it.used_ratio = e.used_ratio;
        it.free_bytes = e.free_bytes;
        it.total_bytes = e.total_bytes;
        it.badge = e.badge;
        it.badge_color = ui::HexColor(e.badge_rgb);
        it.is_tag = e.is_tag;
        it.show_count = e.show_count;
        it.count = e.count;
        it.tag_dot = e.tag_dot;
        it.expandable = e.expandable;
        g.items.push_back(std::move(it));
    }
    return g;
}

// Stored in places.json: a name never changes, new links get new names.
constexpr const wchar_t* kBuiltinQuickAccessNames[] = {
    L"recent", L"desktop", L"downloads", L"recycle_bin",
    L"pictures", L"videos", L"documents", L"music",
};
static_assert(sizeof(kBuiltinQuickAccessNames) / sizeof(kBuiltinQuickAccessNames[0]) ==
              static_cast<size_t>(BuiltinQuickAccess::Count));
constexpr std::wstring_view kBuiltinQuickAccessPrefix = L"builtin:";  // IsBuiltinQuickAccessKey

std::wstring BuiltinQuickAccessKey(int builtin) {
    if (builtin < 0 || builtin >= static_cast<int>(BuiltinQuickAccess::Count)) return {};
    return std::wstring(kBuiltinQuickAccessPrefix) + kBuiltinQuickAccessNames[builtin];
}

int BuiltinQuickAccessFromKey(const std::wstring& key) {
    std::wstring_view name = key;
    if (!name.starts_with(kBuiltinQuickAccessPrefix)) return -1;
    name.remove_prefix(kBuiltinQuickAccessPrefix.size());
    for (int i = 0; i < static_cast<int>(BuiltinQuickAccess::Count); ++i) {
        if (name == kBuiltinQuickAccessNames[i]) return i;
    }
    return -1;
}

std::vector<std::wstring> BuiltinQuickAccessDefaultKeys(const std::vector<SidebarEntry>& quick_access) {
    std::vector<std::wstring> keys;
    keys.reserve(quick_access.size());
    for (const auto& entry : quick_access) {
        std::wstring key = BuiltinQuickAccessKey(entry.builtin);
        if (!key.empty()) keys.push_back(std::move(key));
    }
    return keys;
}

std::vector<std::wstring> QuickAccessDisplayKeys(const std::vector<SidebarEntry>& quick_access,
                                                 const PlacesCatalog* places,
                                                 uint32_t hidden_mask, bool include_hidden) {
    std::vector<std::wstring> keys = BuiltinQuickAccessDefaultKeys(quick_access);
    if (places) {
        keys.insert(keys.end(), places->quick_access_paths.begin(),
                    places->quick_access_paths.end());
        keys = MergeQuickAccessOrder(places->quick_access_order, keys);
    }
    if (!include_hidden) {
        std::erase_if(keys, [&](const std::wstring& key) {
            const int builtin = BuiltinQuickAccessFromKey(key);
            return builtin >= 0 && ((hidden_mask >> builtin) & 1u) != 0;
        });
    }
    return keys;
}

static std::wstring EntryPathOf(const Tab& tab, const fs::DirEntry& e) {
    if (!e.full_path.empty()) return e.full_path;
    if (fs::IsVirtualPath(tab.current_path)) return {};
    std::wstring full = tab.current_path;
    if (!full.empty() && full.back() != L'\\') full += L'\\';
    full += e.name;
    return full;
}

static std::wstring ToLowerCopy(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

static bool WildcardMatch(const std::wstring& text, const std::wstring& pat) {
    size_t si = 0, pi = 0, star = static_cast<size_t>(-1), match = 0;
    const size_t n = text.size();
    const size_t pn = pat.size();
    while (si < n) {
        if (pi < pn && pat[pi] == L'*') {
            star = pi++;
            match = si;
        } else if (pi < pn && (pat[pi] == L'?' || pat[pi] == text[si])) {
            ++si;
            ++pi;
        } else if (star != static_cast<size_t>(-1)) {
            pi = star + 1;
            si = ++match;
        } else {
            return false;
        }
    }
    while (pi < pn && pat[pi] == L'*') ++pi;
    return pi == pn;
}

bool NameMatchesPattern(std::wstring_view name, std::wstring_view needle) {
    if (needle.empty()) return true;
    const std::wstring folded_name = ToLowerCopy(std::wstring(name));
    const std::wstring folded_pat = ToLowerCopy(std::wstring(needle));
    if (folded_pat.find(L'*') != std::wstring::npos ||
        folded_pat.find(L'?') != std::wstring::npos) {
        return WildcardMatch(folded_name, folded_pat);
    }
    return folded_name.find(folded_pat) != std::wstring::npos;
}

// Filter box tag syntax (pulse_tag_redesign §7): "#a #b" = both, "#a|b" =
// either, "-#a" = without; #"two words" quotes a name with spaces.
struct FilterTagTerm {
    std::vector<std::wstring> alternatives;  // lowercase needles, any may match
    bool exclude = false;
};

static std::wstring UnquoteTagNeedle(std::wstring value) {
    if (!value.empty() && value[0] == L'#') value.erase(0, 1);
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
        value = value.substr(1, value.size() - 2);
    else if (!value.empty() && value.front() == L'"')
        value.erase(0, 1);  // still typing the closing quote
    return ToLowerCopy(value);
}

static void ParseFilterText(const std::wstring& text, std::wstring& name_needle,
                            std::vector<FilterTagTerm>& tag_terms) {
    name_needle.clear();
    tag_terms.clear();
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::iswspace(text[i])) ++i;
        if (i >= text.size()) break;
        const size_t start = i;
        bool quoted = false;
        while (i < text.size() && (quoted || !std::iswspace(text[i]))) {
            if (text[i] == L'"') quoted = !quoted;
            ++i;
        }
        std::wstring tok = text.substr(start, i - start);
        const bool exclude = tok.size() > 2 && tok[0] == L'-' && tok[1] == L'#';
        FilterTagTerm term;
        term.exclude = exclude;
        if (exclude || (tok.size() > 1 && tok[0] == L'#')) {
            size_t part = exclude ? 2 : 1;
            bool in_quotes = false;
            for (size_t k = part; k <= tok.size(); ++k) {
                if (k < tok.size() && tok[k] == L'"') in_quotes = !in_quotes;
                if (k == tok.size() || (tok[k] == L'|' && !in_quotes)) {
                    std::wstring needle = UnquoteTagNeedle(tok.substr(part, k - part));
                    if (!needle.empty()) term.alternatives.push_back(std::move(needle));
                    part = k + 1;
                }
            }
        }
        if (!term.alternatives.empty()) {
            tag_terms.push_back(std::move(term));
        } else if (!tok.empty()) {
            if (!name_needle.empty()) name_needle += L' ';
            name_needle += tok;
        }
    }
    name_needle = ToLowerCopy(name_needle);
}

// The color family of a tag's actual color (HSV hue; low saturation is gray),
// so #红 finds every reddish tag whatever its position or name.
enum class TagHue { Red, Orange, Yellow, Green, Cyan, Blue, Purple, Pink, Gray };

// The condition bar of a tag view: the terms (且 / 或 / 不含) in TagQueryTerms
// order, the 位置 chip, ＋ 条件, 保存视图; then the 细分 chips counted at load time.
// tag_view.cpp maps clicks back by the same order.
static void FillTagQueryBar(const PlacesCatalog& places, const Tab& tab, ui::PaneViewModel& out) {
    TagQuery query;
    if (!TagQueryFromPath(places, tab.current_path, &query)) return;
    using Chip = ui::TagQueryChipView;
    for (const auto& term : TagQueryTerms(query)) {
        Chip chip;
        const ColorTag* tag = places.FindTag(term.id);
        chip.label = tag ? tag->name : l10n::Get(l10n::StringId::TagDeleted);
        chip.color = tag ? ui::HexColor(tag->rgb) : ui::HexColor(0x8E8E93);
        chip.kind = term.kind == TagTermKind::All ? Chip::All
                  : term.kind == TagTermKind::Any ? Chip::Any : Chip::Not;
        out.tag_query_chips.push_back(std::move(chip));
    }
    if (!query.in.empty()) {
        Chip chip;
        chip.label = TagQueryFolderLabel(query.in);
        chip.kind = Chip::Location;
        out.tag_query_chips.push_back(std::move(chip));
    }
    Chip add;
    add.label = l10n::Get(l10n::StringId::TagAddCondition);
    add.kind = Chip::Add;
    out.tag_query_chips.push_back(std::move(add));
    if (!query.Empty()) {
        Chip save;
        save.label = l10n::Get(l10n::StringId::TagSaveView);
        save.kind = Chip::Save;
        out.tag_query_chips.push_back(std::move(save));
    }
    for (const auto& [id, count] : tab.tag_refine) {
        const ColorTag* tag = places.FindTag(id);
        if (!tag) continue;
        Chip chip;
        chip.label = tag->name;
        chip.color = ui::HexColor(tag->rgb);
        chip.kind = Chip::RefineTag;
        chip.count = count;
        out.tag_query_refine.push_back(std::move(chip));
    }
    for (const auto& [folder, count] : tab.tag_locations) {
        Chip chip;
        chip.label = TagQueryFolderLabel(folder);
        chip.kind = Chip::RefineLocation;
        chip.count = count;
        out.tag_query_refine.push_back(std::move(chip));
    }
}

static TagHue TagHueOf(uint32_t rgb) {
    const float r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
    const float g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
    const float b = static_cast<float>(rgb & 0xFF) / 255.0f;
    const float hi = (std::max)({r, g, b});
    const float lo = (std::min)({r, g, b});
    const float delta = hi - lo;
    if (hi <= 0.0f || delta / hi < 0.25f) return TagHue::Gray;
    float hue;
    if (hi == r) hue = 60.0f * std::fmod((g - b) / delta + 6.0f, 6.0f);
    else if (hi == g) hue = 60.0f * ((b - r) / delta + 2.0f);
    else hue = 60.0f * ((r - g) / delta + 4.0f);
    if (hue < 15.0f || hue >= 345.0f) return TagHue::Red;
    if (hue < 40.0f) return TagHue::Orange;
    if (hue < 70.0f) return TagHue::Yellow;
    if (hue < 165.0f) return TagHue::Green;
    if (hue < 195.0f) return TagHue::Cyan;
    if (hue < 255.0f) return TagHue::Blue;
    if (hue < 300.0f) return TagHue::Purple;
    return TagHue::Pink;
}

static bool TagMatchesNeedle(const ColorTag& tag, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (ToLowerCopy(tag.name).find(needle) != std::wstring::npos) return true;
    struct Nick { TagHue hue; const wchar_t* names[4]; };
    static const Nick kNicks[] = {
        {TagHue::Red, {L"红", L"紅", L"red", nullptr}},
        {TagHue::Orange, {L"橙", L"orange", nullptr, nullptr}},
        {TagHue::Yellow, {L"黄", L"黃", L"yellow", nullptr}},
        {TagHue::Green, {L"绿", L"綠", L"green", nullptr}},
        {TagHue::Cyan, {L"青", L"cyan", nullptr, nullptr}},
        {TagHue::Blue, {L"蓝", L"藍", L"blue", nullptr}},
        {TagHue::Purple, {L"紫", L"purple", nullptr, nullptr}},
        {TagHue::Pink, {L"粉", L"pink", nullptr, nullptr}},
        {TagHue::Gray, {L"灰", L"gray", L"grey", nullptr}},
    };
    for (const Nick& nick : kNicks) {
        for (const wchar_t* name : nick.names) {
            if (name && needle == name) return TagHueOf(tag.rgb) == nick.hue;
        }
    }
    return false;
}

// Whether the tags on one item (indices into places.tags) satisfy every term.
static bool TagTermsMatch(const PlacesCatalog& places, const std::vector<int>& ids,
                          const std::vector<FilterTagTerm>& terms) {
    for (const auto& term : terms) {
        bool hit = false;
        for (int id : ids) {
            if (id < 0 || id >= static_cast<int>(places.tags.size())) continue;
            for (const auto& needle : term.alternatives) {
                if (TagMatchesNeedle(places.tags[static_cast<size_t>(id)], needle)) { hit = true; break; }
            }
            if (hit) break;
        }
        if (hit == term.exclude) return false;
    }
    return true;
}

index::ContentResultStore::Filter ContentFilter(const std::wstring& text, const PlacesCatalog& places) {
    if(text.empty()) return {};
    std::wstring name; std::vector<FilterTagTerm> terms;
    ParseFilterText(text,name,terms);
    if(name.empty() && terms.empty()) name=ToLowerCopy(text);
    // Per term: the lowercase paths carrying a matching tag.
    struct TermPaths { std::unordered_set<std::wstring> paths; bool exclude = false; };
    std::vector<TermPaths> sets;
    for(const auto& term:terms) {
        TermPaths set; set.exclude=term.exclude;
        for(const auto& tag:places.tags) {
            bool hit=false;
            for(const auto& needle:term.alternatives) hit = hit || TagMatchesNeedle(tag,needle);
            if(hit) for(const auto& path:tag.paths) set.paths.insert(ToLowerCopy(path));
        }
        sets.push_back(std::move(set));
    }
    return [pattern=NamePattern(name, true),sets=std::move(sets)](const fs::DirEntry& entry) {
        if(!pattern.Matches(entry.name)) return false;
        if(sets.empty()) return true;
        const std::wstring key=ToLowerCopy(entry.full_path);
        for(const auto& set:sets) if(set.paths.contains(key)==set.exclude) return false;
        return true;
    };
}

void CollectFilterMatches(const Tab& tab, const PlacesCatalog* places, std::vector<int>& out) {
    out.clear();
    if (!tab.snapshot) return;
    const int n = static_cast<int>(tab.EntryCount());
    if(tab.content_results) { out.reserve(n); for(int i=0;i<n;++i) out.push_back(i); return; }
    if (tab.filter_text.empty() && tab.ShowsEveryEntry()) {
        out.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) if (tab.EntryVisible(i)) out.push_back(i);
        return;
    }
    std::wstring name_needle;
    std::vector<FilterTagTerm> tag_terms;
    ParseFilterText(tab.filter_text, name_needle, tag_terms);
    if (name_needle.empty() && tag_terms.empty())
        name_needle = ToLowerCopy(tab.filter_text);
    const NamePattern pattern(name_needle, true);
    const auto* rows = tab.TreeEntries();
    if (!rows) rows = tab.snapshot.get();
    const int available = static_cast<int>((std::min)(static_cast<size_t>(n), rows->size()));
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < available; ++i) {
        const auto& e = (*rows)[static_cast<size_t>(i)];
        if (!tab.EntryVisible(i)) continue;
        bool name_ok = pattern.Matches(e.name);
        // 别名: the filter also finds an item by its alias, or its pinyin.
        if (!name_ok && !name_needle.empty() && !AliasStore::Instance().Empty()) {
            const std::wstring alias = AliasStore::Instance().Find(EntryPathOf(tab, e));
            name_ok = AliasStore::AliasMatches(alias, name_needle);
        }
        bool tag_ok = tag_terms.empty();
        if (!tag_ok && places && name_ok)
            tag_ok = TagTermsMatch(*places, places->TagsForPath(EntryPathOf(tab, e)), tag_terms);
        if (name_ok && tag_ok) out.push_back(i);
    }
    // Maps cached by each tab should not retain a full-directory allocation
    // after a selective query. Keep dense results and small reusable buffers.
    if (out.capacity() > 4096 && out.size() < out.capacity() / 8)
        std::vector<int>(out.begin(), out.end()).swap(out);
}

namespace {
PaneDecorator g_dup_decorator = nullptr;
MediaPaneDecorator g_media_decorator = nullptr;
void FillPaneViewModelBody(ui::PaneViewModel& out, const Pane& pane, const PlacesCatalog* places);
} // namespace

void SetDupPaneDecorator(PaneDecorator decorator) { g_dup_decorator = decorator; }
void SetMediaPaneDecorator(MediaPaneDecorator decorator) { g_media_decorator = decorator; }

void FillPaneViewModel(ui::PaneViewModel& out, const Pane& pane, const PlacesCatalog* places) {
    out.banner_action_labels[0].clear();
    out.banner_action_labels[1].clear();
    out.dup_funnel = {};
    out.dup_chips = false;
    out.dup_empty_title.clear();
    out.media_columns = 0;
    out.media_cell = nullptr;
    FillPaneViewModelBody(out, pane, places);
    const Tab* tab = pane.ActiveTab();
    if (g_media_decorator && tab) g_media_decorator(*tab, out);
    // A media column order without those columns on screen (switched off,
    // another folder) sorts by name (SortEntries); show it as such.
    if (!out.media_columns && out.sort_column >= ui::SortColumn::MediaTaken) out.sort_column = ui::SortColumn::Name;
    if (g_dup_decorator && tab && tab->current_path.starts_with(L"pulse:ai-list:"))
        g_dup_decorator(tab->current_path, out);
}

namespace {
void FillPaneViewModelBody(ui::PaneViewModel& out, const Pane& pane, const PlacesCatalog* places) {
    const Tab* tab = pane.ActiveTab();
    if (!tab) return;
    out.path = tab->virtual_title.empty() ? DisplayPath(tab->current_path) : tab->virtual_title;
    out.header_text = PaneHeaderText(*tab);
    out.filter_text = tab->filter_text;
    out.filter_expand = pane.filter_expand;
    out.header_controls_opacity = pane.header_animation.opacity;
    out.size_control = true;
    out.size_slider_open = pane.size_slider_open;
    out.banner_title = tab->banner_title;
    out.banner_message = tab->banner_message;
    out.banner_kind = tab->net_readonly ? 2 : 0;  // refined below, once the view kind is known
    out.filter_map.reset();
    out.tag_dots.reset();
    out.loading = tab->loading;
    out.search_retaining_results = tab->search_retaining_results;
    out.can_go_back = tab->CanGoBack();
    out.can_go_forward = tab->CanGoForward();
    std::wstring virtual_kind;
    ParsePulsePath(tab->current_path, &virtual_kind, nullptr);
    out.can_go_up = virtual_kind == L"recycle" || virtual_kind == L"device"
        ? true
        : (fs::IsVirtualPath(tab->current_path) ? tab->CanGoBack() : !tab->current_path.empty());
    out.is_file_system = !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path);
    out.cloud_status_column = out.is_file_system && IsUnderCloudFilesRoot(tab->current_path);
    out.can_create = out.is_file_system && !tab->net_readonly && !IsUncServerRoot(tab->current_path);
    out.can_create_folder = out.can_create;  // the app adds a writable phone folder
    // The network page and a server's share list: 连接服务器 takes 新建's slot.
    out.connect_server = tab->current_path == L"pulse:networks" || IsUncServerRoot(tab->current_path);
    out.curated_order = virtual_kind == L"recent" || virtual_kind == L"changes" ||
                        virtual_kind == L"ai-list";
    out.is_starred = virtual_kind == L"starred";
    out.is_recent = virtual_kind == L"recent";
    out.is_recycle = virtual_kind == L"recycle";
    // Tag views list items from many folders too: they get the 路径 column.
    out.is_search = virtual_kind == L"search" || virtual_kind == L"saved-search"
        || virtual_kind == L"recycle" || virtual_kind == L"changes" || virtual_kind == L"ai-list"
        || IsTagViewKind(virtual_kind);
    out.is_tag_query = places && IsTagViewKind(virtual_kind);
    if (out.is_tag_query) FillTagQueryBar(*places, *tab, out);
    out.is_query_search = virtual_kind == L"search";
    if (out.is_query_search) {
        std::wstring rest;
        ParsePulsePath(tab->current_path, nullptr, &rest);
        out.search_query = rest;
        out.is_content_search = SplitSearchQueryText(rest).content.present();
        out.network_live_action = !out.is_content_search && !tab->network_live_root.empty() &&
                                  !tab->banner_message.empty();
        // The breadcrumb shows where the search started, then one search segment.
        out.has_search_origin = true;
        if (tab->search_origin_valid) {
            out.search_origin = tab->search_origin_path;
        } else {
            const auto spec = ParseSearchQuery(rest);
            out.search_origin = spec.location == LocationScope::CustomFolder ? spec.custom_folder
                : spec.location == LocationScope::CurrentFolder ? spec.current_folder : std::wstring{};
        }
        std::wstring needle = SearchDisplayNeedle(rest);
        if (needle.size() > 24) needle = needle.substr(0, 23) + L"\u2026";
        std::wstring crumb(needle.size() + 64, L'\0');
        const int written = swprintf_s(crumb.data(), crumb.size(),
            l10n::Get(l10n::StringId::SearchCrumbFormat).c_str(), needle.c_str());
        crumb.resize(written > 0 ? static_cast<size_t>(written) : 0);
        out.search_crumb = crumb.empty() ? l10n::Get(l10n::StringId::Search) : crumb;
        // Keep pulse:search:... so the address bar is one segment, not C:\ splits.
        out.path = tab->current_path;
    }
    if (!out.banner_message.empty()) {
        const PaneBannerStyle banner = ClassifyPaneBanner(*tab, out.can_go_up, out.is_content_search,
                                                          out.network_live_action);
        out.banner_kind = banner.kind;
        out.banner_actions[0] = banner.actions[0];
        out.banner_actions[1] = banner.actions[1];
        out.banner_dismissible = banner.dismissible;
    }
    out.content_results = tab->content_results;
    out.search_snippets = tab->search_snippets;
    out.recent_filter = tab->recent_filter;
    out.recent_total = out.is_recent && places ? places->recent_items.size() : 0;
    // Phones: the breadcrumb splits the pulse:device: path (此电脑 › 手机 › …).
    if (virtual_kind == L"device") out.path = tab->current_path;
    if (virtual_kind == L"recycle") {
        out.path = MakeRecyclePath();
        out.date_column_label = l10n::Get(l10n::StringId::ColumnDeleted);
    } else {
        out.date_column_label = l10n::Get(out.is_recent
            ? l10n::StringId::ColumnRecentOpened : l10n::StringId::ColumnModified);
    }
    // The assistant's cleanup lists (查看): each row's reason in the 类型 column.
    // The duplicates list names it 状态 (保留 / 副本).
    if (virtual_kind == L"ai-list")
        out.type_column_label = tab->virtual_type_column.empty() ? l10n::Get(l10n::StringId::AiClReasonColumn)
                                                                 : tab->virtual_type_column;
    out.selected_index = tab->selected_index;
    out.selected_count = tab->SelectedCount();
    // A lone details-tree "more" row is keyboard focus, not a selection.
    if (out.selected_count == 1 && tab->TreeEntries() && tab->selected_index >= 0 &&
        tab->tree->IsPlaceholder(static_cast<size_t>(tab->selected_index)))
        out.selected_count = 0;
    out.all_selected = tab->all_selected;
    out.selected_indices = tab->all_selected ? nullptr : &tab->selected;
    out.selection_owner = tab;
    out.selection_revision = tab->selection_revision;
    out.focus_first_hint = tab->HasFocusHint();
    out.focus_ring_source = tab->HasCaret() && !tab->IsSelected(tab->caret_index) ? tab->caret_index : -1;
    out.scroll_y = tab->scroll_y;
    out.scroll_x = tab->scroll_x;
    out.view_mode = tab->view_mode;
    out.icon_dip = tab->icon_dip;
    out.wall = tab->wall;
    out.wall_view_identity = reinterpret_cast<uintptr_t>(tab);
    out.view_generation = tab->view_generation;
    out.sort_column = tab->sort_column;
    out.sort_direction = tab->sort_direction;
    out.details_column_dividers = tab->details_column_dividers;
    out.media_column_widths = tab->media_column_widths;
    out.search_column_dividers = tab->search_column_dividers;
    FillColumnStripView(out.column_strip, *tab);
    // This PC in details view: its own columns, and 存储总览 over two or more
    // local drives whose sizes are known.
    out.this_pc_columns = tab->current_path.empty() && tab->view_mode == ui::ViewMode::Details &&
                          !out.column_strip.Active();
    out.drive_overview = {};
    if (out.this_pc_columns && tab->snapshot && !tab->loading) {
        for (size_t i = 0; i < tab->EntryCount(); ++i) {
            const fs::DirEntry& e = tab->EntryAt(i);
            if (!e.drive_type || e.drive_remote_like || e.drive_total == 0) continue;
            ui::DriveOverviewView::Drive drive;
            const std::wstring& root = e.full_path;
            const size_t colon = root.find(L':');
            if (colon != std::wstring::npos && colon > 0) drive.letter = root.substr(colon - 1, 2);
            drive.total = e.drive_total;
            drive.free = std::min(e.drive_free, e.drive_total);
            drive.used = static_cast<float>(static_cast<double>(drive.total - drive.free) /
                                            static_cast<double>(drive.total));
            out.drive_overview.free += drive.free;
            out.drive_overview.total += drive.total;
            out.drive_overview.drives.push_back(std::move(drive));
        }
        // Drive letter order, whatever the rows are sorted by.
        std::sort(out.drive_overview.drives.begin(), out.drive_overview.drives.end(),
                  [](const auto& a, const auto& b) { return a.letter < b.letter; });
    }
    out.focused = pane.focused;
    out.snapshot = tab->snapshot;
    out.tag_catalog = places;
    const uint64_t places_revision = places ? places->TagRevision() : 0;
    const bool source_changed = tab->view_cache_snapshot != tab->snapshot ||
        tab->view_cache_places != places ||
        tab->view_cache_places_revision != places_revision;
    if (source_changed || !tab->view_tag_dots) {
        auto dots_by_row = std::make_shared<ui::PaneViewModel::TagDots>();
        tab->view_cache_snapshot = tab->snapshot;
        tab->view_cache_places = places;
        tab->view_cache_places_revision = places_revision;
        tab->view_tag_dots = std::move(dots_by_row);
        tab->view_filter_map.reset();
        tab->view_row_cache = std::make_shared<ui::RowPresentationCache>();
        tab->view_row_cache->snapshot = tab->snapshot;
    }
    if (!tab->view_row_cache) {
        tab->view_row_cache = std::make_shared<ui::RowPresentationCache>();
        tab->view_row_cache->snapshot = tab->snapshot;
    }
    out.row_cache = tab->view_row_cache;
    out.tag_dots = tab->view_tag_dots;
    if (!tab->content_results && tab->snapshot &&
        (!tab->ShowsEveryEntry() || !tab->filter_text.empty())) {
        if (!tab->view_filter_map || tab->view_cache_filter_text != tab->filter_text) {
            auto filtered = std::make_shared<ui::PaneViewModel::FilterMap>();
            CollectFilterMatches(*tab, places, *filtered);
            tab->view_cache_filter_text = tab->filter_text;
            tab->view_filter_map = std::move(filtered);
        }
        out.filter_map = tab->view_filter_map;
    }
    out.compare_marks = tab->compare_marks;
    out.compare_active = tab->compare_marks != nullptr;
    out.compare_diff_only = out.compare_active && tab->compare_diff_only;
    out.compare_counts = out.compare_active ? tab->compare_counts : std::array<int, 5>{};
    if (out.compare_active) {
        const auto& c = tab->compare_counts;
        // Summary only (no title): split panes are narrow.
        out.banner_title.clear();
        if (c[1] + c[2] + c[3] + c[4] == 0) {
            out.banner_message = l10n::Get(l10n::StringId::CompareIdentical);
        } else {
            wchar_t summary[256]{};
            swprintf_s(summary, l10n::Get(l10n::StringId::CompareSummaryFormat).c_str(),
                       c[1], c[2], c[3], c[4]);
            out.banner_message = summary;
        }
        out.banner_kind = 4;
        out.banner_actions[0] = out.banner_actions[1] = ui::PaneBannerAction::None;
        out.banner_dismissible = false;
        if (out.compare_diff_only && tab->snapshot && !tab->content_results &&
            tab->compare_marks->size() == tab->EntryCount()) {
            if (!tab->view_compare_map || tab->view_compare_base != out.filter_map ||
                tab->view_compare_marks != tab->compare_marks) {
                auto diff = std::make_shared<ui::PaneViewModel::FilterMap>();
                const auto& marks = *tab->compare_marks;
                if (out.filter_map) {
                    for (int i : *out.filter_map)
                        if (i >= 0 && static_cast<size_t>(i) < marks.size() && marks[static_cast<size_t>(i)])
                            diff->push_back(i);
                } else {
                    const int n = static_cast<int>(tab->EntryCount());
                    for (int i = 0; i < n; ++i)
                        if (marks[static_cast<size_t>(i)] && tab->EntryVisible(i)) diff->push_back(i);
                }
                tab->view_compare_base = out.filter_map;
                tab->view_compare_marks = tab->compare_marks;
                tab->view_compare_map = std::move(diff);
            }
            out.filter_map = tab->view_compare_map;
        }
    }
    // "Group by": the worker sorted with the same key, so each group is one
    // run of view rows. Spans are cached until an input changes.
    out.group_by = tab->EffectiveGroup();
    if (out.group_by != 0 && tab->snapshot && !tab->content_results && !out.compare_active) {
        const GroupBy by = GroupByFromInt(out.group_by);
        GroupClock clock = by == GroupBy::Date ? MakeGroupClock(tab->sort_column) : GroupClock{};
        clock.folders = tab->EffectiveFolderSort();
        // Tag groups re-split when the catalog changes; dates at midnight;
        // media groups as values come in (each paced re-sort pass).
        const auto tag_catalog = by == GroupBy::Tag ? CurrentTagCatalog() : nullptr;
        const bool media_groups = IsMediaGroup(by);
        const uint64_t day_key = by == GroupBy::Tag ? (tag_catalog ? tag_catalog->revision : 0)
                               : media_groups ? tab->media_resort_revision : clock.today;
        const int sort_key = static_cast<int>(tab->sort_column) * 2 +
                             static_cast<int>(tab->sort_direction) + 128 * static_cast<int>(clock.folders);
        if (!tab->view_groups || tab->view_groups_snapshot != tab->snapshot ||
            tab->view_groups_filter != out.filter_map || tab->view_groups_by != out.group_by ||
            tab->view_groups_sort != sort_key || tab->view_groups_day != day_key ||
            tab->view_groups_rev != tab->group_collapse_rev) {
            auto groups = std::make_shared<ui::ListGroups>();
            if (by == GroupBy::Tag) clock.tags = TagGroupsForFolder(tab->current_path);
            if (media_groups) clock.media = MediaGroupsForFolder(tab->current_path, by);
            const auto& entries = *tab->snapshot;
            const auto folded = tab->collapsed_groups.find(tab->current_path);
            const std::set<std::wstring>* collapsed =
                folded != tab->collapsed_groups.end() ? &folded->second : nullptr;
            const size_t rows = out.filter_map ? out.filter_map->size() : entries.size();
            // A key seen twice means the snapshot is not in grouped order yet
            // (re-sort pending, day rollover): draw it flat until it is.
            std::set<std::wstring> seen;
            bool contiguous = true;
            int prev = -1;
            int prev_rank = 0;
            for (size_t row = 0; row < rows; ++row) {
                const int src = out.filter_map ? (*out.filter_map)[row] : static_cast<int>(row);
                if (src < 0 || static_cast<size_t>(src) >= entries.size()) {
                    if (!groups->empty()) ++groups->back().count;
                    continue;
                }
                const fs::DirEntry& e = entries[static_cast<size_t>(src)];
                const int rank = GroupRank(e, by, clock);
                if (groups->empty() || prev < 0 ||
                    GroupCompareRanks(entries[static_cast<size_t>(prev)], e, by,
                                      tab->sort_column, tab->sort_direction, prev_rank, rank, clock) != 0) {
                    ui::ListGroup g;
                    g.first = static_cast<int>(row);
                    g.rank = rank;
                    g.sample = src;
                    g.key = GroupKey(e, by, clock);
                    g.collapsed = collapsed && collapsed->contains(g.key);
                    if (by == GroupBy::Tag && tag_catalog && g.rank >= 0 &&
                        static_cast<size_t>(g.rank) < tag_catalog->tags.size()) {
                        g.label = tag_catalog->tags[static_cast<size_t>(g.rank)].name;
                        g.color_rgb = tag_catalog->tags[static_cast<size_t>(g.rank)].rgb;
                    }
                    if (by == GroupBy::Location) g.label = GroupLocationLabel(e);
                    if (by == GroupBy::AiDup) g.label = e.change_old_path;
                    if (media_groups)   // empty: 未知 (the renderer names it)
                        g.label = g.rank < 0 ? l10n::Get(l10n::StringId::GroupFolders) : MediaGroupLabel(clock, g.rank);
                    if (rank == kFolderGroupRank) g.label = l10n::Get(l10n::StringId::GroupFolders);
                    if (!seen.insert(g.key).second) {
                        contiguous = false;
                        break;
                    }
                    groups->push_back(std::move(g));
                }
                ui::ListGroup& g = groups->back();
                ++g.count;
                if (!e.is_dir) g.bytes += e.size;
                prev = src;
                prev_rank = rank;
            }
            if (!contiguous) groups->clear();
            tab->view_groups = std::move(groups);
            tab->view_groups_snapshot = tab->snapshot;
            tab->view_groups_filter = out.filter_map;
            tab->view_groups_by = out.group_by;
            tab->view_groups_sort = sort_key;
            tab->view_groups_day = day_key;
            tab->view_groups_rev = tab->group_collapse_rev;
        }
        out.groups = tab->view_groups;
    }
    // Details tree: children follow their folder, so the view rows come from
    // the tree (app_details_tree.cpp) instead of the sorted filter map.
    if (tab->TreeEntries() && tab->tree->rows && tab->tree->map) {
        out.snapshot = tab->tree->combined;
        out.filter_map = tab->tree->map;
        out.tree = tab->tree->rows;
        out.groups.reset();
        // One child row selected: the breadcrumb continues to its folder.
        if (tab->SelectedCount() == 1 && tab->selected_index >= 0 &&
            tab->selected_index < static_cast<int>(tab->EntryCount()) &&
            !tab->tree->IsPlaceholder(static_cast<size_t>(tab->selected_index))) {
            const std::wstring full = tab->EntryAt(tab->selected_index).full_path;
            const size_t slash = full.find_last_of(L'\\');
            const std::wstring& here = tab->current_path;
            const size_t base = here.size() + (here.ends_with(L'\\') ? 0 : 1);
            if (slash != std::wstring::npos && slash > base &&
                _wcsnicmp(full.c_str(), here.c_str(), here.size()) == 0) {
                out.tree_selection_dir = full.substr(0, slash);
                out.tree_selection_rel = full.substr(base, slash - base);
            }
        }
    }
}
} // namespace

namespace {
std::wstring CompareKey(const std::wstring& name) {
    std::wstring key = name;
    if (!key.empty()) CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
    return key;
}
uint64_t CompareTicks(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
} // namespace

FolderCompareResult CompareFolderTabs(const Tab& a, const Tab& b) {
    FolderCompareResult result;
    if (!a.snapshot || !b.snapshot || a.content_results || b.content_results) return result;
    using M = ui::CompareMark;
    const auto& ea = *a.snapshot;
    const auto& eb = *b.snapshot;
    auto ma = std::make_shared<std::vector<uint8_t>>(ea.size(), static_cast<uint8_t>(M::Same));
    auto mb = std::make_shared<std::vector<uint8_t>>(eb.size(), static_cast<uint8_t>(M::Same));
    std::unordered_map<std::wstring, int> index_b;
    index_b.reserve(eb.size());
    for (int j = 0; j < static_cast<int>(eb.size()); ++j)
        if (b.EntryVisible(j)) index_b.emplace(CompareKey(eb[static_cast<size_t>(j)].name), j);
    std::vector<char> matched(eb.size(), 0);
    constexpr uint64_t kTolerance = 20000000ull; // 2 s: FAT/exFAT timestamp granularity
    for (int i = 0; i < static_cast<int>(ea.size()); ++i) {
        if (!a.EntryVisible(i)) continue;
        const fs::DirEntry& x = ea[static_cast<size_t>(i)];
        const auto found = index_b.find(CompareKey(x.name));
        if (found == index_b.end()) {
            (*ma)[static_cast<size_t>(i)] = static_cast<uint8_t>(M::OnlyHere);
            continue;
        }
        const int j = found->second;
        matched[static_cast<size_t>(j)] = 1;
        const fs::DirEntry& y = eb[static_cast<size_t>(j)];
        M mark_a = M::Same, mark_b = M::Same;
        if (x.is_dir != y.is_dir) {
            mark_a = mark_b = M::Differs;
        } else if (!x.is_dir) {
            const uint64_t tx = CompareTicks(x.mtime), ty = CompareTicks(y.mtime);
            if (tx > ty + kTolerance) { mark_a = M::Newer; mark_b = M::Older; }
            else if (ty > tx + kTolerance) { mark_a = M::Older; mark_b = M::Newer; }
            else if (x.size != y.size) mark_a = mark_b = M::Differs;
        }
        (*ma)[static_cast<size_t>(i)] = static_cast<uint8_t>(mark_a);
        (*mb)[static_cast<size_t>(j)] = static_cast<uint8_t>(mark_b);
    }
    for (int j = 0; j < static_cast<int>(eb.size()); ++j)
        if (!matched[static_cast<size_t>(j)] && b.EntryVisible(j))
            (*mb)[static_cast<size_t>(j)] = static_cast<uint8_t>(M::OnlyHere);
    for (size_t i = 0; i < ma->size(); ++i) if (a.EntryVisible(static_cast<int>(i))) ++result.counts_a[(*ma)[i]];
    for (size_t j = 0; j < mb->size(); ++j) if (b.EntryVisible(static_cast<int>(j))) ++result.counts_b[(*mb)[j]];
    result.marks_a = std::move(ma);
    result.marks_b = std::move(mb);
    return result;
}

std::wstring LayoutTabTitle(const LayoutTab& tab) {
    if (!tab.title.empty()) return tab.title;
    const Tab* view = tab.ActiveFolder();
    if (!view) return {};
    std::wstring name = view->virtual_title.empty()
        ? TabTitle(view->current_path) : view->virtual_title;
    const size_t n = tab.layout == LayoutPreset::Custom ? tab.VisiblePanes().size() : LayoutPresetCount(tab.layout);
    if (n > 1) {
        name += L" · ";
        name += std::to_wstring(n);
    }
    return name;
}

void FillWindowTabStrip(ui::WindowViewModel& vm, const WindowTabs& tabs) {
    vm.tab_groups.clear();
    vm.tabs.clear();
    vm.tab_groups.reserve(tabs.tab_groups.size());
    for (const auto& g : tabs.tab_groups) {
        ui::TabGroupView gv;
        gv.id = g.id;
        gv.name = g.name;
        gv.color_rgb = g.color_rgb;
        gv.collapsed = g.collapsed;
        vm.tab_groups.push_back(std::move(gv));
    }
    vm.tabs.reserve(tabs.items.size());
    for (size_t i = 0; i < tabs.items.size(); ++i) {
        ui::TabView tv;
        tv.title = LayoutTabTitle(*tabs.items[i]);
        tv.active = i == tabs.active;
        tv.pinned = tabs.items[i]->pinned;
        tv.marker_rgb = tabs.items[i]->marker_rgb;
        tv.id = reinterpret_cast<uintptr_t>(tabs.items[i].get());
        if (tabs.items[i]->tab_group != 0) {
            for (size_t gi = 0; gi < tabs.tab_groups.size(); ++gi) {
                if (tabs.tab_groups[gi].id == tabs.items[i]->tab_group) {
                    tv.group = static_cast<int>(gi);
                    tv.color_rgb = tabs.tab_groups[gi].color_rgb;
                    tv.hidden = tabs.tab_groups[gi].collapsed;
                    break;
                }
            }
        }
        vm.tabs.push_back(std::move(tv));
    }
    vm.active_tab = static_cast<int>(tabs.active);
}

// Sidebar 标签 (pulse_tag_redesign §4): the favorites in slot order, then the
// other tags under their groups (or every tag by group, or favorites only),
// and 所有标签 › last. A group header row folds its tags; without groups the
// tags list flat as before.
static void AppendSidebarTags(ui::SidebarGroup& out, const PlacesCatalog& places) {
    const auto tag_row = [&](size_t index, int indent) {
        const auto& t = places.tags[index];
        ui::SidebarItem tag;
        tag.label = t.name;
        tag.path = MakeTagPath(t.id);
        tag.is_tag = true;
        tag.show_count = true;
        tag.count = static_cast<int>(t.paths.size());
        tag.tag_dot = ui::HexColor(t.rgb);
        tag.indent = indent;
        return tag;
    };
    const auto header_row = [](const std::wstring& id, const std::wstring& name,
                               bool expandable, bool expanded, int count) {
        ui::SidebarItem item;
        item.label = name;
        item.path = MakeTagGroupPath(id);
        item.icon_glyph = L"\xE8B7";
        item.expandable = expandable;
        item.expanded = expanded;
        item.show_count = true;
        item.count = count;
        return item;
    };
    const TagSidebarMode mode = places.tag_sidebar_mode;
    if (mode != TagSidebarMode::AllByGroup) {
        for (int slot = 1; slot <= kMaxFavoriteTags; ++slot) {
            if (const int index = places.FavoriteTagIndex(slot); index >= 0)
                out.items.push_back(tag_row(static_cast<size_t>(index), 0));
        }
    }
    if (mode != TagSidebarMode::FavoritesOnly) {
        const bool skip_favorites = mode == TagSidebarMode::FavoritesAndGroups;
        std::vector<size_t> loose;
        for (size_t i = 0; i < places.tags.size(); ++i) {
            const auto& t = places.tags[i];
            if (!(skip_favorites && t.favorite > 0) && places.TagGroupOf(t).empty()) loose.push_back(i);
        }
        for (const auto& group : places.tag_groups) {
            std::vector<size_t> members;
            for (size_t i = 0; i < places.tags.size(); ++i) {
                const auto& t = places.tags[i];
                if (!(skip_favorites && t.favorite > 0) && places.TagGroupOf(t) == group.id)
                    members.push_back(i);
            }
            out.items.push_back(header_row(group.id, group.name, true, !group.collapsed,
                                           static_cast<int>(members.size())));
            if (group.collapsed) continue;
            for (size_t i : members) out.items.push_back(tag_row(i, 1));
        }
        const bool grouped = !places.tag_groups.empty();
        if (grouped && !loose.empty()) {
            out.items.push_back(header_row(L"", l10n::Get(l10n::StringId::TagUngrouped), false, true,
                                           static_cast<int>(loose.size())));
        }
        for (size_t i : loose) out.items.push_back(tag_row(i, grouped ? 1 : 0));
    }
    ui::SidebarItem all;
    all.label = l10n::Get(l10n::StringId::TagAllTags);
    all.path = MakeTagManagerPath();
    all.icon_glyph = L"\xE8FD";
    out.items.push_back(std::move(all));
}

ui::WindowViewModel BuildWindowViewModel(const Pane& pane,
                                         const SidebarModel& sidebar, bool focused,
                                         bool maximized, bool dark, const PlacesCatalog* places,
                                         uint32_t sidebar_collapsed_mask,
                                         uint32_t sidebar_hidden_mask,
                                         bool starred_expanded,
                                         const std::vector<int>* sidebar_order,
                                         uint32_t quick_access_hidden_mask) {
    ui::WindowViewModel vm;
    const Tab* tab = pane.ActiveTab();
    if (!tab) return vm;
    // Sections are keyed by logical id and emitted in the user's order, so the
    // masks stay valid after a header drag reorders them.
    const std::vector<int> order =
        NormalizeSidebarOrder(sidebar_order ? *sidebar_order : std::vector<int>());
    std::array<ui::SidebarGroup, static_cast<size_t>(kSidebarSectionCount)> sections;
    for (int id = 0; id < kSidebarSectionCount; ++id) {
        sections[static_cast<size_t>(id)].id = id;
        sections[static_cast<size_t>(id)].collapsed = ((sidebar_collapsed_mask >> id) & 1u) != 0;
        sections[static_cast<size_t>(id)].hidden = ((sidebar_hidden_mask >> id) & 1u) != 0;
    }
    auto& workspaces = sections[static_cast<size_t>(SidebarSectionId::Workspaces)];
    auto& access = sections[static_cast<size_t>(SidebarSectionId::QuickAccess)];
    auto& savedSearches = sections[static_cast<size_t>(SidebarSectionId::SavedSearches)];
    auto& cloud = sections[static_cast<size_t>(SidebarSectionId::Cloud)];
    auto& drives = sections[static_cast<size_t>(SidebarSectionId::Drives)];
    auto& tags = sections[static_cast<size_t>(SidebarSectionId::Tags)];
    auto& nets = sections[static_cast<size_t>(SidebarSectionId::Networks)];
    auto& starred_section = sections[static_cast<size_t>(SidebarSectionId::Starred)];

    vm.focused = focused;
    vm.maximized = maximized;
    vm.dark = dark;
    vm.can_go_back = tab->CanGoBack();
    vm.can_go_forward = tab->CanGoForward();
    vm.paste_target = !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path) && !tab->net_readonly &&
        !IsUncServerRoot(tab->current_path);
    {   // Cut / copy / rename / delete by what is selected (a drive, a share, ...).
        const SelectionCaps caps = ComputeSelectionCaps(*tab, places);
        vm.selection_cut = caps.cut;
        vm.selection_copy = caps.copy;
        vm.selection_rename = caps.rename;
        vm.selection_delete = caps.del;
    }
    vm.clipboard_files = ops::ClipboardHasPasteableFiles();

    FillPaneViewModel(vm.pane, pane, places);
    vm.pane.focused = focused;

    vm.status.status_text = StatusText(*tab);
    vm.status.selection_text = SelectionText(*tab);
    vm.status.query_active=tab->search_content_active;
    vm.status.query_cancellable = !tab->search_content_stopped &&
        (tab->search_input_content || tab->content_results || tab->search_content_active) &&
        (tab->search_content_active || tab->search_awaiting_content || tab->search_live_generation);
    if (vm.status.query_active) {
        vm.status.query_text=l10n::Get(l10n::StringId::Searching);
        if (tab->content_total_files) {
            const auto scanned=std::min(tab->content_scanned_files,tab->content_total_files);
            vm.status.query_progress=static_cast<float>(static_cast<double>(scanned)/static_cast<double>(tab->content_total_files));
            wchar_t progress[128]{};
            swprintf_s(progress,l10n::Get(l10n::StringId::ContentQueryProgress).c_str(),
                static_cast<unsigned long long>(scanned),static_cast<unsigned long long>(tab->content_total_files));
            const unsigned percent = scanned == tab->content_total_files ? 100u :
                std::min(99u, static_cast<unsigned>(static_cast<double>(scanned) * 100.0 /
                    static_cast<double>(tab->content_total_files)));
            vm.status.query_text=std::to_wstring(percent)+L"% · "+progress;
        }
    }

    workspaces.header = l10n::Get(l10n::StringId::SidebarWorkspaces);
    workspaces.add_action = ui::SidebarAddAction::SaveWorkspace;
    workspaces.badge_count = places ? static_cast<int>(places->workspaces.size()) : 0;
    if (places) {
        for (int i = 0; i < static_cast<int>(places->workspaces.size()); ++i) {
            const auto& w = places->workspaces[static_cast<size_t>(i)];
            ui::SidebarItem it;
            it.label = w.name.empty() ? TabTitle(w.root) : w.name;
            it.path = MakeWorkspacePath(i);
            it.workspace_index = i;
            it.workspace_root = w.root;
            it.workspace_layout = w.layout;
            it.workspace_active = i == places->active_workspace;
            it.workspace_color_rgb = w.color_rgb;
            it.workspace_panes = w.pane_paths.empty() ? std::vector<std::wstring>{w.root} : w.pane_paths;
            it.workspace_views = w.pane_views;
            it.expandable = true;
            it.expanded = w.expanded;
            it.icon_color = ui::HexColor(w.color_rgb);
            for (const auto& path : it.workspace_panes) {
                if (!it.detail.empty()) it.detail += L" \x00B7 ";
                it.detail += TabTitle(path);
            }
            if (it.workspace_active) it.badge = l10n::Get(l10n::StringId::Current);
            else if (fs::IsUncPath(w.root)) it.badge = l10n::Get(l10n::StringId::Server);
            workspaces.items.push_back(std::move(it));
            if (w.expanded) {
                const auto paths = w.pane_paths.empty() ? std::vector<std::wstring>{w.root} : w.pane_paths;
                for (size_t j = 0; j < paths.size(); ++j) {
                    ui::SidebarItem sub;
                    sub.label = TabTitle(paths[j]);
                    sub.path = paths[j];
                    sub.indent = 1;
                    sub.fallback_text = std::to_wstring(j + 1);
                    sub.workspace_child_number = static_cast<int>(j + 1);
                    sub.workspace_parent_index = i;
                    sub.workspace_child_view = j < w.pane_views.size() ? w.pane_views[j] : ui::ViewMode::Details;
                    sub.icon_glyph = L"\xE8B7";
                    sub.icon_color = ui::HexColor(w.color_rgb);
                    workspaces.items.push_back(std::move(sub));
                }
                for (const auto& child : places->FrequentChildren(i, 8)) {
                    if (std::find(paths.begin(), paths.end(), child) != paths.end()) continue;
                    ui::SidebarItem sub;
                    sub.label = TabTitle(child);
                    sub.path = child;
                    sub.indent = 1;
                    sub.icon_glyph = L"\xE8B7";
                    sub.icon_color = ui::HexColor(0x94A3B8);
                    workspaces.items.push_back(std::move(sub));
                }
            }
        }
    }

    if (workspaces.items.empty()) {
        ui::SidebarItem empty;
        empty.workspace_empty = true;
        empty.label = l10n::Get(l10n::StringId::WorkspaceEmptyHint);
        empty.detail = l10n::Get(l10n::StringId::WorkspaceSaveCurrent);
        workspaces.items.push_back(std::move(empty));
    }

    // Quick access: the built-in links (minus the ones switched off) and the
    // pinned folders, mixed in the user's order when there is one. The Git
    // project row goes in front of the first pin, where it has always been.
    access = ConvertGroup(l10n::Get(l10n::StringId::SidebarQuickAccess), {}, false);
    access.add_action = ui::SidebarAddAction::AddQuickAccess;
    size_t first_pin_row = static_cast<size_t>(-1);
    for (const auto& key : QuickAccessDisplayKeys(sidebar.quick_access, places,
                                                  quick_access_hidden_mask, false)) {
        const int builtin = BuiltinQuickAccessFromKey(key);
        if (builtin >= 0) {
            const auto entry = std::find_if(sidebar.quick_access.begin(), sidebar.quick_access.end(),
                [&](const SidebarEntry& candidate) { return candidate.builtin == builtin; });
            if (entry == sidebar.quick_access.end()) continue;
            ui::SidebarGroup converted = ConvertGroup({}, {*entry}, false);
            access.items.push_back(std::move(converted.items.front()));
            continue;
        }
        if (first_pin_row == static_cast<size_t>(-1)) first_pin_row = access.items.size();
        ui::SidebarItem pin;
        pin.label = TabTitle(key);
        pin.path = key;
        pin.icon_glyph = L"\xE8B7";
        pin.fallback_text = L"Dir";
        pin.icon_color = ui::HexColor(0xFBBF24);
        access.items.push_back(std::move(pin));
    }
    starred_section = ConvertGroup(l10n::Get(l10n::StringId::StarredItems),
                                   sidebar.starred, false);
    if (!starred_section.items.empty()) {
        starred_section.items[0].expandable = true;
        starred_section.items[0].expanded = starred_expanded;
    }
    if (places && starred_expanded) {
        size_t starred_insert = starred_section.items.size();
        const auto add = [&](ui::SidebarItem row) {
            starred_section.items.insert(
                starred_section.items.begin() + static_cast<std::ptrdiff_t>(starred_insert),
                std::move(row));
            ++starred_insert;
        };
        const auto starred_row = [](const StarredItem& entry, int indent) {
            ui::SidebarItem child;
            child.label = TabTitle(entry.path);
            child.path = entry.path;
            child.indent = indent;
            child.starred_child = true;
            child.icon_glyph = entry.kind == PlaceItemKind::File ? L"\xE8A5" : L"\xE8B7";
            child.fallback_text = entry.kind == PlaceItemKind::File ? L"File" : L"Dir";
            child.icon_color = ui::HexColor(0xFBBF24);
            child.badge = entry.badge;
            child.badge_color = ui::HexColor(entry.badge_rgb);
            return child;
        };
        // 收藏夹分组与分隔线: a group folds its folders under one row; a
        // separator is a line above the next row.
        bool line = false;
        for (const auto& row : places->StarredRows(false)) {
            if (row.kind == StarredRow::Kind::Separator) {
                line = true;
                continue;
            }
            if (row.kind == StarredRow::Kind::Group) {
                const StarredGroup* group = places->FindStarredGroup(row.group);
                if (!group) continue;
                ui::SidebarItem header;
                header.label = group->name;
                header.path = MakeStarredGroupPath(group->id);
                header.indent = 1;
                header.icon_glyph = L"\xE8F1";
                header.fallback_text = L"Grp";
                header.icon_color = ui::HexColor(0xFBBF24);
                header.expandable = true;
                header.expanded = !group->collapsed;
                header.show_count = true;
                header.count = static_cast<int>(row.members.size());
                header.divider_above = std::exchange(line, false);
                add(std::move(header));
                if (group->collapsed) continue;
                for (size_t member : row.members) add(starred_row(places->starred_items[member], 2));
                continue;
            }
            ui::SidebarItem child = starred_row(places->starred_items[row.index], 1);
            child.divider_above = std::exchange(line, false);
            add(std::move(child));
        }
    }
    const std::wstring& gitRoot = tab->git_root;
    // A pinned repository already has its row.
    if (!gitRoot.empty() &&
        (!places || (!places->IsStarred(gitRoot) && !places->IsQuickAccessPinned(gitRoot)))) {
        ui::SidebarItem project;
        wchar_t project_label[512]{};
        swprintf_s(project_label, l10n::Get(l10n::StringId::ProjectFormat).c_str(),
                   TabTitle(gitRoot).c_str());
        project.label = project_label;
        project.path = gitRoot;
        project.icon_glyph = L"\xE8B7";
        project.fallback_text = L"Repo";
        project.icon_color = ui::HexColor(0x34D399);
        // The branch (and how far it is from its upstream) once git answered.
        const auto git = GitStatusFor(gitRoot);
        project.badge = git ? GitBadgeText(*git) : std::wstring(L"Git");
        const size_t project_at = (std::min)(first_pin_row, access.items.size());
        access.items.insert(access.items.begin() + static_cast<std::ptrdiff_t>(project_at),
                            std::move(project));
    }
    savedSearches = ConvertGroup(l10n::Get(l10n::StringId::SidebarSavedSearches),
                                 sidebar.saved_searches, false);
    // The title is only used by the section menus: the pane itself shows the
    // account rows without a header (see IsHeaderlessSection).
    cloud = ConvertGroup(l10n::Get(l10n::StringId::SidebarCloudDrives), sidebar.cloud, false);
    if (places) {
        // Persisted badges win over the freshly rebuilt sidebar model (#41).
        for (auto* group : { &access, &cloud }) {
            for (auto& item : group->items) {
                if (const QuickAccessBadge* badge = places->FindQuickAccessBadge(item.path)) {
                    item.badge = badge->badge;
                    item.badge_color = ui::HexColor(badge->badge_rgb);
                }
            }
        }
    }
    drives = ConvertGroup(l10n::Get(l10n::StringId::SidebarDrives),
                          sidebar.drives, false);
    // Phones follow the drives, as in File Explorer's This PC.
    for (auto& phone : ConvertGroup(std::wstring(), sidebar.devices, false).items)
        drives.items.push_back(std::move(phone));
    // Section icons: every header section names itself with a glyph, so the wide
    // header and the narrow rail read the same. The PC glyph (monitor on a
    // stand) keeps "This PC" apart from the monitor-only Desktop row.
    workspaces.icon_glyph = L"\xF5BE";    // shared workspace layout icon
    access.icon_glyph = L"\xE8A9";        // app grid
    savedSearches.icon_glyph = L"\xE721"; // search
    tags.icon_glyph = L"\xE8EC";          // tag
    drives.icon_glyph = L"\xE977";        // this PC
    drives.navigable = true;             // the title opens This PC (#80)
    nets.icon_glyph = L"\xE968";          // network

    tags.header = l10n::Get(l10n::StringId::SidebarTags);
    tags.add_action = ui::SidebarAddAction::CreateTag;
    if (places) AppendSidebarTags(tags, *places);
    nets.header = l10n::Get(l10n::StringId::SidebarNetworkLocations);
    nets.add_action = ui::SidebarAddAction::AddNetwork;
    if (places) {
        for (const auto& n : places->networks) {
            ui::SidebarItem it;
            it.label = n.name.empty() ? TabTitle(n.unc) : n.name;
            it.detail = DisplayPath(n.unc);
            it.path = n.unc;
            it.icon_glyph = L"\xE968";
            it.fallback_text = L"Net";
            it.icon_color = ui::HexColor(0x38BDF8);
            it.status_dot = n.status != fs::NetStatus::Unknown;
            if (n.status == fs::NetStatus::Online) it.status_color = ui::HexColor(0x22C55E);
            else if (n.status == fs::NetStatus::Slow) it.status_color = ui::HexColor(0xF59E0B);
            else if (n.status == fs::NetStatus::Offline) it.status_color = ui::HexColor(0x94A3B8);
            nets.items.push_back(std::move(it));
        }
    }
    AppendSystemNetworkLocations(nets, sidebar.system_networks);
    // Emit the sections in the user's order. Ids and the collapse/hide bits are
    // re-applied here because ConvertGroup builds fresh groups.
    vm.sidebar.reserve(sections.size());
    for (int id : order) {
        auto& section = sections[static_cast<size_t>(id)];
        section.id = id;
        // Header-less sections drop the title ConvertGroup set: the rows are the
        // section, so a header above them would just repeat their own name.
        if (IsHeaderlessSection(static_cast<SidebarSectionId>(id))) section.header.clear();
        section.collapsed = ((sidebar_collapsed_mask >> id) & 1u) != 0;
        section.hidden = ((sidebar_hidden_mask >> id) & 1u) != 0;
        vm.sidebar.push_back(std::move(section));
    }

    return vm;
}

} // namespace pulse::app
