#include "folder_sizes_ui.h"
#include "folder_size_store.h"
#include "app_state.h"
#include "session.h"
#include "../common/text_format.h"
#include "../common/localization.h"
#include "app_runtime.h"
#include "entry_group.h"
#include "entry_sort.h"
#include <algorithm>
#include <unordered_set>

namespace pulse {
namespace {
// A Size sort needs every folder's total, not only the rows on screen (#58);
// half of the size cache leaves room for what other panes show.
constexpr size_t kSortRequestLimit = 2048;
// Live re-sorts copy the listing on the window thread: keep them small and paced.
constexpr size_t kLiveResortLimit = 20000;
constexpr uint64_t kResortIntervalMs = 750;
// Column view rows probed for emptiness, around the row on the navigation path.
constexpr int kColumnProbeRows = 128;

std::wstring ChildPath(const std::wstring& parent, const std::wstring& name) {
    return !parent.empty() && parent.back() == L'\\' ? parent + name : parent + L"\\" + name;
}

// Drive rows keep their drive icon; virtual and network places are never probed.
bool ProbesEmptiness(const std::wstring& path) {
    return !path.empty() && !fs::IsVirtualPath(path) && !fs::IsUncPath(path) &&
        !(path.size() <= 3 && path.size() >= 2 && path[1] == L':');
}

bool SortsByFolderSize(const ui::PaneViewModel& pane) {
    return pane.sort_column == ui::SortColumn::Size && pane.snapshot && pane.is_file_system &&
        !pane.is_search && !pane.path.empty() && !fs::IsVirtualPath(pane.path);
}
} // namespace

void FillFolderSizes(AppState& s, ui::WindowViewModel& vm) {
    struct Row { ui::PaneViewModel* pane; int index; std::wstring path; };
    struct ColumnRow { ui::ColumnStripColumnView* column; int source; std::wstring path; };
    std::vector<Row> rows, probe_rows;
    std::vector<ColumnRow> column_rows;
    std::vector<app::FolderSizeRequest> requests;
    std::vector<std::wstring> roots;
    // Column view: every column is a listing of its own; probe the rows near
    // the navigation path (or the scrolled-to rows) without size work.
    const auto probe_column = [&](ui::ColumnStripColumnView& column) {
        column.empty_sources.clear();
        if (column.loading || column.error || !column.snapshot || !column.rows || !ProbesEmptiness(column.path)) return;
        const auto& order = *column.rows;
        const int count = static_cast<int>(order.size());
        const int anchor = column.auto_scroll && column.highlight_row >= 0
            ? column.highlight_row - kColumnProbeRows / 2
            : static_cast<int>(column.scroll_dip / 28.0f) - kColumnProbeRows / 4;
        const int begin = std::clamp(anchor, 0, std::max(0, count - kColumnProbeRows));
        for (int r = begin; r < std::min(count, begin + kColumnProbeRows); ++r) {
            const int source = order[static_cast<size_t>(r)];
            if (source < 0 || static_cast<size_t>(source) >= column.snapshot->size()) continue;
            const auto& entry = (*column.snapshot)[static_cast<size_t>(source)];
            if (!entry.is_dir || entry.drive_type != 0) continue;
            column_rows.push_back({&column, source, ChildPath(column.path, entry.name)});
        }
    };
    for (auto& slot : vm.pane_slots) {
        auto& pane = slot.pane;
        pane.folder_size_labels.clear();
        pane.folder_size_actions.clear();
        pane.folder_size_muted.clear();
        pane.folder_size_running.clear();
        pane.folder_size_pending.clear();
        pane.folder_size_idle.clear();
        pane.empty_folders.clear();
        for (auto& column : pane.column_strip.ancestors) column.empty_sources.clear();
        pane.column_strip.child.empty_sources.clear();
        if (pane.column_strip.enabled && pane.column_strip.eligible) {
            for (auto& column : pane.column_strip.ancestors) probe_column(column);
            if (pane.column_strip.has_child) probe_column(pane.column_strip.child);
        }
        // Content hits are files held in a separate paged store, not snapshot/entries.
        if (pane.loading || pane.content_results ||
            (!pane.is_file_system && !pane.is_search) || pane.is_recycle) continue;
        const bool labels = ui::ShowsFolderSize(pane.view_mode);
        const bool sorted = SortsByFolderSize(pane);
        std::unordered_set<int> shown;
        const auto list = s.renderer.PaneListRect(pane, slot.rect);
        ui::ViewLayout layout(pane.view_mode, list, pane.EntryCount(), pane.scroll_x, pane.scroll_y, s.scale,
                              s.renderer.ListRowHeightDip(pane, list), pane.Groups(), s.renderer.ItemSelectionSlotDip());
        const auto [first, last] = layout.VisibleRange();
        const size_t probed_before = probe_rows.size();
        // Every view probes its visible folders for emptiness; label views also want sizes.
        for (int i = std::max(0, first); i <= last; ++i) {
            const int source = pane.SourceIndex(i);
            if (source < 0) continue;
            std::wstring path;
            if (pane.snapshot) {
                if (static_cast<size_t>(source) >= pane.snapshot->size()) continue;
                const auto& entry = (*pane.snapshot)[static_cast<size_t>(source)];
                if (!entry.is_dir) continue;
                path = entry.full_path.empty() && !pane.is_search ? ChildPath(pane.path, entry.name) : entry.full_path;
            } else {
                if (static_cast<size_t>(source) >= pane.entries.size()) continue;
                const auto& entry = pane.entries[static_cast<size_t>(source)];
                if (!entry.is_dir) continue;
                path = entry.path;
            }
            if (path.empty() || fs::IsVirtualPath(path)) continue;
            if (ProbesEmptiness(path)) probe_rows.push_back({&pane, source, path});
            if (!labels) continue;
            rows.push_back({&pane, source, path});
            requests.push_back({path, !fs::IsUncPath(path)});
            shown.insert(source);
        }
        if (sorted) {
            size_t count = shown.size();
            const auto& entries = *pane.snapshot;
            for (size_t i = 0; i < entries.size() && count < kSortRequestLimit; ++i) {
                const auto& entry = entries[i];
                if (!entry.is_dir || entry.drive_type != 0 || shown.contains(static_cast<int>(i))) continue;
                const std::wstring path = entry.full_path.empty() ? ChildPath(pane.path, entry.name) : entry.full_path;
                if (fs::IsVirtualPath(path)) continue;
                requests.push_back({path, !fs::IsUncPath(path), false});
                ++count;
            }
        }
        // The watch also keeps emptiness current, so List and Small icons watch too.
        if ((labels || sorted || probe_rows.size() > probed_before) && !pane.path.empty() && !fs::IsVirtualPath(pane.path))
            roots.push_back(pane.path);
    }
    if (!s.isolatedTest && !s.shot.active)
        s.folderSizes.SetCachePath([] {
            const auto dir = app::GetPulseDataDir();
            return dir.empty() ? std::wstring() : dir + L"\\folder_sizes.json";
        });
    if (s.isolatedTest && s.shot.active) {
        wchar_t cache[32768]{};
        const auto count = GetEnvironmentVariableW(L"PULSE_TEST_FOLDER_SIZE_CACHE", cache, ARRAYSIZE(cache));
        if (count && count < ARRAYSIZE(cache))
            s.folderSizes.SetCachePath([file = std::wstring(cache)] { return file; });
    }
    const bool probe_index = (s.isolatedTest || s.shot.active) &&
        GetEnvironmentVariableW(L"PULSE_TEST_FOLDER_INDEX", nullptr, 0) > 0;
    s.folderSizes.SetIndexEnabled((!s.isolatedTest && !s.shot.active) || probe_index);
    s.folderSizes.Sync(std::move(requests), std::move(roots));
    // Same visibility rule as the listing: a folder holding only entries the
    // current settings hide looks empty, like the listing inside it.
    const bool show_hidden = s.appPrefs.show_hidden_files;
    const bool show_protected = s.appPrefs.show_protected_os_files;
    std::vector<std::wstring> probes;
    probes.reserve(probe_rows.size() + column_rows.size());
    for (const auto& row : probe_rows) {
        if (app::FolderLooksEmpty(s.folderSizes.Content(row.path), show_hidden, show_protected))
            row.pane->empty_folders.insert(row.index);
        probes.push_back(row.path);
    }
    for (const auto& row : column_rows) {
        if (app::FolderLooksEmpty(s.folderSizes.Content(row.path), show_hidden, show_protected))
            row.column->empty_sources.insert(row.source);
        probes.push_back(row.path);
    }
    s.folderSizes.Probe(std::move(probes));
    for (const auto& row : rows) {
        // An empty folder says so instead of a size; it has nothing to calculate.
        if (row.pane->empty_folders.contains(row.index)) {
            row.pane->folder_size_muted.insert(row.index);
            row.pane->folder_size_labels.emplace(row.index, l10n::Pick(L"空", L"Empty"));
            continue;
        }
        const auto value = s.folderSizes.Get(row.path);
        const auto work = s.folderSizes.GetWork(row.path);
        // Without a value: an answer that is on its way (index reply, queue,
        // first scan delay) is a quiet placeholder, an active scan says so,
        // and only rows that will not resolve by themselves offer the action.
        const bool scanning = work.activity == app::FolderSizeActivity::Scanning ||
            (work.activity == app::FolderSizeActivity::Queued && work.manual);
        const bool pending = !value.has_value && !scanning &&
            (work.activity == app::FolderSizeActivity::Queued ||
             (work.activity == app::FolderSizeActivity::Idle && !fs::IsUncPath(row.path)));
        std::wstring text;
        if (value.has_value) text = format::ByteSize(value.bytes);
        else if (scanning) text = l10n::Get(l10n::StringId::Calculating);
        else if (pending) row.pane->folder_size_pending.insert(row.index);
        else { text = L"\u2014"; row.pane->folder_size_idle.insert(row.index); }
        if (value.has_value && value.partial) text = L"\u2265 " + text;
        if (value.has_value && (value.state == app::FolderSizeState::Cached ||
            value.source != app::FolderSizeSource::Scan)) row.pane->folder_size_muted.insert(row.index);
        if (work.Running()) row.pane->folder_size_running.insert(row.index);
        row.pane->folder_size_labels.emplace(row.index, std::move(text));
        // The size cell is the explicit calculate/cancel entry point; work
        // activity belongs to its tooltip, not the published numeric label.
        row.pane->folder_size_actions.insert(row.index);
    }
    for (const auto& slot : vm.pane_slots) if (slot.focused) {
        vm.pane.folder_size_labels = slot.pane.folder_size_labels;
        vm.pane.folder_size_actions = slot.pane.folder_size_actions;
        vm.pane.folder_size_muted = slot.pane.folder_size_muted;
        vm.pane.folder_size_running = slot.pane.folder_size_running;
        vm.pane.folder_size_pending = slot.pane.folder_size_pending;
        vm.pane.folder_size_idle = slot.pane.folder_size_idle;
        vm.pane.empty_folders = slot.pane.empty_folders;
        auto& ancestors = vm.pane.column_strip.ancestors;
        const auto& source = slot.pane.column_strip.ancestors;
        for (size_t i = 0; i < ancestors.size() && i < source.size(); ++i)
            if (ancestors[i].path == source[i].path) ancestors[i].empty_sources = source[i].empty_sources;
        if (vm.pane.column_strip.child.path == slot.pane.column_strip.child.path)
            vm.pane.column_strip.child.empty_sources = slot.pane.column_strip.child.empty_sources;
    }
}

std::wstring DescribeFolderSize(AppState& s, const std::wstring& path) {
    const auto value = s.folderSizes.Get(path);
    const auto work = s.folderSizes.GetWork(path);
    std::wstring text;
    if (!value.has_value) text = l10n::Pick(L"尚未统计文件夹大小", L"Folder size not calculated");
    else if (value.partial) text = l10n::Pick(L"部分统计 · 显示已知下限", L"Partial calculation · known lower bound");
    else if (value.source == app::FolderSizeSource::Index)
        text = l10n::Pick(L"索引估算 · 尚未完整核验", L"Index estimate · not fully verified");
    else if (value.state == app::FolderSizeState::Cached || value.source == app::FolderSizeSource::Unknown)
        text = l10n::Pick(L"上次统计 · 待核验", L"Previous calculation · awaiting verification");
    else text = value.verified ? l10n::Pick(L"完整统计 · 已监控目录变化", L"Complete calculation · changes monitored") :
        l10n::Pick(L"完整统计 · 本次扫描快照", L"Complete calculation · scan snapshot");
    if (value.has_value) {
        text += std::wstring(L"\n") + (value.partial ? l10n::Pick(L"至少 ", L"At least ") : L"") +
            format::ByteSize(value.bytes) + l10n::Pick(L" · 不跟随目录链接", L" · directory links excluded");
        if (value.verified_at) {
            const auto now = app::folder_size::NowUtcMs();
            const auto seconds = now >= value.verified_at ? (now - value.verified_at) / 1000 : 0;
            text += std::wstring(L"\n") + l10n::Pick(L"结果时间：", L"Calculated: ");
            if (seconds < 60) text += l10n::Pick(L"刚刚", L"just now");
            else if (seconds < 3600) text += std::to_wstring(seconds / 60) + l10n::Pick(L" 分钟前", L" minutes ago");
            else if (seconds < 86400) text += std::to_wstring(seconds / 3600) + l10n::Pick(L" 小时前", L" hours ago");
            else text += std::to_wstring(seconds / 86400) + l10n::Pick(L" 天前", L" days ago");
        }
    }
    // Explain omissions in the published result, not an unrelated in-flight subtotal.
    const auto issues = value.has_value ? value.issues : work.issues;
    const auto skipped = value.has_value ? value.skipped : work.skipped;
    if (issues || skipped) {
        text += std::wstring(L"\n") + l10n::Pick(L"未计入目录：", L"Excluded directories: ") + std::to_wstring(skipped);
        if (issues & app::SizeAccessDenied) text += l10n::Pick(L" · 权限不足", L" · access denied");
        if (issues & app::SizeOffline) text += l10n::Pick(L" · 离线", L" · offline");
        if (issues & app::SizeLink) text += l10n::Pick(L" · 目录链接", L" · directory links");
        if (issues & app::SizeIoError) text += l10n::Pick(L" · 读取失败", L" · I/O error");
    }
    if (work.Running()) {
        text += std::wstring(L"\n") + (work.activity == app::FolderSizeActivity::Queued ?
            l10n::Pick(L"等待统计：", L"Queued: ") : l10n::Pick(L"正在统计：", L"Calculating: ")) + std::to_wstring(work.entries) +
            l10n::Pick(L" 项 · ", L" entries · ") + format::ByteSize(work.bytes);
        text += std::wstring(L"\n") + (work.manual ? l10n::Pick(L"点击取消，保留上次结果", L"Click to cancel and keep the previous result")
                                          : l10n::Pick(L"点击优先完整统计", L"Click to prioritize a full calculation"));
    } else {
        if (work.activity == app::FolderSizeActivity::Deferred)
            text += std::wstring(L"\n") + l10n::Pick(L"已暂停自动统计，避免影响浏览", L"Automatic calculation paused to keep browsing responsive");
        if (work.activity == app::FolderSizeActivity::Failed)
            text += std::wstring(L"\n") + l10n::Pick(L"部分目录不可统计，已停止自动重试", L"Incomplete coverage; automatic retry is backed off");
        if (work.activity == app::FolderSizeActivity::Cancelled)
            text += std::wstring(L"\n") + l10n::Pick(L"统计已取消，保留原结果", L"Calculation cancelled; previous result retained");
        text += std::wstring(L"\n") + l10n::Pick(L"点击重新统计", L"Click to recalculate");
    }
    return text;
}

bool ResortForFolderSizes(AppState& s) {
    bool waiting = false;
    const uint64_t now = GetTickCount64();
    // Rows must not move under a press (the click resolves by index), a drag
    // or the rename box.
    const bool busy = s.scrollAnimating || s.scrollbarDragging ||
        s.renameIndex >= 0 || s.marqueeActive || s.dragPending ||
        (GetKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetKeyState(VK_RBUTTON) & 0x8000) != 0;
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (!tab || tab->sort_column != ui::SortColumn::Size || !tab->snapshot || tab->loading ||
            tab->pending_generation != 0 || tab->content_results || tab->search_content_active ||
            tab->current_path.empty() || fs::IsVirtualPath(tab->current_path) ||
            tab->snapshot_path != tab->current_path || !tab->held_renames.empty() ||
            tab->snapshot->size() > kLiveResortLimit)
            return;
        // Do not build/copy the size lookup on every timer tick during motion.
        if (busy) { waiting = true; return; }
        const auto sizes = s.folderSizes.KnownChildren(tab->current_path);
        const uint64_t signature = app::FolderSizeSignature(sizes);
        if (signature == tab->folder_size_signature) return;
        if (now - tab->folder_size_resorted_at < kResortIntervalMs) {
            waiting = true;
            return;
        }
        tab->folder_size_signature = signature;
        tab->folder_size_resorted_at = now;
        auto sorted = std::make_shared<std::vector<fs::DirEntry>>(*tab->snapshot);
        {
            const app::ScopedEntryGrouping grouping(tab->EffectiveGroup(), tab->current_path);
            app::SortEntriesBySize(*sorted, tab->sort_direction, sizes);
        }
        if (std::equal(sorted->begin(), sorted->end(), tab->snapshot->begin(), tab->snapshot->end(),
                       [](const fs::DirEntry& a, const fs::DirEntry& b) { return a.name == b.name; }))
            return;
        // The selection stays on the same items while the rows move.
        const bool all = tab->all_selected;
        std::vector<std::wstring> names;
        std::wstring focus;
        if (!all) {
            const int count = static_cast<int>(tab->EntryCount());
            for (const int index : tab->SelectedIndices())
                if (index >= 0 && index < count) names.push_back(tab->EntryAt(static_cast<size_t>(index)).name);
            if (tab->selected_index >= 0 && tab->selected_index < count)
                focus = tab->EntryAt(static_cast<size_t>(tab->selected_index)).name;
        }
        tab->SetSnapshot(std::move(sorted));
        tab->order_held = false;
        if (!all) {
            if (names.empty()) tab->ClearSelection();
            else tab->RemapSelection(names, focus);
        }
        if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
    });
    return waiting;
}
}
