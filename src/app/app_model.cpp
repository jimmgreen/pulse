// app_model.cpp
#include "app_model.h"
#include "../common/json_utils.h"
#include "../common/text_format.h"
#include <commctrl.h>
#include <prsht.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <utility>

namespace pulse::app {

int MoveTabRun(std::vector<int>& order, int pos, int len, int dir) {
    const int n = static_cast<int>(order.size());
    if (pos < 0 || len < 1 || pos + len > n) return pos;
    if (dir < 0 && pos > 0) {
        std::rotate(order.begin() + pos - 1, order.begin() + pos,
                    order.begin() + pos + len);
        return pos - 1;
    }
    if (dir > 0 && pos + len < n) {
        std::rotate(order.begin() + pos, order.begin() + pos + len,
                    order.begin() + pos + len + 1);
        return pos + 1;
    }
    return pos;
}

float CollapsedChipBlockW(float chip_w, float chip_gap) {
    return chip_w + chip_gap;
}

bool ChipBlockCrossed(float block_left, float block_w, float neighbor_center, int dir) {
    if (dir < 0) return block_left < neighbor_center;
    if (dir > 0) return block_left + block_w > neighbor_center;
    return false;
}

float DisplacedRestDelta(float old_rest, float cur_off, float new_rest) {
    return old_rest + cur_off - new_rest;
}

GroupRun FindGroupRun(const std::vector<int>& order,
                      const std::vector<int>& tab_group_of,
                      int at, int gid) {
    const int n = static_cast<int>(order.size());
    if (gid == 0 || at < 0 || at >= n) return {};
    const int tab = order[static_cast<size_t>(at)];
    if (tab < 0 || tab >= static_cast<int>(tab_group_of.size()) ||
        tab_group_of[static_cast<size_t>(tab)] != gid)
        return {};
    auto groupAt = [&](int p) {
        const int t = order[static_cast<size_t>(p)];
        return t >= 0 && t < static_cast<int>(tab_group_of.size())
            ? tab_group_of[static_cast<size_t>(t)] : 0;
    };
    int pos = at;
    while (pos > 0 && groupAt(pos - 1) == gid) --pos;
    int end = at;
    while (end + 1 < n && groupAt(end + 1) == gid) ++end;
    return GroupRun{ pos, end - pos + 1 };
}

void NormalizeGroupRuns(Pane& pane) {
    // Group ids in first-appearance order.
    std::vector<int> groups;
    for (const auto& t : pane.tabs) {
        const int g = t->tab_group;
        if (g != 0 && std::find(groups.begin(), groups.end(), g) == groups.end())
            groups.push_back(g);
    }
    if (groups.empty()) return;
    const Tab* active = pane.ActiveTab() ? pane.ActiveTab() : nullptr;
    for (const int gid : groups) {
        std::vector<size_t> members;
        for (size_t i = 0; i < pane.tabs.size(); ++i)
            if (pane.tabs[i]->tab_group == gid) members.push_back(i);
        if (members.size() < 2) continue;
        if (members.back() - members.front() + 1 == members.size()) continue;
        // Extract members (descending erase keeps lower indices valid), then
        // reinsert the run at the first member's slot.
        std::vector<std::unique_ptr<Tab>> held;
        held.reserve(members.size());
        for (auto it = members.rbegin(); it != members.rend(); ++it) {
            held.push_back(std::move(pane.tabs[*it]));
            pane.tabs.erase(pane.tabs.begin() + static_cast<ptrdiff_t>(*it));
        }
        std::reverse(held.begin(), held.end());
        const size_t at = std::min(members.front(), pane.tabs.size());
        pane.tabs.insert(pane.tabs.begin() + static_cast<ptrdiff_t>(at),
                         std::make_move_iterator(held.begin()),
                         std::make_move_iterator(held.end()));
    }
    if (active) {
        for (size_t i = 0; i < pane.tabs.size(); ++i)
            if (pane.tabs[i].get() == active) { pane.active_tab = i; break; }
    }
}

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
    directory_count = 0;
    file_count = 0;
    if (!snapshot) return;

    for (const auto& entry : *snapshot) {
        if (entry.is_dir) ++directory_count;
        else ++file_count;
    }
}

void Tab::ClearSelection() {
    selected_index = -1;
    selection_anchor = -1;
    all_selected = false;
    selected.clear();
}

int Tab::CountBound() const {
    return snapshot ? static_cast<int>(snapshot->size()) : 0;
}

void Tab::MaterializeSelection() {
    if (!all_selected) return;
    all_selected = false;
    selected.clear();
    const int n = CountBound();
    selected.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) selected.insert(i);
}

void Tab::SelectOnly(int index) {
    selected.clear();
    all_selected = false;
    const int n = CountBound();
    if (index < 0 || index >= n) {
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
    if (index < 0 || index >= n) return;
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
    for (int i = lo; i <= hi; ++i) selected.insert(i);
    selected_index = to;
    if (selection_anchor < 0) selection_anchor = from;
}

void Tab::SelectAll() {
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
    if (index < 0 || index >= CountBound()) return false;
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
        out.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) out[static_cast<size_t>(i)] = i;
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
        if (!want.contains((*snapshot)[static_cast<size_t>(i)].name)) continue;
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
    auto t = std::make_unique<Tab>();
    if (const Tab* source = ActiveTab()) {
        t->view_mode = source->view_mode;
        t->details_column_dividers = source->details_column_dividers;
    }
    t->current_path = fs::NormalizePath(path);
    t->loading = true;
    tabs.push_back(std::move(t));
    active_tab = tabs.size() - 1;
}

void Pane::NewTabAt(size_t index, const std::wstring& path) {
    auto t = std::make_unique<Tab>();
    if (const Tab* source = ActiveTab()) {
        t->view_mode = source->view_mode;
        t->details_column_dividers = source->details_column_dividers;
    }
    t->current_path = fs::NormalizePath(path);
    t->loading = true;
    // New tabs are never pinned: the pinned block is a prefix (Chrome).
    size_t first_unpinned = 0;
    while (first_unpinned < tabs.size() && tabs[first_unpinned]->pinned)
        ++first_unpinned;
    index = std::clamp(index, first_unpinned, tabs.size());
    tabs.insert(tabs.begin() + static_cast<ptrdiff_t>(index), std::move(t));
    active_tab = index;
}

void Pane::CloseTab(size_t idx) {
    if (idx >= tabs.size()) return;
    if (tabs.size() <= 1) return;
    if (tabs[idx]->pinned) return; // pinned tabs refuse to close (Chrome)
    tabs.erase(tabs.begin() + idx);
    if (active_tab >= tabs.size()) active_tab = tabs.size() - 1;
}

void Pane::SwitchTab(size_t idx) {
    if (idx < tabs.size()) active_tab = idx;
}

void Pane::MoveTab(size_t from, size_t to) {
    if (from >= tabs.size() || to >= tabs.size() || from == to) return;

    auto moved = std::move(tabs[from]);
    tabs.erase(tabs.begin() + static_cast<std::ptrdiff_t>(from));
    tabs.insert(tabs.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));

    if (active_tab == from) {
        active_tab = to;
    } else if (from < active_tab && active_tab <= to) {
        --active_tab;
    } else if (to <= active_tab && active_tab < from) {
        ++active_tab;
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
static std::wstring FormatCompactSize(uint64_t size) {
    if (size == 0) return L"0B";
    const wchar_t* units[] = { L"B", L"K", L"M", L"G", L"TB" };
    int unit = 0;
    double s = static_cast<double>(size);
    while (s >= 1024.0 && unit < 4) {
        s /= 1024.0;
        ++unit;
    }
    wchar_t buf[32];
    if (unit == 0) {
        swprintf_s(buf, L"%lluB", size);
    } else if (unit == 4) {
        if (s >= 10.0) swprintf_s(buf, L"%.0fTB", s);
        else swprintf_s(buf, L"%.1fTB", s);
    } else {
        if (s >= 10.0) swprintf_s(buf, L"%.0f%s", s, units[unit]);
        else swprintf_s(buf, L"%.1f%s", s, units[unit]);
    }
    return buf;
}

static bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

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
        if (PathExists(path + L"\\.git")) return path;
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
    if (path.empty()) return L"This PC";
    std::wstring kind, rest;
    if (ParsePulsePath(path, &kind, &rest)) {
        if (kind == L"settings") return L"设置";
        if (kind == L"starred") return L"星标项目";
        if (kind == L"recent") return L"最近使用";
        if (kind == L"search") return rest.empty() ? L"搜索" : L"搜索";
        if (kind == L"tag") return rest.empty() ? L"标签" : rest;
    }
    std::wstring_view v = path;
    if (v.size() > 1 && v.back() == L'\\') v.remove_suffix(1);
    auto pos = v.find_last_of(L"\\/");
    if (pos != std::wstring_view::npos && pos + 1 < v.size())
        return std::wstring(v.substr(pos + 1));
    return std::wstring(v);
}

static std::wstring DisplayPath(const std::wstring& path) {
    if (path.empty()) return L"This PC";
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

SidebarModel BuildSidebarModel() {
    SidebarModel m;
    SidebarEntry starred;
    starred.glyph = L"\xE735";
    starred.fallback = L"Starred";
    starred.color = ui::HexColor(0xFBBF24);
    starred.label = L"星标项目";
    starred.path = MakeStarredPath();
    starred.expandable = true;
    m.quick_access.push_back(std::move(starred));
    SidebarEntry recent;
    recent.glyph = L"\xE823";
    recent.fallback = L"Recent";
    recent.color = ui::HexColor(0x60A5FA);
    recent.label = L"最近使用";
    recent.path = MakeRecentPath();
    m.quick_access.push_back(std::move(recent));
    m.quick_access.push_back(MakeKnownEntry(FOLDERID_Downloads, L"\xE896", L"Downloads",
        ui::HexColor(0xC084FC), L"Downloads"));

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
        e.label = std::wstring(volName[0] ? volName : L"\u672C\u5730\u78C1\u76D8") + L" (" + root[0] + L":)"; // 本地磁盘
        uint64_t total = totalBytes.QuadPart;
        uint64_t free = freeBytes.QuadPart;
        uint64_t used = total > free ? total - free : 0;
        e.detail = FormatCompactSize(used) + L" / " + FormatCompactSize(total);
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
    swprintf_s(buf, L"%zu \u4E2A\u9879\u76EE (%zu \u6587\u4EF6\u5939, %zu \u6587\u4EF6)",
        tab.directory_count + tab.file_count, tab.directory_count, tab.file_count);
    return buf;
}

static std::wstring SelectionText(const Tab& tab) {
    const int count = tab.SelectedCount();
    if (count <= 0) return L"\u672A\u9009\u4E2D"; // 未选中
    if (count == 1 && tab.snapshot && tab.selected_index >= 0 &&
        tab.selected_index < static_cast<int>(tab.snapshot->size())) {
        const auto& e = (*tab.snapshot)[tab.selected_index];
        wchar_t buf[256];
        swprintf_s(buf, L"\u5DF2\u9009\u4E2D 1 \u9879: %s  %s", e.name.c_str(),
                   pulse::format::ByteSize(e.size, true).c_str()); // 已选中 1 项
        return buf;
    }
    wchar_t buf[64];
    swprintf_s(buf, L"\u5DF2\u9009\u4E2D %d \u9879", count); // 已选中 N 项
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
    out.is_file_system = !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path);
    out.can_create = out.is_file_system && !tab->net_readonly;
    std::wstring virtual_kind;
    ParsePulsePath(tab->current_path, &virtual_kind, nullptr);
    out.curated_order = virtual_kind == L"starred" || virtual_kind == L"recent";
    out.is_starred = virtual_kind == L"starred";
    out.is_recent = virtual_kind == L"recent";
    out.recent_filter = tab->recent_filter;
    out.recent_total = out.is_recent && places ? places->recent_items.size() : 0;
    out.date_column_label = out.is_recent ? L"最近打开" : L"修改日期";
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
    if (tab->snapshot && !tab->filter_text.empty()) {
        if (!tab->view_filter_map || tab->view_cache_filter_text != tab->filter_text) {
            auto filtered = std::make_shared<ui::PaneViewModel::FilterMap>();
            std::wstring name_needle;
            std::vector<std::wstring> tag_needles;
            ParseFilterText(tab->filter_text, name_needle, tag_needles);
            if (name_needle.empty() && tag_needles.empty())
                name_needle = ToLowerCopy(tab->filter_text);
            filtered->reserve(tab->snapshot->size());
            for (int i = 0; i < static_cast<int>(tab->snapshot->size()); ++i) {
                const auto& e = (*tab->snapshot)[static_cast<size_t>(i)];
                std::wstring name = ToLowerCopy(e.name);
                const bool name_ok = name_needle.empty() || name.find(name_needle) != std::wstring::npos;
                bool tag_ok = tag_needles.empty();
                if (!tag_ok && places) {
                    const std::wstring full = EntryPathOf(*tab, e);
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
                if (name_ok && tag_ok) filtered->push_back(i);
            }
            tab->view_cache_filter_text = tab->filter_text;
            tab->view_filter_map = std::move(filtered);
        }
        out.filter_map = tab->view_filter_map;
    }
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

    vm.tab_groups.reserve(pane.tab_groups.size());
    for (const auto& g : pane.tab_groups) {
        ui::TabGroupView gv;
        gv.id = g.id;
        gv.name = g.name;
        gv.color_rgb = g.color_rgb;
        gv.collapsed = g.collapsed;
        vm.tab_groups.push_back(std::move(gv));
    }
    vm.tabs.reserve(pane.tabs.size());
    for (size_t i = 0; i < pane.tabs.size(); ++i) {
        ui::TabView tv;
        tv.title = pane.tabs[i]->virtual_title.empty()
            ? TabTitle(pane.tabs[i]->current_path) : pane.tabs[i]->virtual_title;
        tv.active = i == pane.active_tab;
        tv.pinned = pane.tabs[i]->pinned;
        if (pane.tabs[i]->tab_group != 0) {
            for (size_t gi = 0; gi < pane.tab_groups.size(); ++gi) {
                if (pane.tab_groups[gi].id == pane.tabs[i]->tab_group) {
                    tv.group = static_cast<int>(gi);
                    tv.color_rgb = pane.tab_groups[gi].color_rgb;
                    tv.hidden = pane.tab_groups[gi].collapsed;
                    break;
                }
            }
        }
        vm.tabs.push_back(std::move(tv));
    }
    vm.active_tab = static_cast<int>(pane.active_tab);

    FillPaneViewModel(vm.pane, pane, places);
    vm.pane.focused = focused;

    vm.status.status_text = StatusText(*tab);
    vm.status.selection_text = SelectionText(*tab);
    vm.status.mode_text = dark ? L"Windows 11 Fluent \u6DF1\u8272" : L"Windows 11 Fluent \u6D45\u8272"; // 深色 / 浅色

    ui::SidebarGroup workspaces;
    workspaces.header = L"\u5DE5\u4F5C\u533A"; // 工作区
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
            if (unc) it.badge = L"服务器";
            if (i == places->active_workspace)
                it.badge = unc ? L"当前 · 服务器" : L"当前";
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
        L"\u5FEB\u901F\u8BBF\u95EE", sidebar.quick_access, false); // 快速访问
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
        project.label = L"\u9879\u76EE " + TabTitle(gitRoot); // 项目
        project.path = gitRoot;
        project.icon_glyph = L"\xE8B7";
        project.fallback_text = L"Repo";
        project.icon_color = ui::HexColor(0x34D399);
        project.badge = L"Git";
        access.items.insert(access.items.begin() + static_cast<std::ptrdiff_t>(access_insert),
                            std::move(project));
    }
    vm.sidebar.push_back(std::move(access));
    vm.sidebar.push_back(ConvertGroup(L"\u78C1\u76D8", sidebar.drives, false)); // 磁盘

    ui::SidebarGroup tags;
    tags.header = L"\u6807\u7B7E"; // 标签
    tags.add_action = true;
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
    nets.header = L"\u7F51\u7EDC\u4F4D\u7F6E"; // 网络位置
    nets.add_action = true;
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

    // Groups are pushed in a fixed order (工作区/快速访问/磁盘/标签/网络位置);
    // bit i of the mask collapses group i.
    for (size_t i = 0; i < vm.sidebar.size() && i < 32; ++i)
        vm.sidebar[i].collapsed = ((sidebar_collapsed_mask >> i) & 1u) != 0;

    return vm;
}

} // namespace pulse::app
