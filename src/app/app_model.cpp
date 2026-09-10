// app_model.cpp
#include "app_model.h"
#include "../common/json_utils.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include "../common/text_format.h"
#include <commctrl.h>
#include <prsht.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>
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
void Tab::NavigateTo(const std::wstring& path) {
    ++view_generation;
    if (!current_path.empty()) back_stack.push(current_path);
    while (!forward_stack.empty()) forward_stack.pop();
    current_path = path;
    ClearSelection();
    scroll_y = 0.0f;
    scroll_x = 0.0f;
    filter_text.clear();
    virtual_title.clear();
    banner_title.clear();
    banner_message.clear();
    net_readonly = false;
    git_root.clear();
    cache_unix = 0;
    loading = true;
}

void Tab::SetSnapshot(fs::SnapshotPtr value) {
    snapshot = std::move(value);
    view_cache_snapshot.reset();
    view_cache_places = nullptr;
    view_cache_places_revision = 0;
    view_cache_filter_text.clear();
    view_filter_map.reset();
    view_tag_dots.reset();
    snapshot_path = snapshot ? current_path : L"";
    if (all_selected && !show_hidden_files) MaterializeSelection();
    std::erase_if(selected, [&](int i) { return !EntryVisible(i); });
    if (selected_index >= 0 && !EntryVisible(selected_index)) {
        selected_index = selected.empty() ? -1 : *selected.begin();
        selection_anchor = selected_index;
    }
    directory_count = 0;
    file_count = 0;
    if (!snapshot) return;

    for (const auto& entry : *snapshot) {
        if (!show_hidden_files && !current_path.starts_with(L"pulse:recycle") &&
            (entry.attrs & FILE_ATTRIBUTE_HIDDEN)) continue;
        if (entry.is_dir) ++directory_count;
        else ++file_count;
    }
}

bool Tab::EntryVisible(int index) const {
    if (!snapshot || index < 0 || index >= static_cast<int>(snapshot->size())) return false;
    // Recycle Bin lists payloads whose hidden attributes belong to Windows.
    return show_hidden_files || current_path.starts_with(L"pulse:recycle") ||
        (((*snapshot)[static_cast<size_t>(index)].attrs & FILE_ATTRIBUTE_HIDDEN) == 0);
}

void Tab::SetShowHiddenFiles(bool show) {
    if (show_hidden_files == show) return;
    show_hidden_files = show;
    ClearSelection();
    SetSnapshot(snapshot);
    scroll_y = 0.0f;
    ++view_generation;
}

void Tab::ClearSelection() {
    selected_index = -1;
    selection_anchor = -1;
    all_selected = false;
    selected.clear();
}

int Tab::CountBound() const {
    if (search_retaining_results) return 0;
    return snapshot ? static_cast<int>(snapshot->size()) : 0;
}

void Tab::MaterializeSelection() {
    if (!all_selected) return;
    all_selected = false;
    selected.clear();
    const int n = CountBound();
    selected.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) if (EntryVisible(i)) selected.insert(i);
}

void Tab::SelectOnly(int index) {
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
        if (selection_anchor < 0) selection_anchor = index;
    }
}

void Tab::SelectRange(int from, int to) {
    const int n = CountBound();
    if (n <= 0) {
        ClearSelection();
        return;
    }
    from = std::clamp(from, 0, n - 1);
    to = std::clamp(to, 0, n - 1);
    all_selected = false;
    selected.clear();
    const int lo = std::min(from, to);
    const int hi = std::max(from, to);
    selected.reserve(static_cast<size_t>(hi - lo + 1));
    for (int i = lo; i <= hi; ++i) if (EntryVisible(i)) selected.insert(i);
    selected_index = to;
    if (selection_anchor < 0) selection_anchor = from;
}

void Tab::SelectAll() {
    if (!show_hidden_files) {
        std::vector<int> visible;
        for (int i = 0; i < CountBound(); ++i) if (EntryVisible(i)) visible.push_back(i);
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
        if (index >= 0 && index < n && EntryVisible(index)) uni.insert(index);
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

void Tab::MoveFocus(int index, bool extend) {
    const int n = CountBound();
    if (n <= 0) {
        ClearSelection();
        return;
    }
    index = std::clamp(index, 0, n - 1);
    if (extend) {
        if (selection_anchor < 0)
            selection_anchor = selected_index >= 0 ? selected_index : index;
        SelectRange(selection_anchor, index);
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

void Tab::RemapSelection(const std::vector<std::wstring>& names, const std::wstring& focus_name) {
    ClearSelection();
    const int n = CountBound();
    if (n <= 0 || names.empty()) {
        if (n > 0) SelectOnly(0);
        return;
    }
    std::unordered_set<std::wstring> want(names.begin(), names.end());
    for (int i = 0; i < n; ++i) {
        if (!EntryVisible(i) || !want.contains((*snapshot)[static_cast<size_t>(i)].name)) continue;
        selected.insert(i);
        if ((*snapshot)[static_cast<size_t>(i)].name == focus_name) selected_index = i;
    }
    if (selected.empty()) {
        SelectOnly(0);
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
    forward_stack.push(current_path);
    ++view_generation;
    current_path = back_stack.top();
    back_stack.pop();
    ClearSelection();
    scroll_y = 0.0f;
    scroll_x = 0.0f;
    filter_text.clear();
    virtual_title.clear();
    loading = true;
    return current_path;
}

std::wstring Tab::GoForward() {
    if (forward_stack.empty()) return current_path;
    back_stack.push(current_path);
    ++view_generation;
    current_path = forward_stack.top();
    forward_stack.pop();
    ClearSelection();
    scroll_y = 0.0f;
    scroll_x = 0.0f;
    filter_text.clear();
    virtual_title.clear();
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
    view = Tab{};
    view.view_mode = mode;
    view.details_column_dividers = columns;
    view.search_column_dividers = search_columns;
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
        pane->view.search_column_dividers = source->search_column_dividers;
    }
    pane->NewTab(path.empty() ? L"C:\\" : path);
    tab->panes.push_back(std::move(pane));
    tab->root = SplitContainer::CreateLeaf(tab->panes[0].get());
    tab->layout = LayoutPreset::Single;
    tab->focused_index = 0;
    return tab;
}

void RebuildLayoutRoot(LayoutTab& tab) {
    const size_t n = LayoutPresetCount(tab.layout);
    std::vector<Pane*> used;
    used.reserve(n);
    for (size_t i = 0; i < n && i < tab.panes.size(); ++i)
        used.push_back(tab.panes[i].get());
    if (used.empty() && !tab.panes.empty()) used.push_back(tab.panes[0].get());
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
    if (items[idx]->pinned) return;
    items.erase(items.begin() + static_cast<ptrdiff_t>(idx));
    if (active >= items.size()) active = items.size() - 1;
    else if (idx < active) --active;
}

void WindowTabs::SwitchTab(size_t idx) {
    if (idx < items.size()) active = idx;
}

void WindowTabs::MoveTab(size_t from, size_t to) {
    if (from >= items.size() || to >= items.size() || from == to) return;

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

float ClampSplitRatio(float ratio, const D2D1_RECT_F& bounds, SplitOrientation orientation,
                      float gap) {
    const float g = std::max(0.0f, gap);
    const float span = (orientation == SplitOrientation::Vertical)
        ? std::max(0.0f, bounds.right - bounds.left - g)
        : std::max(0.0f, bounds.bottom - bounds.top - g);
    const float minPx = std::min(std::max(80.0f, g * 20.0f), span * 0.35f);
    float lo = span > 1.0f ? std::clamp(minPx / span, 0.08f, 0.45f) : 0.12f;
    float hi = 1.0f - lo;
    if (lo >= hi) {
        lo = 0.12f;
        hi = 0.88f;
    }
    return std::clamp(ratio, lo, hi);
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

void LayoutSplitTree(const SplitContainer& node, const D2D1_RECT_F& bounds, float gap,
                     std::vector<std::pair<Pane*, D2D1_RECT_F>>& out,
                     std::vector<SplitterLayout>* splitters) {
    if (node.is_leaf) {
        if (node.pane) out.push_back({node.pane, bounds});
        return;
    }
    const float g = std::max(0.0f, gap);
    const float span = (node.orientation == SplitOrientation::Vertical)
        ? std::max(0.0f, bounds.right - bounds.left - g)
        : std::max(0.0f, bounds.bottom - bounds.top - g);
    const float ratio = ClampSplitRatio(node.ratio, bounds, node.orientation, gap);
    D2D1_RECT_F a = bounds;
    D2D1_RECT_F b = bounds;
    D2D1_RECT_F hit = bounds;
    if (node.orientation == SplitOrientation::Vertical) {
        const float mid = bounds.left + span * ratio;
        a.right = mid;
        b.left = mid + g;
        const float hitHalf = std::max(g, 8.0f) * 0.5f;
        const float center = mid + g * 0.5f;
        hit.left = center - hitHalf;
        hit.right = center + hitHalf;
    } else {
        const float mid = bounds.top + span * ratio;
        a.bottom = mid;
        b.top = mid + g;
        const float hitHalf = std::max(g, 8.0f) * 0.5f;
        const float center = mid + g * 0.5f;
        hit.top = center - hitHalf;
        hit.bottom = center + hitHalf;
    }
    if (splitters) {
        SplitterLayout slot;
        slot.node = const_cast<SplitContainer*>(&node);
        slot.hit_rect = hit;
        slot.parent_bounds = bounds;
        slot.orientation = node.orientation;
        splitters->push_back(slot);
    }
    if (node.first) LayoutSplitTree(*node.first, a, gap, out, splitters);
    if (node.second) LayoutSplitTree(*node.second, b, gap, out, splitters);
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

static std::wstring TabTitle(const std::wstring& path) {
    if (path.empty()) return l10n::Get(l10n::StringId::ThisPc);
    std::wstring kind, rest;
    if (ParsePulsePath(path, &kind, &rest)) {
        if (kind == L"settings") return l10n::Get(l10n::StringId::Settings);
        if (kind == L"starred") return l10n::Get(l10n::StringId::StarredItems);
        if (kind == L"recent") return l10n::Get(l10n::StringId::Recent);
        if (kind == L"recycle") return l10n::Get(l10n::StringId::RecycleBin);
        if (kind == L"search") return l10n::Get(l10n::StringId::Search);
        if (kind == L"tag") return rest.empty() ? l10n::Get(l10n::StringId::Tag) : rest;
    }
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
    if (path.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + path.substr(8);
    if (path.starts_with(L"\\\\?\\")) return path.substr(4);
    return path;
}

// ---------------------------------------------------------------------------
// StagingTray
// ---------------------------------------------------------------------------
void StagingTray::Collect(const std::vector<std::wstring>& paths, bool move_intent) {
    if (paths.empty()) return;
    TrayBatch batch;
    batch.move_intent = move_intent;
    batch.total_size = 0;
    for (const auto& p : paths) {
        TrayItem it;
        it.path = fs::NormalizePath(p);
        // One probe covers existence, icon attrs and the rough size sum.
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(it.path.c_str(), GetFileExInfoStandard, &fad)) {
            it.exists = true;
            it.attrs = fad.dwFileAttributes;
            it.is_dir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!it.is_dir) {
                ULARGE_INTEGER sz;
                sz.LowPart = fad.nFileSizeLow;
                sz.HighPart = fad.nFileSizeHigh;
                batch.total_size += sz.QuadPart;
            }
        } else {
            it.exists = false;
        }
        batch.items.push_back(std::move(it));
    }
    batches_.push_back(std::move(batch));
}

void StagingTray::RemoveBatch(size_t idx) {
    if (idx < batches_.size()) batches_.erase(batches_.begin() + idx);
}

void StagingTray::RemoveItem(size_t batch_idx, size_t item_idx) {
    if (batch_idx >= batches_.size()) return;
    auto& b = batches_[batch_idx];
    if (item_idx < b.items.size()) b.items.erase(b.items.begin() + item_idx);
    if (b.items.empty()) batches_.erase(batches_.begin() + batch_idx);
}

void StagingTray::Clear() { batches_.clear(); }

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
    batches_.clear();
    // Minimal parser: enough for our own serialization.
    size_t i = in.find(L'[');
    if (i == std::wstring::npos) return false;
    ++i;
    while (i < in.size()) {
        while (i < in.size() && (in[i] == L' ' || in[i] == L'\n' || in[i] == L'\r' || in[i] == L'\t' || in[i] == L',')) ++i;
        if (i >= in.size() || in[i] == L']') break;
        if (in[i] != L'{') return false;
        ++i;
        TrayBatch batch;
        while (i < in.size() && in[i] != L'}') {
            while (i < in.size() && (in[i] == L' ' || in[i] == L'\n' || in[i] == L'\r' || in[i] == L'\t' || in[i] == L',')) ++i;
            size_t keyEnd = in.find(L'"', i + 1);
            if (keyEnd == std::wstring::npos) return false;
            std::wstring key = in.substr(i + 1, keyEnd - i - 1);
            i = keyEnd + 1;
            while (i < in.size() && in[i] != L':') ++i;
            if (i >= in.size()) return false;
            ++i;
            while (i < in.size() && (in[i] == L' ' || in[i] == L'\n' || in[i] == L'\r' || in[i] == L'\t')) ++i;
            if (key == L"move") {
                batch.move_intent = (i + 4 <= in.size() && in.compare(i, 4, L"true") == 0);
                if (batch.move_intent) i += 4; else i += 5;
            } else if (key == L"items") {
                while (i < in.size() && in[i] != L'[') ++i;
                if (i >= in.size()) return false;
                ++i;
                while (i < in.size() && in[i] != L']') {
                    while (i < in.size() && (in[i] == L' ' || in[i] == L'\n' || in[i] == L'\r' || in[i] == L'\t' || in[i] == L',')) ++i;
                    if (in[i] != L'"') { ++i; continue; }
                    ++i;
                    std::wstring path;
                    while (i < in.size() && in[i] != L'"') {
                        if (in[i] == L'\\' && i + 1 < in.size()) {
                            ++i;
                            if (in[i] == L'n') path += L'\n';
                            else if (in[i] == L'r') path += L'\r';
                            else if (in[i] == L't') path += L'\t';
                            else path += in[i];
                        } else {
                            path += in[i];
                        }
                        ++i;
                    }
                    if (i < in.size()) ++i;
                    TrayItem it;
                    it.path = path;
                    const DWORD attrs = GetFileAttributesW(path.c_str());
                    it.exists = attrs != INVALID_FILE_ATTRIBUTES;
                    it.attrs = it.exists ? attrs : 0;
                    it.is_dir = it.exists && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    batch.items.push_back(std::move(it));
                }
                if (i < in.size()) ++i;
            } else {
                while (i < in.size() && in[i] != L',' && in[i] != L'}') ++i;
            }
        }
        if (i < in.size()) ++i;
        if (!batch.items.empty()) batches_.push_back(std::move(batch));
    }
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

SidebarModel BuildSidebarModel(const fs::RecycleBinInfo* recycle) {
    SidebarModel m;
    SidebarEntry starred;
    starred.glyph = L"\xE735";
    starred.fallback = L"Starred";
    starred.color = ui::HexColor(0xFBBF24);
    starred.label = l10n::Get(l10n::StringId::StarredItems);
    starred.path = MakeStarredPath();
    starred.expandable = true;
    m.quick_access.push_back(std::move(starred));
    SidebarEntry recent;
    recent.glyph = L"\xE823";
    recent.fallback = L"Recent";
    recent.color = ui::HexColor(0x60A5FA);
    recent.label = l10n::Get(l10n::StringId::Recent);
    recent.path = MakeRecentPath();
    m.quick_access.push_back(std::move(recent));
    SidebarEntry desktop = MakeKnownEntry(FOLDERID_Desktop, L"\xE7F4", L"Desktop",
        ui::HexColor(0x38BDF8), l10n::Get(l10n::StringId::Desktop).c_str());
    desktop.badge = l10n::Get(l10n::StringId::Desktop);
    desktop.badge_rgb = 0x0078D4;
    m.quick_access.push_back(std::move(desktop));
    m.quick_access.push_back(MakeKnownEntry(FOLDERID_Downloads, L"\xE896", L"Downloads",
        ui::HexColor(0xC084FC), L"Downloads"));
    SidebarEntry recycle_bin;
    recycle_bin.glyph = L"\xE75C";
    recycle_bin.fallback = L"Bin";
    recycle_bin.color = ui::HexColor(0x94A3B8);
    recycle_bin.label = l10n::Get(l10n::StringId::RecycleBin);
    recycle_bin.path = MakeRecyclePath();
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
        ULARGE_INTEGER freeBytes{}, totalBytes{};
        GetDiskFreeSpaceExW(root, &freeBytes, &totalBytes, nullptr);
        SidebarEntry e;
        e.label = std::wstring(volName[0] ? volName : l10n::Get(l10n::StringId::LocalDisk).c_str()) +
                  L" (" + root[0] + L":)";
        uint64_t total = totalBytes.QuadPart;
        uint64_t free = freeBytes.QuadPart;
        uint64_t used = total > free ? total - free : 0;
        e.detail = pulse::format::ByteSize(used, false, pulse::format::ByteSizeStyle::Compact)
                 + L" / " + pulse::format::ByteSize(total, false,
                                                     pulse::format::ByteSizeStyle::Compact);
        e.glyph = L"\xE7F1"; // HardDrive (Segoe Fluent Icons)
        e.fallback = L"Drive";
        e.path = fs::NormalizePath(std::wstring(root));
        e.is_drive = true;
        e.color = ui::HexColor(kDrivePalette[m.drives.size() % 4]);
        if (total > 0) {
            e.used_ratio = (float)((double)(total - free) / (double)total);
            if ((double)free / (double)total < 0.10) e.danger = true;
        }
        m.drives.push_back(std::move(e));
    }
    return m;
}

// ---------------------------------------------------------------------------
// Build window view-model
// ---------------------------------------------------------------------------
static std::wstring PaneHeaderText(const Tab& tab) {
    return tab.virtual_title.empty() ? TabTitle(tab.current_path) : tab.virtual_title;
}

static std::wstring StatusText(const Tab& tab) {
    if (tab.loading || !tab.snapshot) return L"";
    wchar_t buf[128];
    swprintf_s(buf, l10n::Get(l10n::StringId::StatusItemsFormat).c_str(),
        tab.directory_count + tab.file_count, tab.directory_count, tab.file_count);
    return buf;
}

static std::wstring SelectionText(const Tab& tab) {
    const int count = tab.SelectedCount();
    if (count <= 0) return l10n::Get(l10n::StringId::NotSelected);
    if (count == 1 && tab.snapshot && tab.selected_index >= 0 &&
        tab.selected_index < static_cast<int>(tab.snapshot->size())) {
        const auto& e = (*tab.snapshot)[tab.selected_index];
        wchar_t buf[256];
        swprintf_s(buf, l10n::Get(l10n::StringId::SelectedOneFormat).c_str(), e.name.c_str(),
                   pulse::format::ByteSize(e.size, true).c_str());
        return buf;
    }
    wchar_t buf[64];
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
        it.fallback_text = e.fallback;
        it.icon_color = e.color;
        it.danger = e.danger;
        it.path = e.path;
        it.is_drive = e.is_drive;
        it.used_ratio = e.used_ratio;
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

static void ParseFilterText(const std::wstring& text, std::wstring& name_needle,
                            std::vector<std::wstring>& tag_needles) {
    name_needle.clear();
    tag_needles.clear();
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::iswspace(text[i])) ++i;
        if (i >= text.size()) break;
        const size_t start = i;
        while (i < text.size() && !std::iswspace(text[i])) ++i;
        std::wstring tok = text.substr(start, i - start);
        if (!tok.empty() && tok[0] == L'#' && tok.size() > 1) {
            tag_needles.push_back(ToLowerCopy(tok.substr(1)));
        } else if (!tok.empty()) {
            if (!name_needle.empty()) name_needle += L' ';
            name_needle += tok;
        }
    }
    name_needle = ToLowerCopy(name_needle);
}

static bool TagMatchesNeedle(const ColorTag& tag, int index, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (ToLowerCopy(tag.name).find(needle) != std::wstring::npos) return true;
    static const wchar_t* kNicks[] = { L"红", L"橙", L"绿", L"青", L"紫", L"蓝", L"灰" };
    if (index >= 0 && index < 7 && needle == kNicks[index]) return true;
    if (index == 1 && needle == L"黄") return true;
    return false;
}

void CollectFilterMatches(const Tab& tab, const PlacesCatalog* places, std::vector<int>& out) {
    out.clear();
    if (!tab.snapshot) return;
    const int n = static_cast<int>(tab.snapshot->size());
    if (tab.filter_text.empty() && tab.show_hidden_files) {
        out.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) if (tab.EntryVisible(i)) out.push_back(i);
        return;
    }
    std::wstring name_needle;
    std::vector<std::wstring> tag_needles;
    ParseFilterText(tab.filter_text, name_needle, tag_needles);
    if (name_needle.empty() && tag_needles.empty())
        name_needle = ToLowerCopy(tab.filter_text);
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const auto& e = (*tab.snapshot)[static_cast<size_t>(i)];
        if (!tab.EntryVisible(i)) continue;
        const bool name_ok = NameMatchesPattern(e.name, name_needle);
        bool tag_ok = tag_needles.empty();
        if (!tag_ok && places) {
            const std::wstring full = EntryPathOf(tab, e);
            const auto ids = places->TagsForPath(full);
            for (int id : ids) {
                if (id < 0 || id >= static_cast<int>(places->tags.size())) continue;
                for (const auto& needle : tag_needles) {
                    if (TagMatchesNeedle(places->tags[static_cast<size_t>(id)], id, needle)) {
                        tag_ok = true;
                        break;
                    }
                }
                if (tag_ok) break;
            }
        }
        if (name_ok && tag_ok) out.push_back(i);
    }
}

void FillPaneViewModel(ui::PaneViewModel& out, const Pane& pane, const PlacesCatalog* places) {
    const Tab* tab = pane.ActiveTab();
    if (!tab) return;
    out.path = tab->virtual_title.empty() ? DisplayPath(tab->current_path) : tab->virtual_title;
    out.header_text = PaneHeaderText(*tab);
    out.filter_text = tab->filter_text;
    out.filter_expand = pane.filter_expand;
    out.banner_title = tab->banner_title;
    out.banner_message = tab->banner_message;
    out.banner_kind = tab->net_readonly ? 2 : 0;
    out.filter_map.reset();
    out.tag_dots.reset();
    out.loading = tab->loading;
    out.search_retaining_results = tab->search_retaining_results;
    out.can_go_back = tab->CanGoBack();
    out.can_go_forward = tab->CanGoForward();
    std::wstring virtual_kind;
    ParsePulsePath(tab->current_path, &virtual_kind, nullptr);
    out.can_go_up = virtual_kind == L"recycle"
        ? true
        : (fs::IsVirtualPath(tab->current_path) ? tab->CanGoBack() : !tab->current_path.empty());
    out.is_file_system = !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path);
    out.can_create = out.is_file_system && !tab->net_readonly;
    out.curated_order = virtual_kind == L"starred" || virtual_kind == L"recent";
    out.is_starred = virtual_kind == L"starred";
    out.is_recent = virtual_kind == L"recent";
    out.is_recycle = virtual_kind == L"recycle";
    out.is_search = virtual_kind == L"search" || virtual_kind == L"saved-search"
        || virtual_kind == L"recycle";
    out.is_query_search = virtual_kind == L"search";
    if (out.is_query_search) {
        std::wstring rest;
        ParsePulsePath(tab->current_path, nullptr, &rest);
        out.search_query = rest;
        // Keep pulse:search:... so the address bar is one segment, not C:\ splits.
        out.path = tab->current_path;
    }
    out.search_snippets = tab->search_snippets;
    out.recent_filter = tab->recent_filter;
    out.recent_total = out.is_recent && places ? places->recent_items.size() : 0;
    if (virtual_kind == L"recycle") {
        out.path = MakeRecyclePath();
        out.date_column_label = l10n::Get(l10n::StringId::ColumnDeleted);
    } else {
        out.date_column_label = l10n::Get(out.is_recent
            ? l10n::StringId::ColumnRecentOpened : l10n::StringId::ColumnModified);
    }
    out.selected_index = tab->selected_index;
    out.selected_count = tab->SelectedCount();
    out.all_selected = tab->all_selected;
    out.selected_indices = tab->all_selected ? nullptr : &tab->selected;
    out.scroll_y = tab->scroll_y;
    out.scroll_x = tab->scroll_x;
    out.view_mode = tab->view_mode;
    out.view_generation = tab->view_generation;
    out.sort_column = tab->sort_column;
    out.sort_direction = tab->sort_direction;
    out.details_column_dividers = tab->details_column_dividers;
    out.search_column_dividers = tab->search_column_dividers;
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
    if (tab->snapshot && (!tab->show_hidden_files || !tab->filter_text.empty())) {
        if (!tab->view_filter_map || tab->view_cache_filter_text != tab->filter_text) {
            auto filtered = std::make_shared<ui::PaneViewModel::FilterMap>();
            CollectFilterMatches(*tab, places, *filtered);
            tab->view_cache_filter_text = tab->filter_text;
            tab->view_filter_map = std::move(filtered);
        }
        out.filter_map = tab->view_filter_map;
    }
}

std::wstring LayoutTabTitle(const LayoutTab& tab) {
    if (!tab.title.empty()) return tab.title;
    const Tab* view = tab.ActiveFolder();
    if (!view) return {};
    std::wstring name = view->virtual_title.empty()
        ? TabTitle(view->current_path) : view->virtual_title;
    const size_t n = LayoutPresetCount(tab.layout);
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

ui::WindowViewModel BuildWindowViewModel(const Pane& pane,
                                         const SidebarModel& sidebar, bool focused,
                                         bool maximized, bool dark, const PlacesCatalog* places,
                                         uint32_t sidebar_collapsed_mask,
                                         bool starred_expanded) {
    ui::WindowViewModel vm;
    const Tab* tab = pane.ActiveTab();
    if (!tab) return vm;

    vm.focused = focused;
    vm.maximized = maximized;
    vm.dark = dark;
    vm.can_go_back = tab->CanGoBack();
    vm.can_go_forward = tab->CanGoForward();

    FillPaneViewModel(vm.pane, pane, places);
    vm.pane.focused = focused;

    vm.status.status_text = StatusText(*tab);
    vm.status.selection_text = SelectionText(*tab);

    ui::SidebarGroup workspaces;
    workspaces.header = l10n::Get(l10n::StringId::SidebarWorkspaces);
    if (places) {
        for (int i = 0; i < static_cast<int>(places->workspaces.size()); ++i) {
            const auto& w = places->workspaces[static_cast<size_t>(i)];
            ui::SidebarItem it;
            it.label = w.name.empty() ? TabTitle(w.root) : w.name;
            it.detail = DisplayPath(w.root);
            it.path = MakeWorkspacePath(i);
            const bool unc = fs::IsUncPath(w.root);
            it.icon_glyph = unc ? L"\xE968" : L"\xE8B7";
            it.fallback_text = L"WS";
            it.icon_color = ui::HexColor(unc ? 0x38BDF8 : 0x34D399);
            if (unc) it.badge = l10n::Get(l10n::StringId::Server);
            if (i == places->active_workspace)
                it.badge = l10n::Get(unc ? l10n::StringId::CurrentServer
                                         : l10n::StringId::Current);
            workspaces.items.push_back(std::move(it));
            for (const auto& child : places->FrequentChildren(i, 8)) {
                ui::SidebarItem sub;
                sub.label = TabTitle(child);
                sub.path = child;
                sub.indent = 1;
                sub.icon_glyph = unc ? L"\xE968" : L"\xE8B7";
                sub.fallback_text = L"Dir";
                sub.icon_color = ui::HexColor(unc ? 0x38BDF8 : 0x94A3B8);
                workspaces.items.push_back(std::move(sub));
            }
        }
    }
    vm.sidebar.push_back(std::move(workspaces));

    ui::SidebarGroup access = ConvertGroup(
        l10n::Get(l10n::StringId::SidebarQuickAccess), sidebar.quick_access, false);
    if (!access.items.empty()) {
        access.items[0].expandable = true;
        access.items[0].expanded = starred_expanded;
    }
    size_t access_insert = 1;
    if (places && starred_expanded) {
        for (const auto& starred : places->starred_items) {
            if (starred.kind != PlaceItemKind::Folder) continue;
            ui::SidebarItem child;
            child.label = TabTitle(starred.path);
            child.path = starred.path;
            child.indent = 1;
            child.starred_child = true;
            child.icon_glyph = L"\xE8B7";
            child.fallback_text = L"Dir";
            child.icon_color = ui::HexColor(0xFBBF24);
            child.badge = starred.badge;
            child.badge_color = ui::HexColor(starred.badge_rgb);
            access.items.insert(access.items.begin() + static_cast<std::ptrdiff_t>(access_insert),
                                std::move(child));
            ++access_insert;
        }
    }
    const std::wstring& gitRoot = tab->git_root;
    if (!gitRoot.empty() && (!places || !places->IsStarred(gitRoot))) {
        ui::SidebarItem project;
        wchar_t project_label[512]{};
        swprintf_s(project_label, l10n::Get(l10n::StringId::ProjectFormat).c_str(),
                   TabTitle(gitRoot).c_str());
        project.label = project_label;
        project.path = gitRoot;
        project.icon_glyph = L"\xE8B7";
        project.fallback_text = L"Repo";
        project.icon_color = ui::HexColor(0x34D399);
        project.badge = L"Git";
        access.items.insert(access.items.begin() + static_cast<std::ptrdiff_t>(access_insert),
                            std::move(project));
    }
    if (places) {
        for (const auto& path : places->quick_access_paths) {
            ui::SidebarItem item;
            item.label = TabTitle(path);
            item.path = path;
            item.icon_glyph = L"\xE8B7";
            item.fallback_text = L"Dir";
            item.icon_color = ui::HexColor(0xFBBF24);
            access.items.push_back(std::move(item));
        }
    }
    vm.sidebar.push_back(std::move(access));
    vm.sidebar.push_back(ConvertGroup(l10n::Get(l10n::StringId::SidebarSavedSearches),
                                      sidebar.saved_searches, false));
    vm.sidebar.push_back(ConvertGroup(l10n::Get(l10n::StringId::SidebarDrives),
                                      sidebar.drives, false));

    ui::SidebarGroup tags;
    tags.header = l10n::Get(l10n::StringId::SidebarTags);
    tags.add_action = ui::SidebarAddAction::CreateTag;
    if (places) {
        for (int i = 0; i < static_cast<int>(places->tags.size()); ++i) {
            const auto& t = places->tags[static_cast<size_t>(i)];
            ui::SidebarItem tag;
            tag.label = t.name;
            tag.path = MakeTagPath(t.id);
            tag.is_tag = true;
            tag.show_count = true;
            tag.count = static_cast<int>(t.paths.size());
            tag.tag_dot = ui::HexColor(t.rgb);
            tags.items.push_back(std::move(tag));
        }
    }
    vm.sidebar.push_back(std::move(tags));

    ui::SidebarGroup nets;
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
    vm.sidebar.push_back(std::move(nets));

    // Collapse bits follow the displayed group order; actions use explicit identifiers.
    // bit i of the mask collapses group i.
    for (size_t i = 0; i < vm.sidebar.size() && i < 32; ++i)
        vm.sidebar[i].collapsed = ((sidebar_collapsed_mask >> i) & 1u) != 0;

    return vm;
}

} // namespace pulse::app
