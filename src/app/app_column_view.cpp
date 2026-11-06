#include "app_internal.h"
#include "app_column_view.h"
#include "column_drop_target.h"
#include "entry_sort.h"
#include "../ui/column_strip_layout.h"
#include "../ui/ui_renderer.h"
#include "../ui/typography.h"
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace pulse {

namespace {

constexpr size_t kMaxAncestors = 12; // bounded by the snapshot store's LRU

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L'\\' + name;
}

std::wstring Leaf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
}

fs::SnapshotPtr SortedForColumn(const fs::SnapshotPtr& snapshot, const app::Tab& tab) {
    if (!snapshot) return {};
    auto sorted = std::make_shared<std::vector<fs::DirEntry>>(*snapshot);
    const app::ScopedFolderSortMode folder_mode(tab.EffectiveFolderSort());
    app::SortEntries(*sorted, tab.sort_column, tab.sort_direction);
    return sorted;
}

bool Eligible(const app::Tab& tab) {
    return tab.column_layout && !tab.current_path.empty() &&
           !fs::IsVirtualPath(tab.current_path) && !tab.content_results &&
           !tab.search_entries;
}

// Folder the column to the right of the list shows, or empty.
std::wstring SelectedFolder(const app::Tab& tab) {
    if (tab.SelectedCount() != 1 || tab.selected_index < 0 ||
        tab.selected_index >= static_cast<int>(tab.EntryCount())) {
        return {};
    }
    const fs::DirEntry entry = tab.EntryAt(static_cast<size_t>(tab.selected_index));
    if (entry.change_record_only) return {};
    if (entry.is_dir) return fs::NormalizePath(Join(tab.current_path, entry.name));
    if (entry.link_target_is_dir && !entry.link_target.empty())
        return fs::NormalizePath(entry.link_target);
    return {};
}

void RequestColumn(AppState& s, const app::Tab& tab, app::ColumnStripColumn& col) {
    col.requested = true;
    col.error = false;
    uint64_t gen = 0;
    if (fs::SnapshotPtr fresh = s.store.GetOrStart(col.path, gen)) {
        col.snapshot = SortedForColumn(fresh, tab);
        col.generation = 0;
        col.store_revision = s.store.State(col.path).revision;
        return;
    }
    if (!col.snapshot) col.snapshot = s.store.Peek(col.path);
    // A pane already loading this folder owns the refresh; issuing another
    // would cancel its generation.
    uint64_t shared = 0;
    ForEachPane(s, [&](app::Pane& pane) {
        const app::Tab* other = pane.ActiveTab();
        if (shared == 0 && other && other->pending_generation != 0 &&
            SamePath(other->current_path, col.path) &&
            other->sort_column == tab.sort_column &&
            other->sort_direction == tab.sort_direction &&
            other->EffectiveFolderSort() == tab.EffectiveFolderSort()) {
            shared = other->pending_generation;
        }
    });
    col.generation = shared != 0 ? shared
                                 : s.worker.Refresh(col.path, tab.sort_column, tab.sort_direction, 0, nullptr,
                                                    tab.EffectiveFolderSort());
}

void RebuildRows(app::ColumnStripColumn& col, const app::Tab& tab) {
    if (col.rows && col.rows_snapshot == col.snapshot &&
        col.rows_show_hidden == tab.show_hidden_files &&
        col.rows_show_protected == tab.show_protected_os_files) return;
    col.rows_snapshot = col.snapshot;
    col.rows_show_hidden = tab.show_hidden_files;
    col.rows_show_protected = tab.show_protected_os_files;
    col.highlight_row = -1;
    if (!col.snapshot) {
        col.rows.reset();
        return;
    }
    auto rows = std::make_shared<std::vector<int>>();
    rows->reserve(col.snapshot->size());
    const auto& entries = *col.snapshot;
    for (size_t i = 0; i < entries.size(); ++i) {
        const fs::DirEntry& e = entries[i];
        if (e.change_record_only) continue;
        if (!tab.AllowsAttributes(e.attrs)) continue;
        if (!col.highlight_name.empty() && col.highlight_row < 0 &&
            SamePath(e.name, col.highlight_name)) {
            col.highlight_row = static_cast<int>(rows->size());
        }
        rows->push_back(static_cast<int>(i));
    }
    col.rows = std::move(rows);
}

void SyncColumn(AppState& s, const app::Tab& tab, app::ColumnStripColumn& col) {
    if (col.path.empty()) return;
    const fs::SnapshotState cached = s.store.State(col.path);
    if (cached.revision != col.store_revision) {
        col.store_revision = cached.revision;
        if (cached.snapshot && cached.snapshot != col.snapshot) {
            col.snapshot = SortedForColumn(cached.snapshot, tab);
            col.rows.reset();
        }
        if (cached.dirty) col.requested = false;
    }
    if (!col.requested) RequestColumn(s, tab, col);
    RebuildRows(col, tab);
}

// Drive roots show their sidebar label ("Data (D:)") instead of "D:".
std::wstring RootTitle(const AppState& s, const std::wstring& path) {
    for (const auto& drive : s.sidebar.drives) {
        if (SamePath(fs::NormalizePath(drive.path), path)) return drive.label;
    }
    return {};
}

void RebuildAncestors(AppState& s, app::Tab& tab) {
    app::ColumnStripState& state = tab.column_strip;
    // Merely visiting a directory never creates a width setting. The previous
    // role-slot sampling caused width changes whenever a column moved roles.
    std::vector<std::wstring> chain; // parent first
    std::wstring child = tab.current_path;
    while (chain.size() < kMaxAncestors) {
        std::wstring parent = fs::ParentPath(child);
        if (parent.empty() || SamePath(parent, child)) break;
        chain.push_back(parent);
        child = std::move(parent);
    }
    std::vector<app::ColumnStripColumn> next;
    next.reserve(chain.size());
    for (size_t i = chain.size(); i-- > 0;) {
        const std::wstring path = fs::NormalizePath(chain[i]);
        const std::wstring highlight = Leaf(i == 0 ? tab.current_path : chain[i - 1]);
        app::ColumnStripColumn col;
        for (auto& old : tab.column_strip.ancestors) {
            if (SamePath(old.path, path)) {
                col = std::move(old);
                break;
            }
        }
        if (col.path.empty()) col.path = path;
        if (i + 1 == chain.size()) {
            const std::wstring up = fs::ParentPath(path);
            if (up.empty() || SamePath(up, path)) col.title = RootTitle(s, path);
        }
        if (!SamePath(col.highlight_name, highlight)) {
            col.highlight_name = highlight;
            col.rows.reset(); // re-resolve the highlight row
            col.auto_scroll = true;
        }
        next.push_back(std::move(col));
    }
    tab.column_strip.ancestors = std::move(next);
    // Listings may be stale after navigation; revalidate against the store.
    for (auto& col : tab.column_strip.ancestors) col.requested = false;
    tab.column_strip.for_path = tab.current_path;
    if (!state.hold_scroll_on_navigate) state.scroll_left_dip = -1.0f;
    state.hold_scroll_on_navigate = false;
}

void SyncTab(AppState& s, app::Tab& tab) {
    app::ColumnStripState& state = tab.column_strip;
    if (!Eligible(tab)) {
        if (!state.Empty()) {
            // A virtual folder or a temporary view change must not discard
            // user-set widths for the real directories in this tab.
            auto widths = std::move(state.widths_by_path);
            state = {};
            state.widths_by_path = std::move(widths);
        }
        return;
    }
    if (state.for_path != tab.current_path) RebuildAncestors(s, tab);
    for (auto& col : state.ancestors) SyncColumn(s, tab, col);

    const bool pressed_in_this_tab = (s.dragPending || s.columnGesture.armed) &&
        s.pane && s.pane->ActiveTab() == &tab;
    const bool drag_in_this_tab = s.columnDragActive && s.columnDragTab == &tab;
    if (!pressed_in_this_tab && !drag_in_this_tab) {
        const std::wstring folder = SelectedFolder(tab);
        if (!SamePath(folder, state.child.path)) {
            state.child = {};
            state.child.path = folder;
        }
    }
    SyncColumn(s, tab, state.child);
}

app::Tab* TabAtSlot(AppState& s, int pane_index) {
    app::Pane* pane = PaneAtSlot(s, pane_index);
    return pane ? pane->ActiveTab() : nullptr;
}

app::ColumnStripColumn* StripColumn(app::Tab& tab, int id) {
    if (id == ui::kColumnStripChildId) {
        return tab.column_strip.child.path.empty() ? nullptr : &tab.column_strip.child;
    }
    if (id < 0 || static_cast<size_t>(id) >= tab.column_strip.ancestors.size()) return nullptr;
    return &tab.column_strip.ancestors[static_cast<size_t>(id)];
}

const D2D1_RECT_F* SlotRect(const ui::WindowViewModel& vm, int pane_index) {
    if (pane_index < 0 || pane_index >= static_cast<int>(vm.pane_slots.size())) return nullptr;
    return &vm.pane_slots[static_cast<size_t>(pane_index)].rect;
}

const ui::PaneViewModel* SlotPane(const ui::WindowViewModel& vm, int pane_index) {
    if (pane_index >= 0 && pane_index < static_cast<int>(vm.pane_slots.size()))
        return &vm.pane_slots[static_cast<size_t>(pane_index)].pane;
    return pane_index <= 0 ? &vm.pane : nullptr;
}

bool SlotLayout(AppState& s, const ui::WindowViewModel& vm, int pane_index,
                ui::ColumnStripLayout& out) {
    const ui::PaneViewModel* pane = SlotPane(vm, pane_index);
    if (!pane) return false;
    const D2D1_RECT_F* rect = SlotRect(vm, pane_index);
    const D2D1_RECT_F bounds = rect ? *rect
        : s.renderer.ContentRect(static_cast<float>(s.compositor.Width()),
                                 static_cast<float>(s.compositor.Height()));
    out = s.renderer.ColumnStripGeometry(*pane, bounds);
    return out.active;
}

bool ScrollStripBy(AppState& s, const ui::WindowViewModel& vm, int pane_index, float delta_dip) {
    app::Tab* tab = TabAtSlot(s, pane_index);
    ui::ColumnStripLayout layout;
    if (!tab || !SlotLayout(s, vm, pane_index, layout) || layout.max_scroll_px <= 0.0f) return false;
    const float scale = s.scale > 0.0f ? s.scale : 1.0f;
    float& offset = tab->column_strip.scroll_left_dip;
    const float current = offset < 0.0f ? layout.scroll_px / scale : offset;
    offset = std::clamp(current - delta_dip, 0.0f, layout.max_scroll_px / scale);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

// Selects `name` in the active tab now (when listed) and after the pending
// refresh lands.
void SelectByName(AppState& s, app::Tab& tab, const std::wstring& name) {
    tab.pending_selected_names = {name};
    tab.pending_selected_name = name;
    tab.pending_selection_revision = tab.selection_revision;
    tab.pending_ensure_selection_visible = true;
    if (!tab.snapshot) return;
    const auto& entries = *tab.snapshot;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!SamePath(entries[i].name, name) || !tab.EntryVisible(static_cast<int>(i))) continue;
        tab.SelectOnly(static_cast<int>(i));
        tab.pending_selection_revision = tab.selection_revision;
        EnsureRowVisible(s, tab, static_cast<int>(i));
        s.scrollTargetY = tab.scroll_y;
        s.scrollAnimating = false;
        break;
    }
}

bool ClickRow(AppState& s, app::Tab& tab, int column_id, int row, int pane_index) {
    app::ColumnStripColumn* col = StripColumn(tab, column_id);
    if (!Eligible(tab) || !SamePath(tab.column_strip.for_path, tab.current_path) ||
        !col || !col->rows || !col->rows_snapshot || col->snapshot != col->rows_snapshot ||
        row < 0 || static_cast<size_t>(row) >= col->rows->size()) {
        return false;
    }
    const int source = (*col->rows)[static_cast<size_t>(row)];
    if (source < 0 || static_cast<size_t>(source) >= col->rows_snapshot->size()) return false;
    const std::wstring name = (*col->rows_snapshot)[static_cast<size_t>(source)].name;
    const std::wstring folder = col->path; // col is invalidated by navigation
    const bool changes_folder = !SamePath(folder, tab.current_path);
    tab.column_strip.hold_scroll_on_navigate = true;
    if (changes_folder) NavigateTo(s, folder);
    else tab.column_strip.hold_scroll_on_navigate = false;
    if (app::Tab* active = ActiveTab(s)) SelectByName(s, *active, name);
    if (changes_folder) {
        s.stripClickTick = GetTickCount64();
        s.stripClickPane = pane_index;
    } else {
        s.stripClickTick = 0;
        s.stripClickPane = -1;
    }
    return true;
}

} // namespace

void SyncColumnStrips(AppState& s) {
    ForEachPane(s, [&](app::Pane& pane) {
        if (app::Tab* tab = pane.ActiveTab()) SyncTab(s, *tab);
    });
}

void NoteColumnStripResult(AppState& s, const app::WorkResult& res) {
    if (res.cancelled || fs::IsVirtualPath(res.path)) return;
    bool changed = false;
    auto apply = [&](app::ColumnStripColumn& col) {
        if (col.path.empty() || !SamePath(col.path, res.path)) return;
        const fs::SnapshotState cached = s.store.State(col.path);
        // The process-wide store has already arbitrated worker generations.
        // Never let an older completion overwrite the accepted listing here.
        if (col.generation == 0 || res.generation != col.generation) return;
        if (!res.error && cached.snapshot && cached.generation != res.generation) return;
        if (res.error) {
            col.generation = 0;
            col.error = true;
            changed = true;
            return;
        }
        if (!res.snapshot) return;
        col.generation = 0;
        col.error = false;
        col.snapshot = res.snapshot;
        col.store_revision = cached.revision;
        col.rows.reset();
        changed = true;
    };
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (!tab || tab->column_strip.Empty()) return;
        for (auto& col : tab->column_strip.ancestors) apply(col);
        apply(tab->column_strip.child);
    });
    if (changed && s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool HandleColumnStripMouseDown(AppState& s, const ui::WindowViewModel& vm,
                                const ui::HitTestResult& hit, int x, int y) {
    using R = ui::HitTestResult;
    switch (hit.region) {
    case R::PaneColumnLayout:
        ToggleColumnLayout(s, hit.index);
        return true;
    case R::ColumnStripDivider: {
        app::Tab* tab = TabAtSlot(s, hit.pane_index);
        if (!tab) return true;
        const size_t slot = ui::ColumnStripWidthSlot(hit.sub_index,
                                                     tab->column_strip.ancestors.size());
        s.stripResizing = true;
        s.stripHScrolling = false;
        s.stripResizePane = hit.pane_index;
        s.stripResizeColumn = hit.sub_index;
        s.stripResizeStartX = x;
        const ui::PaneViewModel* pane = SlotPane(vm, hit.pane_index);
        s.stripResizeStartDip = ui::ColumnStripWidthDip(
            pane ? pane->column_strip.widths_dip : tab->column_widths_dip, slot);
        const app::ColumnStripColumn* resized_column = StripColumn(*tab, hit.sub_index);
        const std::wstring resize_path = hit.sub_index == ui::kColumnStripBodyId
            ? tab->current_path : resized_column ? resized_column->path : std::wstring{};
        s.stripResizeTab = tab;
        s.stripResizePath = fs::NormalizePath(resize_path);
        const auto original = tab->column_strip.widths_by_path.find(s.stripResizePath);
        s.stripResizeHadSetting = original != tab->column_strip.widths_by_path.end();
        s.stripResizeOriginalSetting = s.stripResizeHadSetting ? original->second
            : app::ColumnPathWidthSetting{s.stripResizeStartDip, app::ColumnPathWidthMode::Manual};
        s.stripResizeMoved = false;
        s.stripClickTick = 0;
        s.stripClickPane = -1;
        s.hoverRegion = static_cast<int>(R::ColumnStripDivider);
        s.hoverSubIndex = hit.sub_index;
        s.hoverPaneIndex = hit.pane_index;
        SetCapture(s.hwnd);
        return true;
    }
    case R::ColumnStripRow: {
        app::Tab* tab = TabAtSlot(s, hit.pane_index);
        if (!tab || !Eligible(*tab)) return true;
        const std::wstring item = app::ColumnStripRowItemPath(
            tab->column_strip, tab->current_path, hit.sub_index, hit.index);
        if (item.empty()) return true;
        s.columnGesture = {};
        s.columnGesture.armed = true;
        s.columnGesture.pane_index = hit.pane_index;
        s.columnGesture.column_id = hit.sub_index;
        s.columnGesture.row_index = hit.index;
        s.columnGesture.press = POINT{x, y};
        s.columnGesture.view_path = tab->current_path;
        s.columnGesture.item_path = item;
        s.stripClickTick = 0;
        s.stripClickPane = -1;
        SetCapture(s.hwnd);
        return true;
    }
    case R::ColumnStripBlank:
        return true;
    case R::ColumnStripHScroll: {
        app::Tab* tab = TabAtSlot(s, hit.pane_index);
        ui::ColumnStripLayout layout;
        D2D1_RECT_F track{}, thumb{};
        if (!tab || !SlotLayout(s, vm, hit.pane_index, layout) ||
            !s.renderer.ColumnStripHScrollRects(layout, track, thumb)) {
            return true;
        }
        const float scale = s.scale > 0.0f ? s.scale : 1.0f;
        const float fx = static_cast<float>(x);
        if (fx < thumb.left || fx >= thumb.right) {
            // Page toward the click, like a classic scrollbar track.
            const float page = (layout.viewport.right - layout.viewport.left) / scale;
            ScrollStripBy(s, vm, hit.pane_index, fx < thumb.left ? page : -page);
            return true;
        }
        const float travel = (track.right - track.left) - (thumb.right - thumb.left);
        s.stripHScrolling = true;
        s.stripResizePane = hit.pane_index;
        s.stripResizeTab = tab;
        s.stripHScrollOriginalDip = tab->column_strip.scroll_left_dip;
        s.stripResizeStartX = x;
        s.stripHScrollStartDip = tab->column_strip.scroll_left_dip < 0.0f
            ? layout.scroll_px / scale : tab->column_strip.scroll_left_dip;
        s.stripHScrollRatio = travel > 0.0f ? layout.max_scroll_px / travel : 0.0f;
        s.stripHScrollMaxDip = layout.max_scroll_px / scale;
        SetCapture(s.hwnd);
        return true;
    }
    default:
        return false;
    }
}

bool FinishColumnStripMouseUp(AppState& s, const ui::WindowViewModel& vm,
                               const ui::HitTestResult& hit) {
    const AppState::ColumnGesture press = s.columnGesture;
    s.columnGesture = {};
    if (!press.armed) return false;
    app::Tab* tab = TabAtSlot(s, press.pane_index);
    if (!tab || !Eligible(*tab) || !SamePath(tab->current_path, press.view_path) ||
        hit.region != ui::HitTestResult::ColumnStripRow ||
        hit.pane_index != press.pane_index || hit.sub_index != press.column_id ||
        !SamePath(app::ColumnStripRowItemPath(tab->column_strip, tab->current_path,
                                              hit.sub_index, hit.index), press.item_path))
        return true;
    float reveal = 0.0f;
    ui::ColumnStripLayout layout;
    if (SlotLayout(s, vm, press.pane_index, layout)) {
        const float scale = s.scale > 0.0f ? s.scale : 1.0f;
        tab->column_strip.scroll_left_dip = layout.scroll_px / scale;
        const app::ColumnStripColumn* col = StripColumn(*tab, hit.sub_index);
        if (col && col->rows && col->rows_snapshot && col->snapshot == col->rows_snapshot &&
            hit.index >= 0 && static_cast<size_t>(hit.index) < col->rows->size()) {
            const int source = (*col->rows)[static_cast<size_t>(hit.index)];
            if (source >= 0 && static_cast<size_t>(source) < col->rows_snapshot->size()) {
                const fs::DirEntry& entry = (*col->rows_snapshot)[static_cast<size_t>(source)];
                if (entry.is_dir || entry.link_target_is_dir) {
                    const std::wstring child_path = entry.link_target_is_dir && !entry.link_target.empty()
                        ? fs::NormalizePath(entry.link_target)
                        : fs::NormalizePath(Join(col->path, entry.name));
                    const auto width_it = tab->column_strip.widths_by_path.find(child_path);
                    const float child_dip = width_it == tab->column_strip.widths_by_path.end()
                        ? ui::kColumnStripChildDefaultDip
                        : std::clamp(width_it->second.width_dip, ui::kColumnStripMinDip, ui::kColumnStripMaxDip);
                    for (const auto& slot : layout.columns) {
                        if (slot.id != hit.sub_index) continue;
                        reveal = std::max(0.0f, slot.rect.right + child_dip * scale - layout.viewport.right) / scale;
                        break;
                    }
                }
            }
        }
    }
    if (ClickRow(s, *tab, hit.sub_index, hit.index, press.pane_index))
        tab->column_strip.scroll_left_dip += reveal;
    return true;
}

bool SelectColumnStripRow(AppState& s, const ui::HitTestResult& hit) {
    if (hit.region != ui::HitTestResult::ColumnStripRow) return false;
    app::Tab* tab = TabAtSlot(s, hit.pane_index);
    if (!tab || !Eligible(*tab) || !SamePath(tab->column_strip.for_path, tab->current_path) ||
        app::ColumnStripRowItemPath(tab->column_strip, tab->current_path, hit.sub_index, hit.index).empty() ||
        !ClickRow(s, *tab, hit.sub_index, hit.index, hit.pane_index)) return false;
    s.stripClickTick = 0;
    s.stripClickPane = -1;
    const app::Tab* active = ActiveTab(s);
    return active && active->SelectedCount() > 0;
}

bool ColumnStripRowTarget(AppState& s, const ui::HitTestResult& hit,
                          std::wstring& owner_folder, std::wstring& full_path,
                          std::wstring& name, bool& directory) {
    owner_folder.clear();
    full_path.clear();
    name.clear();
    directory = false;
    if (hit.region != ui::HitTestResult::ColumnStripRow) return false;
    app::Tab* tab = TabAtSlot(s, hit.pane_index);
    const app::ColumnStripColumn* col = tab ? StripColumn(*tab, hit.sub_index) : nullptr;
    if (!tab || !Eligible(*tab) || !SamePath(tab->column_strip.for_path, tab->current_path) ||
        !col || !col->rows || !col->rows_snapshot || col->snapshot != col->rows_snapshot ||
        hit.index < 0 || static_cast<size_t>(hit.index) >= col->rows->size()) return false;
    const int source = (*col->rows)[static_cast<size_t>(hit.index)];
    if (source < 0 || static_cast<size_t>(source) >= col->rows_snapshot->size()) return false;
    const fs::DirEntry& entry = (*col->rows_snapshot)[static_cast<size_t>(source)];
    if (entry.change_record_only || !entry.recycle_path.empty() || entry.name.empty() ||
        entry.name == L"." || entry.name == L"..") return false;
    owner_folder = col->path;
    name = entry.name;
    full_path = entry.full_path.empty() ? Join(owner_folder, name) : entry.full_path;
    directory = entry.is_dir || entry.link_target_is_dir;
    return !owner_folder.empty() && !full_path.empty();
}

std::wstring ColumnStripBackgroundPath(AppState& s, const ui::HitTestResult& hit) {
    if (hit.region != ui::HitTestResult::ColumnStripBlank) return {};
    app::Tab* tab = TabAtSlot(s, hit.pane_index);
    // Never act on a column from a previous directory's asynchronous snapshot.
    if (!tab || !Eligible(*tab) || !SamePath(tab->column_strip.for_path, tab->current_path)) return {};
    const app::ColumnStripColumn* col = StripColumn(*tab, hit.sub_index);
    return col ? col->path : std::wstring{};
}

std::wstring ColumnStripDropPath(AppState& s, const ui::HitTestResult& hit) {
    if (hit.region != ui::HitTestResult::ColumnStripBlank &&
        hit.region != ui::HitTestResult::ColumnStripRow) return {};
    app::Tab* tab = TabAtSlot(s, hit.pane_index);
    if (!tab || !Eligible(*tab) || !SamePath(tab->column_strip.for_path, tab->current_path)) return {};
    return app::ColumnDropDestination(tab->column_strip, tab->current_path,
                                      hit.sub_index, hit.index,
                                      hit.region == ui::HitTestResult::ColumnStripRow);
}

bool UpdateColumnStripResize(AppState& s, int x) {
    if (s.stripHScrolling) {
        app::Tab* tab = TabAtSlot(s, s.stripResizePane);
        if (!tab || tab != s.stripResizeTab) {
            EndColumnStripResize(s, false);
            return true;
        }
        const float scale = s.scale > 0.0f ? s.scale : 1.0f;
        const float moved = static_cast<float>(x - s.stripResizeStartX) * s.stripHScrollRatio / scale;
        tab->column_strip.scroll_left_dip =
            std::clamp(s.stripHScrollStartDip + moved, 0.0f, s.stripHScrollMaxDip);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return true;
    }
    if (!s.stripResizing) return false;
    app::Tab* tab = TabAtSlot(s, s.stripResizePane);
    if (!tab || tab != s.stripResizeTab) {
        EndColumnStripResize(s, false);
        return true;
    }
    const float scale = s.scale > 0.0f ? s.scale : 1.0f;
    const float dx = static_cast<float>(x - s.stripResizeStartX) / scale;
    if (std::fabs(dx) < 3.0f) return true;
    const float width = std::clamp(s.stripResizeStartDip + dx,
                                   ui::kColumnStripMinDip, ui::kColumnStripMaxDip);
    if (!s.stripResizePath.empty()) {
        tab->column_strip.widths_by_path[s.stripResizePath] =
            {width, app::ColumnPathWidthMode::Manual};
        s.stripResizeMoved = true;
    }
    s.hoverRegion = static_cast<int>(ui::HitTestResult::ColumnStripDivider);
    s.hoverSubIndex = s.stripResizeColumn;
    s.hoverPaneIndex = s.stripResizePane;
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

void ResetColumnStripWidth(AppState& s, const ui::HitTestResult& hit) {
    EndColumnStripResize(s, false);
    app::Tab* tab = TabAtSlot(s, hit.pane_index);
    if (!tab) return;
    const app::ColumnStripColumn* col = StripColumn(*tab, hit.sub_index);
    const std::wstring path = hit.sub_index == ui::kColumnStripBodyId
        ? tab->current_path : col ? col->path : std::wstring{};
    if (path.empty()) return;
    const std::wstring key = fs::NormalizePath(path);
    const auto existing = tab->column_strip.widths_by_path.find(key);
    if (existing != tab->column_strip.widths_by_path.end() &&
        existing->second.mode == app::ColumnPathWidthMode::AutoFit) {
        tab->column_strip.widths_by_path.erase(existing);
        if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }

    ui::Compositor& compositor = s.compositor;
    IDWriteTextFormat* format = compositor.FileNameFormat();
    if (!format || !compositor.DwriteFactory()) return;
    float widest_px = 0.0f;
    const float scale = s.scale > 0.0f ? s.scale : 1.0f;
    const auto measure = [&](const std::wstring& name) {
        if (!name.empty() && widest_px / scale + 70.0f < ui::kColumnStripMaxDip)
            widest_px = std::max(widest_px,
                ui::typography::MeasureLine(&compositor, format, name));
    };
    if (hit.sub_index == ui::kColumnStripBodyId) {
        if (!tab->snapshot) return;
        for (size_t i = 0; i < tab->EntryCount(); ++i)
            if (tab->EntryVisible(static_cast<int>(i))) measure(tab->EntryAt(i).name);
    } else if (col && col->rows && col->rows_snapshot) {
        for (int source : *col->rows)
            if (source >= 0 && static_cast<size_t>(source) < col->rows_snapshot->size())
                measure((*col->rows_snapshot)[static_cast<size_t>(source)].name);
    } else {
        return;
    }
    const float fitted = std::ceil(widest_px / scale + 70.0f);
    const float locked = std::clamp(fitted, ui::kColumnStripMinDip, ui::kColumnStripMaxDip);
    tab->column_strip.widths_by_path[key] =
        {locked, app::ColumnPathWidthMode::AutoFit};
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void EndColumnStripResize(AppState& s, bool commit) {
    if (!s.stripResizing && !s.stripHScrolling) return;
    if (s.stripHScrolling && !commit && s.stripResizeTab) {
        app::Tab* tab = TabAtSlot(s, s.stripResizePane);
        if (tab == s.stripResizeTab)
            tab->column_strip.scroll_left_dip = s.stripHScrollOriginalDip;
    }
    if (s.stripResizing && !commit && s.stripResizeMoved && s.stripResizeTab) {
        app::Tab* tab = TabAtSlot(s, s.stripResizePane);
        if (tab == s.stripResizeTab && !s.stripResizePath.empty()) {
            if (s.stripResizeHadSetting)
                tab->column_strip.widths_by_path[s.stripResizePath] = s.stripResizeOriginalSetting;
            else
                tab->column_strip.widths_by_path.erase(s.stripResizePath);
        }
    }
    s.stripResizing = false;
    s.stripHScrolling = false;
    s.stripResizePane = -1;
    s.stripResizeColumn = -1;
    s.stripResizeTab = nullptr;
    s.stripResizePath.clear();
    s.stripResizeHadSetting = false;
    s.stripResizeMoved = false;
    s.stripHScrollOriginalDip = -1.0f;
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool ColumnStripSwallowDoubleClick(const AppState& s, const ui::HitTestResult& hit) {
    return s.stripClickTick != 0 &&
           GetTickCount64() - s.stripClickTick <= static_cast<ULONGLONG>(GetDoubleClickTime()) &&
           hit.region == ui::HitTestResult::ColumnStripRow &&
           hit.pane_index == s.stripClickPane;
}

bool HandleColumnStripWheel(AppState& s, const ui::WindowViewModel& vm,
                            const ui::HitTestResult& hit, int wheel_delta,
                            bool horizontal_input) {
    using R = ui::HitTestResult;
    const bool over_column = hit.region == R::ColumnStripRow ||
        hit.region == R::ColumnStripBlank;
    const bool horizontal = hit.region == R::ColumnStripHScroll ||
        (horizontal_input && over_column) ||
        ((GetKeyState(VK_SHIFT) & 0x8000) != 0 &&
         hit.sub_index != ui::kColumnStripChildId &&
         over_column);
    if (horizontal) {
        // Wheel up reveals farther ancestors (to the left).
        ScrollStripBy(s, vm, hit.pane_index,
                      static_cast<float>(wheel_delta) / WHEEL_DELTA * 96.0f);
        return true;
    }
    if (hit.region != R::ColumnStripRow && hit.region != R::ColumnStripBlank) return false;
    app::Tab* tab = TabAtSlot(s, hit.pane_index);
    const D2D1_RECT_F* rect = SlotRect(vm, hit.pane_index);
    const ui::PaneViewModel* pane = SlotPane(vm, hit.pane_index);
    if (!tab || !pane) return true;
    D2D1_RECT_F bounds = rect ? *rect
        : s.renderer.ContentRect(static_cast<float>(s.compositor.Width()),
                                 static_cast<float>(s.compositor.Height()));
    const ui::ColumnStripLayout layout = s.renderer.ColumnStripGeometry(*pane, bounds);
    app::ColumnStripColumn* col = StripColumn(*tab, hit.sub_index);
    if (!col) return true;
    const ui::ColumnStripColumnView* view = nullptr;
    if (hit.sub_index == ui::kColumnStripChildId) view = &pane->column_strip.child;
    else if (hit.sub_index >= 0 &&
             static_cast<size_t>(hit.sub_index) < pane->column_strip.ancestors.size())
        view = &pane->column_strip.ancestors[static_cast<size_t>(hit.sub_index)];
    if (!view) return true;
    for (const auto& slot : layout.columns) {
        if (slot.id != hit.sub_index) continue;
        const float view_h = s.renderer.ColumnStripViewHeight(slot.rect);
        const float scale = s.scale > 0.0f ? s.scale : 1.0f;
        const float current = s.renderer.ColumnStripScrollPx(*view, view_h);
        const float max_scroll = s.renderer.ColumnStripMaxScrollPx(*view, view_h);
        const float step = static_cast<float>(wheel_delta) / WHEEL_DELTA * 3.0f *
                           s.renderer.RowHeight();
        const float next = std::clamp(current - step, 0.0f, max_scroll);
        col->auto_scroll = false;
        col->scroll_dip = next / scale;
        InvalidateRect(s.hwnd, nullptr, FALSE);
        break;
    }
    return true;
}

bool HandleColumnArrowKey(AppState& s, bool right) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !Eligible(*tab)) return false;
    if (tab->view_mode != ui::ViewMode::Details && tab->view_mode != ui::ViewMode::Content)
        return false;
    if (!right) {
        GoUp(s);
        return true;
    }
    const std::wstring folder = SelectedFolder(*tab);
    if (folder.empty()) return false;
    NavigateTo(s, folder);
    return true;
}

void ToggleColumnLayout(AppState& s, int pane_index) {
    app::Pane* pane = PaneAtSlot(s, pane_index);
    if (!pane) pane = s.pane;
    app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
    if (!tab) return;
    tab->column_layout = !tab->column_layout;
    if (!tab->column_layout) {
        // Retain per-directory adjustments when toggling the view off/on.
        auto widths = std::move(tab->column_strip.widths_by_path);
        tab->column_strip = {};
        tab->column_strip.widths_by_path = std::move(widths);
    }
    SyncColumnStrips(s);
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace pulse
