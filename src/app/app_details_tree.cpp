// app_details_tree.cpp — Inline folder expansion in the details view.
//
// The tab keeps which folders are open (DetailsTree::folders, by path, for the
// life of the tab). For the current snapshot it builds one combined entry list:
// the snapshot rows, then every listed child and placeholder appended in view
// order. Base indices never move, so selection, sizes, tags and the rest keep
// working on them; child rows are addressed past the snapshot's end and carry
// their full path. The pane view gets the combined list, a view map that puts
// children under their folder, and per-row tree facts for the renderer.
#include "app_internal.h"
#include "app_details_tree.h"
#include "app_messages.h"
#include "create_rename_intent.h"
#include "entry_sort.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../ui/ui_motion.h"
#include "../ui/ui_renderer.h"
#include <algorithm>
#include <cwchar>
#include <unordered_map>

namespace pulse {

namespace {

using app::DetailsTree;
using app::DetailsTreeFolder;
using FolderState = app::DetailsTreeFolder::State;

uint64_t g_last_recursion = 0;

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L'\\' + name;
}

std::wstring FolderKey(const std::wstring& path) {
    std::wstring key = path;
    while (key.size() > 3 && (key.back() == L'\\' || key.back() == L'/')) key.pop_back();
    if (!key.empty()) CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
    return key;
}

bool Eligible(const app::Tab& tab) {
    return tab.view_mode == ui::ViewMode::Details && !tab.column_layout && tab.snapshot &&
           !tab.current_path.empty() && !fs::IsVirtualPath(tab.current_path) &&
           !tab.content_results && !tab.search_entries && tab.filter_text.empty() &&
           tab.EffectiveGroup() == 0 && !tab.compare_marks;
}

// Everything that changes which children show and in what order.
int OrderKey(const app::Tab& tab) {
    return ((static_cast<int>(tab.sort_column) * 2 + static_cast<int>(tab.sort_direction)) * 4 +
            (tab.show_hidden_files ? 1 : 0) + (tab.show_protected_os_files ? 2 : 0)) * 4 +
           static_cast<int>(tab.EffectiveFolderSort());
}

bool Expandable(const fs::DirEntry& e) {
    return e.is_dir && !e.change_record_only;
}

DetailsTree& TreeOf(app::Tab& tab) {
    if (!tab.tree) tab.tree = std::make_shared<DetailsTree>();
    return *tab.tree;
}

DetailsTreeFolder& FolderFor(DetailsTree& t, const std::wstring& path) {
    DetailsTreeFolder& f = t.folders[FolderKey(path)];
    if (f.path.empty()) f.path = fs::NormalizePath(path);
    return f;
}

void ReleaseListing(DetailsTreeFolder& f) {
    f.listing.reset();
    f.order.clear();
    f.order.shrink_to_fit();
    f.order_listing.reset();
    f.order_key = -1;
    if (f.state != FolderState::Loading) f.state = FolderState::Idle;
}

void RequestFolder(AppState& s, const app::Tab& tab, DetailsTreeFolder& f) {
    f.since_ms = ui::motion::NowMs();
    uint64_t gen = 0;
    if (fs::SnapshotPtr fresh = s.store.GetOrStart(f.path, gen)) {
        f.listing = std::move(fresh);
        f.generation = 0;
        f.state = FolderState::Ready;
        return;
    }
    if (!f.listing) f.listing = s.store.Peek(f.path);
    // A pane already loading this folder owns the refresh; issuing another
    // would cancel its generation.
    uint64_t shared = 0;
    ForEachPane(s, [&](app::Pane& pane) {
        const app::Tab* other = pane.ActiveTab();
        if (shared == 0 && other && other->pending_generation != 0 &&
            SamePath(other->current_path, f.path)) {
            shared = other->pending_generation;
        }
    });
    f.generation = shared != 0 ? shared
                               : s.worker.Refresh(f.path, tab.sort_column, tab.sort_direction, 0, nullptr,
                                                  tab.EffectiveFolderSort());
    f.state = FolderState::Loading;
}

// Visible children of `f` in the tab's order (indices into its listing).
void EnsureOrder(DetailsTreeFolder& f, const app::Tab& tab, int key) {
    if (f.order_listing == f.listing && f.order_key == key) return;
    f.order_listing = f.listing;
    f.order_key = key;
    f.order.clear();
    if (!f.listing) return;
    const std::vector<fs::DirEntry>& entries = *f.listing;
    f.order.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const fs::DirEntry& e = entries[i];
        if (e.change_record_only || !tab.AllowsAttributes(e.attrs)) continue;
        f.order.push_back(static_cast<int>(i));
    }
    const ui::SortColumn col = tab.sort_column;
    const ui::SortDirection dir = tab.sort_direction;
    const app::ScopedFolderSortMode folder_mode(tab.EffectiveFolderSort());
    const auto less = [&](int a, int b) {
        return app::EntryLess(entries[static_cast<size_t>(a)], entries[static_cast<size_t>(b)], col, dir);
    };
    // The worker usually sorted it the same way already.
    if (!std::is_sorted(f.order.begin(), f.order.end(), less))
        std::stable_sort(f.order.begin(), f.order.end(), less);
}

// One walk over the snapshot and the open folders below it.
struct Builder {
    AppState& s;
    app::Tab& tab;
    DetailsTree& t;
    int key = 0;
    size_t base_size = 0;
    std::vector<fs::DirEntry> extras;
    std::vector<uint8_t> placeholder;
    std::vector<ui::DetailsTreeRow> rows;
    std::vector<int> map;
    bool recursion_pending = false;

    int AddExtra(fs::DirEntry e, uint8_t kind) {  // kind: see DetailsTree::placeholder
        extras.push_back(std::move(e));
        placeholder.push_back(kind);
        return static_cast<int>(base_size + extras.size() - 1);
    }

    void Push(const ui::DetailsTreeRow& row, int src) {
        rows.push_back(row);
        map.push_back(src);
    }

    void PushPlaceholder(ui::TreeRowKind kind, uint16_t depth, const DetailsTreeFolder& f,
                         size_t shown = 0, size_t total = 0) {
        fs::DirEntry e;
        e.change_record_only = true;  // never a file operation source
        e.full_path = f.path;         // the folder it stands in for
        ui::DetailsTreeRow row;
        row.depth = depth;
        row.kind = kind;
        row.shown = static_cast<uint32_t>(std::min<size_t>(shown, UINT32_MAX));
        row.total = static_cast<uint32_t>(std::min<size_t>(total, UINT32_MAX));
        row.since_ms = f.since_ms;
        Push(row, AddExtra(std::move(e), kind == ui::TreeRowKind::More ? uint8_t{2} : uint8_t{1}));
    }

    void Entry(int src, bool folder, uint16_t depth, const std::wstring& full) {
        ui::DetailsTreeRow row;
        row.depth = depth;
        row.expandable = folder && depth < app::kTreeMaxDepth;
        DetailsTreeFolder* f = nullptr;
        if (row.expandable) {
            const auto it = t.folders.find(FolderKey(full));
            if (it != t.folders.end()) f = &it->second;
        }
        row.expanded = f && f->expanded;
        Push(row, src);
        if (row.expanded) Children(*f, static_cast<uint16_t>(depth + 1));
    }

    // Alt+click: open a child folder one level further down.
    void Recurse(const DetailsTreeFolder& parent, const std::wstring& full) {
        DetailsTree::Recursion& r = t.recursion;
        if (r.id == 0 || r.stopped || parent.recurse_id != r.id) return;
        const std::wstring child_key = FolderKey(full);
        DetailsTreeFolder& c = t.folders[child_key];  // node references stay valid
        if (c.path.empty()) c.path = full;
        if (c.recurse_id == r.id) return;  // visited (maybe closed for size)
        c.recurse_id = r.id;
        c.recurse = parent.recurse - 1;
        if (c.expanded) return;            // the user had it open
        c.expanded = true;
        c.auto_opened = true;
        c.shown = app::kTreeBatch;
        if (c.state != FolderState::Loading) c.state = FolderState::Idle;
        r.opened.push_back(child_key);
    }

    void Children(DetailsTreeFolder& f, uint16_t depth) {
        t.visible_folders.push_back(f.path);
        DetailsTree::Recursion& r = t.recursion;
        const bool in_recursion = r.id != 0 && f.recurse_id == r.id;
        if (f.state == FolderState::Idle) RequestFolder(s, tab, f);
        if (!f.listing) {
            if (f.state == FolderState::Failed) {
                PushPlaceholder(ui::TreeRowKind::Failed, depth, f);
            } else {
                if (in_recursion) recursion_pending = true;
                PushPlaceholder(ui::TreeRowKind::Loading, depth, f);
            }
            return;
        }
        EnsureOrder(f, tab, key);
        if (in_recursion && f.auto_opened && f.order.size() > app::kTreeRecurseSkip) {
            // Too big to open on the way down: leave it closed.
            f.expanded = false;
            f.auto_opened = false;
            ReleaseListing(f);
            ++r.skipped;
            rows.back().expanded = false;
            t.visible_folders.pop_back();
            return;
        }
        if (f.order.empty()) {
            PushPlaceholder(ui::TreeRowKind::Empty, depth, f);
            return;
        }
        const size_t n = std::min(f.shown, f.order.size());
        if (in_recursion) {
            r.rows += n;
            if (r.rows >= app::kTreeRecurseRows) r.stopped = true;
        }
        const fs::SnapshotPtr listing = f.listing;  // f may be released below
        for (size_t k = 0; k < n; ++k) {
            fs::DirEntry child = (*listing)[static_cast<size_t>(f.order[k])];
            child.full_path = Join(f.path, child.name);
            const std::wstring full = child.full_path;
            const bool folder = Expandable(child);
            const int src = AddExtra(std::move(child), uint8_t{0});
            if (folder && in_recursion && f.recurse > 1) Recurse(f, full);
            Entry(src, folder, depth, full);
        }
        if (n < f.order.size()) PushPlaceholder(ui::TreeRowKind::More, depth, f, n, f.order.size());
    }
};

void RestoreStash(app::Tab& tab, DetailsTree& t) {
    if (t.stash_selected.empty() && t.stash_focus.empty()) return;
    const std::vector<fs::DirEntry>& c = *t.combined;
    std::unordered_map<std::wstring, int> where;
    for (size_t i = t.base_size; i < c.size(); ++i)
        if (!t.IsPlaceholder(i)) where.emplace(FolderKey(c[i].full_path), static_cast<int>(i));
    bool changed = false;
    for (const std::wstring& path : t.stash_selected) {
        const auto it = where.find(FolderKey(path));
        if (it == where.end()) continue;
        tab.selected.insert(it->second);
        changed = true;
    }
    if (!t.stash_focus.empty()) {
        const auto it = where.find(FolderKey(t.stash_focus));
        if (it != where.end()) {
            tab.selected_index = it->second;
            if (tab.selection_anchor < 0) tab.selection_anchor = it->second;
            changed = true;
        }
    }
    t.stash_selected.clear();
    t.stash_focus.clear();
    if (changed) ++tab.selection_revision;
}

// Returns true when a recursive expansion just finished (report it).
bool Rebuild(AppState& s, app::Tab& tab, DetailsTree& t, int key) {
    if (t.base == tab.snapshot && t.combined) {
        tab.StashTreeSelection();  // child indices are about to move
    } else {
        // A new listing of the folder (F5, a change patch): re-read the open
        // folders too. GetOrStart returns cached listings that are still fresh.
        for (auto& [folder_key, f] : t.folders) {
            (void)folder_key;
            if (f.expanded && f.state == FolderState::Ready) f.state = FolderState::Idle;
        }
    }
    const std::vector<fs::DirEntry>& base = *tab.snapshot;
    t.visible_folders.clear();
    Builder b{s, tab, t};
    b.key = key;
    b.base_size = base.size();
    b.rows.reserve(base.size());
    b.map.reserve(base.size());
    t.recursion.rows = 0;
    for (size_t i = 0; i < base.size(); ++i) {
        const fs::DirEntry& e = base[i];
        if (!tab.AllowsAttributes(e.attrs)) continue;
        const bool folder = Expandable(e);
        b.Entry(static_cast<int>(i), folder, 0,
                folder ? Join(tab.current_path, e.name) : std::wstring());
    }

    auto rows = std::make_shared<ui::DetailsTreeRows>();
    rows->rows = std::move(b.rows);
    rows->revision = t.revision;
    if (b.extras.empty()) {
        t.combined = tab.snapshot;
    } else {
        auto combined = std::make_shared<std::vector<fs::DirEntry>>();
        combined->reserve(base.size() + b.extras.size());
        combined->insert(combined->end(), base.begin(), base.end());
        combined->insert(combined->end(), std::make_move_iterator(b.extras.begin()),
                         std::make_move_iterator(b.extras.end()));
        t.combined = std::move(combined);
    }
    rows->view_of_source.assign(t.combined->size(), -1);
    for (size_t v = 0; v < b.map.size(); ++v)
        rows->view_of_source[static_cast<size_t>(b.map[v])] = static_cast<int>(v);
    t.map = std::make_shared<const std::vector<int>>(std::move(b.map));
    t.rows = std::move(rows);
    t.placeholder = std::move(b.placeholder);
    t.base = tab.snapshot;
    t.base_size = base.size();
    t.built_revision = t.revision;
    t.built_key = key;
    RestoreStash(tab, t);

    DetailsTree::Recursion& r = t.recursion;
    if (r.id != 0 && !r.reported && !b.recursion_pending) {
        r.reported = true;
        return true;
    }
    return false;
}

void ReportRecursion(AppState& s, const DetailsTree& t) {
    const DetailsTree::Recursion& r = t.recursion;
    int opened = 0;
    for (const std::wstring& k : r.opened) {
        const auto it = t.folders.find(k);
        if (it != t.folders.end() && it->second.expanded) ++opened;
    }
    // Nothing opened below the folder: it was a plain expand, no summary.
    if (opened == 0 && r.skipped == 0 && !r.stopped) return;
    if (!r.root_was_expanded) ++opened;
    wchar_t text[160]{};
    swprintf_s(text, l10n::Get(l10n::StringId::TreeExpandedFormat).c_str(), opened);
    std::wstring detail;
    if (r.skipped > 0) {
        wchar_t part[160]{};
        swprintf_s(part, l10n::Get(l10n::StringId::TreeSkippedFormat).c_str(), r.skipped);
        detail = part;
    }
    if (r.stopped) {
        wchar_t part[160]{};
        swprintf_s(part, l10n::Get(l10n::StringId::TreeStoppedFormat).c_str(),
                   format::GroupedInt(app::kTreeRecurseRows).c_str());
        if (!detail.empty()) detail += L" \u00B7 ";
        detail += part;
    }
    if (!s.hwnd) return;
    s.notification_toast.ShowSnack(
        s.hwnd, ui::SnackIcon::Undo, text, detail,
        {ui::SnackAction{l10n::Get(l10n::StringId::SnackUndo), WM_DETAILS_TREE_UNDO}});
}

void SetExpanded(app::Tab& tab, const std::wstring& path, bool expand) {
    DetailsTree& t = TreeOf(tab);
    DetailsTreeFolder& f = FolderFor(t, path);
    f.recurse = 0;
    f.auto_opened = false;
    if (f.expanded == expand) return;
    f.expanded = expand;
    if (expand) {
        f.shown = app::kTreeBatch;
        if (f.state != FolderState::Loading) f.state = FolderState::Idle;  // refresh on open
    } else {
        ReleaseListing(f);
    }
    ++t.revision;
}

void ExpandRecursive(app::Tab& tab, const std::wstring& path) {
    DetailsTree& t = TreeOf(tab);
    DetailsTreeFolder& f = FolderFor(t, path);
    t.recursion = {};
    t.recursion.id = ++g_last_recursion;
    t.recursion.root = FolderKey(path);
    t.recursion.root_was_expanded = f.expanded;
    if (!f.expanded) f.shown = app::kTreeBatch;
    f.expanded = true;
    f.auto_opened = false;
    f.recurse = app::kTreeRecurseDepth;
    f.recurse_id = t.recursion.id;
    if (f.state != FolderState::Loading) f.state = FolderState::Idle;
    ++t.revision;
}

// Alt+click on an open folder: close it and everything open below it.
void CollapseRecursive(app::Tab& tab, const std::wstring& path) {
    DetailsTree& t = TreeOf(tab);
    const std::wstring root = FolderKey(path);
    const std::wstring prefix = root + L'\\';
    for (auto& [k, f] : t.folders) {
        if (k != root && k.compare(0, prefix.size(), prefix) != 0) continue;
        if (!f.expanded) continue;
        f.expanded = false;
        f.recurse = 0;
        f.auto_opened = false;
        ReleaseListing(f);
    }
    ++t.revision;
}

// Pane view and rect of the hit pane (split layouts have one slot each).
const ui::PaneViewModel& HitPane(AppState& s, const ui::WindowViewModel& vm,
                                 const ui::HitTestResult& hit, D2D1_RECT_F& rect) {
    if (hit.pane_index >= 0 && hit.pane_index < static_cast<int>(vm.pane_slots.size())) {
        const auto& slot = vm.pane_slots[static_cast<size_t>(hit.pane_index)];
        rect = slot.rect;
        return slot.pane;
    }
    rect = FocusedPaneRect(s);
    return vm.pane;
}

// Focus `src` once the rows are rebuilt (child indices move on a rebuild).
void FocusAfterRebuild(app::Tab& tab, int src) {
    DetailsTree& t = TreeOf(tab);
    const auto* combined = tab.TreeEntries();
    if (!combined || src < 0 || static_cast<size_t>(src) >= combined->size()) return;
    if (static_cast<size_t>(src) < t.base_size) {
        tab.SelectOnly(src);
        return;
    }
    const std::wstring path = (*combined)[static_cast<size_t>(src)].full_path;
    tab.ClearSelection();
    t.stash_selected.assign(1, path);
    t.stash_focus = path;
}

// Closing a folder: a selection inside it moves to the folder.
void FocusFolderIfInside(app::Tab& tab, const ui::PaneViewModel& pane, int folder_view) {
    const ui::DetailsTreeRow* folder = pane.tree->At(folder_view);
    if (!folder) return;
    const int focus = pane.ViewIndex(tab.selected_index);
    if (focus <= folder_view) return;
    for (int v = folder_view + 1; v <= focus; ++v) {
        const ui::DetailsTreeRow* row = pane.tree->At(v);
        if (!row || row->depth <= folder->depth) return;  // left the subtree
    }
    FocusAfterRebuild(tab, pane.SourceIndex(folder_view));
}

void Toggle(AppState& s, app::Tab& tab, const ui::PaneViewModel& pane, int view, bool alt) {
    const ui::DetailsTreeRow* row = pane.tree->At(view);
    const int src = pane.SourceIndex(view);
    const std::wstring path = EntryFullPath(tab, src);
    if (!row || path.empty()) return;
    if (row->expanded) FocusFolderIfInside(tab, pane, view);
    if (alt) {
        if (row->expanded) CollapseRecursive(tab, path);
        else ExpandRecursive(tab, path);
    } else {
        SetExpanded(tab, path, !row->expanded);
    }
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

// "More" row actions: 0 show more, 1 show all, 2 open in a new tab.
void RunMoreAction(AppState& s, app::Tab& tab, int src, int action, bool focus_first_new) {
    const auto* combined = tab.TreeEntries();
    if (!combined || src < 0 || static_cast<size_t>(src) >= combined->size()) return;
    const std::wstring path = (*combined)[static_cast<size_t>(src)].full_path;
    if (action == 2) {
        OpenFolderTab(s, path);
        return;
    }
    DetailsTree& t = TreeOf(tab);
    DetailsTreeFolder& f = FolderFor(t, path);
    const size_t before = f.shown;
    f.shown = action == 1 ? f.order.size() : f.shown + app::kTreeMoreStep;
    if (focus_first_new && f.listing && before < f.order.size()) {
        const fs::DirEntry& first = (*f.listing)[static_cast<size_t>(f.order[before])];
        const std::wstring child = Join(f.path, first.name);
        tab.ClearSelection();
        t.stash_selected.assign(1, child);
        t.stash_focus = child;
    }
    ++t.revision;
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool IsStandIn(const ui::DetailsTreeRow* row) {
    return row && row->kind != ui::TreeRowKind::Entry && row->kind != ui::TreeRowKind::More;
}

} // namespace

void SyncDetailsTrees(AppState& s) {
    std::vector<const DetailsTree*> finished;
    bool watches_changed = false;
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (!tab) return;
        if (!Eligible(*tab)) {
            if (tab->tree && tab->tree->combined) {
                watches_changed |= !tab->tree->visible_folders.empty();
                tab->StashTreeSelection();
                tab->tree->Clear();
            }
            return;
        }
        DetailsTree& t = TreeOf(*tab);
        const int key = OrderKey(*tab);
        if (t.combined && t.base == tab->snapshot && t.built_revision == t.revision &&
            t.built_key == key) {
            return;
        }
        const auto old_folders = t.visible_folders;
        if (Rebuild(s, *tab, t, key)) finished.push_back(&t);
        watches_changed |= old_folders != t.visible_folders;
    });
    if (watches_changed) SyncVisibleWatches(s);
    for (const DetailsTree* t : finished) ReportRecursion(s, *t);
    // BuildVm owns this rebuild; open the editor later from the message loop
    // so ShowRenameOverlay cannot reenter an unfinished view build.
    if (s.hwnd && s.renameIndex < 0) {
        if (auto* tab = ActiveTab(s); tab && tab->TreeEntries() &&
            app::PendingCreateRenameIntent(tab->create_rename_intents, tab->current_path, tab->view_generation)) {
            const bool nested = std::any_of(tab->create_rename_intents.begin(), tab->create_rename_intents.end(),
                [](const auto& intent) { return intent.ready && !intent.target_parent.empty() && intent.target_parent != intent.parent; });
            if (nested) PostMessageW(s.hwnd, WM_DETAILS_TREE_RENAME, 0, 0);
        }
    }
}

void TryRenameDetailsTreeCreation(AppState& s) {
    auto* tab = ActiveTab(s);
    if (!tab || !tab->TreeEntries() || s.renameIndex >= 0) return;
    for (size_t i = 0; i < tab->EntryCount(); ++i) {
        if (IsDetailsTreePlaceholder(*tab, static_cast<int>(i))) continue;
        const auto entry = tab->EntryAt(i);
        const app::CreateRenameEntry candidate{entry.name, entry.full_path};
        const auto selected = app::SelectCreateRenameIntent(tab->create_rename_intents,
            tab->current_path, tab->view_generation, {&candidate, 1}, true, false);
        if (!selected) continue;
        tab->SelectOnly(static_cast<int>(i));
        EnsureRowVisible(s, *tab, static_cast<int>(i));
        ShowRenameOverlay(s);
        if (s.renameIndex == static_cast<int>(i))
            app::ConsumeCreateRenameIntent(tab->create_rename_intents, selected->task_id);
        return;
    }
}

void AppendDetailsTreeWatchPaths(const app::Tab& tab, std::vector<std::wstring>& paths) {
    if (!Eligible(tab) || !tab.tree) return;
    paths.insert(paths.end(), tab.tree->visible_folders.begin(), tab.tree->visible_folders.end());
}

bool NoteDetailsTreeChange(AppState& s, const std::wstring& path, bool poll) {
    bool changed = false;
    ForEachPane(s, [&](app::Pane& pane) {
        auto* tab = pane.ActiveTab();
        if (!tab || !Eligible(*tab) || !tab->tree) return;
        auto& tree = *tab->tree;
        if (std::none_of(tree.visible_folders.begin(), tree.visible_folders.end(),
                [&](const auto& folder) { return SamePath(folder, path); })) return;
        const auto it = tree.folders.find(FolderKey(path));
        if (it == tree.folders.end() || !it->second.expanded) return;
        auto& folder = it->second;
        if (poll && (!folder.listing || folder.state == FolderState::Failed)) return;
        s.store.MarkDirty(path);
        // Finish an in-flight read before following its dirty mark with one
        // more pass, rather than repeatedly cancelling during a download.
        if (folder.state != FolderState::Loading) folder.state = FolderState::Idle;
        ++tree.revision;
        changed = true;
    });
    return changed;
}

void NoteDetailsTreeResult(AppState& s, const app::WorkResult& res, bool again) {
    if (res.cancelled || fs::IsVirtualPath(res.path)) return;
    bool changed = false;
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (!tab || !tab->tree) return;
        DetailsTree& t = *tab->tree;
        const auto it = t.folders.find(FolderKey(res.path));
        if (it == t.folders.end()) return;
        DetailsTreeFolder& f = it->second;
        if (!f.expanded) {
            // Closed while reading: settle so the next open reads again.
            if (f.generation != 0 && res.generation >= f.generation) {
                f.generation = 0;
                f.state = FolderState::Idle;
            }
            return;
        }
        if (f.generation != 0) {
            if (res.generation < f.generation) return;
        } else if (res.error || !res.snapshot || !f.listing || f.listing == res.snapshot) {
            return;  // someone else's read; only a newer listing is of use
        }
        f.generation = 0;
        if (res.error) {
            f.state = f.listing ? FolderState::Ready : FolderState::Failed;
        } else if (res.snapshot) {
            f.listing = res.snapshot;
            f.state = FolderState::Ready;
            if (again) {
                s.store.MarkDirty(res.path);
                f.state = FolderState::Idle;
            }
        }
        ++t.revision;
        changed = true;
    });
    if (changed && s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool HandleDetailsTreeMouseDown(AppState& s, const ui::WindowViewModel& vm,
                                const ui::HitTestResult& hit, int x, int y, bool alt) {
    const bool sticky = hit.region == ui::HitTestResult::TreeSticky;
    if ((!sticky && hit.region != ui::HitTestResult::Row) || hit.index < 0) return false;
    D2D1_RECT_F rect{};
    const ui::PaneViewModel& pane = HitPane(s, vm, hit, rect);
    app::Tab* tab = ActiveTab(s);
    if (!pane.tree || !tab || !tab->TreeEntries()) return false;
    const int view = sticky ? hit.index : pane.ViewIndex(hit.index);
    const ui::DetailsTreeRow* row = pane.tree->At(view);
    if (!row) return false;
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    if (row->kind == ui::TreeRowKind::More) {
        const int action = s.renderer.TreeMoreActionAt(pane, rect, view, fx, fy);
        if (action >= 0) RunMoreAction(s, *tab, hit.index, action, false);
        else tab->SelectOnly(hit.index);
        return true;
    }
    if (row->kind != ui::TreeRowKind::Entry) return true;  // placeholders do nothing
    if (!row->expandable || s.renderer.TreeChevronAt(pane, rect, fx, fy) != view) return false;
    if (sticky) {
        if (s.scrollAnimating) CancelScrollAnimation(s);
        const float row_top = s.renderer.ItemRectInPane(pane, rect, view).top;
        const float list_top = s.renderer.PaneListRect(pane, rect).top;
        tab->scroll_y = (std::max)(0.0f, tab->scroll_y + row_top - list_top);
    }
    Toggle(s, *tab, pane, view, alt);
    return true;
}

bool HandleDetailsTreeKey(AppState& s, const ui::PaneViewModel& pane, WPARAM key) {
    if (key != VK_LEFT && key != VK_RIGHT && key != VK_MULTIPLY && key != VK_RETURN) return false;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 || (GetKeyState(VK_MENU) & 0x8000) != 0) return false;
    app::Tab* tab = ActiveTab(s);
    if (!pane.tree || !tab || !tab->TreeEntries() || tab->SelectedCount() > 1) return false;
    const int view = pane.ViewIndex(tab->selected_index);
    const ui::DetailsTreeRow* row = pane.tree->At(view);
    if (!row) return false;
    switch (key) {
    case VK_RETURN:
        if (row->kind != ui::TreeRowKind::More) return false;
        RunMoreAction(s, *tab, tab->selected_index, 0, true);
        return true;
    case VK_MULTIPLY: {
        // Always opens (Alt+click on an open folder closes instead).
        if (row->kind != ui::TreeRowKind::Entry || !row->expandable) return false;
        const std::wstring path = EntryFullPath(*tab, tab->selected_index);
        if (path.empty()) return false;
        ExpandRecursive(*tab, path);
        if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
        return true;
    }
    case VK_RIGHT: {
        if (row->kind != ui::TreeRowKind::Entry || !row->expandable) return false;
        if (!row->expanded) {
            Toggle(s, *tab, pane, view, false);
            return true;
        }
        const ui::DetailsTreeRow* next = pane.tree->At(view + 1);
        if (next && next->depth > row->depth && !IsStandIn(next)) {
            tab->SelectOnly(pane.SourceIndex(view + 1));
            EnsureRowVisible(s, *tab, tab->selected_index);
        }
        return true;
    }
    case VK_LEFT: {
        if (row->kind == ui::TreeRowKind::Entry && row->expandable && row->expanded) {
            Toggle(s, *tab, pane, view, false);
            return true;
        }
        const int parent = pane.tree->ParentRow(view);
        if (parent < 0) return false;
        tab->SelectOnly(pane.SourceIndex(parent));
        EnsureRowVisible(s, *tab, tab->selected_index);
        return true;
    }
    default:
        return false;
    }
}

int SkipDetailsTreePlaceholders(const ui::PaneViewModel& pane, int view, int dir) {
    if (!pane.tree) return view;
    const int n = static_cast<int>(pane.EntryCount());
    if (n <= 0 || view < 0 || view >= n) return view;
    const int step = dir < 0 ? -1 : 1;
    int v = view;
    while (v >= 0 && v < n && IsStandIn(pane.tree->At(v))) v += step;
    if (v < 0 || v >= n) {
        v = view;
        while (v >= 0 && v < n && IsStandIn(pane.tree->At(v))) v -= step;
    }
    return std::clamp(v, 0, n - 1);
}

bool IsDetailsTreePlaceholder(const app::Tab& tab, int index) {
    return index >= 0 && tab.TreeEntries() && tab.tree->IsPlaceholder(static_cast<size_t>(index));
}

bool PointOnDetailsTreeChevron(AppState& s, int pane_index, POINT point) {
    app::Pane* slot_pane = PaneAtSlot(s, pane_index);
    const app::Tab* tab = slot_pane ? slot_pane->ActiveTab() : ActiveTab(s);
    if (!tab || !tab->TreeEntries()) return false;
    const ui::WindowViewModel vm = BuildVm(s, false);
    ui::HitTestResult where;
    where.pane_index = pane_index;
    D2D1_RECT_F rect{};
    const ui::PaneViewModel& pane = HitPane(s, vm, where, rect);
    return pane.tree && s.renderer.TreeChevronAt(pane, rect, static_cast<float>(point.x),
                                                 static_cast<float>(point.y)) >= 0;
}

void UndoDetailsTreeExpand(AppState& s) {
    bool changed = false;
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (!tab || !tab->tree) return;
        DetailsTree& t = *tab->tree;
        DetailsTree::Recursion& r = t.recursion;
        if (r.id == 0 || r.id != g_last_recursion) return;
        for (const std::wstring& k : r.opened) {
            const auto it = t.folders.find(k);
            if (it == t.folders.end() || !it->second.auto_opened) continue;
            it->second.expanded = false;
            it->second.auto_opened = false;
            it->second.recurse = 0;
            ReleaseListing(it->second);
        }
        if (const auto root = t.folders.find(r.root); root != t.folders.end()) {
            root->second.recurse = 0;
            if (!r.root_was_expanded) {
                root->second.expanded = false;
                ReleaseListing(root->second);
            }
        }
        // A selection inside closed folders has nowhere to go; keep the rest.
        r = {};
        ++t.revision;
        changed = true;
    });
    if (changed && s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace pulse
